/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { FormSwitch } from "@vencord/types/components";
import { Modal } from "@vencord/types/webpack/common";
import type { StreamAudioSettings } from "@shared/settings";

import { useSettings } from "../settings";

const OPTIONS: { key: keyof StreamAudioSettings; title: string; description: string; resets?: boolean }[] = [
    {
        key: "initialMute",
        title: "Start muted",
        description: "Hold the stream audio back until the stream is live. Avoids a burst of noise at the start."
    },
    {
        key: "micWorkaround",
        title: "Microphone workaround",
        description: "Only if viewers hear your microphone instead of the app you picked."
    },
    {
        key: "onlySpeakers",
        title: "Entire System: only apps playing to a hardware device",
        description: "Leave off when apps play into virtual sinks (BetterBanana's VAIO, cables); they would all be skipped."
    },
    {
        key: "onlyDefaultSpeakers",
        title: "Entire System: only apps playing to the default output",
        description: "Turn off to also take apps routed to other outputs. Picked apps and buses ignore both of these."
    },
    {
        key: "ignoreInputs",
        title: "Hide capture streams",
        description: "Leave out other apps' microphone streams."
    },
    {
        key: "ignoreVirtual",
        title: "Hide virtual buses",
        description: "Leave virtual sources (BetterBanana buses, loopbacks) out of the list and out of Entire System."
    },
    {
        key: "ignoreDevices",
        title: "Hide devices",
        description: "Leave out hardware such as microphones and speakers.",
        resets: true
    },
    {
        key: "granularSelect",
        title: "List each stream separately",
        description: "Pick a single stream of an app (by process, binary and media name) instead of the whole app.",
        resets: true
    },
    {
        key: "deviceSelect",
        title: "Offer devices as sources",
        description: "Lets you stream a device, e.g. a second microphone or an instrument input. Needs Hide devices off.",
        resets: true
    }
];

export function StreamAudioSettingsModal({
    modalProps,
    onSelectionInvalidated
}: {
    modalProps: any;
    /** The source list changes shape with some options, which would orphan the current pick. */
    onSelectionInvalidated(): void;
}) {
    const settings = useSettings();
    const audio = settings.streamAudio;

    return (
        <Modal
            {...modalProps}
            size="md"
            title="Stream audio options"
            actions={[{ text: "Done", variant: "primary", onClick: modalProps.onClose }]}
        >
            <div className="evecord-ssp-audio-options">
                {OPTIONS.map(o => (
                    <FormSwitch
                        key={o.key}
                        title={o.title}
                        description={o.description}
                        value={audio[o.key]}
                        disabled={o.key === "deviceSelect" && audio.ignoreDevices}
                        hideBorder
                        onChange={value => {
                            audio[o.key] = value;
                            if (o.key === "ignoreDevices" && value) audio.deviceSelect = false;
                            if (o.resets) onSelectionInvalidated();
                        }}
                    />
                ))}
            </div>
        </Modal>
    );
}
