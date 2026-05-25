"""
uzbek_lang.errors
=================

Uzbek-language error subsystem.

The job of this module is to take any Python ``Exception`` produced by
the Uzbek-PY compiler or runtime and render it as a clean, contextual
**Uzbek-language diagnostic** — with line, column, source excerpt,
caret indicator and a localised description.

Two flavours of error can occur:

* **Compile-time** (``SyntaxError`` raised by :func:`ast.parse`). The
  exception itself carries ``filename``, ``lineno``, ``offset`` and a
  description that we translate.
* **Run-time** (``NameError``, ``TypeError``, ``ZeroDivisionError`` …
  raised during ``exec``). We walk the traceback, find the deepest
  frame inside the user's ``.uz`` file and render *that* frame.
"""

from __future__ import annotations

import re
import traceback
from dataclasses import dataclass
from types import TracebackType
from typing import List, Optional

from uzbek_lang.translator import SourceMap


# --------------------------------------------------------------------------- #
# Translation tables                                                          #
# --------------------------------------------------------------------------- #

#: Python exception class name -> Uzbek display name.
ERROR_NAMES = {
    "SyntaxError":          "SintaksisXatoligi",
    "IndentationError":     "ChekinishXatoligi",
    "TabError":             "TabXatoligi",
    "NameError":            "NomXatoligi",
    "UnboundLocalError":    "BogʻlanmaganLokalXatoligi",
    "TypeError":            "TurXatoligi",
    "ValueError":           "QiymatXatoligi",
    "AttributeError":       "AtributXatoligi",
    "KeyError":             "KalitXatoligi",
    "IndexError":           "IndeksXatoligi",
    "ZeroDivisionError":    "NolgaBoʻlishXatoligi",
    "ArithmeticError":      "ArifmetikXato",
    "OverflowError":        "OshibKetishXatoligi",
    "FloatingPointError":   "SuzuvchiNuqtaXatoligi",
    "ImportError":          "YuklashXatoligi",
    "ModuleNotFoundError":  "ModulTopilmadiXatoligi",
    "FileNotFoundError":    "FaylTopilmadiXatoligi",
    "PermissionError":      "RuxsatXatoligi",
    "IsADirectoryError":    "BuPapkaXatoligi",
    "NotADirectoryError":   "PapkaEmasXatoligi",
    "IOError":              "KiritishChiqarishXatoligi",
    "OSError":              "TizimXatoligi",
    "RuntimeError":         "BajarilishXatoligi",
    "RecursionError":       "RekursiyaXatoligi",
    "StopIteration":        "IteratsiyaToʻxtadi",
    "StopAsyncIteration":   "AsinxronIteratsiyaToʻxtadi",
    "AssertionError":       "TasdiqlashXatoligi",
    "NotImplementedError":  "BajarilmaganXatoligi",
    "MemoryError":          "XotiraXatoligi",
    "SystemError":          "TizimMantiqiyXatoligi",
    "SystemExit":           "TizimdanChiqish",
    "KeyboardInterrupt":    "KlaviaturaUzilishi",
    "GeneratorExit":        "GeneratorChiqishi",
    "Exception":            "Istisno",
    "BaseException":        "AsosiyIstisno",
    "LookupError":          "QidiruvXatoligi",
    "EOFError":             "FaylOxiriXatoligi",
    "UnicodeError":         "UnikodXatoligi",
    "UnicodeDecodeError":   "UnikodDekodXatoligi",
    "UnicodeEncodeError":   "UnikodKodXatoligi",
    "UnicodeTranslateError":"UnikodTarjimaXatoligi",
}

