#include "InfoPanel.hpp"

#include "xpcog/core/library/Library.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLocale>
#include <QPixmap>
#include <QScrollArea>
#include <QStringList>
#include <QVBoxLayout>

namespace xpcog::app {

namespace info {

QString trackText(std::int32_t track, std::int32_t disc) {
    if (track == 0) {
        return {};
    }
    if (disc == 0) {
        return QStringLiteral("%1").arg(track, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1.%2").arg(disc).arg(track, 2, 10, QLatin1Char('0'));
}

QString lengthText(double seconds) {
    if (seconds <= 0.0) {
        // A live stream has no length, and "0:00.000" would be a claim rather
        // than an absence.
        return {};
    }
    const auto milliseconds = static_cast<qint64>(seconds * 1000.0 + 0.5);
    return QStringLiteral("%1:%2.%3")
        .arg(milliseconds / 60000)
        .arg((milliseconds / 1000) % 60, 2, 10, QLatin1Char('0'))
        .arg(milliseconds % 1000, 3, 10, QLatin1Char('0'));
}

QString replayGainText(const ReplayGainInfo& gain) {
    QStringList lines;
    const auto decibels = [](float value) {
        // The sign is always shown: +0.00 dB and -0.00 dB are different claims
        // from "no gain tag", and this panel exists to tell them apart.
        return QStringLiteral("%1%2 dB")
            .arg(value < 0.0F ? QString{} : QStringLiteral("+"))
            .arg(static_cast<double>(value), 0, 'f', 2);
    };
    const auto peak = [](float value) {
        return QStringLiteral("%1").arg(static_cast<double>(value), 0, 'f', 6);
    };

    if (gain.albumGain) {
        lines << InfoPanel::tr("Album Gain: %1").arg(decibels(*gain.albumGain));
    }
    if (gain.albumPeak) {
        lines << InfoPanel::tr("Album Peak: %1").arg(peak(*gain.albumPeak));
    }
    if (gain.trackGain) {
        lines << InfoPanel::tr("Track Gain: %1").arg(decibels(*gain.trackGain));
    }
    if (gain.trackPeak) {
        lines << InfoPanel::tr("Track Peak: %1").arg(peak(*gain.trackPeak));
    }
    if (gain.soundcheck && !gain.soundcheck->empty()) {
        lines << InfoPanel::tr("SoundCheck: %1")
                     .arg(QString::fromStdString(*gain.soundcheck));
    }
    // Cog's condition exactly: a volume of 1.0 is no scaling and says nothing.
    if (gain.volume && *gain.volume != 1.0F) {
        lines << InfoPanel::tr("Volume Scale: %1×")
                     .arg(static_cast<double>(*gain.volume), 0, 'f', 2);
    }
    return lines.join(QLatin1Char('\n'));
}

QString playCountText(std::int64_t count, std::int64_t firstSeen,
                      std::int64_t lastPlayed) {
    const auto stamp = [](std::int64_t unixSeconds) {
        return QLocale{}.toString(QDateTime::fromSecsSinceEpoch(unixSeconds),
                                  QLocale::ShortFormat);
    };

    QStringList lines;
    lines << QString::number(count);
    if (firstSeen != 0) {
        lines << InfoPanel::tr("First seen: %1").arg(stamp(firstSeen));
    }
    if (lastPlayed != 0) {
        lines << InfoPanel::tr("Last played: %1").arg(stamp(lastPlayed));
    }
    return lines.join(QLatin1Char('\n'));
}

}  // namespace info

namespace {

/// Marked for extraction under this context rather than the class's, because a
/// table of string literals cannot call tr() itself and the lookup below has to
/// name the context the strings were extracted into. Getting this wrong compiles
/// and runs and simply stays English -- see ActionRegistry, which does the same
/// thing with its menu table, and tests/app/test_translations.cpp.
constexpr auto kFieldContext = "InfoPanel";

/// Cog's labels, verbatim, in Cog's order.
struct Row {
    const char* section;  ///< non-null starts a new group
    const char* label;    ///< null on a section row
};

constexpr Row kRows[] = {
    {QT_TRANSLATE_NOOP("InfoPanel", "Tags"), nullptr},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Album Artist:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Artist:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Composer:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Album:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Title:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Track:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Length:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Date:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Genre:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Filename:")},

    {QT_TRANSLATE_NOOP("InfoPanel", "Stream"), nullptr},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Sample Rate:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Channels:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Bitrate:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Bits Per Sample:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Codec:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Encoding:")},

    {QT_TRANSLATE_NOOP("InfoPanel", "Library"), nullptr},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Cuesheet:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "ReplayGain:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Play Count:")},
    {nullptr, QT_TRANSLATE_NOOP("InfoPanel", "Comment:")},
};

/// Artwork is shown at a fixed height rather than scaled to the dock: a dock
/// narrowed to a sliver would otherwise leave a one-pixel smear of a cover, and
/// widened to half the screen would show a 300px JPEG blown up to fill it.
constexpr int kArtHeight = 180;

[[nodiscard]] QString fromStd(const std::string& text) {
    return QString::fromStdString(text);
}

}  // namespace

InfoPanel::InfoPanel(const Library* library, QWidget* parent)
    : QWidget(parent), library_(library) {
    values_.resize(FieldCount);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
    // Wrap long rows onto two lines rather than forcing the dock wider than the
    // playlist. A file path is the row that makes this necessary.
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    art_ = new QLabel;
    art_->setAlignment(Qt::AlignCenter);
    art_->setMinimumHeight(kArtHeight);
    art_->hide();
    form->addRow(art_);

    int field = 0;
    for (const Row& row : kRows) {
        if (row.section != nullptr) {
            auto* heading =
                new QLabel(QCoreApplication::translate(kFieldContext, row.section));
            QFont bold = heading->font();
            bold.setBold(true);
            heading->setFont(bold);
            // A little air above each group, except the first, which sits under
            // the artwork and needs none.
            if (field != 0) {
                auto* rule = new QFrame;
                rule->setFrameShape(QFrame::HLine);
                rule->setFrameShadow(QFrame::Sunken);
                form->addRow(rule);
            }
            form->addRow(heading);
            continue;
        }

        auto* value = new QLabel;
        value->setWordWrap(true);
        // Selectable, because the reason to open this panel on a path or a
        // codec string is usually to take it somewhere else.
        value->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                       Qt::TextSelectableByKeyboard);
        form->addRow(QCoreApplication::translate(kFieldContext, row.label), value);
        values_[field++] = value;
    }

