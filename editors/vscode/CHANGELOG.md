# Changelog

All notable changes to the **Uzbek-PY** VS Code extension are recorded
in this file. The format follows [Keep a Changelog][kac]; this project
adheres to [Semantic Versioning][semver].

[kac]:    https://keepachangelog.com/en/1.1.0/
[semver]: https://semver.org/spec/v2.0.0.html


## [1.0.0] — 2026-05-25

### Added
- Full TextMate grammar (`source.uzbek-py`) with proper Uzbek-aware
  identifier rules: `qo'sh`, `o'zi`, `Yolgʻon`, `Boʻsh` all colourise
  correctly.
- Language configuration: `#` line comments, bracket matching,
  Python-style off-side folding with `# region` / `# endregion`
  markers, and `onEnterRules` that drive correct indentation after a
  colon line.
- Command **Uzbek-PYTHON: Kodni Ishga Tushirish** (`uzbek-py.runFile`)
  — runs the active `.uz` file in a dedicated, reused integrated
  terminal via `python engine.py <file>`. Exposed via a play button
  in the editor title bar, the editor context menu, the explorer
  context menu, the command palette, and `Ctrl/Cmd+F5`.
- Command **Tarjima Qilingan Python Kodini Koʻrsatish**
  (`uzbek-py.showCompiledPython`) — opens the engine's
  `--emit-python` output in a side editor.
- Robust auto-discovery of `engine.py`: walks up from the current
  file, then through every workspace folder. Override via
  `uzbekPy.enginePath`.
- Settings: `uzbekPy.pythonPath`, `uzbekPy.enginePath`,
  `uzbekPy.saveBeforeRun`, `uzbekPy.terminalName`,
  `uzbekPy.clearTerminalBeforeRun`, `uzbekPy.runArgs`.
- Starter snippets for `funksiya`, `sinf`, `agar`, `uchun`, `toki`,
  `harakat_qil`, `oraliq`, `yoz`, `asosiy`.