#: Regex patterns that translate common exception messages into Uzbek.
#: The order matters: the first match wins.
_MESSAGE_PATTERNS: List[tuple] = [
    (re.compile(r"^name '(.+)' is not defined$"),
        lambda m: f"'{m.group(1)}' nomi aniqlanmagan"),
    (re.compile(r"^global name '(.+)' is not defined$"),
        lambda m: f"'{m.group(1)}' global nomi aniqlanmagan"),
    (re.compile(r"^local variable '(.+)' referenced before assignment$"),
        lambda m: f"'{m.group(1)}' lokal oʻzgaruvchisi qiymat berilishidan oldin chaqirildi"),
    (re.compile(r"^cannot access local variable '(.+)' .*$"),
        lambda m: f"'{m.group(1)}' lokal oʻzgaruvchisiga murojaat qilib boʻlmaydi"),
    (re.compile(r"^division by zero$"),
        lambda _m: "nolga boʻlish mumkin emas"),
    (re.compile(r"^integer division or modulo by zero$"),
        lambda _m: "butun sonni nolga boʻlish yoki nolga modul olish mumkin emas"),
    (re.compile(r"^float division by zero$"),
        lambda _m: "suzuvchi sonni nolga boʻlish mumkin emas"),
    (re.compile(r"^list index out of range$"),
        lambda _m: "roʻyxat indeksi diapazondan tashqarida"),
    (re.compile(r"^string index out of range$"),
        lambda _m: "satr indeksi diapazondan tashqarida"),
    (re.compile(r"^tuple index out of range$"),
        lambda _m: "kortej indeksi diapazondan tashqarida"),
    (re.compile(r"^'(.+)' object is not subscriptable$"),
        lambda m: f"'{m.group(1)}' turidagi obyektni indekslash mumkin emas"),
    (re.compile(r"^'(.+)' object is not callable$"),
        lambda m: f"'{m.group(1)}' turidagi obyektni chaqirish mumkin emas"),
    (re.compile(r"^'(.+)' object is not iterable$"),
        lambda m: f"'{m.group(1)}' turidagi obyekt iteratsiya qilinmaydi"),
    (re.compile(r"^'(.+)' object has no attribute '(.+)'$"),
        lambda m: f"'{m.group(1)}' turidagi obyektda '{m.group(2)}' atributi yoʻq"),
    (re.compile(r"^module '(.+)' has no attribute '(.+)'$"),
        lambda m: f"'{m.group(1)}' modulida '{m.group(2)}' atributi yoʻq"),
    (re.compile(r"^unsupported operand type\(s\) for (.+): '(.+)' and '(.+)'$"),
        lambda m: f"{m.group(1)} amali uchun '{m.group(2)}' va '{m.group(3)}' turlari mos kelmaydi"),
    (re.compile(r"^can('|\u2019)t multiply sequence by non-int of type '(.+)'$"),
        lambda m: f"ketma-ketlikni '{m.group(2)}' turidagi butun bo'lmagan qiymatga ko'paytirib bo'lmaydi"),
    (re.compile(r"^maximum recursion depth exceeded.*$"),
        lambda _m: "rekursiya chuqurligi maksimal qiymatdan oshib ketdi"),
    (re.compile(r"^expected an indented block.*$"),
        lambda _m: "chekingan blok kutilgan edi"),
    (re.compile(r"^unexpected indent$"),
        lambda _m: "kutilmagan chekinish"),
    (re.compile(r"^unindent does not match any outer indentation level$"),
        lambda _m: "chekinish hech qaysi tashqi daraja bilan mos kelmaydi"),
    (re.compile(r"^invalid syntax$"),
        lambda _m: "notoʻgʻri sintaksis"),
    (re.compile(r"^invalid character '(.+)' \(U\+([0-9A-Fa-f]+)\)$"),
        lambda m: f"notoʻgʻri belgi {m.group(1)!r} (U+{m.group(2)})"),
    (re.compile(r"^EOL while scanning string literal$"),
        lambda _m: "satr literali ichida qator oxiriga yetildi"),
    (re.compile(r"^EOF while scanning .*$"),
        lambda _m: "tahlil paytida fayl oxiriga yetildi"),
]


# --------------------------------------------------------------------------- #
# Public exception                                                            #
# --------------------------------------------------------------------------- #


class UzbekError(Exception):
    """Wraps any underlying Python exception and renders it in Uzbek."""

    def __init__(
        self,
        report: str,
        *,
        original: Optional[BaseException] = None,
    ) -> None:
        super().__init__(report)
        self.report = report
        self.original = original

    def __str__(self) -> str:
        return self.report


# --------------------------------------------------------------------------- #
# Diagnostic record                                                           #
# --------------------------------------------------------------------------- #


