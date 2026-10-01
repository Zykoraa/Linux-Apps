/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { BrowserWindow } from "electron";
import { join } from "path";
import { Ipc } from "@shared/ipc";

import { loadView } from "./protocol";
import { Settings } from "./settings";

let splash: BrowserWindow | undefined;

/** A colour string safe to drop into a CSS declaration (no ; or } to break out of it). */
function cssColor(value: string | undefined) {
    return value && /^[#\w\s(),.%-]+$/.test(value) ? value : undefined;
}

export function createSplash(show: boolean) {
    splash = new BrowserWindow({
        width: 300,
        height: 350,
        // Fixed size makes Hyprland float it instead of tiling a loading screen.
        resizable: false,
        maximizable: false,
        frame: false,
        transparent: true,
        center: true,
        alwaysOnTop: true,
        skipTaskbar: true,
        show,
        title: "Evecord",
        webPreferences: { preload: join(__dirname, "splashPreload.js") }
    });

    const { splashTheming, splashColor, splashBackground } = Settings.store;
    const fg = splashTheming ? cssColor(splashColor) : undefined;
    const bg = splashTheming ? cssColor(splashBackground) : undefined;
    const css = [fg && `--fg: ${fg} !important;`, bg && `--bg: ${bg} !important;`].filter(Boolean).join(" ");
    if (css) splash.webContents.on("dom-ready", () => splash?.webContents.insertCSS(`:root { ${css} }`));

    loadView(splash, "splash.html");
    return splash;
}

export function splashMessage(message: string) {
    if (splash && !splash.isDestroyed()) splash.webContents.send(Ipc.SPLASH_MESSAGE, message);
}

export function closeSplash() {
    if (splash && !splash.isDestroyed()) splash.destroy();
    splash = undefined;
}
