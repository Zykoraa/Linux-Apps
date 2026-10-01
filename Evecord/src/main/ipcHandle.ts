/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { ipcMain, type IpcMainEvent, type IpcMainInvokeEvent, type WebFrameMain } from "electron";

export const DISCORD_HOSTS = new Set(["discord.com", "ptb.discord.com", "canary.discord.com"]);

/**
 * Only Discord's own pages and Evecord's bundled views may talk to the main process.
 * Anything else that ends up in a window (an iframe, a page reached by a stray
 * navigation) gets an exception instead of an answer.
 */
function assertTrustedSender(frame: WebFrameMain | null, channel: string) {
    if (!frame) throw new Error(`ipc ${channel}: no sender frame`);
    let url: URL;
    try {
        url = new URL(frame.url);
    } catch {
        throw new Error(`ipc ${channel}: unparseable sender ${frame.url}`);
    }
    if (url.protocol === "evecord:") return;
    if (url.protocol === "https:" && DISCORD_HOSTS.has(url.hostname)) return;
    throw new Error(`ipc ${channel}: sender ${url.origin} is not allowed`);
}

export function handle(channel: string, fn: (e: IpcMainInvokeEvent, ...args: any[]) => unknown) {
    ipcMain.handle(channel, (e, ...args) => {
        assertTrustedSender(e.senderFrame, channel);
        return fn(e, ...args);
    });
}

export function handleSync(channel: string, fn: (e: IpcMainEvent, ...args: any[]) => unknown) {
    ipcMain.on(channel, (e, ...args) => {
        // sendSync blocks the renderer until returnValue is set, so a throw here
        // would freeze the page rather than surface as an error. Always answer.
        try {
            assertTrustedSender(e.senderFrame, channel);
            e.returnValue = fn(e, ...args);
        } catch (err) {
            console.error(`[Evecord] ${channel}:`, err);
            e.returnValue = null;
        }
    });
}
