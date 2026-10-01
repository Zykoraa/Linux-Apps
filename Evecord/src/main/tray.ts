/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { Menu, nativeImage, Tray } from "electron";
import { join } from "path";

import { STATIC_DIR } from "./paths";
import { Settings } from "./settings";
import { getVencordInfo } from "./vencord";
import { getMainWindow, quitApp, relaunchApp, showMainWindow } from "./window";

let tray: Tray | undefined;
let unread = false;

const icon = (name: string) => nativeImage.createFromPath(join(STATIC_DIR, "tray", `${name}.png`));

function buildMenu() {
    const vencord = getVencordInfo();
    return Menu.buildFromTemplate([
        { label: "Open Evecord", click: showMainWindow },
        { type: "separator" },
        {
            label: "Reload Discord",
            click: () => getMainWindow()?.webContents.reload()
        },
        {
            label: `Vencord: ${vencord.kind === "download" ? `release ${vencord.tag ?? ""}` : vencord.kind}`,
            enabled: false
        },
        { type: "separator" },
        { label: "Restart Evecord", click: relaunchApp },
        { label: "Quit", click: quitApp }
    ]);
}

export function initTray() {
    if (tray) return;
    tray = new Tray(icon(unread ? "trayUnread" : "tray"));
    tray.setToolTip("Evecord");
    tray.setContextMenu(buildMenu());
    tray.on("click", () => {
        const win = getMainWindow();
        if (Settings.store.trayClickToggles && win?.isVisible() && win.isFocused()) win.hide();
        else showMainWindow();
    });
}

export function destroyTray() {
    tray?.destroy();
    tray = undefined;
}

export function setTrayUnread(value: boolean) {
    if (unread === value) return;
    unread = value;
    tray?.setImage(icon(unread ? "trayUnread" : "tray"));
}

Settings.onChange("tray", on => {
    if (on) return initTray();
    destroyTray();
    // Without a tray icon a hidden window could never be brought back.
    showMainWindow();
});
