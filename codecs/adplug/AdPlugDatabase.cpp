// AdPlug's song database, loaded from memory rather than from a file.
//
// Around forty DOS-era formats and hardly any of them carry a title, a length
// or an author -- an AdLib tracker wrote out what the OPL chip needed and
// nothing else. AdPlug's answer is a database keyed by a CRC of the file's
// contents, shipped as `adplug.db` in a repository of its own
// (github.com/adplug/database), and consulted by every player through
// CAdPlug::set_database(). Without it a great many tunes are titled after their
// filename and report a length of zero.
//
// Cog loads it from its plugin bundle. There is no bundle here and there are
// three platforms, so it is compiled in: 7.8 KB, turned into a byte array by
// CMake at build time and read through libbinio's memory stream. That removes
// an install step, a search path, and the failure mode where the database is
// simply missing on somebody's machine and the symptom is untitled tracks.
//
// The file itself stays in the tree as the upstream artefact rather than as a
// wall of hex in a header -- see codecs/adplug/CMakeLists.txt for the
// generation, and codecs/adplug/adplug.db for the original.

#include "AdPlugDatabase.hpp"

#include <adplug/adplug.h>
#include <adplug/database.h>

#include <binstr.h>

#include <mutex>

/// The generated array, at global scope with C linkage because that is what the
/// generated translation unit defines. See codecs/adplug/CMakeLists.txt.
extern "C" {
extern const unsigned char kAdPlugDatabaseBytes[];
extern const unsigned int  kAdPlugDatabaseSize;
}

namespace xpcog::adplug {

void installDatabase() {
    // Once per process, and process-wide: CAdPlug::set_database() writes a
    // global that every player reads while loading. Constructed on first use
    // rather than at static-init time so the order against AdPlug's own globals
    // -- CAdPlug::players among them -- is not a question anyone has to answer.
    //
    // Leaked deliberately. The database has to outlive every player that might
    // still be consulting it, and the last of those is destroyed at a moment no
    // static destructor can be ordered against; 7.8 KB is a fair price for not
    // having to reason about that at exit.
    static std::once_flag once;
    std::call_once(once, [] {
        auto* database = new CAdPlugDatabase;

        // binisstream takes a non-const pointer and does not write through it.
        binisstream stream(const_cast<unsigned char*>(kAdPlugDatabaseBytes),
                           kAdPlugDatabaseSize);
        stream.setFlag(binio::BigEndian, false);
        stream.setFlag(binio::FloatIEEE);

        if (database->load(stream)) {
            CAdPlug::set_database(database);
        } else {
            // Nothing to report to and nothing to do about it: every player
            // still works, and the tunes that would have been named from the
            // database keep their filenames. Deleting it is the only thing that
            // would be worse than useless.
            delete database;
        }
    });
}

}  // namespace xpcog::adplug
