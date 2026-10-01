/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { app } from "electron";
import { existsSync, mkdirSync, rmSync, writeFileSync } from "fs";
import { homedir } from "os";
import { join } from "path";
import { desktopExecArg } from "@shared/desktopEntry";

import { Settings } from "./settings";

const AUTOSTART_FILE = join(process.env.XDG_CONFIG_HOME || join(homedir(), ".config"), "autostart", "evecord.desktop");

/**
 * The command that starts this copy of Evecord. Always absolute: the uwsm session
 * replaces PATH, so a bare `evecord` in an autostart entry would silently do nothing.
 */
function command() {
    const args = [process.execPath];
    if (!app.isPackaged) args.push(app.getAppPath());
    if (Settings.store.startMinimized) args.push("--start-minimized");
    return args.map(desktopExecArg).join(" ");
}

export const autostart = {
    isEnabled: () => existsSync(AUTOSTART_FILE),
    enable() {
        mkdirSync(join(AUTOSTART_FILE, ".."), { recursive: true });
        writeFileSync(
            AUTOSTART_FILE,
            [
                "[Desktop Entry]",
                "Type=Application",
                "Name=Evecord",
                "Comment=Start Evecord when you log in",
                `Exec=${command()}`,
                "Icon=evecord",
                "Terminal=false",
                "StartupNotify=false",
                ""
            ].join("\n")
        );
    },
    disable: () => rmSync(AUTOSTART_FILE, { force: true })
};

// The entry carries --start-minimized, so rewrite it when that choice changes.
Settings.onChange("startMinimized", () => autostart.isEnabled() && autostart.enable());
