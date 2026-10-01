/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { onceReady } from "@vencord/types/webpack";
import { FluxDispatcher, UserStore } from "@vencord/types/webpack/common";
import type { StreamPick } from "@shared/ipc";

import { getStreamQuality } from "../settings";
import { addPatch } from "./addPatch";
import { setVirtmicDeviceId, VIRTMIC_LABEL } from "./media";

/** What the picker returned for the stream being started. */
let currentPick: StreamPick | null = null;
export const setCurrentPick = (pick: StreamPick | null) => (currentPick = pick);

function qualityNumbers() {
    const q = getStreamQuality();
    const height = Number(q.resolution);
    return { height, width: Math.round((height * 16) / 9), frameRate: Number(q.frameRate) };
}

// Discord picks Go Live capture/encode sizes from its own defaults (720p30 on web).
// Replace them with what was chosen in the picker.
addPatch(
    [
        {
            find: "this.getDefaultGoliveQuality()",
            replacement: { match: /this\.getDefaultGoliveQuality\(\)/, replace: "$self.goLiveQuality($&)" }
        }
    ],
    {
        goLiveQuality(opts: any) {
            const { width, height, frameRate } = qualityNumbers();
            const size = { framerate: frameRate, width, height, pixelCount: width * height };
            Object.assign(opts, { bitrateMin: 500_000, bitrateMax: 8_000_000, bitrateTarget: 600_000 });
            if (opts.encode) Object.assign(opts.encode, size);
            if (opts.capture) Object.assign(opts.capture, size);
            return opts;
        }
    }
);

async function findVirtmic() {
    try {
        const devices = await navigator.mediaDevices.enumerateDevices();
        return devices.find(d => d.kind === "audioinput" && d.label === VIRTMIC_LABEL)?.deviceId;
    } catch {
        return undefined;
    }
}

/** Ask for the size and rate that were picked; Chromium otherwise hands over the portal's native size. */
export function applyQuality(track: MediaStreamTrack) {
    const { width, height, frameRate } = qualityNumbers();
    track.contentHint = currentPick?.contentHint ?? "motion";
    track
        .applyConstraints({
            ...track.getConstraints(),
            frameRate: { min: frameRate, ideal: frameRate },
            width: { min: 640, ideal: width, max: width },
            height: { min: 480, ideal: height, max: height },
            advanced: [{ width, height }],
            resizeMode: "none"
        } as MediaTrackConstraints)
        .catch(err => console.error("[Evecord] Could not apply stream quality", err));
}

const getDisplayMedia = navigator.mediaDevices.getDisplayMedia;
navigator.mediaDevices.getDisplayMedia = async function (options) {
    const stream = await getDisplayMedia.call(this, options);

    const video = stream.getVideoTracks()[0];
    if (video) applyQuality(video);

    // venmic was started by the picker if any audio source was chosen; its node is a
    // capture device now, so pick it up like a microphone and put it on the stream.
    const wantsAudio = currentPick && currentPick.includeSources !== "None";
    const deviceId = wantsAudio ? await findVirtmic() : undefined;
    setVirtmicDeviceId(deviceId);
    if (deviceId) {
        const audio = await navigator.mediaDevices.getUserMedia({
            audio: {
                deviceId: { exact: deviceId },
                // It is program audio, not a voice: no voice processing, keep it stereo.
                autoGainControl: false,
                echoCancellation: false,
                noiseSuppression: false,
                channelCount: 2,
                sampleRate: 48000,
                sampleSize: 16
            }
        });
        for (const t of stream.getAudioTracks()) stream.removeTrack(t);
        stream.addTrack(audio.getAudioTracks()[0]);
    }

    return stream;
};

// Tear venmic down with our own stream, and lift its start-up mute once the stream is live.
onceReady.then(() => {
    const isMine = (streamKey: string) => streamKey.split(":").at(-1) === UserStore.getCurrentUser()?.id;

    FluxDispatcher.subscribe("STREAM_CLOSE", ({ streamKey }: { streamKey: string }) => {
        if (!isMine(streamKey)) return;
        EvecordNative.virtmic.stop();
        setVirtmicDeviceId(undefined);
        currentPick = null;
    });
    FluxDispatcher.subscribe("STREAM_UPDATE", ({ streamKey }: { streamKey: string }) => {
        if (isMine(streamKey)) EvecordNative.virtmic.unmute();
    });
});
