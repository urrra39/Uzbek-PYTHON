/**
 * Uzbek-PY VS Code extension — entry point.
 * =========================================
 *
 * Provides:
 *   1. A "run" command (`uzbek-py.runFile`) that executes the active
 *      `.uz` file via `python engine.py <file>` inside a dedicated
 *      integrated terminal.
 *   2. A diagnostic command (`uzbek-py.showCompiledPython`) that opens
 *      the engine's `--emit-python` output in a fresh editor pane so
 *      authors can see exactly what their Uzbek code translates to.
 *
 * Design goals
 * ------------
 *   * No build step. Pure JavaScript; ships exactly as written.
 *   * Single, reused terminal panel ("Uzbek-PY Runner") — no new
 *     terminal on every Run-button click.
 *   * Robust `engine.py` auto-discovery: walk up from the file, then
 *     scan every workspace folder. Configurable override via settings.
 *   * Cross-platform: Windows / macOS / Linux. Path quoting handled
 *     defensively with `JSON.stringify` (gives us a valid double-quoted
 *     literal in every major shell).
 *   * No third-party runtime dependencies — only `vscode` and Node's
 *     standard library (`fs`, `path`, `child_process`).
 *
 * @module extension
 */

"use strict";

const vscode = require("vscode");
const path   = require("path");
const fs     = require("fs");
const cp     = require("child_process");


/* -------------------------------------------------------------------------- *
 *  Module state                                                              *
 * -------------------------------------------------------------------------- */

/**
 * The persistent runner terminal. Re-used across runs so the user
 * doesn't accumulate panels. Recreated on demand if the user closes it.
 * @type {vscode.Terminal | null}
 */
let runnerTerminal = null;

/** Channel used for non-fatal warnings and discovery diagnostics. */
let outputChannel = /** @type {vscode.OutputChannel | null} */ (null);


/* -------------------------------------------------------------------------- *
 *  Activation                                                                *
 * -------------------------------------------------------------------------- */

/**
 * Called by VS Code the first time something the extension contributes
 * to is needed (a `.uz` file is opened, or one of our commands is
 * invoked from the palette).
 *
 * @param {vscode.ExtensionContext} context
 */
function activate(context) {
    outputChannel = vscode.window.createOutputChannel("Uzbek-PY");
    context.subscriptions.push(outputChannel);

    // ---- Commands ---------------------------------------------------------
    context.subscriptions.push(
        vscode.commands.registerCommand("uzbek-py.runFile",
            (uri) => runUzbekFile(uri).catch(reportError)),

        vscode.commands.registerCommand("uzbek-py.showCompiledPython",
            (uri) => showCompiledPython(uri).catch(reportError)),
    );

    // ---- Terminal lifecycle ----------------------------------------------
    // If the user closes our terminal manually we forget it, so the
    // next run creates a fresh one instead of using the dead handle.
    context.subscriptions.push(
        vscode.window.onDidCloseTerminal((t) => {
            if (t === runnerTerminal) {
                runnerTerminal = null;
            }
        }),
    );
}


/** Called when VS Code unloads the extension. */
function deactivate() {
    if (runnerTerminal) {
        runnerTerminal.dispose();
        runnerTerminal = null;
    }
}


/* -------------------------------------------------------------------------- *
 *  Command: Run File                                                         *
 * -------------------------------------------------------------------------- */

/**
 * Top-level handler for `uzbek-py.runFile`. Resolves the target file
 * (CLI argument from the menu, or the active editor), saves it if so
 * configured, locates `engine.py`, then dispatches to a dedicated
 * integrated terminal.
 *
 * @param {vscode.Uri | undefined} uri
 *        Provided by VS Code when the command is invoked from a
 *        context menu. Undefined for keybindings / command palette.
 */
async function runUzbekFile(uri) {
    const target = await resolveTargetFile(uri);
    if (!target) {
        return; // resolveTargetFile already showed an error
    }

    const config = vscode.workspace.getConfiguration("uzbekPy");

    if (config.get("saveBeforeRun", true)) {
        await saveIfDirty(target.documentUri);
    }

    const engine = await locateEngine(target.documentUri);
    if (!engine) {
        return; // locateEngine already informed the user
    }

    const pythonExe = String(config.get("pythonPath", "python")).trim() || "python";
    const extraArgs = /** @type {string[]} */ (config.get("runArgs", []) || []);
    const clearFirst = config.get("clearTerminalBeforeRun", false);

    const terminal = ensureTerminal(config, engine.cwd);

    if (clearFirst) {
        // VS Code lets us send a clear via the standard escape sequence
        // instead of issuing a shell-specific command (cls/clear).
        terminal.sendText("\u001Bc", false);
    }

    const command = buildRunCommand(pythonExe, engine, target.absolutePath, extraArgs);
    terminal.sendText(command, true);
    terminal.show(/* preserveFocus */ true);
}


