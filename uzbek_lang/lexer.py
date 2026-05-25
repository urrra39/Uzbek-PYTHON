"""
uzbek_lang.lexer
================

Hand-written lexer for the Uzbek-PY language.

It transforms a raw Uzbek source string into a stream of :class:`Token`
objects carrying byte-precise line, column and absolute-offset
information. The lexer is the only stage of the pipeline that ever
reads the original source, so any later stage that needs to point at a
piece of code does so through a token's offsets.

Design notes
------------
* The lexer is hand-written rather than ``re.scanner`` based, because
  Uzbek identifiers may legally contain the ASCII apostrophe
  (e.g. ``qo'sh``) and U+02BB MODIFIER LETTER TURNED COMMA
  (e.g. ``Yolgʻon``, ``Boʻsh``). No ``tokenize.generate_tokens`` style
  shortcut would handle ``qo'sh`` correctly — Python's tokenizer would
  see ``qo`` followed by an unterminated string literal.

* Whitespace inside lines is *not* emitted as a token. It is preserved
  implicitly through token offsets, which makes byte-perfect source
  reconstruction trivial: outside translated identifier spans the
  output is identical to the input.

* Strings, comments and numbers are recognised greedily and treated as
  opaque blobs — the lexer never inspects their interior. This keeps
  Uzbek keywords inside string literals from being accidentally
  translated.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import List, Optional


# --------------------------------------------------------------------------- #
# Public types                                                                #
# --------------------------------------------------------------------------- #


class TokenType(str, Enum):
    """The lexical category of a :class:`Token`."""

    NAME    = "NAME"      # identifier or Uzbek keyword
    NUMBER  = "NUMBER"    # int / float / hex / oct / bin / complex
    STRING  = "STRING"    # any quoted string, incl. f-strings & triples
    OP      = "OP"        # operators and punctuation
    NEWLINE = "NEWLINE"   # logical end-of-line
    COMMENT = "COMMENT"   # # ... to end-of-line
    EOF     = "EOF"       # synthetic end-of-stream marker


@dataclass(frozen=True)
class Token:
    """A single lexical unit of an Uzbek source file.

    Positions follow CPython conventions:

    * ``line`` / ``end_line`` are **1-indexed**.
    * ``col`` / ``end_col`` are **0-indexed** byte offsets within the line.
    * ``offset`` / ``end_offset`` are absolute byte offsets within the
      whole source string.
    """

    type:       TokenType
    value:      str
    line:       int
    col:        int
    end_line:   int
    end_col:    int
    offset:     int
    end_offset: int

    def __repr__(self) -> str:                       # pragma: no cover
        return (
            f"Token({self.type.name}, {self.value!r}, "
            f"{self.line}:{self.col}-{self.end_line}:{self.end_col})"
        )


class LexError(Exception):
    """Raised by :class:`UzbekLexer` when input cannot be tokenised."""

    def __init__(self, message: str, line: int, col: int):
        super().__init__(message)
        self.message = message
        self.line = line
        self.col = col


# --------------------------------------------------------------------------- #
# Character classes                                                           #
# --------------------------------------------------------------------------- #
#
# Uzbek-PY accepts the standard ASCII letter/digit set plus:
#   * underscore  ``_``
#   * Latin-1 + Latin Extended-A      (U+00C0 .. U+024F)  — for diacritics
#   * Cyrillic                         (U+0400 .. U+04FF)  — for Cyrillic Uzbek
#   * U+02BB MODIFIER LETTER TURNED COMMA  (the ``ʻ`` in ``Yolgʻon``)
#   * ASCII apostrophe ``'`` — only mid-identifier, only when followed by
#     a letter, so ``qo'sh`` is one token but ``'salom'`` is a string.

def _build_start_set() -> frozenset:
    chars: set = set()
    chars.update("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_")
    chars.update(chr(c) for c in range(0x00C0, 0x0250))
    chars.update(chr(c) for c in range(0x0400, 0x0500))
    return frozenset(chars)


def _build_cont_set() -> frozenset:
    chars: set = set(_IDENT_START)
    chars.update("0123456789")
    chars.add("\u02BB")  # ʻ
    return frozenset(chars)


_IDENT_START: frozenset = _build_start_set()
_IDENT_CONT:  frozenset = _build_cont_set()


# --------------------------------------------------------------------------- #
# Operator tables (longest-match wins)                                        #
# --------------------------------------------------------------------------- #

_THREE_OPS = ("**=", "//=", ">>=", "<<=", "...")

_TWO_OPS = (
    "**", "//", ">>", "<<", "<=", ">=", "==", "!=",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "@=",
    ":=", "->",
)

_ONE_OPS = frozenset("+-*/%@&|^~<>=(),:;[]{}.!?")

_STRING_PREFIXES = (
    # Order matters: longest first.
    "rb", "Rb", "rB", "RB", "br", "Br", "bR", "BR",
    "fr", "Fr", "fR", "FR", "rf", "Rf", "rF", "RF",
    "r", "R", "b", "B", "u", "U", "f", "F",
)


# --------------------------------------------------------------------------- #
# Lexer                                                                       #
# --------------------------------------------------------------------------- #


class UzbekLexer:
    """Tokenise an Uzbek source string into a list of :class:`Token`.

    The lexer is single-pass and allocates O(n) memory in the source
    length. It emits a synthetic :data:`TokenType.EOF` marker as the
    last token so consumers can dispatch on a final sentinel.
    """

    __slots__ = ("source", "filename", "_pos", "_line", "_col", "_n")

    def __init__(self, source: str, filename: str = "<uz>") -> None:
        # Always normalise CRLF / CR to LF up-front so column tracking
        # and offsets stay consistent regardless of the host platform.
        self.source = source.replace("\r\n", "\n").replace("\r", "\n")
        self.filename = filename
        self._pos = 0
        self._line = 1
        self._col = 0
        self._n = len(self.source)

    # --------------------------------------------------------------- public

    def tokenize(self) -> List[Token]:
        """Return the full token stream for ``self.source``."""

        tokens: List[Token] = []
        src = self.source
        n = self._n

        while self._pos < n:
            ch = src[self._pos]

            # 1. Newline
            if ch == "\n":
                tokens.append(self._emit(TokenType.NEWLINE, "\n", 1))
                self._line += 1
                self._col = 0
                continue

            # 2. Skip insignificant whitespace inside a line. Preserve
            #    indentation implicitly through offsets.
            if ch == " " or ch == "\t":
                self._pos += 1
                self._col += 1
                continue

            # 3. Line continuation:  "\\\n" — Python convention.
            if ch == "\\" and self._pos + 1 < n and src[self._pos + 1] == "\n":
                self._pos += 2
                self._line += 1
                self._col = 0
                continue

            # 4. Comment:  # ...  (to end-of-line, exclusive)
            if ch == "#":
                tokens.append(self._read_comment())
                continue

            # 5. String literal (with optional prefix).
            if ch == "'" or ch == '"' or self._looks_like_string_prefix():
                tokens.append(self._read_string())
                continue

            # 6. Number — leading digit, or ``.`` followed by digit.
            if ch.isdigit() or (
                ch == "."
                and self._pos + 1 < n
                and src[self._pos + 1].isdigit()
            ):
                tokens.append(self._read_number())
                continue

            # 7. Identifier or keyword.
            if ch in _IDENT_START:
                tokens.append(self._read_identifier())
                continue

            # 8. Operator / punctuation.
            op = self._read_operator()
            if op is not None:
                tokens.append(op)
                continue

            raise LexError(
                f"Lekser xatosi: kutilmagan belgi {ch!r}",
                self._line, self._col,
            )

        tokens.append(Token(
            TokenType.EOF, "",
            self._line, self._col,
            self._line, self._col,
            self._pos, self._pos,
        ))
        return tokens

    # ---------------------------------------------------------- helpers

    def _emit(
        self,
        kind: TokenType,
        value: str,
        consumed: int,
        *,
        end_line: Optional[int] = None,
        end_col:  Optional[int] = None,
    ) -> Token:
        """Build a token starting at the current position and advance."""

        start_line = self._line
        start_col = self._col
        start_off = self._pos

        self._pos += consumed
        if end_line is None:
            self._col += consumed
            end_line = start_line
            end_col = self._col
        else:
            assert end_col is not None
            self._col = end_col

        return Token(
            kind, value,
            start_line, start_col,
            end_line, end_col,  # type: ignore[arg-type]
            start_off, self._pos,
        )

    # ------- comments

    def _read_comment(self) -> Token:
        src = self.source
        start = self._pos
        while self._pos < self._n and src[self._pos] != "\n":
            self._pos += 1
        value = src[start:self._pos]
        end_col = self._col + (self._pos - start)
        tok = Token(
            TokenType.COMMENT, value,
            self._line, self._col,
            self._line, end_col,
            start, self._pos,
        )
        self._col = end_col
        return tok

    # ------- strings

    def _looks_like_string_prefix(self) -> bool:
        '''True when the cursor is at e.g. ``f"``, ``rb'``, ``B"""``...'''

        src = self.source
        for prefix in _STRING_PREFIXES:
            plen = len(prefix)
            if (
                self._pos + plen < self._n
                and src[self._pos:self._pos + plen] == prefix
                and src[self._pos + plen] in ("'", '"')
            ):
                return True
        return False

    def _read_string(self) -> Token:
        """Consume a complete string literal — single, triple, prefixed."""

        src = self.source
        start = self._pos
        start_line = self._line
        start_col = self._col

        # Optional prefix (longest match wins).
        prefix_len = 0
        for prefix in _STRING_PREFIXES:
            plen = len(prefix)
            if src[self._pos:self._pos + plen] == prefix and (
                self._pos + plen < self._n
                and src[self._pos + plen] in ("'", '"')
            ):
                prefix_len = plen
                break

        self._pos += prefix_len
        self._col += prefix_len

        if self._pos >= self._n:
            raise LexError(
                "Qator xatosi: tugatilmagan satr literali",
                start_line, start_col,
            )

        quote = src[self._pos]
        triple = (
            self._pos + 2 < self._n
            and src[self._pos + 1] == quote
            and src[self._pos + 2] == quote
        )

        if triple:
            self._pos += 3
            self._col += 3
            self._consume_triple_quoted(quote, start_line, start_col)
        else:
            self._pos += 1
            self._col += 1
            self._consume_single_quoted(quote, start_line, start_col)

        end_line = self._line
        end_col = self._col

        return Token(
            TokenType.STRING,
            src[start:self._pos],
            start_line, start_col,
            end_line, end_col,
            start, self._pos,
        )

    def _consume_single_quoted(
        self, quote: str, start_line: int, start_col: int,
    ) -> None:
        src = self.source
        while self._pos < self._n:
            ch = src[self._pos]
            if ch == "\\" and self._pos + 1 < self._n:
                # Skip the escape and the escaped character. If the
                # escape is ``\\\n`` (line-continuation in the string)
                # update the line counter.
                if src[self._pos + 1] == "\n":
                    self._pos += 2
                    self._line += 1
                    self._col = 0
                else:
                    self._pos += 2
                    self._col += 2
                continue
            if ch == "\n":
                raise LexError(
                    "Qator xatosi: bir qatorli satr literali yopilmagan",
                    start_line, start_col,
                )
            if ch == quote:
                self._pos += 1
                self._col += 1
                return
            self._pos += 1
            self._col += 1
        raise LexError(
            "Qator xatosi: satr literali yopilmagan (EOF)",
            start_line, start_col,
        )

    def _consume_triple_quoted(
        self, quote: str, start_line: int, start_col: int,
    ) -> None:
        src = self.source
        triple = quote * 3
        while self._pos < self._n:
            if src[self._pos:self._pos + 3] == triple:
                self._pos += 3
                self._col += 3
                return
            ch = src[self._pos]
            if ch == "\\" and self._pos + 1 < self._n:
                if src[self._pos + 1] == "\n":
                    self._pos += 2
                    self._line += 1
                    self._col = 0
                else:
                    self._pos += 2
                    self._col += 2
                continue
            if ch == "\n":
                self._pos += 1
                self._line += 1
                self._col = 0
                continue
            self._pos += 1
            self._col += 1
        raise LexError(
            "Qator xatosi: uch qoʻshtirnoqli satr yopilmagan",
            start_line, start_col,
        )

    # ------- numbers

    def _read_number(self) -> Token:
        """Consume an integer / float / hex / oct / bin / complex literal."""

        src = self.source
        start = self._pos
        start_line = self._line
        start_col = self._col

        # Prefixed integer (0x, 0o, 0b).
        if (
            src[self._pos] == "0"
            and self._pos + 1 < self._n
            and src[self._pos + 1] in "xXoObB"
        ):
            self._pos += 2
            self._col += 2
            while self._pos < self._n and (
                src[self._pos].isalnum() or src[self._pos] == "_"
            ):
                self._pos += 1
                self._col += 1
        else:
            self._consume_decimal_number()

        # Trailing ``j``/``J`` for complex.
        if self._pos < self._n and src[self._pos] in "jJ":
            self._pos += 1
            self._col += 1

        return Token(
            TokenType.NUMBER, src[start:self._pos],
            start_line, start_col,
            self._line, self._col,
            start, self._pos,
        )

    def _consume_decimal_number(self) -> None:
        src = self.source

        # Integer part.
        while self._pos < self._n and (
            src[self._pos].isdigit() or src[self._pos] == "_"
        ):
            self._pos += 1
            self._col += 1

        # Fractional part.
        if (
            self._pos < self._n
            and src[self._pos] == "."
            and (
                self._pos + 1 >= self._n
                or src[self._pos + 1].isdigit()
                or src[self._pos + 1] == "_"
            )
        ):
            self._pos += 1
            self._col += 1
            while self._pos < self._n and (
                src[self._pos].isdigit() or src[self._pos] == "_"
            ):
                self._pos += 1
                self._col += 1

        # Exponent.
        if self._pos < self._n and src[self._pos] in "eE":
            self._pos += 1
            self._col += 1
            if self._pos < self._n and src[self._pos] in "+-":
                self._pos += 1
                self._col += 1
            while self._pos < self._n and (
                src[self._pos].isdigit() or src[self._pos] == "_"
            ):
                self._pos += 1
                self._col += 1

    # ------- identifiers

    def _read_identifier(self) -> Token:
        """Consume an Uzbek-aware identifier.

        Allows mid-token ASCII apostrophes only when the next character
        is a letter — so ``qo'sh`` lexes as one identifier but
        ``salom = 'a'`` lexes as ``salom``, ``=``, ``'a'``.
        """

        src = self.source
        start = self._pos
        start_line = self._line
        start_col = self._col

        # First char already known to be in _IDENT_START.
        self._pos += 1
        self._col += 1

        while self._pos < self._n:
            ch = src[self._pos]
            if ch in _IDENT_CONT:
                self._pos += 1
                self._col += 1
                continue
            if ch == "'" and self._pos + 1 < self._n and src[self._pos + 1] in _IDENT_START:
                self._pos += 1
                self._col += 1
                continue
            break

        return Token(
            TokenType.NAME, src[start:self._pos],
            start_line, start_col,
            self._line, self._col,
            start, self._pos,
        )

    # ------- operators

    def _read_operator(self) -> Optional[Token]:
        src = self.source
        start = self._pos
        start_line = self._line
        start_col = self._col

        # Three-character operators first.
        if self._pos + 3 <= self._n and src[self._pos:self._pos + 3] in _THREE_OPS:
            return self._emit_op(src[self._pos:self._pos + 3], 3, start, start_line, start_col)

        # Then two-character.
        if self._pos + 2 <= self._n and src[self._pos:self._pos + 2] in _TWO_OPS:
            return self._emit_op(src[self._pos:self._pos + 2], 2, start, start_line, start_col)

        # Finally single-character.
        if src[self._pos] in _ONE_OPS:
            return self._emit_op(src[self._pos], 1, start, start_line, start_col)

        return None

    def _emit_op(
        self,
        value: str,
        consumed: int,
        start: int,
        start_line: int,
        start_col: int,
    ) -> Token:
        self._pos += consumed
        self._col += consumed
        return Token(
            TokenType.OP, value,
            start_line, start_col,
            self._line, self._col,
            start, self._pos,
        )


# --------------------------------------------------------------------------- #
# Convenience                                                                 #
# --------------------------------------------------------------------------- #


def tokenize(source: str, filename: str = "<uz>") -> List[Token]:
    """One-shot helper around :class:`UzbekLexer.tokenize`."""

    return UzbekLexer(source, filename).tokenize()


__all__ = [
    "Token",
    "TokenType",
    "LexError",
    "UzbekLexer",
    "tokenize",
]
