/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { showNotice } from "@vencord/types/api/Notices";
import { onceReady } from "@vencord/types/webpack";

import { localStore, Settings } from "./settings";

// Clicking a notification should bring the window back from the tray.
const onclick = Object.getOwnPropertyDescriptor(Notification.prototype, "onclick")!;
Object.defineProperty(Notification.prototype, "onclick", {
    ...onclick,
    set(handler: ((this: Notification, ev: Event) => any) | null) {
        onclick.set!.call(
            this,
            handler &&
                function (this: Notification, ev: Event) {
                    EvecordNative.win.focus();
                    return handler.call(this, ev);
                }
        );
    }
});

// The "get the desktop app" banner, which this is.
localStore.setItem("hideNag", "true");

/**
 * Remember Discord's theme colours so the next splash screen matches. Read when the
 * page goes away, which is when the theme the user actually had is certain.
 */
function captureSplashColors() {
    if (!Settings.store.splashTheming) return;
    const probe = (variable: string) => {
        const el = document.createElement("span");
        el.style.cssText = `display:none;color:var(${variable})`;
        document.body.append(el);
        const color = getComputedStyle(el).color;
        el.remove();
        return color && color !== "rgba(0, 0, 0, 0)" ? toRgb(color) : undefined;
    };
    const fg = probe("--text-default");
    const bg = probe("--background-base-lowest");
    if (fg && fg !== Settings.store.splashColor) Settings.store.splashColor = fg;
    if (bg && bg !== Settings.store.splashBackground) Settings.store.splashBackground = bg;
}

/** Computed colours can come back as oklab()/color(); paint onto a canvas to get plain rgb. */
function toRgb(color: string) {
    if (color.startsWith("rgb")) return color;
    const ctx = document.createElement("canvas").getContext("2d");
    if (!ctx) return undefined;
    ctx.fillStyle = color;
    ctx.fillRect(0, 0, 1, 1);
    const [r, g, b] = ctx.getImageData(0, 0, 1, 1).data;
    return `rgb(${r}, ${g}, ${b})`;
}

window.addEventListener("beforeunload", captureSplashColors);
// Also once Discord has settled, so the very first splash after install is themed too.
onceReady.then(() => setTimeout(captureSplashColors, 5000));

/** A Vencord rebuild on disk (pnpm build in the checkout) is offered as a reload, or a restart if main changed. */
EvecordNative.vencord.onChanged(({ needsRestart }) => {
    if (!Settings.store.vencordRebuildNotice) return;
    if (needsRestart) {
        showNotice("Vencord was rebuilt, including its main process.", "Restart Evecord", () =>
            EvecordNative.app.relaunch()
        );
    } else {
        showNotice("Vencord was rebuilt.", "Reload", () => location.reload());
    }
});
