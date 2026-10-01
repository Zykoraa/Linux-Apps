/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { contextBridge, ipcRenderer } from "electron/renderer";
import { Ipc } from "@shared/ipc";

contextBridge.exposeInMainWorld("EvecordSetup", {
    info: () => ipcRenderer.sendSync(Ipc.FIRST_LAUNCH_INFO),
    done: (choices: unknown) => ipcRenderer.invoke(Ipc.FIRST_LAUNCH_DONE, choices)
});
