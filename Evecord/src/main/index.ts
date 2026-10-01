/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Order matters: cli may print --help/--version and exit before anything touches
// the disk, paths sets userData/sessionData, and the protocol scheme has to be
// registered before the app is ready.
import "./cli";
import "./paths";
import "./protocol";

import { app, BrowserWindow, dialog } from "electron";
import { execFile } from "child_process";
import { join } from "path";
import { Ipc } from "@shared/ipc";

import { startRichPresence } from "./arrpc";
import { applyChromiumFlags } from "./chromiumFlags";
import { CommandLine, findDeepLink } from "./cli";
import { runFirstLaunch } from "./firstLaunch";
import { registerScreenShare } from "./screenShare";
import { Settings, State } from "./settings";
import { createSplash, splashMessage } from "./splash";
import { ensureVencord, vencordDir, watchVencord } from "./vencord";
import { createWindows, getMainWindow, navigateTo, showMainWindow } from "./window";

import "./ipc";
import "./venmic";

console.log(`[Evecord] ${app.getVersion()}, Electron ${process.versions.electron}, Chrome ${process.versions.chrome}`);

app.setName("Evecord");
// On Wayland the app_id comes from here; it has to match evecord.desktop for the
// taskbar, notifications and Hyprland window rules to find the window.
app.setDesktopName("evecord.desktop");

if (!app.requestSingleInstanceLock()) {
    // Another Evecord is running; it gets our argv (and any discord:// link) and shows itself.
    app.quit();
} else {
    start();
}

let mainWindowCreated = false;

function start() {
    applyChromiumFlags();

    app.on("second-instance", (_e, argv) => {
        const link = findDeepLink(argv);
        if (link) navigateTo(link);
        else showMainWindow();
    });

    // With close-to-tray the main window only ever hides, so reaching this means it
    // really was closed. Ignored before it exists: the first-launch window closing
    // must not end the app on its way to opening Discord.
    app.on("window-all-closed", () => {
        if (mainWindowCreated) app.quit();
    });

    app.on("web-contents-created", (_e, wc) => wc.setWebRTCIPHandlingPolicy(Settings.store.webRTCIPHandlingPolicy));
    Settings.onChange("webRTCIPHandlingPolicy", policy => {
        for (const win of BrowserWindow.getAllWindows()) win.webContents.setWebRTCIPHandlingPolicy(policy);
    });

    Settings.onChange("handleDiscordLinks", on => on && registerDiscordLinks());

    app.whenReady().then(async () => {
        registerScreenShare();

        if (!State.store.firstLaunchDone) await runFirstLaunch();
        if (Settings.store.handleDiscordLinks) registerDiscordLinks();

        // Splash first, so a Vencord download on a fresh install has somewhere to report.
        if (Settings.store.splashScreen) createSplash(!CommandLine.startMinimized);
        while (true) {
            try {
                await ensureVencord(splashMessage);
                break;
            } catch (err) {
                console.error("[Evecord] No Vencord:", err);
                const { response } = await dialog.showMessageBox({
                    type: "error",
                    title: "Evecord",
                    message: "Evecord could not find or download Vencord.",
                    detail: `${err}\n\nBuild your Vencord checkout (pnpm build in ~/Projects/Vencord) or check the network, then retry.`,
                    buttons: ["Retry", "Quit"],
                    defaultId: 0,
                    cancelId: 1
                });
                if (response === 1) return app.quit();
            }
        }

        // Vencord's main half (its IPC handlers for settings, QuickCSS and themes, the CSP
        // allowlist, the vencord:// protocol) has to be up before Discord loads, because
        // Vencord's preload calls into it synchronously.
        require(join(vencordDir(), "vencordDesktopMain.js"));

        createWindows(CommandLine.deepLink);
        mainWindowCreated = true;

        if (Settings.store.vencordRebuildNotice) {
            watchVencord(change => getMainWindow()?.webContents.send(Ipc.VENCORD_CHANGED, change));
        }
        startRichPresence();
    });
}

/** Make Evecord the handler for discord:// links (invites opened from a browser, etc). */
function registerDiscordLinks() {
    // Electron's setAsDefaultProtocolClient goes through xdg-settings; xdg-mime is the
    // direct route and works without a desktop-environment-specific backend.
    execFile("xdg-mime", ["default", "evecord.desktop", "x-scheme-handler/discord"], err => {
        if (err) console.error("[Evecord] Could not register discord:// handler:", err);
    });
}
