/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/** Quote one Exec= argument per the desktop entry spec. */
export function desktopExecArg(arg: string) {
    // % starts a field code (%u, %f...) anywhere in Exec, quoted or not.
    arg = arg.replace(/%/g, "%%");
    if (!/[\s"'\\`$<>~|&;*?#()]/.test(arg) && arg !== "") return arg;
    // Inside double quotes, ", `, $ and \ need a backslash; the spec then wants every
    // backslash doubled again because Exec values are themselves an escaped string.
    const quoted = arg.replace(/(["`$\\])/g, "\\$1");
    return `"${quoted.replace(/\\/g, "\\\\")}"`;
}
