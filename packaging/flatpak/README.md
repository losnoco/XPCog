# XPCog as a Flatpak

```sh
flatpak run org.flatpak.Builder --force-clean --user --install \
    build-dir packaging/flatpak/co.losno.XPCog.yml
flatpak run co.losno.XPCog
```

`org.gnome.Platform` 50, which is what makes this the only artefact the project
produces that runs on a distribution regardless of its glibc. The tarball is
built against whatever built it; this is built against a runtime.

The application ID is the one everything else already agreed on —
`XPCOG_DESKTOP_ID` in the root `CMakeLists.txt`. Flatpak requires the desktop
file, the AppStream metainfo and the icons to be named for it, and
`packaging/linux/` installs them that way already, so nothing here renames a
file or patches an install rule. That was the point of doing layer 1 first.

## It is not ready for Flathub, and the obstacle is specific

The `xpcog` module asks for `--share=network`. `flatpak-builder` builds offline
by design, and vcpkg fetches each port's sources itself rather than through
`sources:`. Flathub does not accept that, and it is the only thing standing in
the way — everything else about this manifest is submittable.

Removing it means removing vcpkg, and the GNOME 50 SDK gets most of the way
there on its own. It already supplies SQLite, curl, libarchive, Ogg, Vorbis,
FLAC, Opus, opusfile, TagLib, WavPack and FFmpeg, all of which
`cmake/XPCogSystemDeps.cmake` knows how to substitute — which is why the vcpkg
build inside this manifest is a fraction of a normal one. What is left:

| Missing | Why it is awkward |
| --- | --- |
| `FLACConfig.cmake`, `VorbisConfig.cmake` | `codecs/flac` and `codecs/vorbis` call `find_package(... CONFIG)`, and the SDK ships the libraries without config packages. The ones in use today are vcpkg's own, so a module would have to install a config package as well as a library. |
| `mgba`, `libvgm` | No system path in `XPCogSystemDeps` at all — GSF and the VGM family would each need a module. |
| `libopenmpt`, `libgme`, `soxr`, `rubberband`, `nlohmann/json` | Simply not in the SDK. Ordinary builds, just more of them. |

None of it is hard. All of it is work, and it is worth doing separately from
getting the thing to run at all.

## Permissions

Every `finish-arg` is there because something in the source asks for it, and
the manifest says which. The four D-Bus names are the interesting ones:
`org.kde.StatusNotifierWatcher` for the tray icon, `org.mpris.MediaPlayer2.*`
for the media keys and panel widget, `org.freedesktop.FileManager1` for
revealing a track, and `org.freedesktop.secrets` for the Last.fm session key,
which lives in `wxSecretStore` rather than in settings.

`--filesystem=home` is the broad one. A music library is wherever the listener
keeps it, and XPCog is pointed at directories rather than handed files one at a
time, so a narrower `xdg-music` alone would break the common case.

## Two differences from `linux-repo-release`

Both are the ones `packaging/arch/PKGBUILD` makes, for the same reasons: the
crash reporter is off, because it is the upstream project's Sentry and a
distributed package is not the right thing to report from, and the test suite
is off because it is not what ships.

## Building from a release rather than a checkout

The `dir` source builds the working tree, which is what makes this useful while
developing. For a release, replace it with the tag:

```yaml
- type: archive
  url: https://github.com/losnoco/XPCog/archive/refs/tags/v1.0.0.tar.gz
  sha256: ...
```

Like the Arch package, it cannot build a version older than 1.0.0 — the install
rules it depends on did not exist before then.
