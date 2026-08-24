#!/usr/bin/env python3
"""Builds the Cog fixtures the import tests read.

Synthetic on purpose. A real Cog store is someone's listening history, it is
several hundred kilobytes of it, and it names paths that exist on exactly one
machine -- none of which belongs in a repository. What the tests need is the
*shape*: the schema Core Data generates, and one row for each case the reader
has to get right. Those are enumerated here, so a case that is not covered is
visible as an absence in this file rather than as a row nobody noticed missing.

The schema is copied verbatim from a real store (Cog 3633, macOS). See
docs/COGIMPORT.md for where it comes from and what each column means.

    python3 tools/cogimport-fixture/make-fixture.py tests/fixtures/cog
"""

import plistlib
import sqlite3
import sys
from pathlib import Path

# Verbatim from `sqlite3 DataModel.sqlite .schema`. Not reformatted: the point
# of a fixture is that it is the same shape as the thing it stands in for, and
# tidying the DDL is how that stops being checkable.
SCHEMA = """
CREATE TABLE ZALBUMARTWORK ( Z_PK INTEGER PRIMARY KEY, Z_ENT INTEGER, Z_OPT INTEGER, ZARTHASH VARCHAR, ZARTDATA BLOB );
CREATE TABLE ZPLAYCOUNT ( Z_PK INTEGER PRIMARY KEY, Z_ENT INTEGER, Z_OPT INTEGER, ZCOUNT INTEGER, ZFIRSTSEEN TIMESTAMP, ZLASTPLAYED TIMESTAMP, ZRATING FLOAT, ZALBUM VARCHAR, ZARTIST VARCHAR, ZFILENAME VARCHAR, ZTITLE VARCHAR );
CREATE TABLE ZPLAYLISTENTRY ( Z_PK INTEGER PRIMARY KEY, Z_ENT INTEGER, Z_OPT INTEGER, ZBITRATE INTEGER, ZBITSPERSAMPLE INTEGER, ZCHANNELCONFIG INTEGER, ZCHANNELS INTEGER, ZCOUNTADDED INTEGER, ZCURRENT INTEGER, ZDBINDEX INTEGER, ZDELETED INTEGER, ZENTRYID INTEGER, ZERROR INTEGER, ZFLOATINGPOINT INTEGER, ZINDEX INTEGER, ZMETADATALOADED INTEGER, ZQUEUEPOSITION INTEGER, ZQUEUED INTEGER, ZREMOVED INTEGER, ZSEEKABLE INTEGER, ZSHUFFLEINDEX INTEGER, ZSTOPAFTER INTEGER, ZTOTALFRAMES INTEGER, ZUNSIGNED INTEGER, ZCURRENTPOSITION FLOAT, ZREPLAYGAINALBUMGAIN FLOAT, ZREPLAYGAINALBUMPEAK FLOAT, ZREPLAYGAINTRACKGAIN FLOAT, ZREPLAYGAINTRACKPEAK FLOAT, ZSAMPLERATE FLOAT, ZVOLUME FLOAT, ZSPOTLIGHTLENGTH DECIMAL, ZARTHASH VARCHAR, ZCODEC VARCHAR, ZCUESHEET VARCHAR, ZENCODING VARCHAR, ZENDIAN VARCHAR, ZERRORMESSAGE VARCHAR, ZSOUNDCHECK VARCHAR, ZSPOTLIGHTTRACK VARCHAR, ZTRASHURLSTRING VARCHAR, ZURLSTRING VARCHAR, ZMETADATABLOB BLOB, ZURLBOOKMARK BLOB );
CREATE TABLE ZSANDBOXTOKEN ( Z_PK INTEGER PRIMARY KEY, Z_ENT INTEGER, Z_OPT INTEGER, ZFOLDER INTEGER, ZPATH VARCHAR, ZBOOKMARK BLOB );
CREATE TABLE Z_PRIMARYKEY (Z_ENT INTEGER PRIMARY KEY, Z_NAME VARCHAR, Z_SUPER INTEGER, Z_MAX INTEGER);
CREATE TABLE Z_METADATA (Z_VERSION INTEGER PRIMARY KEY, Z_UUID VARCHAR(255), Z_PLIST BLOB);
CREATE TABLE Z_MODELCACHE (Z_CONTENT BLOB);
"""

# Entity numbers, as Core Data assigns them: alphabetical, from 1.
ENT_ARTWORK, ENT_PLAYCOUNT, ENT_ENTRY, ENT_TOKEN = 1, 2, 3, 4


