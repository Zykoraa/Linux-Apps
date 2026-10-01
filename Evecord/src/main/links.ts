/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { BrowserWindow, type BrowserWindowConstructorOptions, shell } from "electron";

import { DISCORD_HOSTS } from "./ipcHandle";
import { Settings } from "./settings";

const EXTERNAL_PROTOCOLS = new Set(["http:", "https:", "mailto:", "spotify:", "steam:"]);

/** Hand a URL to the desktop (xdg-open). Never opens anything inside Evecord. */
export function openExternal(url: string) {
    let protocol: string;
    try {
        protocol = new URL(url).protocol;
    } catch {
        return;
    }
    if (EXTERNAL_PROTOCOLS.has(protocol)) shell.openExternal(url);
}

/** Popout windows (call popout, stream popout) keyed by Discord's frame name. */
export const popouts = new Map<string, BrowserWindow>();

// window.open() feature names Discord passes that are safe to forward to BrowserWindow.
const POPOUT_FEATURES = new Set(["width", "height", "left", "top", "resizable", "movable", "alwaysOnTop", "frame"]);

function parseFeatures(features: string): BrowserWindowConstructorOptions {
    const out: Record<string, unknown> = {};
    for (const pair of features.split(",")) {
        const [key, raw = ""] = pair.split("=").map(s => s.trim());
        if (!POPOUT_FEATURES.has(key)) continue;
        out[key] = raw === "yes" ? true : raw === "no" ? false : isNaN(Number(raw)) ? raw : Number(raw);
    }
    // window.open speaks left/top, BrowserWindow speaks x/y.
    if ("left" in out) (out.x = out.left), delete out.left;
    if ("top" in out) (out.y = out.top), delete out.top;
    return out as BrowserWindowConstructorOptions;
}

/** Route window.open from a Discord page: popouts open in-app, everything else goes to the desktop. */
export function wireLinks(win: BrowserWindow) {
    win.webContents.setWindowOpenHandler(({ url, frameName, features }) => {
        let parsed: URL;
        try {
            parsed = new URL(url);
        } catch {
            return { action: "deny" };
        }

        const isPopout =
            frameName.startsWith("DISCORD_") && parsed.pathname === "/popout" && DISCORD_HOSTS.has(parsed.hostname);
        if (isPopout) {
            const existing = popouts.get(frameName);
            if (existing && !existing.isDestroyed()) {
                existing.show();
                existing.focus();
                return { action: "deny" };
            }
            return {
                action: "allow",
                overrideBrowserWindowOptions: {
                    title: "Discord Popout",
                    backgroundColor: Settings.store.splashBackground ?? "#1a1a1e",
                    minWidth: 320,
                    minHeight: 180,
                    frame: Settings.store.nativeTitleBar,
                    autoHideMenuBar: true,
                    ...parseFeatures(features),
                    webPreferences: { nodeIntegration: false, contextIsolation: true, sandbox: true }
                }
            };
        }

        // Discord web opens about:blank first for some flows (e.g. connections) and fills it in.
        if (url === "about:blank") return { action: "allow" };
        // ...and loads a static placeholder for the account-connection popup, which is useless outside a browser.
        if (frameName === "authorize" && parsed.searchParams.get("loading") === "true") return { action: "deny" };

        openExternal(url);
        return { action: "deny" };
    });

    win.webContents.on("did-create-window", (child, { frameName }) => {
        if (!frameName.startsWith("DISCORD_")) return;
        child.setMenuBarVisibility(false);
        popouts.set(frameName, child);
        child.webContents.setWindowOpenHandler(({ url }) => {
            openExternal(url);
            return { action: "deny" };
        });
        child.once("closed", () => popouts.delete(frameName));
    });

    // A link that would navigate the main window away from Discord opens in the browser instead.
    win.webContents.on("will-navigate", (e, url) => {
        try {
            if (DISCORD_HOSTS.has(new URL(url).hostname)) return;
        } catch {}
        e.preventDefault();
        openExternal(url);
    });
}
