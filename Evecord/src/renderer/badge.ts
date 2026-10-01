/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { filters, waitFor } from "@vencord/types/webpack";
import { RelationshipStore } from "@vencord/types/webpack/common";

import { Settings } from "./settings";

let readStates: any;
let notificationSettings: any;
let last: number | undefined;

/** Mentions + friend requests, -1 for "something unread" without a number, 0 for nothing. */
function count() {
    const mentions = readStates.getTotalMentionCount() + RelationshipStore.getPendingCount();
    if (mentions) return mentions;
    return readStates.hasAnyUnread() && !notificationSettings.getDisableUnreadBadge() ? -1 : 0;
}

function update() {
    if (!readStates || !notificationSettings) return;
    let n = 0;
    try {
        if (Settings.store.unreadBadge) n = count();
    } catch (err) {
        console.error("[Evecord] Unread count failed", err);
    }
    if (n === last) return;
    last = n;
    EvecordNative.app.setBadgeCount(n);
}

waitFor(filters.byStoreName("GuildReadStateStore"), s => {
    readStates = s;
    s.addChangeListener(update);
    update();
});
waitFor(filters.byStoreName("NotificationSettingsStore"), s => {
    notificationSettings = s;
    s.addChangeListener(update);
    update();
});
waitFor(filters.byStoreName("RelationshipStore"), s => s.addChangeListener(update));

Settings.onChange("unreadBadge", update);
