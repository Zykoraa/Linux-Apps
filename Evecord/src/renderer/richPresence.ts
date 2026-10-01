/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { findLazy, onceReady } from "@vencord/types/webpack";
import { FluxDispatcher, InviteActions } from "@vencord/types/webpack/common";
import { Command } from "@shared/ipc";

import { onCommand } from "./commands";
import { Settings } from "./settings";

/*
 * arRPC runs in the main process; what it learns arrives here. Vencord's
 * "WebRichPresence (arRPC)" plugin already knows how to turn an arRPC message into
 * a Discord activity (looking up app names and asset URLs), so it does the work.
 * The plugin itself stays disabled: it would try to reach arRPC over a websocket.
 */

const arRpcPlugin = () => Vencord.Plugins.plugins["WebRichPresence (arRPC)"] as any;

onCommand(Command.RPC_ACTIVITY, async (json: string) => {
    if (!Settings.store.richPresence) return;
    await onceReady;
    await arRpcPlugin().handleEvent(new MessageEvent("message", { data: json }));
});

onCommand(Command.RPC_INVITE, async (code: string) => {
    const { invite } = await InviteActions.resolveInvite(code, "Desktop Modal");
    if (!invite) return false;
    EvecordNative.win.focus();
    FluxDispatcher.dispatch({ type: "INVITE_MODAL_OPEN", invite, code, context: "APP" });
    return true;
});

// Discord's handler for discord:// deep links that arrive over RPC (e.g. "join game").
const deepLinks = findLazy(m => m.DEEP_LINK?.handler);

onCommand(Command.RPC_DEEP_LINK, (data: unknown) => {
    try {
        deepLinks.DEEP_LINK.handler({ args: data });
        return true;
    } catch (err) {
        console.error("[Evecord] Deep link failed", err);
        return false;
    }
});