def keyed_archive(fields: dict[str, str]) -> bytes:
    """An NSKeyedArchiver archive of a flat NSDictionary of strings.

    This is what Core Data's `Transformable` attribute produces for
    PlaylistEntry.metadataBlob -- see the model, whose transformer is
    MaybeSecureValueDataTransformer. The layout is an object table plus UID
    references into it, which is the whole reason reading one is more work than
    reading a plist: the dictionary is a graph, not a tree.
    """
    keys = list(fields)
    values = [fields[k] for k in keys]

    # $objects[0] is always "$null"; UIDs are indices into this array.
    objects: list = ["$null"]
    key_uids, value_uids = [], []
    for k in keys:
        objects.append(k)
        key_uids.append(plistlib.UID(len(objects) - 1))
    for v in values:
        objects.append(v)
        value_uids.append(plistlib.UID(len(objects) - 1))

    dict_index = len(objects)
    objects.append({"$class": plistlib.UID(dict_index + 1),
                    "NS.keys": key_uids,
                    "NS.objects": value_uids})
    objects.append({"$classname": "NSDictionary",
                    "$classes": ["NSDictionary", "NSObject"]})

    return plistlib.dumps({
        "$version": 100000,
        "$archiver": "NSKeyedArchiver",
        "$top": {"root": plistlib.UID(dict_index)},
        "$objects": objects,
    }, fmt=plistlib.FMT_BINARY)


def entry(pk, index, url, **over):
    """One ZPLAYLISTENTRY row, with the defaults a real one carries."""
    row = dict(Z_PK=pk, Z_ENT=ENT_ENTRY, Z_OPT=1, ZINDEX=index, ZURLSTRING=url,
               ZBITRATE=750, ZBITSPERSAMPLE=16, ZCHANNELCONFIG=3, ZCHANNELS=2,
               ZCOUNTADDED=0, ZCURRENT=0, ZDBINDEX=0, ZDELETED=0, ZENTRYID=0,
               ZERROR=0, ZFLOATINGPOINT=0, ZMETADATALOADED=1, ZQUEUEPOSITION=-1,
               ZQUEUED=None, ZREMOVED=None, ZSEEKABLE=1, ZSHUFFLEINDEX=0,
               ZSTOPAFTER=0, ZTOTALFRAMES=4410000, ZUNSIGNED=0,
               ZCURRENTPOSITION=0.0, ZREPLAYGAINALBUMGAIN=0.0,
               ZREPLAYGAINALBUMPEAK=0.0, ZREPLAYGAINTRACKGAIN=0.0,
               ZREPLAYGAINTRACKPEAK=0.0, ZSAMPLERATE=44100.0, ZVOLUME=1.0,
               ZSPOTLIGHTLENGTH=None, ZARTHASH=None, ZCODEC="FLAC",
               ZCUESHEET=None, ZENCODING="lossless", ZENDIAN=None,
               ZERRORMESSAGE=None, ZSOUNDCHECK=None, ZSPOTLIGHTTRACK=None,
               ZTRASHURLSTRING=None, ZMETADATABLOB=None, ZURLBOOKMARK=None)
    row.update(over)
    return row


