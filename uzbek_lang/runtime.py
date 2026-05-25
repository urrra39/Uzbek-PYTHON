"""
uzbek_lang.runtime
==================

Runtime environment and execution entry points for Uzbek-PY.

Built-in functions (``yoz``, ``kirit``, ``uzunlik``, ``oraliq``, …) are
deliberately *not* rewritten by the translator — they are injected here
as global aliases pointing at the equivalent CPython built-ins. This
keeps three properties:

* **Zero runtime overhead** — ``yoz`` is bound to the *same* function
  object as ``print``; calling it is a single ``LOAD_GLOBAL`` followed
  by the call, just like vanilla Python.
* **Dynamic-name correctness** — ``f"{oraliq(5)}"`` and
  ``getattr(obj, "yoz")`` keep working.
* **Clean shadowing** — ``yoz = "salom"`` rebinds *only* ``yoz``,
  it does not silently shadow Python's ``print``.
"""

from __future__ import annotations

import builtins
from types import CodeType
from typing import Any, Dict, Optional

from uzbek_lang.compiler import CompilationUnit, UzbekCompiler
from uzbek_lang.errors import UzbekError, format_exception
from uzbek_lang.translator import BUILTIN_MAP


# --------------------------------------------------------------------------- #
# Built-in environment                                                        #
# --------------------------------------------------------------------------- #


def build_uzbek_builtins() -> Dict[str, Any]:
    """Resolve :data:`uzbek_lang.translator.BUILTIN_MAP` against
    :mod:`builtins` and return a name -> callable mapping ready to be
    spread into a globals dict.

    Any entry whose Python target does not exist (e.g. on stripped-down
    Pythons) is silently skipped, so the function is forward-compatible.
    """

    env: Dict[str, Any] = {}
    for uz_name, py_name in BUILTIN_MAP.items():
        target = getattr(builtins, py_name, None)
        if target is not None:
            env[uz_name] = target
    return env


#: Module-level cache so we do not rebuild the dict on every run.
UZBEK_BUILTINS: Dict[str, Any] = build_uzbek_builtins()


# --------------------------------------------------------------------------- #
# Runtime                                                                     #
# --------------------------------------------------------------------------- #


class UzbekRuntime:
    """Compiles and executes Uzbek-PY programs.

    A runtime instance owns:

    * an :class:`UzbekCompiler` (stateless),
    * a *fresh* globals dict per execution, pre-populated with Uzbek
      built-in aliases.

    Multiple executions on the same runtime are independent, just like
    multiple ``python file.py`` invocations.
    """

    def __init__(self, *, optimize: int = -1) -> None:
        self._compiler = UzbekCompiler(optimize=optimize)

    # --------------------------------------------------------------- public

    def make_globals(self, *, filename: str = "<uz>") -> Dict[str, Any]:
        """Return a freshly-populated globals dict for one execution."""

        g: Dict[str, Any] = {
            "__name__":    "__main__",
            "__doc__":     None,
            "__package__": None,
            "__loader__":  None,
            "__spec__":    None,
            "__file__":    filename,
            "__builtins__": builtins,
        }
        g.update(UZBEK_BUILTINS)
        return g

    def execute(
        self,
        unit: CompilationUnit,
        *,
        globals_dict: Optional[Dict[str, Any]] = None,
        translate_errors: bool = True,
    ) -> Dict[str, Any]:
        """Execute a previously-compiled :class:`CompilationUnit`.

        Returns the globals dict so callers can inspect what the program
        defined. If ``translate_errors`` is true (the default) any
        exception thrown by user code is caught, rewritten into an
        :class:`UzbekError` carrying a beautiful Uzbek diagnostic, and
        re-raised.
        """

        if unit.code is None:                              # pragma: no cover
            raise RuntimeError("UzbekRuntime.execute called on uncompiled unit")

        g = globals_dict if globals_dict is not None else self.make_globals(
            filename=unit.filename,
        )

        try:
            exec(unit.code, g)                            # noqa: S102
        except UzbekError:
            raise
        except BaseException as exc:                      # noqa: BLE001
            if not translate_errors:
                raise
            report = format_exception(
                exc,
                source=unit.uzbek_source,
                source_map=unit.source_map,
                filename=unit.filename,
            )
            raise UzbekError(report, original=exc) from exc

        return g

    # ------------------------------------------------------------- helpers

    def run_source(
        self,
        source: str,
        filename: str = "<uz>",
        *,
        globals_dict: Optional[Dict[str, Any]] = None,
        translate_errors: bool = True,
    ) -> Dict[str, Any]:
        """Compile then execute an Uzbek source string in one call."""

        try:
            unit = self._compiler.compile_source(source, filename=filename)
        except SyntaxError as exc:
            if not translate_errors:
                raise
            # Compile-time errors do not have a SourceMap when ast.parse
            # fails before translation completes, so we recover whatever
            # we have. The compiler always sets python_source / source_map
            # before calling ast.parse, so they are available here.
            report = format_exception(
                exc, source=source, source_map=None, filename=filename,
            )
            raise UzbekError(report, original=exc) from exc

        return self.execute(
            unit,
            globals_dict=globals_dict,
            translate_errors=translate_errors,
        )

    def run_file(
        self,
        path: str,
        *,
        translate_errors: bool = True,
    ) -> Dict[str, Any]:
        """Read and execute an Uzbek source file."""

        with open(path, "r", encoding="utf-8") as fh:
            source = fh.read()
        return self.run_source(
            source, filename=path, translate_errors=translate_errors,
        )

    def compile_only(self, source: str, filename: str = "<uz>") -> CompilationUnit:
        """Return the :class:`CompilationUnit` without executing it.

        Handy for tooling such as ``engine.py --emit-python``.
        """

        return self._compiler.compile_source(source, filename=filename)


# --------------------------------------------------------------------------- #
# Module-level convenience                                                    #
# --------------------------------------------------------------------------- #


_DEFAULT_RUNTIME: Optional[UzbekRuntime] = None


def _runtime() -> UzbekRuntime:
    """Lazy singleton runtime for one-shot helpers."""

    global _DEFAULT_RUNTIME
    if _DEFAULT_RUNTIME is None:
        _DEFAULT_RUNTIME = UzbekRuntime()
    return _DEFAULT_RUNTIME


def run_source(source: str, filename: str = "<uz>") -> Dict[str, Any]:
    """One-shot: compile and execute an Uzbek source string."""

    return _runtime().run_source(source, filename=filename)


def run_file(path: str) -> Dict[str, Any]:
    """One-shot: compile and execute an Uzbek source file."""

    return _runtime().run_file(path)


__all__ = [
    "UZBEK_BUILTINS",
    "build_uzbek_builtins",
    "UzbekRuntime",
    "run_source",
    "run_file",
]
