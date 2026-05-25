# Uzbek-PY for VS Code

> **Pythonning oʻzbekcha modern ukasi** — full IDE support for `.uz`
> files: rich syntax highlighting, smart indentation, snippets, and a
> one-click runner.

This is the official VS Code extension for the
[Uzbek-PYTHON](https://github.com/urrra39/Uzbek-PYTHON) language. It
turns VS Code into a dedicated Uzbek-PY editor — no manual terminal
juggling, no fiddling with build tasks.

---

## Features

### Beautiful syntax highlighting

Every Uzbek keyword, built-in, literal and operator is recognised by a
purpose-built TextMate grammar:

* Keywords — `funksiya`, `qaytarsin`, `agar`, `yoki_agar`, `aks_holda`,
  `uchun`, `toki`, `ichida`, `harakat_qil`, `istisno`, `sinf`,
  `yuklab_ol`, …
* Built-ins — `yoz`, `kirit`, `uzunlik`, `oraliq`
* Method calls — `lst.qo'sh(x)` highlights `qo'sh` as a built-in method
* Literals — `Rost`, `Yolgʻon`, `Boʻsh`, numbers, strings, f-strings
* `o'zi` (the Uzbek `self`) is highlighted as a language variable

The grammar fully handles the awkward characters Uzbek code has and
Python doesn't: identifiers like `qo'sh` and `Yolgʻon` lex as a single
token, and an apostrophe inside an identifier never accidentally starts
a string.

### One-click runner

* **Editor title-bar play button** — visible whenever a `.uz` file is
  active.
* **Right-click → "Uzbek-PYTHON: Kodni Ishga Tushirish"** — same effect.
* **`Ctrl+F5` / `⌘F5`** — keyboard shortcut.
* **Command palette** — `Uzbek-PYTHON: Kodni Ishga Tushirish (Run
  Uzbek-PY Script)`.
* **Explorer context menu** on any `.uz` file.

The runner uses a single **dedicated integrated terminal** ("Uzbek-PY
Runner"), reused across runs so your panel doesn't pollute. It executes:

```
python <engine.py> <your-file.uz> [extra args from settings]
```

`engine.py` is auto-discovered: the extension walks up from your file,
then through every workspace folder. If you keep the engine elsewhere,
point at it via the `uzbekPy.enginePath` setting.

### Show Compiled Python

A second command — **Tarjima Qilingan Python Kodini Koʻrsatish** — runs
`engine.py --emit-python` and opens the result in a side editor as a
read-only Python document. Educational and great for debugging.

### Smart editing

* Python-style indentation: pressing **Enter** after a `:` line indents
  automatically. Pressing it after `qaytarsin`, `tanaffus` or `davom`
  outdents.
* Folding by indentation, plus `# region` / `# endregion` markers.
* `qo'sh`, `o'zi`, `Yolgʻon`, `Boʻsh` are recognised as single words by
  the editor — double-click selects the whole identifier.
* Auto-closing pairs for `()`, `[]`, `{}`, `""`, `''`, `'''`, `"""`.

### Snippets

Type a prefix and press <kbd>Tab</kbd>:

| Prefix         | Inserts                              |
|----------------|--------------------------------------|
| `funksiya`     | A function declaration               |
| `sinf`         | A class with `__init__(o'zi, …)`     |
| `agar`         | `agar` / `yoki_agar` / `aks_holda`   |
| `uchun`        | `uchun … ichida …:`                  |
| `toki`         | `toki <shart>:`                      |
| `harakat_qil`  | `harakat_qil` / `istisno`            |
| `oraliq`       | A counted `for`-style loop           |
| `yoz`          | `yoz(...)`                           |
| `asosiy`       | The `if __name__ == "__main__":` guard |

---

## Requirements

* **VS Code 1.74** or newer
* **Python 3.10+** on your `PATH`, or set `uzbekPy.pythonPath`
* The Uzbek-PYTHON engine — anything that exposes a runnable
  `engine.py` works. Most users will simply clone the
  [main repository](https://github.com/urrra39/Uzbek-PYTHON) and open
  it as a VS Code workspace.

The extension has **zero third-party runtime dependencies** beyond
the VS Code API itself.

---

## Settings

All settings live under the `uzbekPy.*` namespace.

| Setting                          | Default            | Meaning                                                                        |
|----------------------------------|--------------------|--------------------------------------------------------------------------------|
| `uzbekPy.pythonPath`             | `python`           | Python interpreter used to launch `engine.py`.                                 |
| `uzbekPy.enginePath`             | *(empty)*          | Override `engine.py` path. Empty = auto-discover.                              |
| `uzbekPy.saveBeforeRun`          | `true`             | Save the active `.uz` file before running.                                     |
| `uzbekPy.terminalName`           | `Uzbek-PY Runner`  | Name of the integrated terminal panel reused for runs.                         |
| `uzbekPy.clearTerminalBeforeRun` | `false`            | Clear the terminal before each run for a fresh slate.                          |
| `uzbekPy.runArgs`                | `[]`               | Extra arguments passed to `engine.py` after the file path.                     |

---

## Example

`hisoblar.uz`:

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

yoz("Yakuniy ro'yxat:", hisobla_va_sarala(15))
```

Hit `Ctrl+F5` (or click the ▶ button in the editor title bar) and the
program runs at native CPython speed inside the integrated terminal:

```text
Yakuniy ro'yxat: [1, 2, 3, 4, 25, 60, 7, 8, 9, 50, 11, 120, 13, 14]
```

---

## Building from source

```bash
git clone https://github.com/urrra39/Uzbek-PYTHON.git
cd Uzbek-PYTHON/editors/vscode
npm install --no-save @vscode/vsce
npx vsce package
# -> uzbek-py-1.0.0.vsix
code --install-extension uzbek-py-1.0.0.vsix
```

To publish to the marketplace, edit `package.json` to set your own
`publisher`, then run `npx vsce publish`.

---

## License

MIT — see [`LICENSE`](https://github.com/urrra39/Uzbek-PYTHON/blob/main/LICENSE).