def build(out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    db_path = out_dir / "DataModel.sqlite"
    db_path.unlink(missing_ok=True)

    db = sqlite3.connect(db_path)
    db.executescript(SCHEMA)

    # One row per case the reader has to get right. The comment on each is the
    # test it exists for.
    rows = [
        # An ordinary file. The base case.
        entry(1, 0, "file:///music/Artist/Album/01%20First.flac",
              ZMETADATABLOB=keyed_archive({"title": "First", "artist": "Artist",
                                           "album": "Album", "track": "1",
                                           "genre": "Rock"})),
        # A cue sheet track. The fragment is the track number, which is exactly
        # what xpcog::Url models -- so this must survive as one URL, not two.
        entry(2, 1, "file:///music/Artist/Album/Album.cue#01"),
        entry(3, 2, "file:///music/Artist/Album/Album.cue#02"),
        # Non-ASCII, percent-encoded, which is how a real store holds it.
        entry(4, 3, "file:///music/Bj%C3%B6rk/Post/02%20Hyperballad.flac",
              ZMETADATABLOB=keyed_archive({"title": "Hyperballad",
                                           "artist": "Björk", "album": "Post"})),
        # Deleted. Cog prunes these on load and so must the reader.
        entry(5, 4, "file:///music/gone.flac", ZDELETED=1),
        # Empty URL. Pruned for the same reason, and separately, because the
        # check in Cog is `deLeted || !urlString || !length`.
        entry(6, 5, ""),
        entry(7, 6, None),
        # A macOS file reference URL: identifies a file by inode, resolves only
        # while it exists, and cannot be handed to fopen(). Current Cog
        # normalizes these on load; a store written by an older one still holds
        # them, and this is the row that says so.
        entry(8, 7, "file:///.file/id=6571714.6571725"),
        # A stream. Not every entry is a local file.
        entry(9, 8, "http://example.com/stream.ogg", ZCODEC="Vorbis",
              ZSEEKABLE=0, ZTOTALFRAMES=0),
        # Carries album art, joined by hash rather than by a foreign key.
        entry(10, 9, "file:///music/Artist/Album/02%20Second.flac",
              ZARTHASH="deadbeef"),
        # Queued, and the queue position matters. Cog reads the queue back from
        # the same store.
        entry(11, 10, "file:///music/Artist/Album/03%20Third.flac",
              ZQUEUED=1, ZQUEUEPOSITION=0),
        # The entry that was playing, and where it had got to.
        entry(12, 11, "file:///music/Artist/Album/04%20Fourth.flac",
              ZCURRENT=1, ZCURRENTPOSITION=42.5),
        # Real ReplayGain, so a reader that drops the fields is visible.
        entry(13, 12, "file:///music/Artist/Album/05%20Fifth.flac",
              ZREPLAYGAINTRACKGAIN=-6.5, ZREPLAYGAINTRACKPEAK=0.98,
              ZREPLAYGAINALBUMGAIN=-5.25, ZREPLAYGAINALBUMPEAK=1.01),
        # Indices deliberately out of Z_PK order: ZINDEX is the playlist order
        # and Z_PK is not, which a reader that forgets to sort will not notice
        # on a store where they happen to agree.
        entry(14, 14, "file:///music/Artist/Album/07%20Seventh.flac"),
        entry(15, 13, "file:///music/Artist/Album/06%20Sixth.flac"),
    ]

    columns = list(rows[0])
    db.executemany(
        f"insert into ZPLAYLISTENTRY ({','.join(columns)}) "
        f"values ({','.join('?' * len(columns))})",
        [[r[c] for c in columns] for r in rows])

    # Play counts. Keyed by strings rather than by a relationship, which is why
    # they survive a file moving and why matching them back up is fuzzy.
    db.executemany(
        "insert into ZPLAYCOUNT (Z_PK, Z_ENT, Z_OPT, ZCOUNT, ZFIRSTSEEN, "
        "ZLASTPLAYED, ZRATING, ZALBUM, ZARTIST, ZFILENAME, ZTITLE) values "
        "(?,?,1,?,?,?,?,?,?,?,?)",
        [(1, ENT_PLAYCOUNT, 7, 706232000.0, 712000000.0, 4.5, "Album",
          "Artist", "01 First.flac", "First"),
         (2, ENT_PLAYCOUNT, 1, 706232000.0, 706232100.0, 0.0, "Post",
          "Björk", "02 Hyperballad.flac", "Hyperballad")])

    # Album art, joined to an entry by ZARTHASH.
    db.execute("insert into ZALBUMARTWORK (Z_PK, Z_ENT, Z_OPT, ZARTHASH, "
               "ZARTDATA) values (1, ?, 1, 'deadbeef', ?)",
               (ENT_ARTWORK, b"\x89PNG\r\n\x1a\n" + b"\x00" * 24))

    # Sandbox tokens. ZPATH is readable; the bookmark is opaque and only macOS
    # can resolve it. A folder token covers everything beneath it.
    db.executemany(
        "insert into ZSANDBOXTOKEN (Z_PK, Z_ENT, Z_OPT, ZFOLDER, ZPATH, "
        "ZBOOKMARK) values (?,?,1,?,?,?)",
        [(1, ENT_TOKEN, 1, "/music", b"book\x00mark"),
         (2, ENT_TOKEN, 0, "/music/Björk/Post/02 Hyperballad.flac",
          b"book\x00mark")])

    db.executemany(
        "insert into Z_PRIMARYKEY (Z_ENT, Z_NAME, Z_SUPER, Z_MAX) values (?,?,0,?)",
        [(ENT_ARTWORK, "AlbumArtwork", 1), (ENT_PLAYCOUNT, "PlayCount", 2),
         (ENT_ENTRY, "PlaylistEntry", len(rows)), (ENT_TOKEN, "SandboxToken", 2)])
    db.execute("insert into Z_METADATA (Z_VERSION, Z_UUID, Z_PLIST) values "
               "(1, 'FIXTURE-0000-0000-0000-000000000000', NULL)")

    db.commit()
    db.close()

    # The defaults, sparse the way a real one is: NSUserDefaults persists only
    # what differs from what the app registered, so a key that is absent means
    # "Cog's default" and not "unset". Two of these XPCog shares key-for-key by
    # design; the rest are Cog-only and must be ignored rather than guessed at.
    plistlib.dump({
        "floatingMiniWindow": False,
        "lastPlaybackStatus": 0,
        "miniMode": False,
        "sentryAskedConsent": True,
        "volume": 78.5,
        "repeat": 2,
        "shuffle": 1,
        "eq20Hz": 3.5,
        "eq1kHz": -2.0,
        "GraphicEQenable": True,
        "GraphicEQpreset": 7,
        "metadataMigrated": True,   # Cog-only
        "miniPlusMode": False,      # Cog-only
        "pitch": 1.0,               # Cog-only (Rubber Band, not ported)
        "tempo": 1.0,               # Cog-only
        "NSWindow Frame Cog": "0 0 1200 800 0 0 1920 1080",  # AppKit, not a setting
    }, (out_dir / "org.cogx.cog.plist").open("wb"), fmt=plistlib.FMT_BINARY)


if __name__ == "__main__":
    build(Path(sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures/cog"))
    print("wrote fixtures")
