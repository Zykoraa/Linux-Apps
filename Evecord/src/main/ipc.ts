/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { app, BrowserWindow, clipboard, ClipboardItem, dialog, type IpcMainInvokeEvent, session, shell } from "electron";
import { readFileSync } from "fs";
import { readFile, stat } from "fs/promises";
import { join } from "path";
import { Ipc } from "@shared/ipc";

import { autostart } from "./autostart";
import { hardwareAcceleration } from "./chromiumFlags";
import { handle, handleSync } from "./ipcHandle";
import { popouts } from "./links";
import { Settings } from "./settings";
import { setTrayUnread } from "./tray";
import { downloadVencord, getVencordInfo, isVencordDir, readVencordFile } from "./vencord";
import { getMainWindow, relaunchApp, showMainWindow } from "./window";

/* Scripts that the preload injects into Discord. Read on every page load, so a
   Ctrl+R picks up a Vencord rebuild without restarting. */
handleSync(Ipc.VENCORD_PRELOAD, () => readVencordFile("vencordDesktopPreload.js"));
handleSync(Ipc.VENCORD_RENDERER, () => readVencordFile("vencordDesktopRenderer.js"));
handleSync(Ipc.EVECORD_RENDERER, () => readFileSync(join(__dirname, "renderer.js"), "utf8"));
handle(Ipc.EVECORD_RENDERER_CSS, () => readFile(join(__dirname, "renderer.css"), "utf8"));

handleSync(Ipc.VERSION, () => app.getVersion());
handleSync(Ipc.HARDWARE_ACCELERATION, () => hardwareAcceleration);
handle(Ipc.RELAUNCH, () => relaunchApp());

handleSync(Ipc.SETTINGS_GET, () => Settings.data);

/** The renderer sends single changes ("streamAudio.ignoreVirtual" = true), never the whole object,
    so a stale copy over there cannot undo a change made here (e.g. choosing a Vencord folder). */
handle(Ipc.SETTINGS_SET, (_e, path: string, value: unknown) => {
    const keys = path.split(".");
    if (keys.some(k => !k || k === "__proto__" || k === "constructor" || k === "prototype")) {
        throw new Error(`Bad settings path ${path}`);
    }
    const last = keys.pop()!;
    let target: any = Settings.store;
    for (const k of keys) {
        if (target[k] === null || typeof target[k] !== "object") target[k] = {};
        target = target[k];
    }
    if (value === undefined) delete target[last];
    else target[last] = value;
});

Settings.onAnyChange(data => {
    const win = getMainWindow();
    if (win && !win.isDestroyed()) win.webContents.send(Ipc.SETTINGS_CHANGED, data);
});

/** The window a call is about: a popout when Discord names one, else the sender's own window. */
function targetWindow(e: IpcMainInvokeEvent, popoutKey?: string) {
    if (popoutKey) return popouts.get(popoutKey);
    return BrowserWindow.fromWebContents(e.sender) ?? getMainWindow();
}

handle(Ipc.WIN_FOCUS, () => showMainWindow());
handle(Ipc.WIN_CLOSE, (e, key?: string) => targetWindow(e, key)?.close());
handle(Ipc.WIN_MINIMIZE, (e, key?: string) => targetWindow(e, key)?.minimize());
handle(Ipc.WIN_MAXIMIZE, (e, key?: string) => {
    const win = targetWindow(e, key);
    if (!win) return;
    if (win.isMaximized()) win.unmaximize();
    else win.maximize();
});
handle(Ipc.WIN_FLASH, (_e, flag: boolean) => {
    const win = getMainWindow();
    if (!win || (flag && win.isFocused())) return;
    win.flashFrame(flag);
});

/** -1 = unread messages but no count, 0 = nothing, n = mentions + requests. */
handle(Ipc.BADGE, (_e, count: number) => {
    setTrayUnread(count !== 0);
    app.setBadgeCount(Math.max(count, 0));
});

handleSync(Ipc.SPELLCHECK_LANGUAGES, () => session.defaultSession.availableSpellCheckerLanguages);
handle(Ipc.SPELLCHECK_REPLACE, (e, word: string) => e.sender.replaceMisspelling(word));
handle(Ipc.SPELLCHECK_LEARN, (e, word: string) => e.sender.session.addWordToSpellCheckerDictionary(word));

function applySpellCheckLanguages(languages: string[] | undefined) {
    if (!languages?.length) return;
    const available = session.defaultSession.availableSpellCheckerLanguages;
    const usable = languages.filter(l => available.includes(l)).slice(0, 5);
    if (usable.length) session.defaultSession.setSpellCheckerLanguages(usable);
}
app.whenReady().then(() => applySpellCheckLanguages(Settings.store.spellCheckLanguages));
Settings.onChange("spellCheckLanguages", applySpellCheckLanguages);

handle(Ipc.CLIPBOARD_IMAGE, (_e, png: Uint8Array, src: string) => {
    const escaped = src.replace(/&/g, "&amp;").replace(/"/g, "&quot;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
    // Both forms, so it pastes as an image into image editors and as a link-backed <img> into rich text.
    clipboard.write([
        new ClipboardItem({
            "image/png": new Blob([png as Uint8Array<ArrayBuffer>], { type: "image/png" }),
            "text/html": `<img src="${escaped}">`
        })
    ]);
});

handleSync(Ipc.AUTOSTART_GET, () => autostart.isEnabled());
handle(Ipc.AUTOSTART_SET, (_e, on: boolean) => (on ? autostart.enable() : autostart.disable()));

handleSync(Ipc.VENCORD_INFO, () => getVencordInfo());

handle(Ipc.VENCORD_CHOOSE_DIR, async () => {
    const win = getMainWindow();
    const options = { title: "Choose a Vencord dist folder", properties: ["openDirectory" as const] };
    const res = win ? await dialog.showOpenDialog(win, options) : await dialog.showOpenDialog(options);
    if (res.canceled || !res.filePaths[0]) return "cancelled";
    if (!isVencordDir(res.filePaths[0])) return "invalid";
    Settings.store.vencordDir = res.filePaths[0];
    return "ok";
});

handle(Ipc.VENCORD_RESET_DIR, () => {
    delete Settings.store.vencordDir;
});

handle(Ipc.VENCORD_DOWNLOAD, async () => {
    await downloadVencord();
    return getVencordInfo();
});

handle(Ipc.OPEN_PATH, async (_e, path: string) => {
    // Only directories Evecord itself told the renderer about.
    const info = getVencordInfo();
    if (path !== info.dir && path !== info.localCheckout) return;
    if ((await stat(path)).isDirectory()) await shell.openPath(path);
});

function debugWindow(url: string) {
    new BrowserWindow({ width: 1100, height: 800, autoHideMenuBar: true }).loadURL(url);
}
handle(Ipc.DEBUG_GPU, () => debugWindow("chrome://gpu"));
handle(Ipc.DEBUG_WEBRTC, () => debugWindow("chrome://webrtc-internals"));
