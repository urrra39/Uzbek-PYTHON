"""
uzbek_lang.translator
=====================

Token-level translation from Uzbek-PY to Python.

The translator is the *only* component that decides what an Uzbek
keyword "means". Everything downstream of it operates on plain Python
syntax, which is what lets the language inherit CPython's parser,
optimiser and bytecode interpreter for free.

Responsibilities
----------------
1. Hold the canonical translation tables: keywords, method names and
   built-in functions.
2. Walk a :class:`~uzbek_lang.lexer.Token` stream and produce a Python
   source string that is **byte-aligned** with the original wherever
   possible — every newline and (where the translation fits) every
   column survives the rewrite. This guarantees that line numbers in
   CPython tracebacks point at the right Uzbek line.
3. Emit a :class:`SourceMap` describing every substitution so that the
   error subsystem can reverse-translate diagnostic messages.

Design choices
--------------
* **Keywords** must be substituted at the token level — ``def`` is a
  reserved word in Python, so an "Uzbek keyword" can never survive
  parsing as an identifier.
* **Method names with non-Python characters** (``qo'sh``) must be
  substituted at the token level too, because ``ast.parse`` would
  otherwise reject the source.
* **Built-in functions** (``yoz``, ``oraliq``, …) are intentionally
  *not* rewritten here. They are injected at runtime as global
  aliases (see :mod:`uzbek_lang.runtime`). That way, dynamic uses such
  as ``f"{oraliq(5)}"`` or ``getattr(obj, 'qo\\'sh')`` keep working,
  and a user who shadows ``yoz`` does not silently shadow ``print``.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Tuple

from uzbek_lang.lexer import Token, TokenType


# --------------------------------------------------------------------------- #
# Translation tables                                                          #
# --------------------------------------------------------------------------- #

#: Uzbek keyword  ->  Python keyword. These are substituted in the token
#: stream so that the resulting source is parseable by ``ast.parse``.
KEYWORD_MAP: Dict[str, str] = {
    "funksiya":     "def",
    "qaytarsin":    "return",
    "agar":         "if",
    "yoki_agar":    "elif",
    "aks_holda":    "else",
    "uchun":        "for",
    "toki":         "while",
    "ichida":       "in",
    "Rost":         "True",
    "Yolgʻon":      "False",
    "Boʻsh":        "None",
    "va":           "and",
    "yoki":         "or",
    "emas":         "not",
    "harakat_qil":  "try",
    "istisno":      "except",
    "sinf":         "class",
    "yuklab_ol":    "import",
    # The following are not in the user-facing spec but are needed for a
    # complete language surface; they are listed as transparent aliases
    # so that nothing surprising happens if they appear in real code.
    "tanaffus":     "break",
    "davom":        "continue",
    "oxir_oqibat":  "finally",
    "tasdiqla":     "assert",
    "lambda_func":  "lambda",
    "global_e'lon": "global",
    "mahalliy":     "nonlocal",
    "bilan":        "with",
    "sifatida":     "as",
    "berib_yubor":  "yield",
    "o'chir":       "del",
    "uchun_passlash": "pass",
    "dan":          "from",
    "is_xuddi":     "is",
}

#: Method names with characters Python's tokenizer cannot accept. These
#: are also substituted in the token stream.
METHOD_MAP: Dict[str, str] = {
    "qo'sh": "append",
}

#: Built-in functions. **Not** substituted in the token stream — these
#: are injected as global aliases at runtime so that:
#:   * dynamic lookups (``f"{oraliq(5)}"``) keep working,
#:   * shadowing ``yoz`` only shadows ``yoz``, not Python's ``print``.
BUILTIN_MAP: Dict[str, str] = {
    "yoz":      "print",
    "kirit":    "input",
    "uzunlik":  "len",
    "oraliq":   "range",
}


# Combined "rewrite at token level" lookup. Methods and keywords share
# the same table because both are pure lexical substitutions.
_REWRITE_MAP: Dict[str, str] = {**KEYWORD_MAP, **METHOD_MAP}


# --------------------------------------------------------------------------- #
# Source map                                                                  #
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class Substitution:
    """A single Uzbek -> Python rewrite recorded by the translator."""

    original:   str
    replacement: str
    line:       int
    col:        int
    end_col:    int


@dataclass
class SourceMap:
    """Records every substitution made when rewriting Uzbek to Python.

    The map is consulted by :mod:`uzbek_lang.errors` so that a Python
    traceback referring to ``def`` can be displayed back to the user as
    referring to ``funksiya``.
    """

    substitutions: List[Substitution] = field(default_factory=list)

    # Reverse lookup, populated lazily.
    _by_python_name: Dict[str, str] = field(default_factory=dict, init=False)

    def add(self, sub: Substitution) -> None:
        self.substitutions.append(sub)
        self._by_python_name.setdefault(sub.replacement, sub.original)

    def uzbek_for(self, python_name: str) -> str:
        """Return the Uzbek spelling that mapped to ``python_name``,
        or ``python_name`` itself if no substitution was recorded."""

        return self._by_python_name.get(python_name, python_name)


# --------------------------------------------------------------------------- #
# Translator                                                                  #
# --------------------------------------------------------------------------- #


@dataclass
class TranslationResult:
    """Output of :meth:`TokenTranslator.translate`."""

    python_source: str
    source_map: SourceMap


class TokenTranslator:
    """Rewrite an Uzbek token stream into a Python source string.

    The output is *byte-aligned* with the input wherever possible:

    * Newlines are never added, removed or moved.
    * When the Python replacement is shorter than the Uzbek original
      (``funksiya`` -> ``def``), the gap is back-filled with spaces so
      that all subsequent column positions on the same line remain
      identical.
    * When the Python replacement is longer (``qo'sh`` -> ``append``),
      the rest of the line shifts right; this is rare and tolerated.
    """

    def __init__(self, source: str) -> None:
        self._source = source

    # --------------------------------------------------------------- public

    def translate(self, tokens: List[Token]) -> TranslationResult:
        """Translate ``tokens`` and produce equivalent Python source."""

        src = self._source
        out: List[str] = []
        cursor = 0
        smap = SourceMap()

        for tok in tokens:
            if tok.type is TokenType.EOF:
                # Flush whatever is left after the last real token.
                out.append(src[cursor:])
                cursor = len(src)
                break

            if tok.type is not TokenType.NAME:
                # Strings, comments, numbers, operators and newlines are
                # passed through unchanged.
                continue

            replacement = _REWRITE_MAP.get(tok.value)
            if replacement is None:
                # Not a keyword or special method, but the identifier
                # may still contain characters CPython's tokeniser
                # rejects (notably the ASCII apostrophe inside names
                # such as ``o'zi`` or ``xavfsiz_bo'lish``). Map those
                # to a Unicode-equivalent that *is* a valid Python
                # identifier character, leaving everything else alone.
                normalised = _normalise_identifier(tok.value)
                if normalised == tok.value:
                    continue
                replacement = normalised

            # Emit everything up to this token unchanged …
            out.append(src[cursor:tok.offset])

            # … then the (possibly padded) replacement …
            padded = self._pad_replacement(tok.value, replacement)
            out.append(padded)

            # … and remember the substitution for diagnostics.
            smap.add(Substitution(
                original=tok.value,
                replacement=replacement,
                line=tok.line,
                col=tok.col,
                end_col=tok.col + len(padded),
            ))

            cursor = tok.end_offset

        # If the loop fell through without seeing EOF (defensive), flush.
        if cursor < len(src):
            out.append(src[cursor:])

        return TranslationResult(
            python_source="".join(out),
            source_map=smap,
        )

    # --------------------------------------------------------------- helpers

    @staticmethod
    def _pad_replacement(original: str, replacement: str) -> str:
        """Pad ``replacement`` with trailing spaces to match the byte
        width of ``original`` when shorter.

        This keeps every column to the *right* of the substitution at
        its original offset, so CPython's reported error columns can be
        used directly without re-mapping.
        """

        delta = len(original) - len(replacement)
        if delta > 0:
            return replacement + " " * delta
        return replacement


# --------------------------------------------------------------------------- #
# Identifier normalisation                                                    #
# --------------------------------------------------------------------------- #


#: ASCII apostrophe is *not* a valid Python identifier character but it
#: is the conventional Uzbek typographic shortcut for U+02BC MODIFIER
#: LETTER APOSTROPHE, which **is** a valid Python identifier character
#: (Unicode category Lm, XID_Continue=Yes). Substituting it lets us
#: pass user-defined names like ``o'zi`` and ``xavfsiz_bo'lish`` through
#: ``ast.parse`` unchanged in meaning.
_APOSTROPHE_TRANSLITERATION = str.maketrans({"'": "\u02BC"})


def _normalise_identifier(name: str) -> str:
    """Map an Uzbek identifier to a Python-acceptable spelling."""

    if "'" in name:
        return name.translate(_APOSTROPHE_TRANSLITERATION)
    return name


# --------------------------------------------------------------------------- #
# Public surface                                                              #
# --------------------------------------------------------------------------- #


def translate(
    tokens: List[Token],
    source: str,
) -> TranslationResult:
    """One-shot helper around :class:`TokenTranslator.translate`."""

    return TokenTranslator(source).translate(tokens)


__all__ = [
    "KEYWORD_MAP",
    "METHOD_MAP",
    "BUILTIN_MAP",
    "Substitution",
    "SourceMap",
    "TranslationResult",
    "TokenTranslator",
    "translate",
]
