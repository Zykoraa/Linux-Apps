/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { contextBridge, ipcRenderer, webFrame } from "electron/renderer";
import { Ipc } from "@shared/ipc";

import { EvecordNative } from "./native";

contextBridge.exposeInMainWorld("EvecordNative", EvecordNative);
contextBridge.exposeInMainWorld("VesktopNative", EvecordNative);

const script = (channel: string) => {
    const src = ipcRenderer.sendSync(channel);
    if (typeof src !== "string") throw new Error(`[Evecord] ${channel} returned nothing`);
    return src;
};

// Vencord's preload. It expects the Node-ish globals a sandboxed preload gets as
// locals (require, Buffer, process...), so they are passed in as parameters.
Function("require", "Buffer", "process", "clearImmediate", "setImmediate", script(Ipc.VENCORD_PRELOAD))(
    require,
    Buffer,
    process,
    clearImmediate,
    setImmediate
);

// Then into the page itself: Vencord first (it hooks webpack before Discord's code
// runs), then Evecord's own patches, which build on Vencord's APIs.
webFrame.executeJavaScript(script(Ipc.VENCORD_RENDERER));
webFrame.executeJavaScript(script(Ipc.EVECORD_RENDERER));
