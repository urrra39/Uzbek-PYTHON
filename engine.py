#!/usr/bin/env python3
"""
engine.py — Uzbek-PY command-line runner.

Usage
-----
::

    python engine.py path/to/program.uz             # run a script
    python engine.py -c 'yoz("Salom!")'             # run a one-liner
    python engine.py --emit-python program.uz       # show compiled Python
    python engine.py --tokens     program.uz        # dump the token stream
    python engine.py --ast        program.uz        # dump the Python AST
    python engine.py --version
    python engine.py                                # interactive REPL

Exit codes
----------
* ``0``   — program completed successfully.
* ``1``   — user code raised an uncaught exception (Uzbek diagnostic
            already printed).
* ``2``   — usage error (bad CLI flags or missing file).
"""

from __future__ import annotations

import argparse
import ast
import sys
from typing import List, Optional, Sequence

from uzbek_lang import (
    UzbekError,
    UzbekRuntime,
    __version__,
    format_exception,
)


# --------------------------------------------------------------------------- #
# CLI plumbing                                                                #
# --------------------------------------------------------------------------- #


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="engine.py",
        description=(
            "Uzbek-PY engine — Pythonning oʻzbekcha modern ukasi. "
            "Runs `.uz` programs by compiling them straight to native "
            "CPython bytecode."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "file",
        nargs="?",
        help="Path to an Uzbek source file (.uz). Omit for the REPL.",
    )
    p.add_argument(
        "-c", "--command",
        metavar="SOURCE",
        help="Execute SOURCE directly instead of reading a file.",
    )
    p.add_argument(
        "--emit-python",
        action="store_true",
        help="Print the compiled Python source instead of running it.",
    )
    p.add_argument(
        "--tokens",
        action="store_true",
        help="Print the lexer's token stream instead of running.",
    )
    p.add_argument(
        "--ast",
        action="store_true",
        help="Print the Python AST (ast.dump, indented) instead of running.",
    )
    p.add_argument(
        "-O", "--optimize",
        type=int, default=-1,
        help="Forwarded to compile() — -1 default, 0 none, 1 -O, 2 -OO.",
    )
    p.add_argument(
        "--no-translate-errors",
        action="store_true",
        help="Show raw Python tracebacks (debugging aid).",
    )
    p.add_argument(
        "-v", "--version",
        action="version",
        version=f"Uzbek-PY {__version__}",
    )
    return p


# --------------------------------------------------------------------------- #
# Entry points                                                                #
# --------------------------------------------------------------------------- #


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    runtime = UzbekRuntime(optimize=args.optimize)
    translate_errors = not args.no_translate_errors

    # Resolve the source.
    source: Optional[str] = None
    filename: str = "<uz>"

    if args.command is not None:
        source = args.command
        filename = "<command>"
    elif args.file is not None:
        try:
            with open(args.file, "r", encoding="utf-8") as fh:
                source = fh.read()
        except FileNotFoundError:
            print(
                f"FaylTopilmadiXatoligi: {args.file!r} fayli topilmadi",
                file=sys.stderr,
            )
            return 2
        filename = args.file
    else:
        # No file, no -c -> drop into REPL.
        return _repl(runtime, translate_errors=translate_errors)

    # Diagnostic-only modes (no execution).
    if args.tokens or args.emit_python or args.ast:
        return _diagnostic(
            runtime, source, filename,
            tokens=args.tokens,
            emit_python=args.emit_python,
            dump_ast=args.ast,
        )

    return _execute(runtime, source, filename, translate_errors=translate_errors)


# --------------------------------------------------------------------------- #
# Sub-commands                                                                #
# --------------------------------------------------------------------------- #


def _execute(
    runtime: UzbekRuntime,
    source: str,
    filename: str,
    *,
    translate_errors: bool,
) -> int:
    """Run a program. Return a Unix-style exit code."""

    try:
        runtime.run_source(
            source, filename=filename, translate_errors=translate_errors,
        )
    except UzbekError as err:
        print(err.report, file=sys.stderr)
        return 1
    except SystemExit as exc:                              # honour sys.exit()
        return int(exc.code) if isinstance(exc.code, int) else (
            0 if exc.code is None else 1
        )
    except KeyboardInterrupt:
        print(
            "\nKlaviaturaUzilishi: dastur foydalanuvchi tomonidan toʻxtatildi",
            file=sys.stderr,
        )
        return 130
    return 0


