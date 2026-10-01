/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { app, BrowserWindow, type Input, Menu } from "electron";
import { join } from "path";
import { Ipc } from "@shared/ipc";

import { CommandLine } from "./cli";
import { rejectPendingCommands } from "./commands";
import { wireLinks } from "./links";
import { Settings } from "./settings";
import { closeSplash, splashMessage } from "./splash";
import { initTray } from "./tray";

const MIN_WIDTH = 940;
const MIN_HEIGHT = 500;

/** Discord web, as a desktop Chrome on Linux. Voice and video need a browser it recognises. */
const USER_AGENT = `Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/${
    process.versions.chrome.split(".")[0]
}.0.0.0 Safari/537.36`;

let mainWin: BrowserWindow | undefined;
let quitting = false;

app.on("before-quit", () => (quitting = true));

export function getMainWindow() {
    return mainWin;
}

export function showMainWindow() {
    if (!mainWin || mainWin.isDestroyed()) return;
    if (mainWin.isMinimized()) mainWin.restore();
    mainWin.show();
    mainWin.focus();
}

export function quitApp() {
    quitting = true;
    app.quit();
}

export function relaunchApp() {
    quitting = true;
    app.relaunch();
    app.quit();
}

function discordUrl(deepLink?: string) {
    const { discordBranch } = Settings.store;
    const host = discordBranch === "stable" ? "discord.com" : `${discordBranch}.discord.com`;
    let path = "app";
    if (deepLink) {
        try {
            // discord://-/channels/1/2 -> channels/1/2
            path = new URL(deepLink).pathname.replace(/^\/+/, "") || "app";
        } catch {}
    }
    return `https://${host}/${path}`;
}

/** Shortcuts a browser would give you. There is no menu bar, so they are handled here. */
function handleShortcut(win: BrowserWindow, input: Input) {
    if (input.type !== "keyDown") return false;
    const ctrl = input.control || input.meta;
    const key = input.key.toLowerCase();
    const wc = win.webContents;

    if (input.key === "F12" || (ctrl && input.shift && key === "i")) {
        wc.toggleDevTools();
    } else if (ctrl && input.shift && key === "r") {
        relaunchApp();
    } else if ((ctrl && !input.shift && key === "r") || input.key === "F5") {
        wc.reload();
    } else if (ctrl && !input.shift && key === "q") {
        quitApp();
    } else if (ctrl && (key === "=" || key === "+" || input.code === "NumpadAdd")) {
        wc.setZoomLevel(Math.min(wc.getZoomLevel() + 0.5, 5));
    } else if (ctrl && (key === "-" || input.code === "NumpadSubtract")) {
        wc.setZoomLevel(Math.max(wc.getZoomLevel() - 0.5, -5));
    } else if (ctrl && (key === "0" || input.code === "Numpad0")) {
        wc.setZoomLevel(0);
    } else {
        return false;
    }
    return true;
}

function createMainWindow(deepLink?: string) {
    const { nativeTitleBar, disableMinSize, staticTitle, splashBackground } = Settings.store;

    const win = new BrowserWindow({
        title: "Evecord",
        width: 1280,
        height: 720,
        minWidth: disableMinSize ? undefined : MIN_WIDTH,
        minHeight: disableMinSize ? undefined : MIN_HEIGHT,
        show: false,
        frame: nativeTitleBar,
        backgroundColor: splashBackground ?? "#1a1a1e",
        autoHideMenuBar: true,
        icon: join(__dirname, "..", "static", "icon.png"),
        webPreferences: {
            preload: join(__dirname, "preload.js"),
            sandbox: true,
            contextIsolation: true,
            nodeIntegration: false,
            spellcheck: true,
            // Keep voice, notifications and the unread badge alive while hidden in the tray.
            backgroundThrottling: false
        }
    });
    mainWin = win;
    Menu.setApplicationMenu(null);

    win.on("close", e => {
        if (quitting || !Settings.store.tray || !Settings.store.closeToTray) return;
        e.preventDefault();
        win.hide();
    });
    win.on("closed", () => {
        if (mainWin === win) mainWin = undefined;
    });
    win.on("focus", () => win.flashFrame(false));

    if (staticTitle) win.on("page-title-updated", e => e.preventDefault());
    Settings.onChange("staticTitle", on => {
        win.removeAllListeners("page-title-updated");
        if (on) {
            win.setTitle("Evecord");
            win.on("page-title-updated", e => e.preventDefault());
        }
    });
    Settings.onChange("disableMinSize", off => {
        win.setMinimumSize(off ? 1 : MIN_WIDTH, off ? 1 : MIN_HEIGHT);
    });

    const wc = win.webContents;
    wc.setUserAgent(USER_AGENT);
    wc.setWebRTCIPHandlingPolicy(Settings.store.webRTCIPHandlingPolicy);
    wc.on("before-input-event", (e, input) => {
        if (handleShortcut(win, input)) e.preventDefault();
    });
    wc.on("context-menu", (_e, params) => {
        wc.send(Ipc.SPELLCHECK_RESULT, params.misspelledWord, params.dictionarySuggestions);
    });
    wc.on("devtools-opened", () => wc.send(Ipc.DEVTOOLS_OPENED));
    wc.on("devtools-closed", () => wc.send(Ipc.DEVTOOLS_CLOSED));
    wc.on("did-start-navigation", details => {
        if (details.isMainFrame && !details.isSameDocument) rejectPendingCommands();
    });
    let lastCrashReload = 0;
    wc.on("render-process-gone", (_e, details) => {
        console.error("[Evecord] Renderer gone:", details);
        if (details.reason === "clean-exit" || Date.now() - lastCrashReload < 60_000) return;
        // One automatic reload a minute at most, so a crash loop does not spin.
        lastCrashReload = Date.now();
        setTimeout(() => !win.isDestroyed() && wc.reload(), 1000);
    });

    wireLinks(win);
    loadDiscord(win, deepLink);
    return win;
}

let retries = 0;

function loadDiscord(win: BrowserWindow, deepLink?: string) {
    const url = discordUrl(deepLink);
    win.loadURL(url).then(
        () => {
            retries = 0;
            onDiscordLoaded(win);
        },
        err => {
            // Offline at login is common (Wi-Fi not up yet); keep trying, a little slower each time.
            const delay = Math.min(1000 * 2 ** retries++, 30_000);
            splashMessage(`Could not reach Discord (${err.code ?? err.message}). Retrying in ${delay / 1000}s…`);
            console.warn(`[Evecord] Loading ${url} failed:`, err.code ?? err);
            setTimeout(() => !win.isDestroyed() && loadDiscord(win, deepLink), delay);
        }
    );
}

let shownOnce = false;

function onDiscordLoaded(win: BrowserWindow) {
    splashMessage("");
    closeSplash();
    if (shownOnce) return;
    shownOnce = true;
    // Starting minimized only makes sense with a tray icon to come back from.
    if (!CommandLine.startMinimized || !Settings.store.tray) win.show();
}

/** Open a discord:// link (or the app) in the existing window. */
export function navigateTo(deepLink: string) {
    if (!mainWin || mainWin.isDestroyed()) return;
    mainWin.loadURL(discordUrl(deepLink));
    showMainWindow();
}

export function createWindows(deepLink?: string) {
    const win = createMainWindow(deepLink);
    if (Settings.store.tray) initTray();
    return win;
}

export function setMainWindowVisible(visible: boolean) {
    if (visible) showMainWindow();
    else mainWin?.hide();
}
