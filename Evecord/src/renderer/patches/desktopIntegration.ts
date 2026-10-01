/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Discord web ships most of the desktop client's code paths but guards them with
// "is this the desktop app" checks. These patches point them at Evecord instead.

import { Settings } from "../settings";
import { addPatch } from "./addPatch";

addPatch([
    // Window buttons in Discord's title bar call DiscordNative.window.*; send them to Evecord.
    {
        find: ",setSystemTrayApplications",
        replacement: [
            { match: /\i\.window\.(close|minimize|maximize)/g, replace: "EvecordNative.win.$1" },
            { match: /(focus(\(\i\)){).{0,150}?\.focus\(\i,\i\)/, replace: "$1EvecordNative.win.focus$2" },
            {
                match: /,getEnableHardwareAcceleration/,
                replace: "$&:EvecordNative.app.getEnableHardwareAcceleration,_discordGetEnableHardwareAcceleration"
            }
        ]
    },
    // Desktop-style notification defaults (web defaults to mentions-only and off).
    {
        find: '"NotificationSettingsStore',
        replacement: { match: /\.isPlatformEmbedded(?=\?\i\.\i\.ALL)/g, replace: "$&||true" }
    },
    // The "Download apps" button at the bottom of the server list.
    {
        find: '"app-download-button"',
        replacement: { match: /return(?=.{0,50}id:"app-download-button")/, replace: "return null;return" }
    },
    // Discord's devtools detector misfires and logs you out to "protect" the token.
    // Feed it Electron's real open/close events instead.
    {
        find: ".setDevtoolsCallbacks(",
        group: true,
        replacement: [
            { match: /if\(null!=(\i)\)(?=.{0,50}\1\.window\.setDevtoolsCallbacks)/, replace: "if(true)" },
            { match: /\b\i\.window\.setDevtoolsCallbacks/g, replace: "EvecordNative.win.setDevtoolsCallbacks" }
        ]
    },
    // Taskbar flash on new messages: an urgency hint on Wayland.
    {
        find: ".flashFrame(!0)",
        replacement: {
            match: /(\i)&&\i\.\i\.taskbarFlash&&\i\.\i\.flashFrame\(!0\)/,
            replace: "$self.flashFrame()"
        }
    }
], {
    flashFrame() {
        if (Settings.store.taskbarFlash) EvecordNative.win.flashFrame(true);
    }
});

// Discord draws its own title bar (with window buttons) only for the Windows client.
// Without a native frame, pretend to be that.
if (!Settings.store.nativeTitleBar) {
    addPatch([
        {
            find: ".USE_OSX_NATIVE_TRAFFIC_LIGHTS",
            replacement: { match: /case \i\.\i\.WINDOWS:/, replace: 'case "WEB":' }
        },
        {
            find: '"refresh-title-bar-small"',
            replacement: [
                { match: /\i===\i\.PlatformTypes\.WINDOWS/g, replace: "true" },
                { match: /\i===\i\.PlatformTypes\.WEB/g, replace: "false" }
            ]
        }
    ]);
}

// The platform class decides title bar layout and some spacing in Discord's CSS.
addPatch(
    [
        {
            find: "platform-web",
            replacement: { match: '"platform-web"', replace: "$self.platformClass()" }
        }
    ],
    {
        platformClass: () => (Settings.store.nativeTitleBar ? "platform-web" : "platform-win")
    }
);