/**
 * Build the exact text we will send to the terminal. We always pass
 * absolute paths so the command is independent of the terminal's cwd
 * (which may have been changed by the user).
 *
 * @param {string}   pythonExe
 * @param {{absolutePath: string}} engine
 * @param {string}   targetPath
 * @param {string[]} extraArgs
 * @returns {string}
 */
function buildRunCommand(pythonExe, engine, targetPath, extraArgs) {
    const parts = [
        shellQuote(pythonExe),
        shellQuote(engine.absolutePath),
        shellQuote(targetPath),
    ];
    for (const arg of extraArgs) {
        parts.push(shellQuote(String(arg)));
    }
    return parts.join(" ");
}


/* -------------------------------------------------------------------------- *
 *  Command: Show Compiled Python                                             *
 * -------------------------------------------------------------------------- */

/**
 * Run `python engine.py --emit-python <file>` and open the result in a
 * fresh, untitled, read-only Python editor pane. Handy for understanding
 * exactly what Uzbek-PY produces under the hood — and for debugging.
 *
 * @param {vscode.Uri | undefined} uri
 */
async function showCompiledPython(uri) {
    const target = await resolveTargetFile(uri);
    if (!target) {
        return;
    }

    const config = vscode.workspace.getConfiguration("uzbekPy");
    if (config.get("saveBeforeRun", true)) {
        await saveIfDirty(target.documentUri);
    }

    const engine = await locateEngine(target.documentUri);
    if (!engine) {
        return;
    }

    const pythonExe = String(config.get("pythonPath", "python")).trim() || "python";

    let stdout;
    try {
        stdout = await new Promise((resolve, reject) => {
            cp.execFile(
                pythonExe,
                [engine.absolutePath, "--emit-python", target.absolutePath],
                { encoding: "utf8", maxBuffer: 16 * 1024 * 1024 },
                (err, out, stderr) => {
                    if (err) {
                        const msg = stderr && stderr.trim()
                            ? stderr.trim()
                            : err.message;
                        reject(new Error(msg));
                        return;
                    }
                    resolve(out);
                },
            );
        });
    } catch (err) {
        vscode.window.showErrorMessage(
            `Uzbek-PY: kompilyatsiya muvaffaqiyatsiz tugadi — ${err.message}`,
        );
        return;
    }

    const doc = await vscode.workspace.openTextDocument({
        content:  stdout,
        language: "python",
    });
    await vscode.window.showTextDocument(doc, {
        preview:    false,
        viewColumn: vscode.ViewColumn.Beside,
    });
}


/* -------------------------------------------------------------------------- *
 *  File / engine resolution                                                  *
 * -------------------------------------------------------------------------- */

/**
 * @typedef ResolvedFile
 * @property {string}      absolutePath  filesystem path
 * @property {vscode.Uri}  documentUri   matching VS Code URI
 * @property {vscode.TextDocument | null} document  if currently open
 */

/**
 * Figure out which file the user wants to run. Prefers:
 *   1. The Uri argument passed by VS Code (right-click on Explorer or tab)
 *   2. The active editor, when it points at a `.uz` file
 *
 * Validates that the file actually has the `.uz` extension and shows a
 * friendly error otherwise. Returns `null` on failure.
 *
 * @param {vscode.Uri | undefined} uri
 * @returns {Promise<ResolvedFile | null>}
 */