    auto* inner = new QWidget;
    inner->setLayout(form);

    // Scrolled, because twenty fields plus artwork do not fit a dock docked to
    // the side of a window at any sensible height.
    auto* scroll = new QScrollArea(this);
    scroll->setWidget(inner);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(scroll);

    showEntry(nullptr);
}

void InfoPanel::set(Field field, const QString& value) {
    if (QLabel* label = values_.value(field); label != nullptr) {
        label->setText(value);
        label->setToolTip(value);
    }
}

void InfoPanel::showEntry(const PlaylistEntry* entry) {
    if (entry == nullptr) {
        for (QLabel* label : values_) {
            label->clear();
            label->setToolTip({});
        }
        art_->clear();
        art_->hide();
        return;
    }

    set(AlbumArtist, fromStd(entry->albumArtist));
    set(Artist, fromStd(entry->artist));
    set(Composer, fromStd(entry->composer));
    set(Album, fromStd(entry->album));
    set(Title, fromStd(entry->title()));
    set(Track, info::trackText(entry->track, entry->disc));
    set(Length, info::lengthText(entry->duration()));
    // Cog binds `date`, not the year, and falls back to nothing rather than
    // inventing a January the first.
    set(Date, entry->date.empty() && entry->year != 0
                  ? QString::number(entry->year)
                  : fromStd(entry->date));
    set(Genre, fromStd(entry->genre));

    // Cog shows the last path component here. The whole path instead, because
    // this panel is resizable where Cog's fixed HUD was not, and because a path
    // you can select and copy is a large part of why an info panel gets opened.
    const auto local = entry->url.localPath();
    set(Filename, local ? QString::fromStdString(local->string())
                        : fromStd(entry->url.toString()));

    const TrackProperties& properties = entry->properties;
    set(SampleRate, properties.format.sampleRate > 0.0
                        ? tr("%1 Hz").arg(static_cast<qint64>(properties.format.sampleRate))
                        : QString{});
    set(Channels, properties.format.channels > 0
                      ? QString::number(properties.format.channels)
                      : QString{});
    set(Bitrate, properties.bitrateKbps > 0
                     ? tr("%1 kbps").arg(properties.bitrateKbps)
                     : QString{});
    set(BitsPerSample, properties.format.bitsPerSample > 0
                           ? QString::number(properties.format.bitsPerSample)
                           : QString{});
    set(Codec, fromStd(properties.codec));
    set(Encoding, fromStd(properties.encoding));

    set(Cuesheet, properties.cuesheet && !properties.cuesheet->empty() ? tr("yes")
                                                                      : tr("no"));
    set(ReplayGain, info::replayGainText(properties.replayGain));

    // The count on the entry is what the playlist carries; the dates only exist
    // in the database, so a build without one shows the count alone.
    std::int64_t firstSeen  = 0;
    std::int64_t lastPlayed = 0;
    if (library_ != nullptr) {
        if (const auto record = library_->playCount(entry->artist, entry->album,
                                                    entry->title());
            record.has_value()) {
            firstSeen  = record->firstSeen;
            lastPlayed = record->lastPlayed;
        }
    }
    set(PlayCount, info::playCountText(entry->playCount, firstSeen, lastPlayed));

    set(Comment, fromStd(entry->comment));

    QPixmap cover;
    if (library_ != nullptr && !entry->artHash.empty()) {
        const std::vector<std::byte> bytes = library_->artwork(entry->artHash);
        if (!bytes.empty()) {
            cover.loadFromData(QByteArray(reinterpret_cast<const char*>(bytes.data()),
                                          static_cast<qsizetype>(bytes.size())));
        }
    }
    if (cover.isNull()) {
        art_->clear();
        art_->hide();
    } else {
        art_->setPixmap(cover.scaledToHeight(kArtHeight, Qt::SmoothTransformation));
        art_->show();
    }
}

}  // namespace xpcog::app
