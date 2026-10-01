import assert from "node:assert/strict";
import { test } from "node:test";

import { DEFAULT_SETTINGS, fillDefaults } from "../src/shared/settings.ts";

test("missing keys are filled, present ones kept, including false and nested ones", () => {
    const user: any = { tray: false, streamAudio: { ignoreVirtual: true } };
    fillDefaults(user, structuredClone(DEFAULT_SETTINGS));
    assert.equal(user.tray, false);
    assert.equal(user.closeToTray, true);
    assert.equal(user.streamAudio.ignoreVirtual, true);
    assert.equal(user.streamAudio.onlyDefaultSpeakers, true);
});

test("a nested object of the wrong type is replaced by the default object", () => {
    const user: any = { streamAudio: "garbage" };
    fillDefaults(user, structuredClone(DEFAULT_SETTINGS));
    assert.deepEqual(user.streamAudio, DEFAULT_SETTINGS.streamAudio);
});

test("optional settings without a default stay absent", () => {
    const user: any = {};
    fillDefaults(user, structuredClone(DEFAULT_SETTINGS));
    assert.ok(!("vencordDir" in user));
    assert.ok(!("splashColor" in user));
});

test("filling does not alias the defaults object", () => {
    const defaults = structuredClone(DEFAULT_SETTINGS);
    const user: any = {};
    fillDefaults(user, defaults);
    user.streamAudio.onlyDefaultSpeakers = false;
    assert.equal(defaults.streamAudio.onlyDefaultSpeakers, true);
});