@dataclass
class Diagnostic:
    """Structured representation of a single Uzbek error diagnostic."""

    error_type:    str           # Python class name, e.g. "NameError"
    uzbek_type:    str           # translated name, e.g. "NomXatoligi"
    message:       str           # already translated
    filename:      str
    line:          Optional[int]
    col:           Optional[int]
    source_line:   Optional[str]

    def render(self) -> str:
        """Render the diagnostic as a multi-line, colour-free string."""

        loc_bits: List[str] = []
        if self.filename:
            loc_bits.append(f"{self.filename!r}")
        if self.line is not None:
            loc_bits.append(f"{self.line}-qator")
        if self.col is not None:
            loc_bits.append(f"{self.col + 1}-ustun")
        location = ", ".join(loc_bits) if loc_bits else "<nomaʼlum joy>"

        lines: List[str] = []
        lines.append("┏━━ Uzbek-PY xatolik hisoboti ━━━━━━━━━━━━━━━━━━━━━━")
        lines.append(f"┃ {self.uzbek_type}: {self.message}")
        lines.append(f"┃ Joylashuv: {location}")
        if self.source_line is not None:
            stripped = self.source_line.rstrip("\n")
            lines.append(f"┃")
            lines.append(f"┃   {stripped}")
            if self.col is not None and 0 <= self.col <= len(stripped):
                lines.append(f"┃   {' ' * self.col}^")
        lines.append("┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
        return "\n".join(lines)


# --------------------------------------------------------------------------- #
# Translators                                                                 #
# --------------------------------------------------------------------------- #


def translate_error_name(python_name: str) -> str:
    """Map a Python exception class name to its Uzbek equivalent."""

    return ERROR_NAMES.get(python_name, python_name)


def translate_error_message(message: str, source_map: Optional[SourceMap]) -> str:
    """Translate a Python exception message into Uzbek where possible.

    If a :class:`SourceMap` is supplied, Python identifiers that came
    from substituted Uzbek keywords are reverse-translated — so a
    ``NameError`` mentioning ``def`` becomes a message mentioning
    ``funksiya``.
    """

    msg = message.strip()

    # Reverse-translate any substituted names referenced in the message.
    if source_map is not None:
        for sub in source_map.substitutions:
            # Word-boundary substitution, double-quoted and single-quoted.
            msg = re.sub(
                rf"(?<![A-Za-z0-9_]){re.escape(sub.replacement)}(?![A-Za-z0-9_])",
                sub.original,
                msg,
            )

    for pattern, render in _MESSAGE_PATTERNS:
        m = pattern.match(msg)
        if m is not None:
            return render(m)

    return msg


# --------------------------------------------------------------------------- #
# Diagnostic construction                                                     #
# --------------------------------------------------------------------------- #


def diagnostic_from_syntax_error(
    exc: SyntaxError,
    source: str,
    source_map: Optional[SourceMap],
) -> Diagnostic:
    """Build a :class:`Diagnostic` for a compile-time ``SyntaxError``."""

    py_name = type(exc).__name__
    line = exc.lineno
    col = (exc.offset - 1) if exc.offset else None
    src_line = _line_at(source, line) if line else None

    return Diagnostic(
        error_type=py_name,
        uzbek_type=translate_error_name(py_name),
        message=translate_error_message(exc.msg or "", source_map),
        filename=exc.filename or "<uz>",
        line=line,
        col=col,
        source_line=src_line,
    )


def diagnostic_from_runtime_error(
    exc: BaseException,
    tb: Optional[TracebackType],
    source: str,
    source_map: Optional[SourceMap],
    filename: str,
) -> Diagnostic:
    """Build a :class:`Diagnostic` for a run-time exception.

    Walks the traceback and selects the deepest frame whose ``co_filename``
    matches the user's ``.uz`` file. Falls back to the deepest frame.
    """

    py_name = type(exc).__name__

    line: Optional[int] = None
    src_line: Optional[str] = None

    frame_tb = tb
    chosen_lineno: Optional[int] = None
    chosen_filename: Optional[str] = None

    while frame_tb is not None:
        co_file = frame_tb.tb_frame.f_code.co_filename
        if co_file == filename:
            chosen_lineno = frame_tb.tb_lineno
            chosen_filename = co_file
        elif chosen_lineno is None:
            chosen_lineno = frame_tb.tb_lineno
            chosen_filename = co_file
        frame_tb = frame_tb.tb_next

    line = chosen_lineno
    if line is not None:
        src_line = _line_at(source, line)

    return Diagnostic(
        error_type=py_name,
        uzbek_type=translate_error_name(py_name),
        message=translate_error_message(str(exc), source_map),
        filename=chosen_filename or filename,
        line=line,
        col=None,
        source_line=src_line,
    )


# --------------------------------------------------------------------------- #
# Top-level formatter                                                         #
# --------------------------------------------------------------------------- #


def format_exception(
    exc: BaseException,
    *,
    source: str,
    source_map: Optional[SourceMap] = None,
    filename: str = "<uz>",
) -> str:
    """Format any exception as a complete Uzbek diagnostic string."""

    if isinstance(exc, SyntaxError):
        diag = diagnostic_from_syntax_error(exc, source, source_map)
    else:
        diag = diagnostic_from_runtime_error(
            exc, exc.__traceback__, source, source_map, filename,
        )
    return diag.render()


def install_excepthook(
    *,
    source: str,
    source_map: Optional[SourceMap] = None,
    filename: str = "<uz>",
) -> None:                                                # pragma: no cover
    """Install a process-wide ``sys.excepthook`` that renders in Uzbek.

    Used by the CLI runner so that any uncaught exception thrown out of
    user code surfaces as a beautiful Uzbek report instead of a raw
    Python traceback.
    """

    import sys

    def _hook(exc_type, exc, tb):
        if isinstance(exc, KeyboardInterrupt):
            print("\nKlaviaturaUzilishi: dastur foydalanuvchi tomonidan toʻxtatildi")
            return
        print(format_exception(
            exc, source=source, source_map=source_map, filename=filename,
        ))

    sys.excepthook = _hook


# --------------------------------------------------------------------------- #
# Helpers                                                                     #
# --------------------------------------------------------------------------- #


def _line_at(source: str, lineno: int) -> Optional[str]:
    """Return ``lineno`` (1-indexed) of ``source``, or ``None`` if out of range."""

    if lineno is None or lineno < 1:
        return None
    lines = source.splitlines()
    if lineno - 1 >= len(lines):
        return None
    return lines[lineno - 1]


def _capture_traceback() -> str:                          # pragma: no cover
    """Return the current Python traceback as a string (debug helper)."""

    return traceback.format_exc()


__all__ = [
    "ERROR_NAMES",
    "UzbekError",
    "Diagnostic",
    "translate_error_name",
    "translate_error_message",
    "diagnostic_from_syntax_error",
    "diagnostic_from_runtime_error",
    "format_exception",
    "install_excepthook",
]
