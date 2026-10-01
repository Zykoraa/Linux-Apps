/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import type { Patch } from "@vencord/types/utils/types";

window.EvecordPatchGlobals = {};

/**
 * Register webpack patches with Vencord under the name "Evecord". Functions passed
 * alongside the patches are reachable from replacement code as `$self.name(...)`.
 * Vencord logs a "had no effect" warning for any patch whose find/match no longer
 * matches Discord's code, which is the first place to look after a Discord update.
 */
export function addPatch(patches: Omit<Patch, "plugin">[], self: Record<string, unknown> = {}) {
    for (const patch of patches) Vencord.Plugins.addPatch(patch, "Evecord", "EvecordPatchGlobals");
    Object.assign(EvecordPatchGlobals, self);
}
