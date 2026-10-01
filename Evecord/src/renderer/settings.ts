/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { useEffect, useReducer } from "@vencord/types/webpack/common";
import { Store } from "@shared/store";
import type { Settings as TSettings } from "@shared/settings";

/**
 * The renderer's copy of Evecord's settings. Writes go to main one path at a time;
 * main echoes the full object back after every change (its own included), which
 * replaces this copy without being sent back again.
 */
export const Settings = new Store<TSettings>(EvecordNative.settings.get());

Settings.onAnyChange((data, path) => {
    if (!path) return; // a replace() from main, not a local edit
    const value = path.split(".").reduce<any>((o, k) => o?.[k], data);
    EvecordNative.settings.set(path, value);
});

EvecordNative.settings.onChanged(data => Settings.replace(data));

/** Settings for React components; re-renders on any change. */
export function useSettings() {
    const [, rerender] = useReducer((n: number) => n + 1, 0);
    useEffect(() => Settings.onAnyChange(() => rerender()) as () => void, []);
    return Settings.store;
}

/**
 * Per-browser preferences that are not worth a round trip to main (last used stream
 * quality). Discord deletes window.localStorage once it has started, so keep a reference.
 */
export const localStore = window.localStorage;

interface StreamQuality {
    resolution: "480" | "720" | "1080" | "1440" | "2160";
    frameRate: "15" | "30" | "60";
}

const QUALITY_KEY = "EvecordStreamQuality";

export function getStreamQuality(): StreamQuality {
    try {
        const stored = JSON.parse(localStore.getItem(QUALITY_KEY) ?? "null");
        if (stored?.resolution && stored?.frameRate) return stored;
    } catch {}
    return { resolution: "720", frameRate: "30" };
}

export function setStreamQuality(q: StreamQuality) {
    localStore.setItem(QUALITY_KEY, JSON.stringify(q));
}
