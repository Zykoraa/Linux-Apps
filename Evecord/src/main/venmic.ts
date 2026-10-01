/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import type { LinkData, PatchBay as TPatchBay } from "@vencord/venmic";
import { app } from "electron";
import { join } from "path";
import { type AudioNode, Ipc, type VirtmicList } from "@shared/ipc";

import { handle } from "./ipcHandle";
import { STATIC_DIR } from "./paths";
import { Settings } from "./settings";

/*
 * venmic creates a PipeWire virtual microphone, "vencord-screen-share", and links the
 * chosen apps' output into it. The renderer then picks that mic up with getUserMedia
 * and puts its track on the screen share stream. Loaded lazily: the addon talks to
 * PipeWire, and there is no reason to do that until someone opens the stream picker.
 */

let patchBay: TPatchBay | null | undefined;
let loadError = "";
let hasPipewirePulse = false;

function getPatchBay() {
    if (patchBay !== undefined) return patchBay;
    try {
        const { PatchBay } = require(join(STATIC_DIR, "dist", `venmic-${process.arch}.node`)) as typeof import("@vencord/venmic");
        hasPipewirePulse = PatchBay.hasPipeWire();
        patchBay = new PatchBay();
    } catch (err: any) {
        console.error("[Evecord] venmic unavailable:", err);
        loadError = String(err?.message ?? err);
        patchBay = null;
    }
    return patchBay;
}

/** Chromium's audio service process: its streams are Discord's own, never something to share. */
function ownAudioPid() {
    return String(app.getAppMetrics().find(p => p.name === "Audio Service")?.pid ?? "none");
}

function linkData(selection: { include?: AudioNode[]; exclude?: AudioNode[] }): LinkData {
    const opts = Settings.store.streamAudio;
    const pid = ownAudioPid();
    const pickedSources = !!selection.include?.length;

    const exclude = [...(selection.exclude ?? []), { "application.process.id": pid }];
    if (opts.ignoreInputs) exclude.push({ "media.class": "Stream/Input/Audio" });
    // A bus picked by name is wanted even if virtual nodes are hidden from "Entire System".
    if (opts.ignoreVirtual && !pickedSources) exclude.push({ "node.virtual": "true" });

    return {
        include: selection.include ?? [],
        exclude,
        mute: opts.initialMute,
        // venmic applies these to picked sources too, where they only get in the way:
        // an app playing into a virtual cable, or a bus (a source, linked to nothing),
        // would be refused although it was chosen by name. Keep them for "Entire System".
        only_speakers: pickedSources ? false : opts.onlySpeakers,
        only_default_speakers: pickedSources ? false : opts.onlyDefaultSpeakers,
        ignore_devices: pickedSources ? false : opts.ignoreDevices,
        workaround: opts.micWorkaround ? [{ "application.process.id": pid, "media.name": "RecordStream" }] : undefined
    };
}

handle(Ipc.VIRTMIC_LIST, (): VirtmicList => {
    const bay = getPatchBay();
    if (!bay) return { ok: false, reason: loadError };

    const pid = ownAudioPid();
    const notOwn = (n: AudioNode) => n["application.process.id"] !== pid && !n["node.name"]?.startsWith("vencord-");

    if (Settings.store.streamAudio.granularSelect) return { ok: true, targets: bay.list([]).filter(notOwn), hasPipewirePulse };

    // list(props) returns only nodes that have all of `props`, one per distinct
    // combination: so this is one entry per app's playback streams...
    const apps = bay
        .list(["application.name", "node.name"])
        .filter(n => n["media.class"] === "Stream/Output/Audio" && notOwn(n));
    // ...and sources (mixer buses such as BetterBanana's, microphones) have no
    // application.name to group by, so they are listed one by one.
    const sources = bay.list([]).filter(n => n["media.class"]?.startsWith("Audio/Source") && notOwn(n));
    return { ok: true, targets: [...apps, ...sources], hasPipewirePulse };
});

handle(Ipc.VIRTMIC_START, (_e, include: AudioNode[]) => getPatchBay()?.link(linkData({ include })) ?? false);
handle(Ipc.VIRTMIC_START_SYSTEM, (_e, exclude: AudioNode[]) => getPatchBay()?.link(linkData({ exclude })) ?? false);
handle(Ipc.VIRTMIC_UNMUTE, () => getPatchBay()?.unmute());
handle(Ipc.VIRTMIC_STOP, () => getPatchBay()?.unlink());
