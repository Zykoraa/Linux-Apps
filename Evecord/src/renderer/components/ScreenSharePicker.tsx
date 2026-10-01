/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import "./screenSharePicker.css";

import { Button, Card, Heading, HeadingTertiary, Paragraph, Span } from "@vencord/types/components";
import { useAwaiter } from "@vencord/types/utils";
import {
    closeModal,
    MediaEngineStore,
    Modal,
    openModal,
    Select,
    UserStore,
    useState
} from "@vencord/types/webpack/common";
import type { AudioNode, CaptureSource, StreamPick } from "@shared/ipc";
import type { StreamAudioSettings } from "@shared/settings";

import { applyQuality, setCurrentPick } from "../patches/screenShare";
import { getStreamQuality, setStreamQuality, useSettings } from "../settings";
import { StreamAudioSettingsModal } from "./StreamAudioSettings";

const RESOLUTIONS = ["480", "720", "1080", "1440", "2160"] as const;
const FRAME_RATES = ["15", "30", "60"] as const;

type Special = "None" | "Entire System";
type Selection = Special | AudioNode[];

interface Choice {
    label: string;
    value: Special | AudioNode;
}

/**
 * Show the picker and resolve with what to stream, or reject if it was closed.
 * `alreadyPicked`: the Wayland portal already chose the source, so skip the grid.
 */
export function openScreenSharePicker(sources: CaptureSource[], alreadyPicked: boolean) {
    return new Promise<StreamPick>((resolve, reject) => {
        let settled = false;
        const settle = (pick: StreamPick | null) => {
            if (settled) return;
            settled = true;
            if (pick) resolve(pick);
            else reject(new Error("cancelled"));
        };

        const key = openModal(
            props => (
                <PickerModal
                    modalProps={props}
                    sources={sources}
                    alreadyPicked={alreadyPicked}
                    onDone={async pick => {
                        // Start venmic before answering, so its node exists by the time
                        // getDisplayMedia() resolves and looks for it. A failure costs the
                        // audio, not the stream.
                        try {
                            if (pick.includeSources === "Entire System") {
                                await EvecordNative.virtmic.startSystem(
                                    pick.excludeSources === "None" ? [] : pick.excludeSources
                                );
                            } else if (pick.includeSources !== "None") {
                                await EvecordNative.virtmic.start(pick.includeSources);
                            }
                        } catch (err) {
                            console.error("[Evecord] Stream audio failed to start", err);
                            pick = { ...pick, includeSources: "None" };
                        }
                        setCurrentPick(pick);
                        settle(pick);
                        closeModal(key);
                    }}
                    onCancel={() => {
                        settle(null);
                        closeModal(key);
                    }}
                />
            ),
            { onCloseCallback: () => settle(null) }
        );
    });
}

function PickerModal({
    modalProps,
    sources,
    alreadyPicked,
    onDone,
    onCancel
}: {
    modalProps: any;
    sources: CaptureSource[];
    alreadyPicked: boolean;
    onDone(pick: StreamPick): void;
    onCancel(): void;
}) {
    const [sourceId, setSourceId] = useState<string | undefined>(alreadyPicked ? sources[0]?.id : undefined);
    const [quality, setQuality] = useState(getStreamQuality);
    const [contentHint, setContentHint] = useState<StreamPick["contentHint"]>("motion");
    const [include, setInclude] = useState<Selection>("None");
    const [exclude, setExclude] = useState<Selection>("None");

    const source = sources.find(s => s.id === sourceId);
    const canGoBack = !!source && !alreadyPicked;

    function goLive() {
        if (!source) return;
        setStreamQuality(quality);
        tuneOwnStreamConnection();
        onDone({
            id: source.id,
            contentHint,
            includeSources: include,
            excludeSources: include === "Entire System" && Array.isArray(exclude) ? exclude : "None"
        });
    }

    return (
        <Modal
            {...modalProps}
            size="lg"
            title="Share your screen"
            actions={[
                {
                    text: canGoBack ? "Back" : "Cancel",
                    variant: "secondary",
                    onClick: () => (canGoBack ? setSourceId(undefined) : onCancel())
                },
                { text: "Go Live", variant: "primary", disabled: !source, onClick: goLive }
            ]}
        >
            {!source ? (
                <SourceGrid sources={sources} onPick={setSourceId} />
            ) : (
                <div className="evecord-ssp-settings">
                    <Preview source={source} alreadyPicked={alreadyPicked} />

                    <HeadingTertiary>Stream settings</HeadingTertiary>
                    <Card className="evecord-ssp-card">
                        <div className="evecord-ssp-row">
                            <section>
                                <Heading tag="h5">Resolution</Heading>
                                <Segmented
                                    options={RESOLUTIONS.map(r => ({ value: r, label: `${r}p` }))}
                                    value={quality.resolution}
                                    onChange={resolution => setQuality(q => ({ ...q, resolution }))}
                                />
                            </section>
                            <section>
                                <Heading tag="h5">Frame rate</Heading>
                                <Segmented
                                    options={FRAME_RATES.map(f => ({ value: f, label: `${f} fps` }))}
                                    value={quality.frameRate}
                                    onChange={frameRate => setQuality(q => ({ ...q, frameRate }))}
                                />
                            </section>
                        </div>
                        <section>
                            <Heading tag="h5">Optimise for</Heading>
                            <Segmented<StreamPick["contentHint"]>
                                options={[
                                    { value: "motion", label: "Smoothness" },
                                    { value: "detail", label: "Clarity" }
                                ]}
                                value={contentHint}
                                onChange={setContentHint}
                            />
                            <Paragraph className="evecord-ssp-hint">
                                Clarity keeps text sharp but drops frames when a lot moves. Pick it for code or documents,
                                Smoothness for games and video.
                            </Paragraph>
                        </section>

                        <AudioSources include={include} exclude={exclude} setInclude={setInclude} setExclude={setExclude} />
                    </Card>
                </div>
            )}
        </Modal>
    );
}

