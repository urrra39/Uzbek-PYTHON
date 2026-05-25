"""
Uzbek-PY — Pythonning oʻzbekcha modern ukasi.
=============================================

A high-performance programming language with 100% Uzbek syntax that
compiles directly to native CPython bytecode — zero interpretation
overhead, full standard-library compatibility.

Pipeline
--------
::

    lexer  ──► translator  ──► ast.parse  ──► compile  ──► exec
   (custom)    (token-level)      (CPython)    (CPython)   (CPython)

Quick start
-----------
::

    from uzbek_lang import run_source

    run_source('yoz("Salom, Olam!")')

CLI
---
::

    python engine.py path/to/program.uz

See the project README for the full language reference.
"""

from uzbek_lang.compiler import (
    CompilationUnit,
    UzbekCompiler,
    compile_uz,
)
from uzbek_lang.errors import (
    UzbekError,
    Diagnostic,
    format_exception,
    install_excepthook,
)
from uzbek_lang.lexer import (
    LexError,
    Token,
    TokenType,
    UzbekLexer,
    tokenize,
)
from uzbek_lang.runtime import (
    UZBEK_BUILTINS,
    UzbekRuntime,
    run_file,
    run_source,
)
from uzbek_lang.translator import (
    BUILTIN_MAP,
    KEYWORD_MAP,
    METHOD_MAP,
    SourceMap,
    Substitution,
    TokenTranslator,
    TranslationResult,
    translate,
)


__version__ = "1.0.0"

__all__ = [
    # version
    "__version__",
    # lexer
    "Token", "TokenType", "UzbekLexer", "LexError", "tokenize",
    # translator
    "KEYWORD_MAP", "METHOD_MAP", "BUILTIN_MAP",
    "Substitution", "SourceMap", "TranslationResult",
    "TokenTranslator", "translate",
    # compiler
    "CompilationUnit", "UzbekCompiler", "compile_uz",
    # runtime
    "UzbekRuntime", "UZBEK_BUILTINS", "run_source", "run_file",
    # errors
    "UzbekError", "Diagnostic", "format_exception", "install_excepthook",
]
