# Uzbek-PY

> **Pythonning oʻzbekcha modern ukasi.**
> A high-performance programming language with **100% Uzbek syntax** that
> compiles directly to native CPython bytecode.

<p align="center">
  <img alt="status"  src="https://img.shields.io/badge/status-production-green">
  <img alt="python"  src="https://img.shields.io/badge/python-3.10%2B-blue">
  <img alt="license" src="https://img.shields.io/badge/license-MIT-lightgrey">
  <img alt="overhead" src="https://img.shields.io/badge/runtime%20overhead-0%25-brightgreen">
</p>

```uzbek
funksiya salom_ber(ism):
    yoz("Salom,", ism, "!")

salom_ber("Olam")
```

```text
$ python engine.py salom.uz
Salom, Olam !
```

---

## Table of contents

1. [Why Uzbek-PY](#why-uzbek-py)
2. [Quick start](#quick-start)
3. [The Golden Test](#the-golden-test)
4. [Architecture: the Zero-Overhead Compiler](#architecture-the-zero-overhead-compiler)
5. [Language reference](#language-reference)
6. [Beautiful Uzbek diagnostics](#beautiful-uzbek-diagnostics)
7. [CLI](#cli)
8. [Performance](#performance)
9. [Project layout](#project-layout)
10. [License](#license)

---

## Why Uzbek-PY

Uzbek-PY is a **compiler**, not an interpreter loop. It uses Python's own
parser and bytecode VM as its execution engine. That gives it three
properties that almost no toy language has at once:

* **Native CPython speed** — code runs at *literal* `compile() + exec()`
  speed. Within measurement noise of vanilla Python.
* **Full standard-library compatibility** — `import json`, `import
  asyncio`, `import numpy` all work because the AST CPython sees is
  ordinary Python.
* **Real diagnostics** — line, column, source excerpt, caret, and a
  description **localised into Uzbek**. No raw English tracebacks in
  front of an Uzbek user.

It runs the exact program below — every keyword, every method,
every literal in Uzbek — at native speed:

```uzbek
funksiya hisobla_va_sarala(chegara):
    natija = []
    uchun son ichida oraliq(1, chegara):
        agar son % 2 == 0 va son % 3 == 0:
            natija.qo'sh(son * 10)
        yoki_agar son % 5 == 0:
            natija.qo'sh(son * 5)
        aks_holda:
            natija.qo'sh(son)
    qaytarsin natija
```

---

## Quick start

```bash
git clone https://github.com/urrra39/Uzbek-PYTHON.git
cd Uzbek-PYTHON

# Run a script
python engine.py examples/golden.uz

# Run a one-liner
python engine.py -c 'yoz("Salom,", "Olam!")'

# Drop into the REPL
python engine.py
```

No dependencies. Pure standard-library Python (3.10+).

---

## The Golden Test

`examples/golden.uz` is the exact program from the design spec, written
in 100% Uzbek:

```uzbek
# Python-ning o'zbekcha modern ukasi
funksiya hisobla_va_sarala(chegara):
    natija = []
    uchun son ichida oraliq(1, chegara):
        agar son % 2 == 0 va son % 3 == 0:
            natija.qo'sh(son * 10)
        yoki_agar son % 5 == 0:
            natija.qo'sh(son * 5)
        aks_holda:
            natija.qo'sh(son)
    qaytarsin natija

hisoblar = hisobla_va_sarala(15)
yoz("Yakuniy ro'yxat:", hisoblar)
yoz("Ro'yxat uzunligi:", uzunlik(hisoblar))
```

```text
$ python engine.py examples/golden.uz
Yakuniy ro'yxat: [1, 2, 3, 4, 25, 60, 7, 8, 9, 50, 11, 120, 13, 14]
Ro'yxat uzunligi: 14
```

The output is **byte-identical** to the equivalent Python program.

---

## Architecture: the Zero-Overhead Compiler

```
   Uzbek source (.uz)
         │
         ▼
   ┌─────────────┐    Hand-written lexer with Uzbek-aware
   │   Lexer     │    identifier rules (qo'sh, Yolgʻon, Boʻsh).
   └──────┬──────┘
          │  Token stream  (line, col, byte-offset)
          ▼
   ┌─────────────┐    Token-level keyword and method substitution,
   │ Translator  │    column-preserving via space padding.
   └──────┬──────┘    Builds a SourceMap for error reverse-translation.
          │  Python source  (byte-aligned with the original)
          ▼
   ┌─────────────┐    CPython's own parser. By construction the
   │  ast.parse  │    Uzbek AST is a Python AST — 1:1, identity bridge.
   └──────┬──────┘
          │  ast.Module
          ▼
   ┌─────────────┐    CPython's own bytecode compiler.
   │  compile()  │    Honours -O / -OO / type hints / decorators / etc.
   └──────┬──────┘
          │  CodeType
          ▼
        exec()  ─►  native CPython VM, full speed, zero overhead.
```

**Why no separate "Uzbek AST"?** Because after lexical translation,
every Uzbek-PY construct *is* a Python construct. Building a parallel
AST hierarchy and walking it with a `NodeTransformer` to copy nodes
one-for-one would add code, complexity and runtime cost without
changing the result. Letting CPython's parser produce the AST directly
*is* the 1:1 bridge — and it is what makes the language genuinely
zero-overhead.

**Built-ins are runtime aliases**, not source rewrites. `yoz` is bound
to the *same function object* as `print`. Calling it is one
`LOAD_GLOBAL` plus one `CALL` — exactly like vanilla Python.

You can inspect every stage:

```bash
python engine.py --tokens       examples/golden.uz   # lexer output
python engine.py --emit-python  examples/golden.uz   # translated source
python engine.py --ast          examples/golden.uz   # ast.dump tree
```

---

## Language reference

### Keywords

| Uzbek         | Python    |
|---------------|-----------|
| `funksiya`    | `def`     |
| `qaytarsin`   | `return`  |
| `agar`        | `if`      |
| `yoki_agar`   | `elif`    |
| `aks_holda`   | `else`    |
| `uchun`       | `for`     |
| `toki`        | `while`   |
| `ichida`      | `in`      |
| `Rost`        | `True`    |
| `Yolgʻon`     | `False`   |
| `Boʻsh`       | `None`    |
| `va`          | `and`     |
| `yoki`        | `or`      |
| `emas`        | `not`     |
| `harakat_qil` | `try`     |
| `istisno`     | `except`  |
| `sinf`        | `class`   |
| `yuklab_ol`   | `import`  |

Plus the rest of Python's surface as transparent aliases: `tanaffus`
(`break`), `davom` (`continue`), `oxir_oqibat` (`finally`), `tasdiqla`
(`assert`), `bilan` (`with`), `sifatida` (`as`), `berib_yubor`
(`yield`), `o'chir` (`del`), `dan` (`from`), and more — see
[`uzbek_lang/translator.py`](./uzbek_lang/translator.py).

### Built-in functions

| Uzbek       | Python   |
|-------------|----------|
| `yoz(...)`  | `print`  |
| `kirit(...)`| `input`  |
| `uzunlik(x)`| `len`    |
| `oraliq(...)` | `range` |

### Built-in methods

| Uzbek         | Python    |
|---------------|-----------|
| `lst.qo'sh(x)`| `lst.append(x)` |

### Identifiers

* Standard ASCII letters, digits and underscores are supported.
* The Uzbek modifier letter `ʻ` (U+02BB) is allowed inside identifiers
  — that is what makes `Yolgʻon` and `Boʻsh` first-class.
* The ASCII apostrophe `'` is allowed inside identifiers when followed
  by another letter (`qo'sh`, `o'zi`, `xavfsiz_bo'lish`). Internally it
  is normalised to U+02BC `ʼ`, which is a valid Python identifier
  character — so all CPython tooling works unchanged.

### What you get for free

Because every `.uz` program compiles to a vanilla Python AST, the
following are all available without any work in Uzbek-PY:

* full Python expression grammar (comprehensions, lambdas, walrus,
  unpacking, decorators, type hints, f-strings, `match`),
* the entire standard library (`yuklab_ol json` works),
* third-party packages (`yuklab_ol numpy sifatida np`),
* CPython's optimiser, peephole pass and `-O` flag,
* `pdb`, `pyinstrument`, `coverage.py`, `traceback` — every tool that
  reads `.pyc`-style code objects.

---

## Beautiful Uzbek diagnostics

Every error — compile-time or run-time — is rendered with line, column,
caret, and a localised description. Examples:

#### NameError

```text
$ python engine.py -c 'yoz(notanish_oʻzgaruvchi)'
┏━━ Uzbek-PY xatolik hisoboti ━━━━━━━━━━━━━━━━━━━━━━
┃ NomXatoligi: 'notanish_oʻzgaruvchi' nomi aniqlanmagan
┃ Joylashuv: '<command>', 1-qator
┃
┃   yoz(notanish_oʻzgaruvchi)
┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

#### ZeroDivisionError

```text
$ python engine.py -c 'yoz(10 / 0)'
┏━━ Uzbek-PY xatolik hisoboti ━━━━━━━━━━━━━━━━━━━━━━
┃ NolgaBoʻlishXatoligi: nolga boʻlish mumkin emas
┃ Joylashuv: '<command>', 1-qator
┃
┃   yoz(10 / 0)
┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

#### SyntaxError (with caret)

```text
$ python engine.py -c $'agar Rost\n    yoz("hi")'
┏━━ Uzbek-PY xatolik hisoboti ━━━━━━━━━━━━━━━━━━━━━━
┃ SintaksisXatoligi: expected ':'
┃ Joylashuv: '<command>', 1-qator, 10-ustun
┃
┃   agar Rost
┃            ^
┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

The error subsystem maintains a `SourceMap` that lets it
**reverse-translate** any Python identifier back to the Uzbek keyword
the user actually wrote. So an error involving `def` is reported as
involving `funksiya`, and so on.

---

## CLI

```text
python engine.py [-h] [-c SOURCE] [--emit-python] [--tokens] [--ast]
                 [-O OPTIMIZE] [--no-translate-errors] [-v]
                 [file]
```

| Flag                     | Meaning                                       |
|--------------------------|-----------------------------------------------|
| `file`                   | Path to a `.uz` source file.                  |
| `-c SOURCE`              | Run a one-liner instead of a file.            |
| *(no args)*              | Drop into an interactive Uzbek-PY REPL.       |
| `--emit-python`          | Print the translated Python source.           |
| `--tokens`               | Print the lexer's token stream.               |
| `--ast`                  | Print `ast.dump` of the compiled AST.         |
| `-O / --optimize {-1,0,1,2}` | Forwarded to `compile()`.                 |
| `--no-translate-errors`  | Show raw Python tracebacks (debugging aid).   |
| `-v / --version`         | Print the engine version.                     |

### Programmatic API

```python
from uzbek_lang import run_source, run_file, compile_uz, UzbekRuntime

run_source('yoz("Salom!")')               # one-shot
run_file("examples/golden.uz")            # one-shot

unit = compile_uz(open("examples/golden.uz").read())
print(unit.python_source)                 # the translated Python
print(unit.code.co_consts)                # native CodeType

rt = UzbekRuntime()
rt.run_file("examples/classes.uz")        # full lifecycle
```

---

## Performance

A tight numerical loop (`2_000_000` iterations) in both languages:

```
Uzbek-PY:    96.9 ms   javob = -1000000
CPython :    96.4 ms   javob = -1000000
Ratio   :  1.005x       (1.000 == identical bytecode speed)
```

The 0.5% delta is measurement noise. There is no interpretation loop;
both runs execute the same bytecode through the same VM. The cost of
"being Uzbek" is paid **once** at compile time (a few milliseconds for
lex + translate + parse + compile) and **never** at run time.

---

## Project layout

```
.
├── engine.py                  # CLI entry point (script + REPL)
├── uzbek_lang/
│   ├── __init__.py            # Public surface re-exports
│   ├── lexer.py               # Hand-written Uzbek-aware lexer
│   ├── translator.py          # Token-level translation + SourceMap
│   ├── compiler.py            # Pipeline: Lex → Translate → ast → bytecode
│   ├── runtime.py             # Globals injection, exec, error wrapping
│   └── errors.py              # Uzbek diagnostic subsystem
└── examples/
    ├── golden.uz              # The spec's canonical test program
    ├── fibonacci.uz           # Recursion + dict memoisation
    ├── classes.uz             # Sinflar / inheritance / `o'zi`
    └── xatolik.uz             # try / except / exception handling
```

Lines of source: under 1500. Zero third-party dependencies.

---

## License

MIT — see [`LICENSE`](./LICENSE). Built with ❤ on the shoulders of
CPython.
