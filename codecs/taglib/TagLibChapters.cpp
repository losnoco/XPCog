#include "TagLibChapters.hpp"

#include <aifffile.h>
#include <audioproperties.h>
#include <chapterframe.h>
#include <dsdifffile.h>
#include <dsffile.h>
#include <id3v2tag.h>
#include <matroskachapter.h>
#include <matroskachapteredition.h>
#include <matroskachapters.h>
#include <matroskafile.h>
#include <mp4chapter.h>
#include <mp4file.h>
#include <mpegfile.h>
#include <tableofcontentsframe.h>
#include <textidentificationframe.h>
#include <trueaudiofile.h>
#include <tbytevector.h>
#include <tbytevectorlist.h>
#include <tstring.h>
#include <wavfile.h>

#include <algorithm>
#include <charconv>
#include <random>
#include <set>
#include <string>
#include <utility>

namespace xpcog {
namespace {

[[nodiscard]] std::string toUtf8(const TagLib::String& text) {
    return text.to8Bit(true);
}

[[nodiscard]] TagLib::String toTagLibString(const std::string& text) {
    return {text, TagLib::String::UTF8};
}

/// The file's ID3v2 tag, or nullptr. `create` asks for one where the container
/// can grow it; reading passes false so a tagless file stays tagless. WAV and
/// AIFF hand out a tag on demand, so they are gated on having one when read.
TagLib::ID3v2::Tag* id3v2TagOf(TagLib::File* file, bool create) {
    if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(file)) {
        return mpeg->ID3v2Tag(create);
    }
    if (auto* wav = dynamic_cast<TagLib::RIFF::WAV::File*>(file)) {
        return (create || wav->hasID3v2Tag()) ? wav->ID3v2Tag() : nullptr;
    }
    if (auto* aiff = dynamic_cast<TagLib::RIFF::AIFF::File*>(file)) {
        return (create || aiff->hasID3v2Tag()) ? aiff->tag() : nullptr;
    }
    if (auto* dsf = dynamic_cast<TagLib::DSF::File*>(file)) {
        return dsf->tag();
    }
    if (auto* dsdiff = dynamic_cast<TagLib::DSDIFF::File*>(file)) {
        return dsdiff->ID3v2Tag(create);
    }
    if (auto* tta = dynamic_cast<TagLib::TrueAudio::File*>(file)) {
        return tta->ID3v2Tag(create);
    }
    return nullptr;
}

ChapterList readID3v2Chapters(const TagLib::ID3v2::Tag* tag) {
    ChapterList chapters;
    if (tag == nullptr) {
        return chapters;
    }
    const TagLib::ID3v2::FrameList& frames = tag->frameList("CHAP");
    if (frames.isEmpty()) {
        return chapters;
    }

    const auto chapterOf = [](const TagLib::ID3v2::ChapterFrame* frame) {
        Chapter chapter;
        chapter.startMs = frame->startTime();
        chapter.endMs   = frame->endTime();
        const TagLib::ID3v2::FrameList& titles = frame->embeddedFrameList("TIT2");
        if (!titles.isEmpty()) {
            chapter.title = toUtf8(titles.front()->toString());
        }
        // The element ID is opaque bytes, kept as they are so a renamed
        // chapter stays the same chapter.
        const TagLib::ByteVector id = frame->elementID();
        if (!id.isEmpty()) {
            chapter.id.assign(id.data(), id.size());
        }
        return chapter;
    };

    // In the top-level table of contents' order when it gives one -- the
    // order a player lists -- otherwise by start.
    const TagLib::ID3v2::TableOfContentsFrame* toc = nullptr;
    for (const auto* frame : tag->frameList("CTOC")) {
        const auto* candidate =
            dynamic_cast<const TagLib::ID3v2::TableOfContentsFrame*>(frame);
        if (candidate != nullptr && candidate->isTopLevel()) {
            toc = candidate;
            break;
        }
    }
    if (toc != nullptr && toc->isOrdered()) {
        for (const TagLib::ByteVector& elementID : toc->childElements()) {
            if (const auto* frame =
                    TagLib::ID3v2::ChapterFrame::findByElementID(tag, elementID)) {
                chapters.push_back(chapterOf(frame));
            }
        }
    }
    if (chapters.empty()) {
        for (const auto* frame : frames) {
            if (const auto* chap =
                    dynamic_cast<const TagLib::ID3v2::ChapterFrame*>(frame)) {
                chapters.push_back(chapterOf(chap));
            }
        }
        std::stable_sort(chapters.begin(), chapters.end(),
                         [](const Chapter& a, const Chapter& b) {
                             return a.startMs < b.startMs;
                         });
    }
    return chapters;
}

ChapterList readMatroskaChapters(const TagLib::Matroska::File* file) {
    ChapterList chapters;
    const TagLib::Matroska::Chapters* all = file->chapters();
    if (all == nullptr) {
        return chapters;
    }
    const auto& editions = all->chapterEditionList();
    if (editions.isEmpty()) {
        return chapters;
    }
    const TagLib::Matroska::ChapterEdition* edition = &editions.front();
    for (const TagLib::Matroska::ChapterEdition& candidate : editions) {
        if (candidate.isDefault()) {
            edition = &candidate;
            break;
        }
    }
    for (const TagLib::Matroska::Chapter& source : edition->chapterList()) {
        if (source.isHidden()) {
            continue;
        }
        Chapter chapter;
        chapter.startMs = source.timeStart() / 1000000;
        chapter.endMs   = source.timeEnd() / 1000000;
        if (!source.displayList().isEmpty()) {
            chapter.title = toUtf8(source.displayList().front().string());
        }
        chapter.id = std::to_string(source.uid());
        chapters.push_back(std::move(chapter));
    }
    return chapters;
}

ChapterList readMP4Chapters(TagLib::MP4::File* file) {
    ChapterList chapters;
    // Apple's chapter track is the fuller form; Nero's chpl is what the rest
    // of the world reads. Prefer the first, fall back to the second.
    TagLib::MP4::ChapterList list = file->qtChapters();
    if (list.isEmpty()) {
        list = file->neroChapters();
    }
    for (const TagLib::MP4::Chapter& source : list) {
        Chapter chapter;
        chapter.startMs =
            source.startTime() > 0 ? static_cast<std::uint64_t>(source.startTime()) : 0;
        chapter.title = toUtf8(source.title());
        chapters.push_back(std::move(chapter));
    }
    std::stable_sort(chapters.begin(), chapters.end(),
                     [](const Chapter& a, const Chapter& b) {
                         return a.startMs < b.startMs;
                     });
    // Neither form stores an end: a chapter runs to the next one's start,
    // and the last to the file's end.
    const std::uint64_t length =
        file->audioProperties() != nullptr
            ? static_cast<std::uint64_t>(file->audioProperties()->lengthInMilliseconds())
            : 0;
    for (std::size_t i = 0; i < chapters.size(); ++i) {
        chapters[i].endMs = i + 1 < chapters.size()
                                ? chapters[i + 1].startMs
                                : std::max(length, chapters[i].startMs);
    }
    return chapters;
}

void writeID3v2Chapters(TagLib::ID3v2::Tag* tag, const ChapterList& chapters) {
    tag->removeFrames("CTOC");
    tag->removeFrames("CHAP");
    if (chapters.empty()) {
        return;
    }

    // CHAP times are 32-bit milliseconds -- clamp rather than wrap a file
    // longer than 49 days.
    const auto clampMs = [](std::uint64_t ms) {
        return static_cast<unsigned int>(std::min<std::uint64_t>(ms, 0xFFFFFFFFu));
    };

    TagLib::ByteVectorList children;
    std::set<std::string> used;
    unsigned int next = 0;
    for (const Chapter& chapter : chapters) {
        // An element ID is kept when it is non-empty and unique; the rest
        // are minted. Players key chapter identity on it.
        std::string elementID = chapter.id;
        if (elementID.empty() || used.contains(elementID)) {
            do {
                elementID = "chp" + std::to_string(next++);
            } while (used.contains(elementID));
        }
        used.insert(elementID);
        const TagLib::ByteVector idBytes(elementID.data(),
                                         static_cast<unsigned int>(elementID.size()));

        auto* title =
            new TagLib::ID3v2::TextIdentificationFrame("TIT2", TagLib::String::UTF8);
        title->setText(toTagLibString(chapter.title));
        TagLib::ID3v2::FrameList embedded;
        embedded.append(title);
        // 0xFFFFFFFF offsets: the times are what count, not byte positions.
        tag->addFrame(new TagLib::ID3v2::ChapterFrame(
            idBytes, clampMs(chapter.startMs), clampMs(chapter.endMs), 0xFFFFFFFFu,
            0xFFFFFFFFu, embedded));
        children.append(idBytes);
    }
    auto* toc = new TagLib::ID3v2::TableOfContentsFrame("toc", children);
    toc->setIsTopLevel(true);
    toc->setIsOrdered(true);
    tag->addFrame(toc);
}

[[nodiscard]] std::uint64_t randomUID() {
    std::random_device random;
    std::uint64_t uid = 0;
    while (uid == 0) {
        uid = (static_cast<std::uint64_t>(random()) << 32) | random();
    }
    return uid;
}

void writeMatroskaChapters(TagLib::Matroska::File* file, const ChapterList& chapters) {
    TagLib::Matroska::Chapters* all = file->chapters(!chapters.empty());
    if (all == nullptr) {
        return;
    }
    all->clear();
    if (chapters.empty()) {
        return;
    }
    TagLib::List<TagLib::Matroska::Chapter> list;
    for (const Chapter& chapter : chapters) {
        std::uint64_t uid = 0;
        if (!chapter.id.empty()) {
            const char* first = chapter.id.data();
            const char* last  = first + chapter.id.size();
            const auto parsed = std::from_chars(first, last, uid);
            if (parsed.ec != std::errc{} || parsed.ptr != last) {
                uid = 0;
            }
        }
        if (uid == 0) {
            uid = randomUID();
        }
        TagLib::List<TagLib::Matroska::Chapter::Display> displays;
        displays.append(TagLib::Matroska::Chapter::Display(toTagLibString(chapter.title),
                                                           "eng"));
        list.append(TagLib::Matroska::Chapter(chapter.startMs * 1000000,
                                              chapter.endMs * 1000000, displays, uid));
    }
    // One default edition, not ordered: the chapters are spans of one file,
    // not a playlist to enforce.
    all->addChapterEdition(TagLib::Matroska::ChapterEdition(list, true, false, randomUID()));
}

void writeMP4Chapters(TagLib::MP4::File* file, const ChapterList& chapters) {
    TagLib::MP4::ChapterList list;
    for (const Chapter& chapter : chapters) {
        list.append(TagLib::MP4::Chapter(toTagLibString(chapter.title),
                                         static_cast<long long>(chapter.startMs)));
    }
    // Both kinds from one list, so Apple's players and everyone else see the
    // same chapters.
    file->setQtChapters(list);
    file->setNeroChapters(list);
}

}  // namespace

