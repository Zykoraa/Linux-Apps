/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import "./settings.css";

import { BaseText, Button, Card, Divider, ErrorBoundary, FormSwitch, Paragraph } from "@vencord/types/components";
import { Select, Toasts, useState } from "@vencord/types/webpack/common";
import type { VencordInfo } from "@shared/ipc";
import type { DiscordBranch, Settings as TSettings, WebRTCIPHandlingPolicy } from "@shared/settings";
import type { ReactNode } from "react";

import { useSettings } from "../settings";

type BoolKey = { [K in keyof TSettings]-?: TSettings[K] extends boolean ? K : never }[keyof TSettings];

interface Toggle {
    key: BoolKey;
    title: string;
    description: string;
    restart?: boolean;
    disabled?(s: TSettings): boolean;
}

function toast(message: string, type: "success" | "failure" = "success") {
    Toasts.show({
        message,
        id: Toasts.genId(),
        type: type === "success" ? Toasts.Type.SUCCESS : Toasts.Type.FAILURE
    });
}

function Section({ title, children }: { title: string; children: ReactNode }) {
    return (
        <section className="evecord-settings-section">
            <BaseText size="lg" weight="semibold" tag="h3">
                {title}
            </BaseText>
            <div className="evecord-settings-items">{children}</div>
            <Divider className="evecord-settings-divider" />
        </section>
    );
}

function Toggles({ items }: { items: Toggle[] }) {
    const s = useSettings();
    return (
        <>
            {items.map(t => (
                <FormSwitch
                    key={t.key}
                    title={t.title}
                    description={t.restart ? `${t.description} Takes effect after a restart.` : t.description}
                    value={s[t.key]}
                    disabled={t.disabled?.(s)}
                    onChange={v => (s[t.key] = v)}
                    hideBorder
                />
            ))}
        </>
    );
}

function Choice<T extends string>({
    title,
    description,
    options,
    value,
    onChange
}: {
    title: string;
    description?: string;
    options: { value: T; label: string }[];
    value: T;
    onChange(v: T): void;
}) {
    return (
        <div className="evecord-settings-choice">
            <BaseText size="md" weight="medium">
                {title}
            </BaseText>
            {description && <Paragraph className="evecord-settings-muted">{description}</Paragraph>}
            <Select
                options={options}
                isSelected={v => v === value}
                select={onChange}
                serialize={String}
                closeOnSelect
            />
        </div>
    );
}

function describeVencord(info: VencordInfo) {
    switch (info.kind) {
        case "local":
            return "your Vencord checkout, with its userplugins";
        case "custom":
            return "a folder you chose";
        case "download":
            return `the official release${info.tag ? ` ${info.tag}` : ""}`;
    }
}

function VencordSource() {
    const settings = useSettings();
    const [info, setInfo] = useState(() => EvecordNative.vencord.info());
    const [busy, setBusy] = useState(false);
    // The source is picked at startup; a change here applies on the next launch.
    const pending = settings.vencordDir !== (info.kind === "custom" ? info.dir : undefined);

    return (
        <Card className="evecord-settings-card">
            <Paragraph>
                Vencord is loaded from <b>{describeVencord(info)}</b>:
            </Paragraph>
            <code className="evecord-settings-path">{info.dir}</code>
            {!settings.vencordDir && info.kind === "download" && (
                <Paragraph className="evecord-settings-muted">
                    No build was found in {info.localCheckout}. Run <code>pnpm build</code> there and restart to use your
                    checkout instead.
                </Paragraph>
            )}
            {pending && (
                <Paragraph className="evecord-settings-warn">
                    {settings.vencordDir ? `Will load from ${settings.vencordDir}` : "Will pick automatically"} after a
                    restart.
                </Paragraph>
            )}
            <div className="evecord-settings-buttons">
                <Button size="small" variant="secondary" onClick={() => EvecordNative.vencord.openDir(info.dir)}>
                    Open folder
                </Button>
                <Button
                    size="small"
                    variant="secondary"
                    onClick={async () => {
                        const res = await EvecordNative.vencord.chooseDir();
                        if (res === "invalid")
                            toast("That folder has no vencordDesktop*.js files. Pick a Vencord dist folder.", "failure");
                    }}
                >
                    Choose folder…
                </Button>
                {settings.vencordDir && (
                    <Button size="small" variant="secondary" onClick={() => EvecordNative.vencord.resetDir()}>
                        Pick automatically
                    </Button>
                )}
                <Button
                    size="small"
                    variant="secondary"
                    disabled={busy}
                    onClick={async () => {
                        setBusy(true);
                        try {
                            setInfo(await EvecordNative.vencord.download());
                            toast("Downloaded the latest Vencord release.");
                        } catch (err) {
                            toast(`Download failed: ${err}`, "failure");
                        } finally {
                            setBusy(false);
                        }
                    }}
                >
                    {busy ? "Downloading…" : "Download latest release"}
                </Button>
                {pending && (
                    <Button size="small" onClick={() => EvecordNative.app.relaunch()}>
                        Restart now
                    </Button>
                )}
            </div>
        </Card>
    );
}

function Autostart() {
    const [on, setOn] = useState(() => EvecordNative.autostart.isEnabled());
    return (
        <FormSwitch
            title="Start with your session"
            description="Adds Evecord to ~/.config/autostart."
            value={on}
            hideBorder
            onChange={async v => {
                await EvecordNative.autostart.set(v);
                setOn(v);
            }}
        />
    );
}

const BRANCHES: { value: DiscordBranch; label: string }[] = [
    { value: "stable", label: "Stable" },
    { value: "ptb", label: "PTB" },
    { value: "canary", label: "Canary" }
];

