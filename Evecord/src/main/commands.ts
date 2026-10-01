/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { randomUUID } from "crypto";
import { ipcMain } from "electron";
import { Ipc, type CommandRequest, type CommandResponse } from "@shared/ipc";

import { getMainWindow } from "./window";

const pending = new Map<string, { resolve: (v: any) => void; reject: (e: any) => void }>();

/**
 * Ask the Discord renderer to do something and wait for its answer. The renderer side
 * registers handlers with onCommand() in src/renderer/commands.ts.
 */
export function rendererCommand<T = unknown>(command: string, data?: unknown): Promise<T> {
    const win = getMainWindow();
    if (!win || win.isDestroyed()) return Promise.reject(new Error("Main window is gone"));

    const nonce = randomUUID();
    const promise = new Promise<T>((resolve, reject) => pending.set(nonce, { resolve, reject }));
    win.webContents.send(Ipc.COMMAND, { nonce, command, data } satisfies CommandRequest);
    return promise;
}

ipcMain.on(Ipc.COMMAND, (_e, { nonce, ok, data }: CommandResponse) => {
    const p = pending.get(nonce);
    if (!p) return;
    pending.delete(nonce);
    if (ok) p.resolve(data);
    else p.reject(data);
});

/** A reload throws away every renderer handler, so nothing pending will ever be answered. */
export function rejectPendingCommands() {
    for (const p of pending.values()) p.reject(new Error("Renderer reloaded"));
    pending.clear();
}
