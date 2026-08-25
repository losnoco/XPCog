# Translations

One `.po` per language, compiled into the binary. `xpcog.pot` is the template:
every message the interface can show, with no translations in it.

```
app/locale/xpcog.pot     the template, regenerated from the sources
app/locale/es.po         Spanish
```

## How a message gets here

Mark it in `app/src` and nowhere else — `core`, `codecs` and `platform` link no
toolkit, so they have no catalogue to consult. See `app/src/Localization.hpp`.

```cpp
_("Open Files")                                   // translated here and now
wxPLURAL("%zu track", "%zu tracks", n)            // and the plural of it
wxTRANSLATE("&File")                              // marked; looked up later
```

`wxTRANSLATE` is for a literal that has to sit in a file-scope table — a menu
row, a settings choice, a field caption — where `_()` cannot run. Whatever reads
that table calls `wxGetTranslation()` on the way to the screen.

Then regenerate the template and add the message to each `.po`:

```sh
python tools/extract-messages.py
```

That script replaces `xgettext`; `cmake/CompileCatalog.cmake` replaces `msgfmt`.
Both exist because gettext is not a dependency this project has anywhere else,
and the one platform most of this is built on is the one least likely to have
it. `extract-messages.py` does **not** merge: it writes a fresh template, and the
diff against the committed one is how you see what a change added. If you do have
gettext, `msgmerge --update es.po xpcog.pot` does the merging properly.

## What is not translated

- **Setting values.** `settings.def` keys and the strings stored under them are
  Cog's, and a Cog plist has to import verbatim. Only the label beside a value is
  language.
- **Equaliser preset names.** They are matched against a track's genre tag, so a
  translated "Rock" would stop matching the tag that chose it. The *Custom* row
  beside them is translated, because it names no preset.
- **File-dialog patterns.** `*.flac;*.mp3` is not language. The description in
  front of each is.
- **Command-line switch names.** `--register` has to be the same word in a
  script whatever the interface is set to. Its help text is translated.
- **The window title's separator.** `Track — XPCog` is a formatting convention;
  making it a message would invite reordering, and a title that does not start
  with the track is a taskbar button that reads "XPCog" forty times.
- **Proper nouns**: XPCog, Cog, Last.fm, MIDI, SoundFont, Rubber Band, the
  format and library names in the About box's licence table, and the SPDX
  identifiers beside them.

## Accelerators

The letter after `&` in a menu label is part of the translation and has to be
reassigned rather than carried across: it must be unique within its menu and it
must be a letter the translated label actually contains. Avoid accented vowels —
several platforms underline them badly.

## Adding a language

1. Copy `xpcog.pot` to `<code>.po`, where `<code>` is what the operating system
   calls the language: `pt_BR`, `fr`, `de`. The filename is the only place the
   build learns it.
2. Fill in `Language:` and `Plural-Forms:` in the header. The plural rule is read
   at run time, so getting it wrong is a wrong sentence rather than a build
   failure.
3. Add the file to `xpcog_add_catalogs()` in `app/CMakeLists.txt`.
4. Add the language's own name for itself to `kEndonyms` in
   `app/src/Localization.cpp`. Without it the Preferences picker shows the bare
   code, which works and looks like an oversight.

A partly translated `.po` is fine: an empty `msgstr` is dropped at compile time
rather than shown, so an untranslated message falls back to English.
