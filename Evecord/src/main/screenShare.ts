/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { desktopCapturer, session } from "electron";
import { Command, type CaptureSource, Ipc, type StreamPick } from "@shared/ipc";

import { rendererCommand } from "./commands";
import { handle } from "./ipcHandle";

const isWayland = process.env.XDG_SESSION_TYPE === "wayland" || !!process.env.WAYLAND_DISPLAY;

/**
 * getDisplayMedia() from Discord lands here. On Wayland every getSources() call opens
 * the desktop portal's own picker and returns just the one source chosen there, so
 * Evecord only asks for stream settings afterwards. On X11 Evecord draws the picker.
 */
export function registerScreenShare() {
    session.defaultSession.setDisplayMediaRequestHandler(async (_request, callback) => {
        const thumbWidth = isWayland ? 1280 : 320;
        const sources = await desktopCapturer
            .getSources({ types: ["screen", "window"], thumbnailSize: { width: thumbWidth, height: (thumbWidth * 9) / 16 } })
            .catch(err => {
                console.error("[Evecord] desktopCapturer failed:", err);
                return null;
            });

        // The portal dialog was dismissed, or capture is not available at all.
        if (!sources?.length) return callback({});

        const choices: CaptureSource[] = sources.map(s => ({ id: s.id, name: s.name, url: s.thumbnail.toDataURL() }));

        const pick = await rendererCommand<StreamPick>(Command.SCREEN_SHARE_PICKER, {
            sources: choices,
            alreadyPicked: isWayland
        }).catch(() => null);

        const source = pick && sources.find(s => s.id === pick.id);
        if (!source) return callback({});

        // Audio is not taken from here: venmic adds a virtual mic that the renderer
        // attaches to the stream (see src/renderer/patches/screenShareAudio.ts).
        callback({ video: source });
    });

    // Bigger preview for the X11 picker's second page. Not offered on Wayland: it would
    // open the portal dialog a second time.
    handle(Ipc.CAPTURER_THUMBNAIL, async (_e, id: string) => {
        if (isWayland) return null;
        const sources = await desktopCapturer.getSources({
            types: ["screen", "window"],
            thumbnailSize: { width: 1280, height: 720 }
        });
        return sources.find(s => s.id === id)?.thumbnail.toDataURL() ?? null;
    });
}
