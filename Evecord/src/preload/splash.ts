/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { contextBridge, ipcRenderer } from "electron/renderer";
import { Ipc } from "@shared/ipc";

contextBridge.exposeInMainWorld("EvecordSplash", {
    onMessage: (cb: (message: string) => void) => ipcRenderer.on(Ipc.SPLASH_MESSAGE, (_e, m: string) => cb(m))
});
