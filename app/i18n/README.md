# Translations

XPCog's interface strings live in `.ts` files here and are compiled into the
binary as a resource under `:/i18n`. `installTranslations()` in `app/src/main.cpp`
picks the one matching the system locale at startup, alongside Qt's own
catalogue for the standard dialogs.

## Working on them

```
cmake --build build/dev --target update_translations   # rescan sources into .ts
cmake --build build/dev --target release_translations  # compile .ts to .qm
```

`update_translations` runs `lupdate` over `XPCOG_APP_SOURCES`. Run it after
adding or changing any `tr()` string; it preserves existing translations and
marks changed ones unfinished. Open the `.ts` files with Qt Linguist.

A partial translation is fine — Qt falls back to the English source string for
anything unfinished.

## Adding a language

Add `i18n/xpcog_<code>.ts` to the `TS_FILES` list in `app/CMakeLists.txt` and
run `update_translations`. Nothing else needs changing.

## Where these came from

`xpcog_es.ts` was seeded from Cog's `Localizable.xcstrings` (284 keys, plus 65
in its preferences bundle) wherever the English source text matched one of
XPCog's, so a Cog user changing over keeps the strings they already had. That
matched twelve. The rest of Cog's Spanish is preferences prose that XPCog words
differently, and its 158 menu items were never translated at all — they live in
`Base.lproj/MainMenu.xib` with no Spanish counterpart.

## Contexts

Qt looks a string up under the context it was recorded in. `tr()` inside a class
uses the fully-qualified class name; `QT_TRANSLATE_NOOP` uses whatever literal
you give it. The menu titles in `ActionRegistry.cpp` sit in a table at namespace
scope, so they are marked with an explicit context — and must be looked up with
`QCoreApplication::translate` using that same context, not `tr()`. A mismatch
compiles, runs, and silently leaves those strings English forever.
