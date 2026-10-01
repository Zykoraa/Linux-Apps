/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/** Channel names for renderer -> main IPC. */
export const Ipc = {
    // scripts injected into Discord
    VENCORD_PRELOAD: "evecord:vencord-preload",
    VENCORD_RENDERER: "evecord:vencord-renderer",
    EVECORD_RENDERER: "evecord:renderer-js",
    EVECORD_RENDERER_CSS: "evecord:renderer-css",

    VERSION: "evecord:version",
    RELAUNCH: "evecord:relaunch",
    HARDWARE_ACCELERATION: "evecord:hardware-acceleration",

    SETTINGS_GET: "evecord:settings-get",
    SETTINGS_SET: "evecord:settings-set",
    /** main -> renderer: the whole settings object after any change, wherever it was made */
    SETTINGS_CHANGED: "evecord:settings-changed",

    WIN_FOCUS: "evecord:win-focus",
    WIN_CLOSE: "evecord:win-close",
    WIN_MINIMIZE: "evecord:win-minimize",
    WIN_MAXIMIZE: "evecord:win-maximize",
    WIN_FLASH: "evecord:win-flash",

    BADGE: "evecord:badge",

    SPELLCHECK_LANGUAGES: "evecord:spellcheck-languages",
    SPELLCHECK_REPLACE: "evecord:spellcheck-replace",
    SPELLCHECK_LEARN: "evecord:spellcheck-learn",

    CAPTURER_THUMBNAIL: "evecord:capturer-thumbnail",

    VIRTMIC_LIST: "evecord:virtmic-list",
    VIRTMIC_START: "evecord:virtmic-start",
    VIRTMIC_START_SYSTEM: "evecord:virtmic-start-system",
    VIRTMIC_UNMUTE: "evecord:virtmic-unmute",
    VIRTMIC_STOP: "evecord:virtmic-stop",

    CLIPBOARD_IMAGE: "evecord:clipboard-image",

    AUTOSTART_GET: "evecord:autostart-get",
    AUTOSTART_SET: "evecord:autostart-set",

    VENCORD_INFO: "evecord:vencord-info",
    VENCORD_CHOOSE_DIR: "evecord:vencord-choose-dir",
    VENCORD_RESET_DIR: "evecord:vencord-reset-dir",
    VENCORD_DOWNLOAD: "evecord:vencord-download",
    OPEN_PATH: "evecord:open-path",

    DEBUG_GPU: "evecord:debug-gpu",
    DEBUG_WEBRTC: "evecord:debug-webrtc",

    FIRST_LAUNCH_INFO: "evecord:first-launch-info",
    FIRST_LAUNCH_DONE: "evecord:first-launch-done",

    // main -> renderer
    COMMAND: "evecord:command",
    SPELLCHECK_RESULT: "evecord:spellcheck-result",
    DEVTOOLS_OPENED: "evecord:devtools-opened",
    DEVTOOLS_CLOSED: "evecord:devtools-closed",
    VENCORD_CHANGED: "evecord:vencord-changed",
    SPLASH_MESSAGE: "evecord:splash-message"
} as const;

/** Requests main sends to the Discord renderer and awaits an answer for. */
export const Command = {
    SCREEN_SHARE_PICKER: "screenshare-picker",
    RPC_ACTIVITY: "rpc-activity",
    RPC_INVITE: "rpc-invite",
    RPC_DEEP_LINK: "rpc-deep-link",
    OPEN_SETTINGS: "open-settings",
    LANGUAGES: "languages"
} as const;

export interface CommandRequest {
    nonce: string;
    command: string;
    data?: unknown;
}

export interface CommandResponse {
    nonce: string;
    ok: boolean;
    data?: unknown;
}

export type VencordSourceKind = "custom" | "local" | "download";

export interface VencordInfo {
    kind: VencordSourceKind;
    dir: string;
    /** Release tag for downloaded builds; git describe-ish info is not tracked for local ones. */
    tag?: string;
    /** The local checkout Evecord looks for in automatic mode. */
    localCheckout: string;
    localCheckoutValid: boolean;
}

/** Which parts of a Vencord rebuild changed: renderer-side files apply on reload, main needs a restart. */
export interface VencordChange {
    needsRestart: boolean;
}

/** One PipeWire node as venmic reports it: a bag of string properties. */
export type AudioNode = Record<string, string>;

export type VirtmicList =
    | { ok: true; targets: AudioNode[]; hasPipewirePulse: boolean }
    | { ok: false; reason: string };

export interface CaptureSource {
    id: string;
    name: string;
    /** data: URL thumbnail */
    url: string;
}

export interface StreamPick {
    id: string;
    contentHint: "motion" | "detail";
    /** Sources to mix into the stream: "None", "Entire System", or a list of node property matches. */
    includeSources: "None" | "Entire System" | AudioNode[];
    excludeSources: "None" | AudioNode[];
}