bool tagLibSupportsChapters(const TagLib::File* file) {
    return dynamic_cast<const TagLib::MPEG::File*>(file) != nullptr ||
           dynamic_cast<const TagLib::RIFF::WAV::File*>(file) != nullptr ||
           dynamic_cast<const TagLib::RIFF::AIFF::File*>(file) != nullptr ||
           dynamic_cast<const TagLib::DSF::File*>(file) != nullptr ||
           dynamic_cast<const TagLib::DSDIFF::File*>(file) != nullptr ||
           dynamic_cast<const TagLib::TrueAudio::File*>(file) != nullptr ||
           dynamic_cast<const TagLib::MP4::File*>(file) != nullptr ||
           dynamic_cast<const TagLib::Matroska::File*>(file) != nullptr;
}

bool hasTagLibChapters(TagLib::File* file) {
    if (file == nullptr) {
        return false;
    }
    if (const auto* matroska = dynamic_cast<const TagLib::Matroska::File*>(file)) {
        const TagLib::Matroska::Chapters* all = matroska->chapters();
        if (all == nullptr) {
            return false;
        }
        for (const TagLib::Matroska::ChapterEdition& edition : all->chapterEditionList()) {
            if (!edition.chapterList().isEmpty()) {
                return true;
            }
        }
        return false;
    }
    if (auto* mp4 = dynamic_cast<TagLib::MP4::File*>(file)) {
        return !mp4->qtChapters().isEmpty() || !mp4->neroChapters().isEmpty();
    }
    const TagLib::ID3v2::Tag* tag = id3v2TagOf(file, false);
    return tag != nullptr && !tag->frameList("CHAP").isEmpty();
}

