# Uzbek-PY — Native Runtime (C++17)

> A complete, standalone implementation of the Uzbek programming language,
> written from scratch in pure C++17. **Python is not required to run it.**

The native runtime is a single-file, ~2,800-line C++17 program that
ships as one statically-linkable executable. It is hand-written from
the lexer up — no parser generators, no virtual machines borrowed from
elsewhere, no language runtimes piggy-backed.

```
$ make
$ ./uzbek examples/golden.uz
Yakuniy ro'yxat: [1, 2, 3, 4, 25, 60, 7, 8, 9, 50, 11, 120, 13, 14]
Ro'yxat uzunligi: 14
```

---

## Architecture

```
.uz source
   |
   v
+--------+
| Lexer  |   UTF-8 aware. Hand-written. Emits Python-style INDENT/DEDENT
+--------+   tokens. Tracks line + codepoint column + byte offset.
   |  Token stream
   v
+--------+
| Parser |   Recursive descent with a precedence cascade for expressions.
+--------+   Produces a typed AST owned by std::unique_ptr.
   |  AST
   v
+----------+
|Interpreter|  Tree-walking evaluator. Enum-tag dispatch (no virtual
+----------+  calls in the hot path). Lexical scope chain via
   |          shared_ptr<Env>. Closures, classes, inheritance, native
   v          built-ins.
 stdout / stderr / Uzbek diagnostics
```

All in a single 2,800-line file (`uzbek.cpp`), compiled by either
**g++ 11+** or **clang++ 15+** with no third-party dependencies, and
zero warnings under `-Wall -Wextra -Wpedantic`.

---

## Build & run

```bash
cd runtime/cpp
make                      # build with optimisation -> ./uzbek
./uzbek examples/golden.uz

# Other modes:
./uzbek -c 'yoz("Salom!")'           # one-liner
./uzbek                              # interactive REPL
./uzbek --tokens examples/golden.uz  # dump the token stream
./uzbek --ast    examples/golden.uz  # dump the AST
./uzbek --version
./uzbek --help

make test                 # run every example under examples/
make debug                # build with -O0 -g for gdb / valgrind
make install PREFIX=/usr/local
```

The output is a single ~210 KB binary on Linux/x86_64.

---

## Language summary

A near-complete, Python-flavoured language with **100% Uzbek surface
syntax**. Implemented in v1:

| Feature              | Uzbek keyword(s)                                 |
|----------------------|--------------------------------------------------|
| Function definition  | `funksiya nom(...): ... qaytarsin ...`           |
| Class definition     | `sinf Nom(Ota): ...`  (single inheritance)       |
| Conditionals         | `agar / yoki_agar / aks_holda`                   |
| Loops                | `toki <shart>:` and `uchun x ichida ...:`        |
| Logical operators    | `va`, `yoki`, `emas`                             |
| Constants            | `Rost`, `Yolgʻon`, `Boʻsh`                       |
| Loop control         | `tanaffus`, `davom`, `oʻt`                       |
| Built-ins            | `yoz`, `kirit`, `uzunlik`, `oraliq`, `type`, `satr`, `butun`, `suzuvchi` |
| List method          | `lst.qo'sh(x)` (append)                          |

All standard data shapes are first-class:

* **Numbers** — `int64_t` (with hex/oct/bin prefixes and `_` separators) and
  IEEE-754 `double`, with auto-promotion.
* **Strings** — UTF-8, with `\n \t \r \\ \" \' \xHH \uHHHH` escapes,
  triple-quoted variants, and codepoint-aware indexing & length.
* **Lists** — `[1, 2, 3]`, `+`, `*`, indexing, `qo'sh`, `ichida`.
* **Dicts** — `{"a": 1, "b": 2}`, indexed lookup, `ichida` membership.
* **Functions** — first-class, closures over their lexical environment.
* **Classes** — instances, methods (auto-bound to `o'zi`), `__init__`,
  single inheritance with method-resolution chain.

