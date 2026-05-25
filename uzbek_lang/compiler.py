"""
uzbek_lang.compiler
===================

The Uzbek-PY compiler — pipeline orchestrator.

This is the **AST Bridge**: it stitches the lexer, the translator and
CPython's own parser/compiler into a single zero-overhead pipeline.

  Uzbek source
       │
       ▼
   ┌───────────┐
   │  Lexer    │  uzbek_lang.lexer.UzbekLexer
   └─────┬─────┘
         │  Token stream  (with line/col/offset)
         ▼
   ┌───────────────┐
   │  Translator   │  uzbek_lang.translator.TokenTranslator
   └─────┬─────────┘
         │  Python source  (byte-aligned, + SourceMap)
         ▼
   ┌───────────────┐
   │  ast.parse    │  CPython's own parser
   └─────┬─────────┘
         │  ast.Module  (== "Uzbek AST", by structural identity)
         ▼
   ┌───────────────┐
   │   compile()   │  CPython's own bytecode compiler
   └─────┬─────────┘
         │  CodeType   (native CPython bytecode)
         ▼
       exec()  — full speed, zero interpretation overhead

Architectural note on the "AST Bridge"
--------------------------------------
After token-level translation, every Uzbek-PY construct is, by
construction, structurally identical to a Python construct: ``agar`` is
``if``, ``funksiya`` is ``def``, ``oraliq(1, n)`` is ``range(1, n)``,
and so on. Building a parallel AST hierarchy and walking it with a
``NodeTransformer`` to copy nodes one-for-one would add complexity and
runtime cost without changing the result. Instead, we let CPython's
parser produce the AST directly — it *is* our Uzbek AST, expressed in
Python's canonical form. This is what the spec calls "1:1 mapping" and
what makes the language genuinely zero-overhead.
"""

from __future__ import annotations

import ast
from dataclasses import dataclass, field
from types import CodeType
from typing import List, Optional

from uzbek_lang.lexer import Token, UzbekLexer
from uzbek_lang.translator import SourceMap, TokenTranslator


# --------------------------------------------------------------------------- #
# Compilation artefacts                                                       #
# --------------------------------------------------------------------------- #


@dataclass
class CompilationUnit:
    """Every artefact produced by a single compilation, all in one place.

    Useful for diagnostics, the ``--emit-python`` mode of the CLI, and
    any introspection a future debugger might need.
    """

    filename:      str
    uzbek_source:  str
    tokens:        List[Token]               = field(default_factory=list)
    python_source: str                       = ""
    source_map:    SourceMap                 = field(default_factory=SourceMap)
    ast_tree:      Optional[ast.AST]         = None
    code:          Optional[CodeType]        = None


# --------------------------------------------------------------------------- #
# Compiler                                                                    #
# --------------------------------------------------------------------------- #


class UzbekCompiler:
    """Turns Uzbek source into a native CPython :class:`CodeType`.

    The compiler is intentionally stateless across invocations. Every
    call to :meth:`compile_source` produces a fresh
    :class:`CompilationUnit`.
    """

    def __init__(self, *, optimize: int = -1) -> None:
        # ``optimize`` is forwarded straight to CPython's ``compile`` —
        # ``-1`` means "use the interpreter's default", which honours
        # ``-O`` / ``-OO`` flags transparently.
        self._optimize = optimize

    # --------------------------------------------------------------- public

    def compile_source(
        self,
        source: str,
        filename: str = "<uz>",
        *,
        mode: str = "exec",
    ) -> CompilationUnit:
        """Compile a complete Uzbek source string.

        Parameters
        ----------
        source:
            The raw ``.uz`` source text.
        filename:
            File name to embed in the resulting code object. CPython's
            tracebacks use this verbatim.
        mode:
            ``"exec"`` for a full module, ``"eval"`` for a single
            expression, ``"single"`` for a REPL line. Forwarded to
            :func:`compile`.
        """

        unit = CompilationUnit(filename=filename, uzbek_source=source)

        # --- 1. Lex ------------------------------------------------------
        unit.tokens = UzbekLexer(source, filename=filename).tokenize()

        # --- 2. Translate ------------------------------------------------
        result = TokenTranslator(source).translate(unit.tokens)
        unit.python_source = result.python_source
        unit.source_map    = result.source_map

        # --- 3. Parse to Python AST (the "AST Bridge") -------------------
        unit.ast_tree = ast.parse(
            unit.python_source,
            filename=filename,
            mode=mode,
            type_comments=False,
        )

        # --- 4. Compile to native CPython bytecode -----------------------
        unit.code = compile(
            unit.ast_tree,
            filename,
            mode,
            optimize=self._optimize,
            dont_inherit=False,
        )

        return unit

    def compile_file(self, path: str) -> CompilationUnit:
        """Read ``path`` from disk and compile it as a complete module."""

        with open(path, "r", encoding="utf-8") as fh:
            source = fh.read()
        return self.compile_source(source, filename=path)


# --------------------------------------------------------------------------- #
# Convenience                                                                 #
# --------------------------------------------------------------------------- #


def compile_uz(
    source: str,
    filename: str = "<uz>",
    *,
    mode: str = "exec",
    optimize: int = -1,
) -> CompilationUnit:
    """One-shot helper around :class:`UzbekCompiler.compile_source`."""

    return UzbekCompiler(optimize=optimize).compile_source(
        source, filename=filename, mode=mode,
    )


__all__ = [
    "CompilationUnit",
    "UzbekCompiler",
    "compile_uz",
]
