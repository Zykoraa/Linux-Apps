// Injected into the renderer bundle: JSX compiles to these, which resolve to the React
// that Discord (via Vencord) already has loaded. Looked up on first use, because the
// bundle starts running before Discord's webpack modules exist.
export const EvecordFragment = Symbol.for("react.fragment");
export let EvecordCreateElement = (...args) =>
    (EvecordCreateElement = Vencord.Webpack.Common.React.createElement)(...args);
