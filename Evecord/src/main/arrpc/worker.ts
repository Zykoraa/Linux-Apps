/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Runs arRPC (a reimplementation of Discord's local RPC server: the discord-ipc socket,
// the localhost websocket and game detection) off the main thread. Activity and
// invite requests are forwarded to the main process, which hands them to Discord.

import Server from "arrpc";
import { randomUUID } from "crypto";
import { type MessagePort, workerData } from "worker_threads";

import type { RpcFromWorker, RpcToWorker } from "./types";

const port: MessagePort = workerData.port;
const waiting = new Map<string, (ok: boolean) => void>();

function ask(type: "invite" | "link", data: unknown, reply: (ok: boolean) => void) {
    const nonce = randomUUID();
    waiting.set(nonce, reply);
    port.postMessage({ type, nonce, data } as RpcFromWorker);
}

port.on("message", (msg: RpcToWorker) => {
    waiting.get(msg.nonce)?.(msg.ok);
    waiting.delete(msg.nonce);
});

(async () => {
    const server = await new Server();
    server.on("activity", (data: unknown) => port.postMessage({ type: "activity", data: JSON.stringify(data) } satisfies RpcFromWorker));
    server.on("invite", (code: string, reply: (ok: boolean) => void) => ask("invite", code, reply));
    server.on("link", (data: unknown, reply: (ok: boolean) => void) => ask("link", data, reply));
})().catch(err => console.error("[Evecord] arRPC failed to start:", err));
