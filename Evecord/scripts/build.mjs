/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bundles main, preloads and the Discord-side renderer with esbuild.
 *   node scripts/build.mjs [--dev] [--watch]
 */

import { context } from "esbuild";
import { copyFileSync, mkdirSync } from "fs";
import { dirname, join } from "path";
import { fileURLToPath } from "url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const dev = process.argv.includes("--dev");
const watch = process.argv.includes("--watch");

const alias = {
    "@shared": join(root, "src/shared"),
    "@main": join(root, "src/main"),
    "@preload": join(root, "src/preload"),
    "@renderer": join(root, "src/renderer")
};

const common = {
    absWorkingDir: root,
    bundle: true,
    minify: !dev,
    sourcemap: dev ? "inline" : false,
    logLevel: "info",
    alias,
    define: { IS_DEV: JSON.stringify(dev) }
};

const node = {
    ...common,
    platform: "node",
    format: "cjs",
    target: "node22",
    external: ["electron"]
};

/**
 * The renderer runs inside Discord next to Vencord and must use Vencord's own copies
 * of React, webpack helpers and components, not bundle new ones. Imports from
 * @vencord/types/... (which only exists for its type declarations) are turned into
 * reads of the matching window.Vencord global.
 */
const vencordGlobals = {
    name: "vencord-globals",
    setup(build) {
        build.onResolve({ filter: /^@vencord\/types(\/|$)/ }, args => ({ path: args.path, namespace: "vencord-global" }));
        build.onLoad({ filter: /.*/, namespace: "vencord-global" }, ({ path }) => {
            const sub = path.replace(/^@vencord\/types\/?/, "");
            let expr;
            if (sub === "webpack") expr = "Vencord.Webpack";
            else if (sub === "webpack/common") expr = "Vencord.Webpack.Common";
            else if (sub === "utils" || sub.startsWith("utils/")) expr = "Vencord.Util";
            else if (sub === "components" || sub.startsWith("components/")) expr = "Vencord.Components";
            else if (sub === "api") expr = "Vencord.Api";
            else if (sub.startsWith("api/")) expr = `Vencord.Api.${sub.split("/")[1]}`;
            else throw new Error(`Evecord: no Vencord global for ${path} (type-only imports should use "import type")`);
            return { contents: `module.exports = ${expr};`, loader: "js" };
        });
    }
};

const builds = [
    { ...node, entryPoints: ["src/main/index.ts"], outfile: "dist/main.js" },
    { ...node, entryPoints: ["src/main/arrpc/worker.ts"], outfile: "dist/arRpcWorker.js" },
    { ...node, entryPoints: ["src/preload/index.ts"], outfile: "dist/preload.js" },
    { ...node, entryPoints: ["src/preload/splash.ts"], outfile: "dist/splashPreload.js" },
    { ...node, entryPoints: ["src/preload/firstLaunch.ts"], outfile: "dist/firstLaunchPreload.js" },
    {
        ...common,
        entryPoints: ["src/renderer/index.ts"],
        outfile: "dist/renderer.js",
        format: "iife",
        globalName: "Evecord",
        target: "esnext",
        jsxFactory: "EvecordCreateElement",
        jsxFragment: "EvecordFragment",
        inject: [join(root, "scripts/reactShim.mjs")],
        plugins: [vencordGlobals],
        // Source shown in DevTools stack traces.
        footer: { js: "//# sourceURL=evecord://renderer.js" }
    }
];

/** venmic ships prebuilt N-API addons; the main process loads them from static/dist. */
function copyVenmic() {
    mkdirSync(join(root, "static/dist"), { recursive: true });
    for (const arch of ["x64", "arm64"]) {
        try {
            copyFileSync(
                join(root, `node_modules/@vencord/venmic/prebuilds/venmic-addon-linux-${arch}/node-napi-v7.node`),
                join(root, `static/dist/venmic-${arch}.node`)
            );
        } catch {
            if (arch === process.arch) console.warn(`venmic for ${arch} not found: screen share audio will be unavailable`);
        }
    }
}

copyVenmic();
const contexts = await Promise.all(builds.map(b => context(b)));
if (watch) {
    await Promise.all(contexts.map(c => c.watch()));
} else {
    await Promise.all(contexts.map(c => c.rebuild()));
    await Promise.all(contexts.map(c => c.dispose()));
}