### Identifiers

The lexer is Uzbek-aware. Names may contain:

* ASCII letters / digits / `_`,
* `ʻ` (U+02BB MODIFIER LETTER TURNED COMMA) — so `Yolgʻon`, `Boʻsh`,
* an ASCII apostrophe `'` followed by another letter — so `qo'sh`,
  `o'zi`, `xavfsiz_bo'lish` lex as a single token.

---

## Diagnostics

Every error — at the lexer, the parser, or the interpreter — is rendered
with **line, column (codepoint-precise), source excerpt, caret pointer,
and a localised Uzbek description**:

```
$ ./uzbek -c 'yoz("salom" + 5)'
┏━━ Uzbek-PY xatolik hisoboti ━━━━━━━━━━━━━━━━━━━━
┃ TurXatoligi: amal '+' 'satr' va 'butun' turlariga mos kelmaydi
┃ Joylashuv: '<uz>', 1-qator, 5-ustun
┃
┃   yoz("salom" + 5)
┃       ^
┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

Error categories (rendered names) include:

* `LeksikXatoligi`     — bad bytes, unterminated strings
* `SintaksisXatoligi`  — parser failures, mismatched indentation
* `NomXatoligi`        — undefined name
* `TurXatoligi`        — type mismatch, wrong arity
* `IndeksXatoligi`     — out-of-range list/string index
* `KalitXatoligi`      — missing dict key
* `AtributXatoligi`    — missing instance/class attribute
* `NolgaBoʻlishXatoligi` — division by zero
* `QiymatXatoligi`     — invalid value (e.g. `oraliq` step 0)

---

## Performance

A tight 500,000-iteration arithmetic loop:

```
Uzbek-PY native:  176 ms
CPython 3.11   :  103 ms
Ratio          :  1.7x slower than CPython
```

This is competitive for a **from-scratch tree-walking interpreter**:
no JIT, no inline caching, no bytecode VM. CPython's bytecode dispatch
loop has had decades of micro-optimisation work. A future v2 could add
a stack-based bytecode VM and target the 0.5x–1.0x range.

---

## REPL

```
$ ./uzbek
Uzbek-PY native runtime 1.0.0  -  Pythonsiz, mustaqil.
Chiqish uchun  Ctrl-D  yoki  oxir  yozing.
uz> x = 10
uz> yoz(x * 2)
20
uz> funksiya kvadrat(n):
..>     qaytarsin n * n
..>
uz> kvadrat(7)
49
uz> oxir
```

Bare expressions are echoed (their `repr()`), statements are silent —
the same convention CPython's REPL uses.

---

## Examples

`examples/` ships five self-contained programs:

| File              | Demonstrates                              |
|-------------------|-------------------------------------------|
| `salom.uz`        | The smallest possible program.            |
| `golden.uz`       | The spec's canonical test program.        |
| `fibonacci.uz`    | Recursion + dict memoisation.             |
| `classes.uz`      | `sinf`, `o'zi`, single inheritance.       |
| `closures.uz`     | Functions returning functions.            |

Run them all at once with `make test`.

---

## Source layout

```
runtime/cpp/
├── uzbek.cpp        # the entire language (one file, ~2,800 lines)
├── Makefile         # build / test / install
├── README.md
└── examples/
    ├── salom.uz
    ├── golden.uz
    ├── fibonacci.uz
    ├── classes.uz
    └── closures.uz
```

Inside `uzbek.cpp` the sections are clearly banner-separated:

1. UTF-8 utilities
2. Source positions and errors
3. Token types
4. Lexer (with INDENT/DEDENT)
5. AST node hierarchy
6. Recursive-descent parser
7. Values and heap objects
8. Environment / lexical scope chain
9. Built-in functions and methods
10. Tree-walking interpreter
11. AST debug printer
12. CLI dispatch
13. `main`

---

## License

MIT — see [`../../LICENSE`](../../LICENSE).