async function resolveTargetFile(uri) {
    let documentUri = uri;
    let document = null;

    if (!documentUri) {
        const editor = vscode.window.activeTextEditor;
        if (!editor) {
            vscode.window.showErrorMessage(
                "Uzbek-PY: hech qaysi muharrir faol emas. Avval .uz faylini oching.",
            );
            return null;
        }
        documentUri = editor.document.uri;
        document    = editor.document;
    }

    if (documentUri.scheme !== "file") {
        vscode.window.showErrorMessage(
            "Uzbek-PY: faqat lokal fayllarni ishga tushirish mumkin (file:// scheme).",
        );
        return null;
    }

    const absolutePath = documentUri.fsPath;
    if (path.extname(absolutePath).toLowerCase() !== ".uz") {
        vscode.window.showErrorMessage(
            "Uzbek-PY: bu fayl .uz emas. Komandani faqat .uz fayllarda ishlatish mumkin.",
        );
        return null;
    }

    if (!document) {
        document = vscode.workspace.textDocuments.find(
            (d) => d.uri.toString() === documentUri.toString(),
        ) || null;
    }

    return { absolutePath, documentUri, document };
}


/**
 * Save the named document if it has unsaved changes. No-op if the
 * document is not currently open in any editor (e.g. ran from the
 * Explorer context menu on an unopened file).
 *
 * @param {vscode.Uri} uri
 */
async function saveIfDirty(uri) {
    const doc = vscode.workspace.textDocuments.find(
        (d) => d.uri.toString() === uri.toString(),
    );
    if (doc && doc.isDirty) {
        await doc.save();
    }
}


/**
 * @typedef LocatedEngine
 * @property {string} absolutePath  full path to engine.py
 * @property {string} cwd           directory containing engine.py
 */

/**
 * Locate a runnable `engine.py`, in priority order:
 *   1. The `uzbekPy.enginePath` setting (absolute or workspace-relative).
 *   2. Walk up from the directory of the .uz file.
 *   3. Walk up from each workspace folder.
 *
 * Reports a friendly Uzbek error and returns `null` on failure.
 *
 * @param {vscode.Uri} fileUri  The .uz file we are trying to run.
 * @returns {Promise<LocatedEngine | null>}
 */
async function locateEngine(fileUri) {
    const config    = vscode.workspace.getConfiguration("uzbekPy");
    const setting   = String(config.get("enginePath", "") || "").trim();
    const fileDir   = path.dirname(fileUri.fsPath);
    const folders   = vscode.workspace.workspaceFolders || [];

    // 1. Explicit setting.
    if (setting) {
        const resolved = resolveSettingPath(setting, folders);
        if (resolved && (await fileExists(resolved))) {
            return toLocatedEngine(resolved);
        }
        vscode.window.showErrorMessage(
            `Uzbek-PY: 'uzbekPy.enginePath' sozlamasi noto'g'ri — fayl topilmadi: ${setting}`,
        );
        return null;
    }

    // 2. Walk up from the .uz file.
    const upFromFile = await walkUpForEngine(fileDir);
    if (upFromFile) {
        return toLocatedEngine(upFromFile);
    }

    // 3. Each workspace folder root, then walking up from each (in case
    //    the user opened a sub-folder of the engine repo).
    for (const folder of folders) {
        const inRoot = path.join(folder.uri.fsPath, "engine.py");
        if (await fileExists(inRoot)) {
            return toLocatedEngine(inRoot);
        }
        const upFromFolder = await walkUpForEngine(folder.uri.fsPath);
        if (upFromFolder) {
            return toLocatedEngine(upFromFolder);
        }
    }

    const message =
        "Uzbek-PY: 'engine.py' topilmadi. " +
        "Iltimos uni .uz faylingizga yaqin papkalardan birida joylashtiring " +
        "yoki 'uzbekPy.enginePath' sozlamasi orqali to'liq yo'lni ko'rsating.";
    vscode.window.showErrorMessage(message, "Sozlamalarni Ochish")
        .then((choice) => {
            if (choice === "Sozlamalarni Ochish") {
                vscode.commands.executeCommand(
                    "workbench.action.openSettings", "uzbekPy.enginePath",
                );
            }
        });
    return null;
}


/**
 * Walk up the directory tree starting at `start`, returning the first
 * `engine.py` we find, or null if we hit the filesystem root first.
 *
 * Capped at 32 levels so a malformed path can never spin forever.
 *
 * @param {string} start
 * @returns {Promise<string | null>}
 */
async function walkUpForEngine(start) {
    let dir = path.resolve(start);
    let lastDir = "";
    for (let depth = 0; depth < 32 && dir !== lastDir; depth++) {
        const candidate = path.join(dir, "engine.py");
        if (await fileExists(candidate)) {
            return candidate;
        }
        lastDir = dir;
        dir     = path.dirname(dir);
    }
    return null;
}


