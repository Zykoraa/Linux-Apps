/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { MediaEngineStore } from "@vencord/types/webpack/common";

import { addPatch } from "./addPatch";

/** The PipeWire node venmic creates for screen share audio. */
export const VIRTMIC_LABEL = "vencord-screen-share";

/*
 * Constraint fixes for Chromium's media stack:
 *  - Discord web leaves automatic gain control at Chromium's default (on), ignoring the
 *    "Automatic Gain Control" toggle in Voice settings. Apply the toggle.
 *  - A plain deviceId string is only a preference, so Chromium may hand back the default
 *    camera instead of the chosen one. Make it exact.
 */

function fixAudio(c: MediaTrackConstraints) {
    const target = c.advanced?.find(o => Object.hasOwn(o, "autoGainControl")) ?? c;
    target.autoGainControl = MediaEngineStore.getAutomaticGainControl();
}

function fixVideo(c: MediaTrackConstraints) {
    if (typeof c.deviceId === "string" && c.deviceId !== "default") c.deviceId = { exact: c.deviceId };
}

const getUserMedia = navigator.mediaDevices.getUserMedia;
navigator.mediaDevices.getUserMedia = function (constraints) {
    try {
        if (constraints?.audio) {
            if (typeof constraints.audio !== "object") constraints.audio = {};
            // Our own virtual mic request (screen share audio) is left exactly as asked.
            if (!isVirtmicRequest(constraints.audio)) fixAudio(constraints.audio);
        }
        if (constraints?.video) {
            if (typeof constraints.video !== "object") constraints.video = {};
            fixVideo(constraints.video);
        }
    } catch (err) {
        console.error("[Evecord] getUserMedia constraint fix failed", err);
    }
    return getUserMedia.call(this, constraints);
};

const applyConstraints = MediaStreamTrack.prototype.applyConstraints;
MediaStreamTrack.prototype.applyConstraints = function (constraints) {
    try {
        if (constraints && this.kind === "audio" && this.label !== VIRTMIC_LABEL) fixAudio(constraints);
        if (constraints && this.kind === "video") fixVideo(constraints);
    } catch (err) {
        console.error("[Evecord] applyConstraints fix failed", err);
    }
    return applyConstraints.call(this, constraints);
};

let virtmicDeviceId: string | undefined;
export const setVirtmicDeviceId = (id: string | undefined) => (virtmicDeviceId = id);

function isVirtmicRequest(audio: MediaTrackConstraints) {
    const id = audio.deviceId;
    const exact = typeof id === "object" && !Array.isArray(id) ? id.exact : id;
    return !!virtmicDeviceId && exact === virtmicDeviceId;
}

// Keep the virtual mic out of Discord's device lists, and stop Discord offering to
// "switch to the new device" every time a stream starts and the node appears.
addPatch(
    [
        {
            find: 'setSinkId"in',
            replacement: {
                match: /navigator\.mediaDevices\.enumerateDevices\(\)/,
                replace: "$self.enumerateDevicesWithoutVirtmic()"
            }
        },
        {
            find: "lastOutputSystemDevice.justChanged",
            replacement: {
                match: /\.getState\(\)\.neverShowModal(?=.{0,50}?(\i)\.lastDeviceConnected)/,
                replace: "$& || $self.isVirtmicConnect($1)"
            }
        }
    ],
    {
        async enumerateDevicesWithoutVirtmic() {
            return (await navigator.mediaDevices.enumerateDevices()).filter(d => d.label !== VIRTMIC_LABEL);
        },
        isVirtmicConnect(state: any) {
            return Object.keys(state.lastDeviceConnected ?? {})[0] === VIRTMIC_LABEL;
        }
    }
);
