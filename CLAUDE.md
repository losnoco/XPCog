# XPCog — working notes

## Versioning

**Any change bumps the version.** Every commit that touches this repository —
code, build files, documentation, this file included — raises the version number
as part of the same change. There is no "too small to bump".

The version lives in exactly two places, and they are kept identical:

- `CMakeLists.txt`, the `VERSION` argument to `project(XPCog ...)`
- `vcpkg.json`, the `"version"` field

Everything else derives from the first of those and must not be edited by hand:
`core/include/xpcog/core/Version.hpp.in` is configured into `Version.hpp`
(`kVersionMajor`/`Minor`/`Patch`, `kVersionString`), `app/XPCog.rc.in` takes the
Windows `FileVersion` and `ProductVersion`, and `app/CMakeLists.txt` sets the
macOS bundle's `CFBundleShortVersionString` and `CFBundleVersion`. The version
string is user-visible in the About dialog, in `xpcog-cli`'s banner and in the
HTTP source's `User-Agent`.

`README.md` is the one place that spells a version out by hand: the Windows
installer section shows `XPCog-<version>-x64-setup.exe` in two examples, and those
follow the bump.

Which component moves:

- **Patch** — the default: fixes, documentation, tests, build tweaks, a codec
  added without changing the plugin contract.
- **Minor** — a milestone lands, a user-visible feature arrives, or a
  `core`/`codecs` public header changes shape.
- **Major** — reserved; not moved without being asked.

Bump both files in the same commit as the change itself, never as a follow-up.
