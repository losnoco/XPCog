# Translations

There are none yet, and that is deliberate: the interface is English only until
the strings settle. This directory is the place they will land.

The `tr()` and `QT_TRANSLATE_NOOP` calls throughout `app/src` stay, because they
cost nothing at runtime and are exactly what a catalogue is extracted from. What
is absent is everything downstream of them — no `.ts` files, no
`qt_add_translations()` in [`../CMakeLists.txt`](../CMakeLists.txt), no
`QTranslator` in [`../src/main.cpp`](../src/main.cpp), and no resource under
`:/i18n`. Nothing has to be kept in step with strings still being written, and
no half-finished catalogue can ship.

## Starting them

1. Add back `qt_add_translations(xpcog-app TS_FILES i18n/xpcog_en.ts
   i18n/xpcog_es.ts SOURCES ${XPCOG_APP_SOURCES} src/main.cpp)` and the
   `qt_add_resources` call that puts the compiled `.qm` files under `:/i18n`.
2. Install a `QTranslator` for the system locale at startup, and a second one
   for Qt's own strings from `QLibraryInfo::TranslationsPath` — without that
   second one a translated menu bar sits above an English file dialog.
3. `cmake --build <dir> --target update_translations` runs `lupdate` over the
   sources; open the result in Qt Linguist.

Spanish is the language to start with: it is the one Cog ships beyond English.
