/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Runs inside Discord, after Vencord, before Discord's own code. Everything here
 * reaches Discord through Vencord: its webpack patches, stores and components.
 */

import "./patches/desktopIntegration";
import "./patches/media";
import "./patches/screenShare";
import "./patches/spellcheck";
import "./commands";
import "./fixes";
import "./badge";
import "./richPresence";

import { VesktopSettingsIcon } from "@vencord/types/components";
import { Command, type CaptureSource } from "@shared/ipc";

import { onCommand } from "./commands";
import EvecordSettings from "./components/EvecordSettings";
import { openScreenSharePicker } from "./components/ScreenSharePicker";
import { Settings } from "./settings";

export { Settings };

onCommand(Command.SCREEN_SHARE_PICKER, ({ sources, alreadyPicked }: { sources: CaptureSource[]; alreadyPicked: boolean }) =>
    openScreenSharePicker(sources, alreadyPicked)
);

// An "Evecord" page among Vencord's settings pages.
(Vencord.Plugins.plugins.Settings as any).customEntries.push({
    key: "evecord",
    title: "Evecord",
    panelTitle: "Evecord Settings",
    Component: EvecordSettings,
    Icon: VesktopSettingsIcon
});

// Vencord's desktop build reads a few things from window.Vesktop (consoleShortcuts'
// branch switcher uses Vesktop.Settings); this object has the same shape.
Object.defineProperty(window, "Vesktop", { get: () => window.Evecord, configurable: true });

console.log(
    "%c Evecord %c " + EvecordNative.app.getVersion(),
    "background:#9ce3b4;color:#0e1a12;font-weight:bold;border-radius:4px 0 0 4px",
    "background:#0e1a12;color:#9ce3b4;border-radius:0 4px 4px 0"
);
