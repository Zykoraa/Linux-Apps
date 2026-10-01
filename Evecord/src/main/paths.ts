/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { app } from "electron";
import { mkdirSync } from "fs";
import { homedir } from "os";
import { join } from "path";

/** ~/.config/evecord, unless EVECORD_DATA_DIR points somewhere else (handy for a throwaway test profile). */
export const DATA_DIR = process.env.EVECORD_DATA_DIR || join(app.getPath("appData"), "evecord");
mkdirSync(DATA_DIR, { recursive: true });
app.setPath("userData", DATA_DIR);

/** Chromium's profile: cookies, local storage, caches. Kept apart so DATA_DIR stays readable. */
export const SESSION_DIR = join(DATA_DIR, "session");
app.setPath("sessionData", SESSION_DIR);

export const SETTINGS_FILE = join(DATA_DIR, "evecord.json");
export const STATE_FILE = join(DATA_DIR, "state.json");

/** Vencord keeps settings/ and themes/ under this, same layout as ~/.config/Vencord. */
export const VENCORD_DATA_DIR = DATA_DIR;
process.env.VENCORD_USER_DATA_DIR = VENCORD_DATA_DIR;

/** Where official Vencord releases are downloaded to when there is no local checkout. */
export const DOWNLOADED_VENCORD_DIR = join(DATA_DIR, "vencord-dist");

/** Eve's Vencord dev checkout; its dist/ carries the userplugins. */
export const LOCAL_VENCORD_DIST = join(homedir(), "Projects", "Vencord", "dist");

/** The Discord desktop client's Vencord profile, offered as an import source on first launch. */
export const DISCORD_VENCORD_DIR = join(app.getPath("appData"), "Vencord");

/** Bundled js lives in dist/, static assets next to it. */
export const STATIC_DIR = join(__dirname, "..", "static");
export const VIEWS_DIR = join(STATIC_DIR, "views");
