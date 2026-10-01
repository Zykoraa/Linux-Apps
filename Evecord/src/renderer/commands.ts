/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { SettingsRouter } from "@vencord/types/webpack/common";
import { Command } from "@shared/ipc";

type Handler = (data: any) => unknown;
const handlers = new Map<string, Handler>();

/** Answer requests from the main process (see rendererCommand in src/main/commands.ts). */
export function onCommand(command: string, handler: Handler) {
    if (handlers.has(command)) throw new Error(`Command ${command} already has a handler`);
    handlers.set(command, handler);
}

EvecordNative.commands.onCommand(async ({ nonce, command, data }) => {
    const handler = handlers.get(command);
    if (!handler) return EvecordNative.commands.respond({ nonce, ok: false, data: `No handler for ${command}` });
    try {
        EvecordNative.commands.respond({ nonce, ok: true, data: await handler(data) });
    } catch (err) {
        EvecordNative.commands.respond({ nonce, ok: false, data: String(err) });
    }
});

onCommand(Command.OPEN_SETTINGS, () => SettingsRouter.openUserSettings("my_account_panel"));
onCommand(Command.LANGUAGES, () => [...navigator.languages]);