/**
 * Discord sets the stream connection's encoder limits from its own quality presets
 * (720p30 on web) before the stream starts. Raise them to the picked size, and once the
 * track exists, ask the capturer for it too.
 */
function tuneOwnStreamConnection() {
    const { resolution, frameRate } = getStreamQuality();
    const height = Number(resolution);
    const width = Math.round((height * 16) / 9);
    const ownConnection = () =>
        [...MediaEngineStore.getMediaEngine().connections].find(
            (c: any) => c.streamUserId === UserStore.getCurrentUser().id
        ) as any;

    try {
        const params = ownConnection()?.videoStreamParameters?.[0];
        if (params) {
            params.maxFrameRate = Number(frameRate);
            params.maxResolution = { ...(params.maxResolution ?? {}), width, height };
        }
    } catch (err) {
        console.error("[Evecord] Could not raise stream limits", err);
    }

    setTimeout(() => {
        try {
            const track = ownConnection()?.input?.stream?.getVideoTracks()[0];
            if (track) applyQuality(track);
        } catch (err) {
            console.error("[Evecord] Could not apply stream quality", err);
        }
    }, 100);
}

function SourceGrid({ sources, onPick }: { sources: CaptureSource[]; onPick(id: string): void }) {
    return (
        <div className="evecord-ssp-grid">
            {sources.map(s => (
                <button key={s.id} className="evecord-ssp-source" onClick={() => onPick(s.id)}>
                    <img src={s.url} alt="" />
                    <Span weight="semibold" className="evecord-ssp-source-name">
                        {s.name}
                    </Span>
                </button>
            ))}
        </div>
    );
}

function Preview({ source, alreadyPicked }: { source: CaptureSource; alreadyPicked: boolean }) {
    const [url] = useAwaiter(
        async () => (alreadyPicked ? source.url : ((await EvecordNative.capturer.getLargeThumbnail(source.id)) ?? source.url)),
        { fallbackValue: source.url, deps: [source.id] }
    );
    return (
        <Card className="evecord-ssp-card evecord-ssp-preview">
            <img src={url} alt="" />
            <Paragraph>{source.name}</Paragraph>
        </Card>
    );
}

function Segmented<T extends string>({
    options,
    value,
    onChange
}: {
    options: { value: T; label: string }[];
    value: T;
    onChange(value: T): void;
}) {
    return (
        <div className="evecord-ssp-segmented" role="radiogroup">
            {options.map(o => (
                <button
                    key={o.value}
                    role="radio"
                    aria-checked={o.value === value}
                    data-checked={o.value === value}
                    onClick={() => onChange(o.value)}
                >
                    {o.label}
                </button>
            ))}
        </div>
    );
}