def _diagnostic(
    runtime: UzbekRuntime,
    source: str,
    filename: str,
    *,
    tokens: bool,
    emit_python: bool,
    dump_ast: bool,
) -> int:
    """Print compiler intermediates instead of running the program."""

    try:
        unit = runtime.compile_only(source, filename=filename)
    except SyntaxError as exc:
        print(format_exception(exc, source=source, filename=filename),
              file=sys.stderr)
        return 1

    if tokens:
        for tok in unit.tokens:
            print(tok)

    if emit_python:
        if tokens:
            print()
            print("# --- compiled Python source ---")
        print(unit.python_source, end="")

    if dump_ast:
        if tokens or emit_python:
            print()
            print("# --- Python AST ---")
        print(ast.dump(unit.ast_tree, indent=2))

    return 0


# --------------------------------------------------------------------------- #
# REPL                                                                        #
# --------------------------------------------------------------------------- #


def _repl(runtime: UzbekRuntime, *, translate_errors: bool) -> int:
    """A minimal, readline-friendly Uzbek-PY REPL."""

    banner = (
        f"Uzbek-PY {__version__}  —  Pythonning oʻzbekcha modern ukasi.\n"
        f"Chiqish uchun  Ctrl-D  yoki  yoz(\"chiqish\")  yozing.\n"
    )
    print(banner)

    globals_dict = runtime.make_globals(filename="<repl>")
    buffer: List[str] = []
    prompt_main = "uz> "
    prompt_cont = "..> "

    while True:
        try:
            prompt = prompt_cont if buffer else prompt_main
            line = input(prompt)
        except EOFError:
            print()
            return 0
        except KeyboardInterrupt:
            print("\nKlaviaturaUzilishi (qaytadan urining)")
            buffer = []
            continue

        # Empty line on a continuation buffer => execute.
        if not line.strip() and buffer:
            source = "\n".join(buffer)
            buffer = []
            _repl_exec(runtime, source, globals_dict, translate_errors)
            continue

        buffer.append(line)
        joined = "\n".join(buffer)

        # Statements that obviously start a block need more input.
        stripped = line.rstrip()
        if stripped.endswith(":") or stripped.endswith("\\"):
            continue

        # Try compiling what we have. If that succeeds and the buffer is
        # a single line, run it; if it fails because the source is
        # incomplete, keep accumulating.
        if len(buffer) == 1:
            try:
                runtime.compile_only(joined, filename="<repl>")
            except SyntaxError as exc:
                if _looks_incomplete(exc):
                    continue
                # Real syntax error — flush, report, restart.
                print(format_exception(exc, source=joined, filename="<repl>"),
                      file=sys.stderr)
                buffer = []
                continue
            buffer = []
            _repl_exec(runtime, joined, globals_dict, translate_errors)


def _repl_exec(
    runtime: UzbekRuntime,
    source: str,
    globals_dict: dict,
    translate_errors: bool,
) -> None:
    try:
        runtime.run_source(
            source,
            filename="<repl>",
            globals_dict=globals_dict,
            translate_errors=translate_errors,
        )
    except UzbekError as err:
        print(err.report, file=sys.stderr)


def _looks_incomplete(exc: SyntaxError) -> bool:
    """Heuristic: is this a 'need more input' SyntaxError?"""

    msg = (exc.msg or "").lower()
    if "unexpected eof" in msg:
        return True
    if "expected an indented block" in msg:
        return True
    if "was never closed" in msg:                          # 3.10+ message
        return True
    if "incomplete input" in msg:
        return True
    return False


# --------------------------------------------------------------------------- #
# Module entry                                                                #
# --------------------------------------------------------------------------- #


if __name__ == "__main__":
    sys.exit(main())
