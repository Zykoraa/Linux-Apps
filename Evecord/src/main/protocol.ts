/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { app, type BrowserWindow, net, protocol } from "electron";
import { relative, resolve } from "path";
import { pathToFileURL } from "url";

import { STATIC_DIR } from "./paths";

// Must happen before the app is ready.
protocol.registerSchemesAsPrivileged([
    {
        scheme: "evecord",
        privileges: { standard: true, secure: true, supportFetchAPI: true, corsEnabled: true, stream: true }
    }
]);

/** evecord://static/<path> serves files from static/, and nothing outside it. */
app.whenReady().then(() => {
    protocol.handle("evecord", req => {
        const url = new URL(req.url);
        if (url.hostname !== "static") return new Response(null, { status: 404 });

        const file = resolve(STATIC_DIR, "." + decodeURIComponent(url.pathname));
        const rel = relative(STATIC_DIR, file);
        if (rel.startsWith("..") || resolve(STATIC_DIR, rel) !== file) return new Response(null, { status: 404 });

        return net.fetch(pathToFileURL(file).href);
    });
});

export function loadView(win: BrowserWindow, view: string, params?: Record<string, string>) {
    const url = new URL(`evecord://static/views/${view}`);
    if (params) url.search = new URLSearchParams(params).toString();
    return win.loadURL(url.href);
}
