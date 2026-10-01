/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { app } from "electron";

import { Settings } from "./settings";

export let hardwareAcceleration = true;

/** Set Chromium switches from settings. Must run before the app is ready. */
export function applyChromiumFlags() {
    const { hardwareAcceleration: hwAccel, hardwareVideoAcceleration, smoothScrolling } = Settings.store;
    const sw = app.commandLine;

    // Merge with whatever was passed on the command line instead of overwriting it.
    const split = (name: string) => new Set(sw.getSwitchValue(name).split(",").filter(Boolean));
    const enable = split("enable-features");
    const disable = split("disable-features");

    if (!hwAccel || sw.hasSwitch("disable-gpu")) {
        hardwareAcceleration = false;
        app.disableHardwareAcceleration();
    } else if (hardwareVideoAcceleration) {
        enable.add("AcceleratedVideoEncoder");
        enable.add("AcceleratedVideoDecoder");
        enable.add("AcceleratedVideoDecodeLinuxGL");
        enable.add("AcceleratedVideoDecodeLinuxZeroCopyGL");
    }

    if (!smoothScrolling) sw.appendSwitch("disable-smooth-scrolling");

    // Notification and call sounds must play without a click first.
    sw.appendSwitch("autoplay-policy", "no-user-gesture-required");

    // Do not register Discord as an MPRIS player or grab the media keys: those belong
    // to the actual music player.
    disable.add("HardwareMediaKeyHandling");
    disable.add("MediaSessionService");

    // Colours wash out when the window crosses between HDR and SDR outputs with this on.
    disable.add("WaylandWpColorManagerV1");

    for (const f of disable) enable.delete(f);
    sw.removeSwitch("enable-features");
    sw.removeSwitch("disable-features");
    if (enable.size) sw.appendSwitch("enable-features", [...enable].join(","));
    if (disable.size) sw.appendSwitch("disable-features", [...disable].join(","));
}
