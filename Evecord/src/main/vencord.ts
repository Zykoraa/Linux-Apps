/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { app } from "electron";
import { existsSync, mkdirSync, readFileSync, renameSync, rmSync, watch, writeFileSync } from "fs";
import { join } from "path";
import type { VencordChange, VencordInfo, VencordSourceKind } from "@shared/ipc";

import { DOWNLOADED_VENCORD_DIR, LOCAL_VENCORD_DIST } from "./paths";
import { Settings, State } from "./settings";

/** The Vesktop flavour of a Vencord build. Vencord's build.mjs emits these next to patcher.js. */
export const VENCORD_FILES = [
    "vencordDesktopMain.js",
    "vencordDesktopPreload.js",
    "vencordDesktopRenderer.js",
    "vencordDesktopRenderer.css"
] as const;

const USER_AGENT = `Evecord/${app.getVersion()} (+https://github.com/Zykoraa/Linux-Apps)`;

export function isVencordDir(dir: string) {
    return VENCORD_FILES.every(f => existsSync(join(dir, f)));
}

interface VencordSource {
    kind: VencordSourceKind;
    dir: string;
}

/**
 * Pick where Vencord comes from, in order:
 *   1. a directory chosen in settings,
 *   2. the local dev checkout (~/Projects/Vencord/dist), so userplugins load,
 *   3. an official release downloaded into the data dir.
 * Returns null when none of them is usable yet.
 */
function findVencord(): VencordSource | null {
    const { vencordDir } = Settings.store;
    if (vencordDir) {
        if (isVencordDir(vencordDir)) return { kind: "custom", dir: vencordDir };
        console.warn(`[Evecord] Chosen Vencord dir ${vencordDir} has no Vencord build; falling back`);
    }
    if (isVencordDir(LOCAL_VENCORD_DIST)) return { kind: "local", dir: LOCAL_VENCORD_DIST };
    if (isVencordDir(DOWNLOADED_VENCORD_DIR)) return { kind: "download", dir: DOWNLOADED_VENCORD_DIR };
    return null;
}

let source: VencordSource | null = null;

/** Resolve (downloading if nothing is on disk) and pin the Vencord source for this run. */
export async function ensureVencord(onProgress?: (msg: string) => void): Promise<VencordSource> {
    source = findVencord();
    if (!source) {
        onProgress?.("Downloading Vencord…");
        await downloadVencord();
        source = { kind: "download", dir: DOWNLOADED_VENCORD_DIR };
    }
    console.log(`[Evecord] Loading Vencord (${source.kind}) from ${source.dir}`);
    return source;
}

export function vencordDir() {
    if (!source) throw new Error("Vencord has not been resolved yet");
    return source.dir;
}

export function readVencordFile(name: (typeof VENCORD_FILES)[number]) {
    return readFileSync(join(vencordDir(), name), "utf8");
}

export function getVencordInfo(): VencordInfo {
    return {
        kind: source?.kind ?? "download",
        dir: source?.dir ?? "",
        tag: source?.kind === "download" ? State.store.downloadedVencordTag : undefined,
        localCheckout: LOCAL_VENCORD_DIST,
        localCheckoutValid: isVencordDir(LOCAL_VENCORD_DIST)
    };
}

interface Release {
    tag_name: string;
    assets: { name: string; browser_download_url: string }[];
}

async function fetchOk(url: string, accept?: string) {
    const headers: Record<string, string> = { "User-Agent": USER_AGENT };
    if (accept) headers.Accept = accept;
    const res = await fetch(url, { headers });
    if (!res.ok) throw new Error(`${url}: HTTP ${res.status} ${res.statusText}`);
    return res;
}

/**
 * Fetch the latest official Vencord release into DOWNLOADED_VENCORD_DIR. Files land
 * in a staging dir first and replace the old set only once all of them arrived.
 */
export async function downloadVencord() {
    const release = (await (
        await fetchOk("https://api.github.com/repos/Vendicated/Vencord/releases/latest", "application/vnd.github+json")
    ).json()) as Release;

    const staging = `${DOWNLOADED_VENCORD_DIR}.part`;
    rmSync(staging, { recursive: true, force: true });
    mkdirSync(staging, { recursive: true });

    const wanted = release.assets.filter(a => VENCORD_FILES.some(f => a.name === f || a.name === `${f}.map`));
    await Promise.all(
        wanted.map(async a => {
            const res = await fetchOk(a.browser_download_url);
            writeFileSync(join(staging, a.name), Buffer.from(await res.arrayBuffer()));
        })
    );
    if (!isVencordDir(staging)) throw new Error(`Vencord release ${release.tag_name} is missing desktop files`);

    // Vencord's main script is require()d from here; an empty package.json keeps Node
    // from walking up and applying some unrelated package's "type": "module".
    writeFileSync(join(staging, "package.json"), "{}");

    rmSync(DOWNLOADED_VENCORD_DIR, { recursive: true, force: true });
    renameSync(staging, DOWNLOADED_VENCORD_DIR);
    State.store.downloadedVencordTag = release.tag_name;
    console.log(`[Evecord] Downloaded Vencord ${release.tag_name}`);
}

/**
 * Watch the Vencord files that are in use and report rebuilds. Vencord's build writes
 * its outputs one after another and not atomically, so changes are gathered for a
 * moment before anyone is told.
 */
export function watchVencord(onChange: (change: VencordChange) => void) {
    const dir = vencordDir();
    let timer: NodeJS.Timeout | undefined;
    let needsRestart = false;

    try {
        watch(dir, { persistent: false }, (_event, file) => {
            if (!file || !VENCORD_FILES.includes(file as any)) return;
            if (file === "vencordDesktopMain.js") needsRestart = true;

            clearTimeout(timer);
            timer = setTimeout(() => {
                if (!isVencordDir(dir)) return;
                onChange({ needsRestart });
                needsRestart = false;
            }, 1500);
        });
    } catch (err) {
        console.error(`[Evecord] Cannot watch ${dir}:`, err);
    }
}