ChapterList readTagLibChapters(TagLib::FileRef& file) {
    TagLib::File* raw = file.file();
    if (raw == nullptr) {
        return {};
    }
    if (auto* matroska = dynamic_cast<TagLib::Matroska::File*>(raw)) {
        return readMatroskaChapters(matroska);
    }
    if (auto* mp4 = dynamic_cast<TagLib::MP4::File*>(raw)) {
        return readMP4Chapters(mp4);
    }
    return readID3v2Chapters(id3v2TagOf(raw, false));
}

void writeTagLibChapters(TagLib::File* file, const ChapterList& chapters,
                         std::uint64_t lengthMs) {
    ChapterList ordered = chapters;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Chapter& a, const Chapter& b) {
                         return a.startMs < b.startMs;
                     });
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        if (i + 1 < ordered.size()) {
            ordered[i].endMs = ordered[i + 1].startMs;
        } else if (lengthMs > ordered[i].startMs) {
            ordered[i].endMs = lengthMs;
        } else if (ordered[i].endMs < ordered[i].startMs) {
            ordered[i].endMs = ordered[i].startMs;
        }
    }

    if (auto* matroska = dynamic_cast<TagLib::Matroska::File*>(file)) {
        writeMatroskaChapters(matroska, ordered);
    } else if (auto* mp4 = dynamic_cast<TagLib::MP4::File*>(file)) {
        writeMP4Chapters(mp4, ordered);
    } else if (auto* tag = id3v2TagOf(file, !ordered.empty())) {
        writeID3v2Chapters(tag, ordered);
    }
}

}  // namespace xpcog
