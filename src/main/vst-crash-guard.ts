// VST crash guard — a dead-man's-pedal that learns which VST3s crash the app.
//
// Some plugins fault when loaded, when their editor is opened, or while their
// editor is being interacted with — a common cause is an editor that needs the
// process's true main thread (which Electron owns, so the audio addon can't
// provide it). We can't predict which plugins are affected. So: before each
// risky in-process VST op, drop a sentinel file naming the plugin; clear it
// once the op has demonstrably survived. For editors — which can fault long
// after open() returns (during user interaction) — the sentinel stays armed
// for the editor's *lifetime* and is cleared on close, processor removal,
// chain clear, or clean shutdown. If a sentinel is still on disk at the next
// startup, that plugin took the app down — record it in a persistent
// blocklist. The blocklist is handed to the addon, which then routes those
// plugins through the out-of-process sandbox (slopsmith-vst-host.exe).

import { app } from 'electron';
import * as fs from 'fs';
import * as path from 'path';

let sentinelPath = '';
let blocklistPath = '';
const blocklist = new Set<string>();

// Path currently armed in the sentinel, if any. Tracked so disarmSentinelForPath
// can leave another plugin's still-open-editor sentinel alone when an
// unrelated slot is closed/removed.
let armedPath: string | null = null;

// Normalise a plugin path for use as a blocklist / armed-path key. Windows
// paths are case-insensitive, so fold case there to match the addon's
// case-insensitive lookup. POSIX paths are case-sensitive — lowercasing them
// would corrupt the path, so preserve case (and the lowercasing wouldn't help
// anyway, as the sandbox is Windows-only today).
const norm = (p: string): string => {
    const trimmed = p.trim();
    return process.platform === 'win32' ? trimmed.toLowerCase() : trimmed;
};

// Run once at startup, before any VST can be loaded. Promotes a leftover
// sentinel (= the app crashed mid-op last run) into the persistent blocklist,
// then returns the full blocklist for handing to the addon.
export function initVstCrashGuard(): string[] {
    const dir = app.getPath('userData');
    sentinelPath = path.join(dir, 'vst-load-sentinel.json');
    blocklistPath = path.join(dir, 'vst-crash-blocklist.json');

    try {
        const raw = JSON.parse(fs.readFileSync(blocklistPath, 'utf8'));
        if (Array.isArray(raw))
            for (const p of raw) if (typeof p === 'string' && p) blocklist.add(norm(p));
    } catch { /* no blocklist yet — fine */ }

    try {
        const s = JSON.parse(fs.readFileSync(sentinelPath, 'utf8'));
        if (s && typeof s.plugin === 'string' && s.plugin) {
            blocklist.add(norm(s.plugin));
            console.warn(`[vst-crash-guard] ${s.plugin} crashed the app during `
                + `'${s.op}' last run — it will load sandboxed from now on`);
            persist();
        }
    } catch { /* no sentinel — the app exited cleanly last run */ }
    clearSentinel();

    // A clean shutdown is, by definition, not a crash. Disarm any sentinel
    // still armed from a still-open editor at quit time so the plugin isn't
    // falsely blocklisted on next launch — only an abrupt termination should
    // leave a sentinel behind.
    app.once('will-quit', () => disarmSentinel());

    return [...blocklist];
}

function persist(): void {
    try {
        fs.writeFileSync(blocklistPath, JSON.stringify([...blocklist], null, 2));
    } catch (e: any) {
        console.warn(`[vst-crash-guard] could not persist blocklist: ${e?.message}`);
    }
}

function clearSentinel(): void {
    armedPath = null;
    try {
        if (sentinelPath) fs.rmSync(sentinelPath, { force: true });
    } catch { /* best-effort */ }
}

// Drop the sentinel just before a risky in-process VST op. A plain
// writeFileSync is enough: an access violation kills the process but not the
// OS page cache, so the file survives for the next startup to find.
export function armSentinel(pluginPath: string, op: 'load' | 'editor'): void {
    if (!pluginPath || !sentinelPath) return;
    try {
        fs.writeFileSync(sentinelPath,
            JSON.stringify({ plugin: pluginPath, op, at: Date.now() }));
        armedPath = norm(pluginPath);
    } catch {
        /* best-effort — a missed sentinel just means no auto-blocklist */
    }
}

// Clear the sentinel unconditionally. Used at clean shutdown and after a
// chain-wide clear when no plugin's identity needs preserving.
export function disarmSentinel(): void {
    clearSentinel();
}

// Clear the sentinel only when it was armed for the given plugin path. Used
// by editor-close and processor-removal handlers so a different plugin's
// still-open-editor sentinel doesn't get wiped by an unrelated slot's close.
export function disarmSentinelForPath(pluginPath: string): void {
    if (!pluginPath || armedPath === null) return;
    if (armedPath === norm(pluginPath)) clearSentinel();
}

// Arm the sentinel for an editor that's about to open. Unlike a synchronous
// load, the editor can fault long after open() returns (during user
// interaction), so the sentinel stays armed until the editor is explicitly
// closed (disarmSentinelForPath from closePluginEditor / removeProcessor) or
// the app exits cleanly (disarmSentinel from clearChain / will-quit).
export function armEditorSentinel(pluginPath: string): void {
    if (!pluginPath || !sentinelPath) return;
    armSentinel(pluginPath, 'editor');
}
