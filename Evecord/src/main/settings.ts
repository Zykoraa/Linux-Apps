/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { readFileSync, renameSync, writeFileSync } from "fs";
import { DEFAULT_SETTINGS, fillDefaults, type Settings as TSettings, type State as TState } from "@shared/settings";
import { Store } from "@shared/store";

import { SETTINGS_FILE, STATE_FILE } from "./paths";

function readJson(file: string): object {
    let text: string;
    try {
        text = readFileSync(file, "utf8");
    } catch {
        return {};
    }
    try {
        const parsed = JSON.parse(text);
        return parsed !== null && typeof parsed === "object" ? parsed : {};
    } catch (err) {
        // Keep the unreadable file around rather than overwriting it on the next change.
        console.error(`[Evecord] ${file} is not valid JSON, moving it aside:`, err);
        try {
            renameSync(file, `${file}.broken-${Date.now()}`);
        } catch {}
        return {};
    }
}

function writeJson(file: string, data: object) {
    const tmp = `${file}.tmp`;
    writeFileSync(tmp, JSON.stringify(data, null, 4));
    renameSync(tmp, file);
}

/**
 * Settings are written the moment they change. Every change comes from a toggle in
 * the settings page, so each write is a deliberate choice, not a session being saved.
 */
function persistentStore<T extends object>(file: string, data: T) {
    const store = new Store<T>(data);
    store.onAnyChange(d => {
        try {
            writeJson(file, d);
        } catch (err) {
            console.error(`[Evecord] Failed to write ${file}:`, err);
        }
    });
    return store;
}

export const Settings = persistentStore<TSettings>(
    SETTINGS_FILE,
    fillDefaults(readJson(SETTINGS_FILE) as TSettings, structuredClone(DEFAULT_SETTINGS))
);

export const State = persistentStore<TState>(STATE_FILE, readJson(STATE_FILE) as TState);
