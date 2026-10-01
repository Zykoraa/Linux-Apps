import assert from "node:assert/strict";
import { test } from "node:test";

import { Store } from "../src/shared/store.ts";

test("path listeners fire for nested writes with the new value", () => {
    const s = new Store<any>({ a: { b: 1 }, c: 2 });
    const seen: unknown[] = [];
    s.onChange("a.b", v => seen.push(v));
    s.store.a.b = 5;
    s.store.c = 3; // a different path
    assert.deepEqual(seen, [5]);
    assert.equal(s.data.a.b, 5);
});

test("any-listeners get the root object and the changed path", () => {
    const s = new Store<any>({ a: { b: 1 } });
    const paths: string[] = [];
    s.onAnyChange((data, path) => {
        assert.equal(data, s.data);
        paths.push(path);
    });
    s.store.a.b = 2;
    s.store.x = true;
    assert.deepEqual(paths, ["a.b", "x"]);
});

test("writing the same value is not a change", () => {
    const s = new Store<any>({ a: 1 });
    let calls = 0;
    s.onAnyChange(() => calls++);
    s.store.a = 1;
    assert.equal(calls, 0);
});

test("deleting a key notifies with undefined; deleting a missing key does nothing", () => {
    const s = new Store<any>({ a: 1 });
    const seen: unknown[] = [];
    s.onChange("a", v => seen.push(v));
    delete s.store.a;
    delete s.store.missing;
    assert.deepEqual(seen, [undefined]);
    assert.ok(!("a" in s.data));
});

test("arrays are values: assigning a new one notifies, in-place pushes do not", () => {
    const s = new Store<any>({ langs: ["en"] });
    let calls = 0;
    s.onChange("langs", () => calls++);
    s.store.langs.push("de");
    s.store.langs = ["fr"];
    assert.equal(calls, 1);
});

test("replace swaps the data, tells any-listeners with an empty path, skips path listeners", () => {
    const s = new Store<any>({ a: 1 });
    const any: string[] = [];
    let pathCalls = 0;
    s.onAnyChange((_d, p) => any.push(p));
    s.onChange("a", () => pathCalls++);
    s.replace({ a: 2 });
    assert.deepEqual(any, [""]);
    assert.equal(pathCalls, 0);
    assert.equal(s.store.a, 2);
});

test("unsubscribe stops further calls", () => {
    const s = new Store<any>({ a: 1 });
    let calls = 0;
    const off = s.onChange("a", () => calls++);
    s.store.a = 2;
    off();
    s.store.a = 3;
    assert.equal(calls, 1);
});