const IP_POLICIES: { value: WebRTCIPHandlingPolicy; label: string }[] = [
    { value: "default", label: "Default: all interfaces" },
    { value: "default_public_and_private_interfaces", label: "Public and private interfaces (try with Tailscale/VPNs)" },
    { value: "default_public_interface_only", label: "Public interface only" },
    { value: "disable_non_proxied_udp", label: "Only through a proxy (TCP), no direct UDP" }
];

function SettingsPage() {
    const s = useSettings();

    return (
        <div className="evecord-settings">
            <Section title="Vencord">
                <VencordSource />
                <Toggles
                    items={[
                        {
                            key: "vencordRebuildNotice",
                            title: "Offer to reload after a Vencord rebuild",
                            description:
                                "Watches the Vencord files in use. When they change, a banner offers a reload (or a restart, if Vencord's main process changed).",
                            restart: true
                        }
                    ]}
                />
            </Section>

            <Section title="Discord">
                <Choice
                    title="Branch"
                    description="Which Discord to load. Changing it restarts Evecord."
                    options={BRANCHES}
                    value={s.discordBranch}
                    onChange={v => {
                        s.discordBranch = v;
                        EvecordNative.app.relaunch();
                    }}
                />
            </Section>

            <Section title="Startup">
                <Autostart />
                <Toggles
                    items={[
                        {
                            key: "startMinimized",
                            title: "Start in the tray when started with the session",
                            description: "The autostart entry passes --start-minimized."
                        },
                        { key: "splashScreen", title: "Splash screen", description: "A small loading window while Discord starts." },
                        {
                            key: "splashTheming",
                            title: "Theme the splash screen",
                            description: "Paint the splash in your Discord theme's colours."
                        }
                    ]}
                />
            </Section>

            <Section title="Window and tray">
                <Toggles
                    items={[
                        { key: "tray", title: "Tray icon", description: "Show Evecord in the system tray." },
                        {
                            key: "closeToTray",
                            title: "Close to tray",
                            description: "Closing the window hides it; quit from the tray or with Ctrl+Q.",
                            disabled: s => !s.tray
                        },
                        {
                            key: "trayClickToggles",
                            title: "Clicking the tray icon hides a focused window",
                            description: "Otherwise a click only ever shows it."
                        },
                        {
                            key: "nativeTitleBar",
                            title: "Native title bar",
                            description: "Let the compositor decorate the window instead of Discord's own title bar.",
                            restart: true
                        },
                        {
                            key: "staticTitle",
                            title: "Fixed window title",
                            description: 'Keep the title "Evecord" instead of the current channel, for stable window rules.'
                        },
                        {
                            key: "disableMinSize",
                            title: "No minimum window size",
                            description: "Allow the window to shrink below 940×500."
                        }
                    ]}
                />
            </Section>

            <Section title="Notifications">
                <Toggles
                    items={[
                        {
                            key: "unreadBadge",
                            title: "Unread indicator",
                            description: "Tray icon (and launcher badge, where supported) show unread messages and mentions."
                        },
                        {
                            key: "taskbarFlash",
                            title: "Urgency hint on new messages",
                            description: "Marks the window urgent, which Hyprland and most bars highlight."
                        }
                    ]}
                />
            </Section>

            <Section title="Performance">
                <Toggles
                    items={[
                        { key: "hardwareAcceleration", title: "GPU acceleration", description: "Render with the GPU.", restart: true },
                        {
                            key: "hardwareVideoAcceleration",
                            title: "Hardware video decode/encode (VA-API)",
                            description:
                                "Can make streams cheaper, can also break them. On NVIDIA, VA-API only decodes.",
                            restart: true,
                            disabled: s => !s.hardwareAcceleration
                        },
                        { key: "smoothScrolling", title: "Smooth scrolling", description: "Animated scrolling.", restart: true }
                    ]}
                />
            </Section>

            <Section title="Integrations">
                <Toggles
                    items={[
                        {
                            key: "richPresence",
                            title: "Rich Presence",
                            description: "Run arRPC, so games and apps can show what you are doing."
                        },
                        {
                            key: "handleDiscordLinks",
                            title: "Open discord:// links in Evecord",
                            description: "Registers Evecord with xdg-mime. The Discord client takes them back if it does the same."
                        }
                    ]}
                />
                <Choice
                    title="WebRTC network interfaces"
                    description="If calls hang at “RTC Connecting” over Tailscale or a VPN, try the second option."
                    options={IP_POLICIES}
                    value={s.webRTCIPHandlingPolicy}
                    onChange={v => (s.webRTCIPHandlingPolicy = v)}
                />
            </Section>

            <section className="evecord-settings-section">
                <BaseText size="lg" weight="semibold" tag="h3">
                    Debugging
                </BaseText>
                <div className="evecord-settings-buttons">
                    <Button size="small" variant="secondary" onClick={() => EvecordNative.debug.openGpu()}>
                        chrome://gpu
                    </Button>
                    <Button size="small" variant="secondary" onClick={() => EvecordNative.debug.openWebrtcInternals()}>
                        chrome://webrtc-internals
                    </Button>
                    <Button size="small" variant="secondary" onClick={() => EvecordNative.app.relaunch()}>
                        Restart Evecord
                    </Button>
                </div>
                <Paragraph className="evecord-settings-muted">
                    Evecord {EvecordNative.app.getVersion()}. Ctrl+Shift+I opens DevTools, Ctrl+Shift+R restarts.
                </Paragraph>
            </section>
        </div>
    );
}

export default ErrorBoundary.wrap(SettingsPage, { message: "The Evecord settings page crashed." });
