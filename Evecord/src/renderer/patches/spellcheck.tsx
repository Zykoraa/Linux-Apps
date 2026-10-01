/*
 * Evecord, Eve's own Discord desktop app
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import { addContextMenuPatch } from "@vencord/types/api/ContextMenu";
import { FluxDispatcher, Menu, SpellCheckStore, useMemo, useStateFromStores } from "@vencord/types/webpack/common";

import { useSettings } from "../settings";
import { addPatch } from "./addPatch";

/*
 * Chromium finds the misspelling; Electron reports it with the context-menu event in
 * main, which forwards it here. Discord's text box menu is then opened only after that
 * report arrived, so it can list the suggestions.
 */

let misspelled = "";
let suggestions: string[] = [];

addPatch(
    [
        {
            find: ".enableSpellCheck",
            replacement: {
                // Desktop: wait for DiscordNative's spellcheck callback, then open the menu. Web: open it now.
                match: /else (\i)\.preventDefault\(\),(\i\(\i\))(?<=(\i)\??\.enableSpellCheck.+?)/,
                replace: "else $self.openWithSpellcheck($1, $3?.enableSpellCheck, () => $2)"
            }
        }
    ],
    {
        openWithSpellcheck(e: MouseEvent, enabled: boolean | undefined, openMenu: () => void) {
            if (!enabled) {
                e.preventDefault();
                openMenu();
                return;
            }
            const once = (word: string, list: string[]) => {
                EvecordNative.spellcheck.offSpellcheckResult(once);
                misspelled = word;
                suggestions = list;
                openMenu();
            };
            EvecordNative.spellcheck.onSpellcheckResult(once);
        }
    }
);

addContextMenuPatch("textarea-context", children => {
    const enabled = useStateFromStores([SpellCheckStore], () => SpellCheckStore.isEnabled());
    const available = useMemo(() => EvecordNative.spellcheck.getAvailableLanguages(), []);
    const settings = useSettings();
    const languages = settings.spellCheckLanguages ?? [...new Set(navigator.languages)];

    const toggleLanguage = (lang: string) => {
        settings.spellCheckLanguages = languages.includes(lang)
            ? languages.filter(l => l !== lang)
            : [...languages, lang];
    };

    // Put the suggestions above Discord's cut/copy/paste group.
    const pasteGroup = children.findIndex(c => c?.props?.children?.some?.((i: any) => i?.props?.id === "paste"));

    children.splice(
        pasteGroup === -1 ? children.length : pasteGroup,
        0,
        <Menu.MenuGroup>
            {misspelled && suggestions.length > 0 && (
                <>
                    {suggestions.map(s => (
                        <Menu.MenuItem
                            key={s}
                            id={"evecord-spell-" + s}
                            label={s}
                            action={() => EvecordNative.spellcheck.replaceMisspelling(s)}
                        />
                    ))}
                    <Menu.MenuSeparator />
                    <Menu.MenuItem
                        id="evecord-spell-learn"
                        label={`Add "${misspelled}" to dictionary`}
                        action={() => EvecordNative.spellcheck.addToDictionary(misspelled)}
                    />
                </>
            )}
            <Menu.MenuItem id="evecord-spell-settings" label="Spellcheck">
                <Menu.MenuCheckboxItem
                    id="evecord-spell-enabled"
                    label="Check spelling"
                    checked={enabled}
                    action={() => FluxDispatcher.dispatch({ type: "SPELLCHECK_TOGGLE" })}
                />
                <Menu.MenuItem id="evecord-spell-languages" label="Languages" disabled={!enabled}>
                    {available.map(lang => (
                        <Menu.MenuCheckboxItem
                            key={lang}
                            id={"evecord-spell-lang-" + lang}
                            label={lang}
                            checked={languages.includes(lang)}
                            // Chromium checks at most five languages at once.
                            disabled={!languages.includes(lang) && languages.length >= 5}
                            action={() => toggleLanguage(lang)}
                        />
                    ))}
                </Menu.MenuItem>
            </Menu.MenuItem>
        </Menu.MenuGroup>
    );
});
