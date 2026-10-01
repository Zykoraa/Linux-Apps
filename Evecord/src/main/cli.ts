/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { app } from "electron";
import { basename } from "path";
import { parseArgs } from "util";

// Under a bare `electron .` argv is [electron, ., ...]; installed it is [evecord, ...].
const argv = process.argv.slice(basename(process.argv[0]).startsWith("electron") ? 2 : 1);

const { values } = parseArgs({
    args: argv,
    strict: false,
    allowPositionals: true,
    options: {
        "start-minimized": { type: "boolean", short: "m" },
        help: { type: "boolean", short: "h" },
        version: { type: "boolean", short: "v" }
    }
});

export const CommandLine = {
    startMinimized: values["start-minimized"] === true,
    /** A discord:// link passed by the desktop (xdg-open), if any. */
    deepLink: findDeepLink(argv)
};

export function findDeepLink(args: readonly string[]) {
    return args.find(a => a.startsWith("discord://"));
}

if (values.version) {
    console.log(`Evecord ${app.getVersion()} (Electron ${process.versions.electron})`);
    process.exit(0);
}

if (values.help) {
    console.log(`Evecord ${app.getVersion()}: Discord with Vencord, for Linux

Usage: evecord [options] [discord://link]

  -m, --start-minimized   Start hidden in the tray
  -v, --version           Print the version and exit
  -h, --help              Show this help and exit

Chromium switches pass through, e.g. --enable-features=... or --ozone-platform=x11.
Set EVECORD_DATA_DIR to run with a separate profile.`);
    process.exit(0);
}
