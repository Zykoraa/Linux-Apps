/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

export type DiscordBranch = "stable" | "ptb" | "canary";

export type WebRTCIPHandlingPolicy =
    | "default"
    | "default_public_interface_only"
    | "default_public_and_private_interfaces"
    | "disable_non_proxied_udp";

/** Options for the virtual microphone that carries screen share audio (venmic). */
export interface StreamAudioSettings {
    /** Start the virtual mic muted, so Chromium's initial burst of noise is not sent. */
    initialMute: boolean;
    /** Route around a PipeWire quirk where the real microphone gets shared instead. */
    micWorkaround: boolean;
    /** "Entire system": only take apps that play to a speaker. */
    onlySpeakers: boolean;
    /** "Entire system": only take apps that play to the default speaker. */
    onlyDefaultSpeakers: boolean;
    /** Never offer capture streams (other apps' microphones) as sources. */
    ignoreInputs: boolean;
    /** Never offer virtual nodes (loopbacks, virtual cables). */
    ignoreVirtual: boolean;
    /** Never offer hardware devices. */
    ignoreDevices: boolean;
    /** List every stream separately (pid, binary, media name) instead of one entry per app. */
    granularSelect: boolean;
    /** Offer hardware devices as sources. Needs ignoreDevices off. */
    deviceSelect: boolean;
}

export interface Settings {
    discordBranch: DiscordBranch;

    /** Directory holding vencordDesktop*.js. Unset = automatic (local checkout, else downloaded release). */
    vencordDir?: string;
    /** Tell me when the Vencord files on disk change, so a rebuild can be picked up. */
    vencordRebuildNotice: boolean;

    nativeTitleBar: boolean;
    staticTitle: boolean;
    disableMinSize: boolean;

    splashScreen: boolean;
    splashTheming: boolean;
    /** Captured from the Discord theme when the window closes; used to paint the next splash. */
    splashColor?: string;
    splashBackground?: string;

    tray: boolean;
    closeToTray: boolean;
    trayClickToggles: boolean;
    startMinimized: boolean;

    hardwareAcceleration: boolean;
    hardwareVideoAcceleration: boolean;
    smoothScrolling: boolean;

    unreadBadge: boolean;
    taskbarFlash: boolean;

    richPresence: boolean;
    handleDiscordLinks: boolean;
    webRTCIPHandlingPolicy: WebRTCIPHandlingPolicy;

    spellCheckLanguages?: string[];

    streamAudio: StreamAudioSettings;
}

export interface State {
    firstLaunchDone?: boolean;
    /** Tag of the Vencord release last downloaded into the data dir. */
    downloadedVencordTag?: string;
}

export const DEFAULT_SETTINGS: Settings = {
    discordBranch: "stable",
    vencordRebuildNotice: true,

    nativeTitleBar: false,
    staticTitle: false,
    disableMinSize: false,

    splashScreen: true,
    splashTheming: true,

    tray: true,
    closeToTray: true,
    trayClickToggles: false,
    startMinimized: false,

    hardwareAcceleration: true,
    hardwareVideoAcceleration: false,
    smoothScrolling: true,

    unreadBadge: true,
    taskbarFlash: false,

    richPresence: true,
    handleDiscordLinks: false,
    webRTCIPHandlingPolicy: "default",

    streamAudio: {
        initialMute: true,
        micWorkaround: false,
        // Off: here the default "speaker" is a BetterBanana virtual sink, not a device,
        // and this would leave Entire System with nothing to share.
        onlySpeakers: false,
        onlyDefaultSpeakers: true,
        ignoreInputs: true,
        ignoreVirtual: false,
        ignoreDevices: true,
        granularSelect: false,
        deviceSelect: false
    }
};

/** Fill in every key `defaults` has and `target` lacks, recursing into plain objects. Mutates `target`. */
export function fillDefaults<T extends object>(target: T, defaults: T): T {
    for (const key of Object.keys(defaults) as (keyof T)[]) {
        const def = defaults[key];
        const cur = target[key];
        if (def !== null && typeof def === "object" && !Array.isArray(def)) {
            if (cur === null || typeof cur !== "object" || Array.isArray(cur)) target[key] = {} as T[keyof T];
            fillDefaults(target[key] as object, def as object);
        } else if (cur === undefined) {
            target[key] = def;
        }
    }
    return target;
}
