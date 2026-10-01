/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

export type RpcFromWorker =
    | { type: "activity"; data: string }
    | { type: "invite"; nonce: string; data: string }
    | { type: "link"; nonce: string; data: unknown };

export interface RpcToWorker {
    nonce: string;
    ok: boolean;
}
