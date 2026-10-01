/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { join } from "path";
import { Command } from "@shared/ipc";
import { MessageChannel, Worker } from "worker_threads";

import { rendererCommand } from "../commands";
import { SESSION_DIR } from "../paths";
import { Settings } from "../settings";
import type { RpcFromWorker, RpcToWorker } from "./types";

let worker: Worker | undefined;

const INVITE_CODE = /^[\w-]+$/;

export function startRichPresence() {
    if (worker || !Settings.store.richPresence) return;

    // arRPC caches Discord's list of detectable games here instead of next to its own source.
    process.env.ARRPC_DETECTABLE_CACHE_PATH = join(SESSION_DIR, "arrpc-detectable.json");

    const { port1, port2 } = new MessageChannel();
    worker = new Worker(join(__dirname, "arRpcWorker.js"), { workerData: { port: port2 }, transferList: [port2] });
    worker.on("error", err => console.error("[Evecord] arRPC worker crashed:", err));
    worker.on("exit", () => (worker = undefined));

    port1.on("message", async (msg: RpcFromWorker) => {
        if (msg.type === "activity") {
            rendererCommand(Command.RPC_ACTIVITY, msg.data).catch(() => {});
            return;
        }

        let ok = false;
        if (msg.type === "invite") {
            ok = INVITE_CODE.test(msg.data) && (await rendererCommand<boolean>(Command.RPC_INVITE, msg.data).catch(() => false));
        } else {
            ok = await rendererCommand<boolean>(Command.RPC_DEEP_LINK, msg.data).catch(() => false);
        }
        port1.postMessage({ nonce: msg.nonce, ok } satisfies RpcToWorker);
    });
}

function stopRichPresence() {
    worker?.terminate();
    worker = undefined;
}

Settings.onChange("richPresence", on => (on ? startRichPresence() : stopRichPresence()));