/**
 * Resolve a user-supplied path: leave absolute paths alone, otherwise
 * try each workspace folder in turn.
 *
 * @param {string} raw
 * @param {readonly vscode.WorkspaceFolder[]} folders
 * @returns {string | null}
 */
function resolveSettingPath(raw, folders) {
    if (path.isAbsolute(raw)) {
        return raw;
    }
    for (const folder of folders) {
        const cand = path.resolve(folder.uri.fsPath, raw);
        if (fs.existsSync(cand)) {
            return cand;
        }
    }
    // Fall back to a CWD-relative path even if it doesn't exist yet,
    // so the caller can produce an accurate "not found" message.
    return path.resolve(raw);
}


/** @param {string} absolutePath */
function toLocatedEngine(absolutePath) {
    return {
        absolutePath,
        cwd: path.dirname(absolutePath),
    };
}


/** @param {string} p */
function fileExists(p) {
    return new Promise((resolve) => {
        fs.stat(p, (err, st) => resolve(!err && st.isFile()));
    });
}


/* -------------------------------------------------------------------------- *
 *  Terminal management                                                       *
 * -------------------------------------------------------------------------- */

/**
 * Return the (cached) runner terminal, creating a fresh one if needed.
 * The terminal's working directory is set to the engine's parent
 * folder so users can run quick `python engine.py ...` follow-ups by
 * hand without typing absolute paths.
 *
 * @param {vscode.WorkspaceConfiguration} config
 * @param {string} cwd
 * @returns {vscode.Terminal}
 */
function ensureTerminal(config, cwd) {
    const desiredName = String(config.get("terminalName", "Uzbek-PY Runner")) ||
        "Uzbek-PY Runner";

    // If a previous terminal exists but its name has been changed via
    // settings, dispose it first so the user always sees the current name.
    if (runnerTerminal && runnerTerminal.name !== desiredName) {
        runnerTerminal.dispose();
        runnerTerminal = null;
    }

    if (runnerTerminal) {
        return runnerTerminal;
    }

    runnerTerminal = vscode.window.createTerminal({
        name: desiredName,
        cwd:  cwd,
    });
    return runnerTerminal;
}


/* -------------------------------------------------------------------------- *
 *  Utilities                                                                 *
 * -------------------------------------------------------------------------- */

/**
 * Quote a string so it survives as a single argument in any of the
 * shells VS Code commonly invokes (bash, zsh, fish, cmd.exe, PowerShell).
 *
 * Strategy:
 *   * If the string is "safe" (only letters, digits and a small set of
 *     punctuation), return it unchanged — keeps the displayed command
 *     readable.
 *   * Otherwise wrap in double quotes and escape the four characters
 *     that have universal meaning inside double-quoted shell strings:
 *     `"`, `\\`, `$` and `` ` ``. PowerShell additionally treats `$`
 *     specially; cmd.exe ignores `$` and `` ` ``. Escaping the
 *     superset is safe everywhere.
 *
 * @param {string} s
 * @returns {string}
 */
function shellQuote(s) {
    if (s.length > 0 && /^[A-Za-z0-9_./:\\-]+$/.test(s)) {
        return s;
    }
    const escaped = s
        .replace(/\\/g, "\\\\")
        .replace(/"/g,  "\\\"")
        .replace(/\$/g, "\\$")
        .replace(/`/g,  "\\`");
    return `"${escaped}"`;
}


/**
 * Last-line defence: an unexpected exception bubbled out of a command
 * handler. Show a non-blocking message and log the full stack trace to
 * the output channel for the bug report.
 *
 * @param {unknown} err
 */
function reportError(err) {
    const message = err instanceof Error ? err.message : String(err);
    vscode.window.showErrorMessage(`Uzbek-PY: kutilmagan xatolik — ${message}`);
    if (outputChannel) {
        outputChannel.appendLine("--- Uzbek-PY internal error ---");
        outputChannel.appendLine(err && err.stack ? String(err.stack) : message);
    }
}


/* -------------------------------------------------------------------------- *
 *  Exports                                                                   *
 * -------------------------------------------------------------------------- */

module.exports = {
    activate,
    deactivate,
    // Exposed for unit tests / introspection — not part of the public API.
    _internals: {
        buildRunCommand,
        shellQuote,
        walkUpForEngine,
    },
};
