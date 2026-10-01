import assert from "node:assert/strict";
import { test } from "node:test";

import { desktopExecArg } from "../src/shared/desktopEntry.ts";

test("plain arguments pass through", () => {
    assert.equal(desktopExecArg("/home/eve/.local/lib/evecord/evecord"), "/home/eve/.local/lib/evecord/evecord");
    assert.equal(desktopExecArg("--start-minimized"), "--start-minimized");
});

test("spaces are quoted", () => {
    assert.equal(desktopExecArg("/home/eve/Linux-Apps I Made/x"), '"/home/eve/Linux-Apps I Made/x"');
});

test("quote, backtick, dollar and backslash are escaped, then backslashes doubled", () => {
    // desired shell-level string: a"b  ->  "a\"b"  ->  as a desktop string value: "a\\"b"
    assert.equal(desktopExecArg('a"b'), '"a\\\\"b"');
    assert.equal(desktopExecArg("$HOME"), '"\\\\$HOME"');
    assert.equal(desktopExecArg("a\\b"), '"a\\\\\\\\b"');
});

test("percent signs are doubled so they are not field codes", () => {
    assert.equal(desktopExecArg("100%"), "100%%");
});

test("an empty argument stays an argument", () => {
    assert.equal(desktopExecArg(""), '""');
});
