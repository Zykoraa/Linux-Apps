/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

declare global {
    var EvecordNative: import("./preload/native").EvecordNativeApi;
    var Evecord: typeof import("./renderer/index");
    /** Holds the functions Evecord's webpack patches call back into ($self in a patch). */
    var EvecordPatchGlobals: Record<string, any>;
    var IS_DEV: boolean;
}

declare module "arrpc" {
    const Server: new () => Promise<import("events").EventEmitter>;
    export default Server;
}

export {};
