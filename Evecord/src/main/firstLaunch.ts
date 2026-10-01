/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { app, BrowserWindow } from "electron";
import { cpSync, existsSync } from "fs";
import { join } from "path";
import { Ipc } from "@shared/ipc";
import type { DiscordBranch } from "@shared/settings";

import { autostart } from "./autostart";
import { handle, handleSync } from "./ipcHandle";
import { openExternal } from "./links";
import { DISCORD_VENCORD_DIR, LOCAL_VENCORD_DIST, VENCORD_DATA_DIR } from "./paths";
import { loadView } from "./protocol";
import { Settings, State } from "./settings";
import { isVencordDir } from "./vencord";

export interface FirstLaunchChoices {
    discordBranch: DiscordBranch;
    importVencordSettings: boolean;
    importThemes: boolean;
    closeToTray: boolean;
    autostart: boolean;
    richPresence: boolean;
}

/** Copy the desktop client's Vencord settings (and themes) so Evecord starts out the same. */
function importFromDiscordVencord(settings: boolean, themes: boolean) {
    const copy = (name: string) => {
        const from = join(DISCORD_VENCORD_DIR, name);
        if (!existsSync(from)) return;
        try {
            cpSync(from, join(VENCORD_DATA_DIR, name), { recursive: true, force: true });
            console.log(`[Evecord] Imported ${from}`);
        } catch (err) {
            console.error(`[Evecord] Could not import ${from}:`, err);
        }
    };
    if (settings) copy("settings");
    if (themes) copy("themes");
}

/** A short window that asks the few questions worth asking once. Resolves when it is answered. */
export function runFirstLaunch(): Promise<void> {
    const win = new BrowserWindow({
        title: "Welcome to Evecord",
        width: 560,
        height: 640,
        resizable: false,
        autoHideMenuBar: true,
        backgroundColor: "#0e1a12",
        webPreferences: { preload: join(__dirname, "firstLaunchPreload.js") }
    });
    win.webContents.setWindowOpenHandler(({ url }) => {
        openExternal(url);
        return { action: "deny" };
    });

    handleSync(Ipc.FIRST_LAUNCH_INFO, () => ({
        version: app.getVersion(),
        hasDiscordVencordSettings: existsSync(join(DISCORD_VENCORD_DIR, "settings", "settings.json")),
        hasDiscordVencordThemes: existsSync(join(DISCORD_VENCORD_DIR, "themes")),
        localVencord: isVencordDir(LOCAL_VENCORD_DIST) ? LOCAL_VENCORD_DIST : null
    }));

    return new Promise(resolve => {
        let answered = false;

        handle(Ipc.FIRST_LAUNCH_DONE, (_e, c: FirstLaunchChoices) => {
            answered = true;
            Settings.store.discordBranch = c.discordBranch;
            Settings.store.closeToTray = c.closeToTray;
            Settings.store.richPresence = c.richPresence;
            if (c.autostart) autostart.enable();
            importFromDiscordVencord(c.importVencordSettings, c.importThemes);
            State.store.firstLaunchDone = true;
            win.close();
            resolve();
        });

        // Closing the window without answering means "not now": quit, ask again next time.
        win.on("closed", () => {
            if (!answered) app.quit();
        });

        loadView(win, "first-launch.html");
    });
}
