/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { ipcRenderer } from "electron/renderer";
import {
    type AudioNode,
    type CommandRequest,
    type CommandResponse,
    Ipc,
    type VencordChange,
    type VencordInfo,
    type VirtmicList
} from "@shared/ipc";
import type { Settings } from "@shared/settings";

const invoke = <T = void>(channel: string, ...args: unknown[]) => ipcRenderer.invoke(channel, ...args) as Promise<T>;
const sendSync = <T>(channel: string, ...args: unknown[]) => ipcRenderer.sendSync(channel, ...args) as T;

type SpellcheckCallback = (word: string, suggestions: string[]) => void;
const spellcheckCallbacks = new Set<SpellcheckCallback>();
ipcRenderer.on(Ipc.SPELLCHECK_RESULT, (_e, word: string, suggestions: string[]) => {
    for (const cb of spellcheckCallbacks) cb(word, suggestions);
});

let devtoolsOpened = () => {};
let devtoolsClosed = () => {};
ipcRenderer.on(Ipc.DEVTOOLS_OPENED, () => devtoolsOpened());
ipcRenderer.on(Ipc.DEVTOOLS_CLOSED, () => devtoolsClosed());

/**
 * What the page gets as window.EvecordNative. It is also exposed as VesktopNative:
 * Vencord's desktop build was written for Vesktop and calls app.getRendererCss,
 * app.getVersion, app.relaunch, app.isOutdated and clipboard.copyImage by that name.
 */
export const EvecordNative = {
    app: {
        getVersion: () => sendSync<string>(Ipc.VERSION),
        relaunch: () => invoke(Ipc.RELAUNCH),
        getEnableHardwareAcceleration: () => sendSync<boolean>(Ipc.HARDWARE_ACCELERATION),
        setBadgeCount: (count: number) => invoke(Ipc.BADGE, count),
        // Vencord loads this as its "vesktop-css-core" stylesheet.
        getRendererCss: () => invoke<string>(Ipc.EVECORD_RENDERER_CSS),
        onRendererCssUpdate: (_cb: (css: string) => void) => {},
        // Evecord updates with git pull + install.sh, so Vencord's "Vesktop is outdated" card never shows.
        isOutdated: () => Promise.resolve(false),
        openUpdater: () => Promise.resolve()
    },
    settings: {
        get: () => sendSync<Settings>(Ipc.SETTINGS_GET),
        set: (path: string, value: unknown) => invoke(Ipc.SETTINGS_SET, path, value),
        onChanged: (cb: (settings: Settings) => void) => {
            ipcRenderer.on(Ipc.SETTINGS_CHANGED, (_e, s: Settings) => cb(s));
        }
    },
    win: {
        focus: () => invoke(Ipc.WIN_FOCUS),
        close: (key?: string) => invoke(Ipc.WIN_CLOSE, key),
        minimize: (key?: string) => invoke(Ipc.WIN_MINIMIZE, key),
        maximize: (key?: string) => invoke(Ipc.WIN_MAXIMIZE, key),
        flashFrame: (flag: boolean) => invoke(Ipc.WIN_FLASH, flag),
        setDevtoolsCallbacks(onOpen: () => void, onClose: () => void) {
            devtoolsOpened = onOpen;
            devtoolsClosed = onClose;
        }
    },
    spellcheck: {
        getAvailableLanguages: () => sendSync<string[]>(Ipc.SPELLCHECK_LANGUAGES) ?? [],
        onSpellcheckResult: (cb: SpellcheckCallback) => void spellcheckCallbacks.add(cb),
        offSpellcheckResult: (cb: SpellcheckCallback) => void spellcheckCallbacks.delete(cb),
        replaceMisspelling: (word: string) => invoke(Ipc.SPELLCHECK_REPLACE, word),
        addToDictionary: (word: string) => invoke(Ipc.SPELLCHECK_LEARN, word)
    },
    capturer: {
        getLargeThumbnail: (id: string) => invoke<string | null>(Ipc.CAPTURER_THUMBNAIL, id)
    },
    virtmic: {
        list: () => invoke<VirtmicList>(Ipc.VIRTMIC_LIST),
        start: (include: AudioNode[]) => invoke<boolean>(Ipc.VIRTMIC_START, include),
        startSystem: (exclude: AudioNode[]) => invoke<boolean>(Ipc.VIRTMIC_START_SYSTEM, exclude),
        unmute: () => invoke(Ipc.VIRTMIC_UNMUTE),
        stop: () => invoke(Ipc.VIRTMIC_STOP)
    },
    clipboard: {
        copyImage: (png: Uint8Array | ArrayBuffer, src: string) =>
            invoke(Ipc.CLIPBOARD_IMAGE, png instanceof Uint8Array ? png : new Uint8Array(png), src)
    },
    autostart: {
        isEnabled: () => sendSync<boolean>(Ipc.AUTOSTART_GET),
        set: (on: boolean) => invoke(Ipc.AUTOSTART_SET, on)
    },
    vencord: {
        info: () => sendSync<VencordInfo>(Ipc.VENCORD_INFO),
        chooseDir: () => invoke<"ok" | "cancelled" | "invalid">(Ipc.VENCORD_CHOOSE_DIR),
        resetDir: () => invoke(Ipc.VENCORD_RESET_DIR),
        download: () => invoke<VencordInfo>(Ipc.VENCORD_DOWNLOAD),
        openDir: (dir: string) => invoke(Ipc.OPEN_PATH, dir),
        onChanged: (cb: (change: VencordChange) => void) => {
            ipcRenderer.on(Ipc.VENCORD_CHANGED, (_e, change: VencordChange) => cb(change));
        }
    },
    debug: {
        openGpu: () => invoke(Ipc.DEBUG_GPU),
        openWebrtcInternals: () => invoke(Ipc.DEBUG_WEBRTC)
    },
    commands: {
        onCommand: (cb: (req: CommandRequest) => void) => {
            ipcRenderer.on(Ipc.COMMAND, (_e, req: CommandRequest) => cb(req));
        },
        respond: (res: CommandResponse) => ipcRenderer.send(Ipc.COMMAND, res)
    }
};

export type EvecordNativeApi = typeof EvecordNative;