/** One picker entry per app (or per stream with granular selection on), and one per source. */
function toChoices(node: AudioNode, opts: StreamAudioSettings): Choice[] {
    const mediaClass = node["media.class"] ?? "";
    if (mediaClass.includes("Video") || mediaClass.includes("Midi")) return [];
    if (node["device.id"] && !opts.deviceSelect) return [];

    // Sources (mixer buses, microphones) are picked by their unique node name. Going by
    // application.name would lump BetterBanana's B1, B2 and A-outputs into one entry.
    if (mediaClass.startsWith("Audio/Source")) {
        if (node["node.virtual"] === "true" && opts.ignoreVirtual) return [];
        const name = node["node.name"];
        if (!name) return [];
        const label = node["node.description"] || node["node.nick"] || name;
        return [{ label: node["device.id"] ? `${label} (device)` : `${label} (bus)`, value: { "node.name": name } }];
    }

    const key = (["application.name", "node.description", "node.name", "application.process.binary"] as const).find(
        k => node[k]
    );
    if (!key) return [];

    const name = node[key];
    const choices: Choice[] = [{ label: name, value: { [key]: name } }];
    if (!opts.granularSelect) return choices;

    // A second, narrower entry that matches just this stream.
    const value: AudioNode = { [key]: name };
    let label = name;
    for (const [prop, open, close] of [
        ["application.process.id", "<", ">"],
        ["application.process.binary", "(", ")"],
        ["media.name", "[", "]"]
    ] as const) {
        if (!node[prop]) continue;
        value[prop] = node[prop];
        label += ` ${open}${node[prop]}${close}`;
    }
    choices.push({ label, value });
    return choices;
}

const sameNode = (a: AudioNode, b: AudioNode) => {
    const keys = Object.keys(a);
    return keys.length === Object.keys(b).length && keys.every(k => a[k] === b[k]);
};

function isSelected(selection: Selection, value: Special | AudioNode) {
    if (typeof selection === "string" || typeof value === "string") return selection === value;
    return selection.some(n => sameNode(n, value));
}

/** Specials replace the selection; apps toggle in and out of it. */
function toggle(selection: Selection, value: Special | AudioNode): Selection {
    if (typeof value === "string") return value;
    if (typeof selection === "string") return [value];
    if (selection.some(n => sameNode(n, value))) {
        const rest = selection.filter(n => !sameNode(n, value));
        return rest.length ? rest : "None";
    }
    return [...selection, value];
}

function AudioSources({
    include,
    exclude,
    setInclude,
    setExclude
}: {
    include: Selection;
    exclude: Selection;
    setInclude(s: Selection): void;
    setExclude(s: Selection): void;
}) {
    const settings = useSettings();
    const opts = settings.streamAudio;
    const [refresh, setRefresh] = useState(0);
    const [list, , loading] = useAwaiter(() => EvecordNative.virtmic.list(), {
        fallbackValue: null,
        deps: [refresh, opts.granularSelect, opts.deviceSelect]
    });

    if (list && !list.ok) {
        return (
            <Paragraph className="evecord-ssp-audio-error">
                Stream audio is unavailable: venmic could not be loaded ({list.reason}).
            </Paragraph>
        );
    }

    const seen = new Set<string>();
    const choices = [
        { label: "None", value: "None" as const },
        { label: "Entire System", value: "Entire System" as const },
        ...(list?.targets ?? []).flatMap(n => toChoices(n, opts))
    ].filter(c => !seen.has(c.label) && seen.add(c.label));

    const selectFor = (selection: Selection, set: (s: Selection) => void, items: Choice[]) => (
        <Select
            options={items.map(c => ({ label: c.label, value: c.value, default: c.label === "None" }))}
            isSelected={v => isSelected(selection, v)}
            select={v => set(toggle(selection, v))}
            serialize={v => JSON.stringify(v)}
            popoutPosition="top"
            closeOnSelect={false}
        />
    );

    return (
        <>
            <div className="evecord-ssp-row">
                <section>
                    <Heading tag="h5">{loading ? "Loading audio sources…" : "Audio"}</Heading>
                    {selectFor(include, setInclude, choices)}
                </section>
                {include === "Entire System" && (
                    <section>
                        <Heading tag="h5">Except</Heading>
                        {selectFor(
                            exclude,
                            setExclude,
                            choices.filter(c => c.value !== "Entire System")
                        )}
                    </section>
                )}
            </div>
            {list?.ok && !list.hasPipewirePulse && (
                <Paragraph className="evecord-ssp-hint">
                    pipewire-pulse is not running, so only apps that talk to PipeWire directly can be shared.
                </Paragraph>
            )}
            <div className="evecord-ssp-buttons">
                <Button variant="secondary" size="small" onClick={() => setRefresh(n => n + 1)}>
                    Refresh sources
                </Button>
                <Button
                    variant="secondary"
                    size="small"
                    onClick={() =>
                        openModal(props => (
                            <StreamAudioSettingsModal
                                modalProps={props}
                                onSelectionInvalidated={() => {
                                    setInclude("None");
                                    setExclude("None");
                                }}
                            />
                        ))
                    }
                >
                    Audio options
                </Button>
            </div>
        </>
    );
}
