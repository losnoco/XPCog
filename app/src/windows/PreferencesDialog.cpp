#include "PreferencesDialog.hpp"

#include "Appearance.hpp"

#include "xpcog/core/audio/IAudioOutput.hpp"
#include "xpcog/core/audio/ReplayGain.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QIcon>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

namespace xpcog::app {
namespace {

/// A setting whose values are a closed set, so it deserves a named list rather
/// than a text box. The strings are Cog's stored values, unchanged.
struct Choice {
    const char* value;
    const char* label;
};

constexpr std::array kVolumeScalingChoices = {
    Choice{"none", QT_TRANSLATE_NOOP("PreferencesDialog", "None")},
    Choice{"volumeScale", QT_TRANSLATE_NOOP("PreferencesDialog", "Volume tag")},
    Choice{"soundcheck", QT_TRANSLATE_NOOP("PreferencesDialog", "iTunes Sound Check")},
    Choice{"trackGain", QT_TRANSLATE_NOOP("PreferencesDialog", "Track gain")},
    Choice{"trackGainWithPeak",
           QT_TRANSLATE_NOOP("PreferencesDialog", "Track gain, peak-limited")},
    Choice{"albumGain", QT_TRANSLATE_NOOP("PreferencesDialog", "Album gain")},
    Choice{"albumGainWithPeak",
           QT_TRANSLATE_NOOP("PreferencesDialog", "Album gain, peak-limited")},
};

constexpr std::array kResamplingChoices = {
    Choice{"quick", QT_TRANSLATE_NOOP("PreferencesDialog", "Quick")},
    Choice{"low", QT_TRANSLATE_NOOP("PreferencesDialog", "Low")},
    Choice{"medium", QT_TRANSLATE_NOOP("PreferencesDialog", "Medium")},
    Choice{"high", QT_TRANSLATE_NOOP("PreferencesDialog", "High")},
    Choice{"best", QT_TRANSLATE_NOOP("PreferencesDialog", "Best")},
};

/// The synthesisers `midiPlugin` can name, in Cog's own spelling.
///
/// Nuked OPL3 twice over -- id's DMX driver, once per instrument bank, and
/// Nuke.YKT's General MIDI one -- and then an emulated Roland. The OPL labels
/// are the drivers' own bank names (vendor/nuked-opl3), spelled out here rather
/// than read back from them so the dialog does not have to construct a
/// synthesiser to draw a combo box. SpessaSynth joins the list at its stage;
/// see docs/MIDI.md.
constexpr std::array kMidiSynthChoices = {
    Choice{"DOOM0", QT_TRANSLATE_NOOP("PreferencesDialog", "OPL3 — DMX default")},
    Choice{"DOOM1", QT_TRANSLATE_NOOP("PreferencesDialog", "OPL3 — DMX Doom")},
    Choice{"DOOM2", QT_TRANSLATE_NOOP("PreferencesDialog", "OPL3 — DMX Doom II")},
    Choice{"DOOM3", QT_TRANSLATE_NOOP("PreferencesDialog", "OPL3 — DMX Raptor")},
    Choice{"DOOM4", QT_TRANSLATE_NOOP("PreferencesDialog", "OPL3 — DMX Strife")},
    Choice{"DOOM5", QT_TRANSLATE_NOOP("PreferencesDialog", "OPL3 — DMXOPL")},
    Choice{"OPL3W0", QT_TRANSLATE_NOOP("PreferencesDialog", "OPL3 — General MIDI")},
    Choice{"Spessa", QT_TRANSLATE_NOOP("PreferencesDialog", "SoundFont — SpessaSynth")},
    Choice{"NukeSc55", QT_TRANSLATE_NOOP("PreferencesDialog", "Roland SC-55")},
};

[[nodiscard]] bool isTrue(const std::string& text) {
    // Cog's plist stores YES/NO; Settings accepts both those and true/false.
    return text == "1" || text == "true" || text == "YES";
}

/// Settings that already have a hand-written row in one of the panes above. The
/// generated pane skips them, so each setting is edited in exactly one place --
/// otherwise a curated combo box and a raw text box for the same key sit two
/// clicks apart, disagreeing about what the value should look like.
constexpr std::array kCuratedKeys = {
    // Playlist
    "alwaysStopAfterCurrent", "readCueSheetsInFolders",
    // Output
    "volumeScaling", "resampling", "enableHDCD", "halveDSDVolume",
    "outputDeviceId", "outputDeviceName", "exclusiveOutput",
    "enableFSurround",
    "enableFading", "suspendOutputOnPause",
    // MIDI
    "midiPlugin", "midiRomPath", "soundFontPath", "synthSampleRate",
    "synthDefaultSeconds",
    "synthDefaultFadeSeconds", "synthDefaultLoopCount",
    // Appearance
    // floatingMiniWindow is deliberately absent: the mini player carries its
    // own checkbox for it and applies it as it is clicked, so a second one
    // here would be the copy that does nothing until the window is rebuilt.
    //
    // closeToTray is listed on every platform even though macOS builds draw no
    // row for it. It does nothing there -- see buildAppearancePane() -- and a
    // key with no effect belongs in Advanced even less than it belongs in
    // Appearance, so this list keeps it out of both.
    "widgetStyle", "closeToTray",
    // Spectrum
    "spectrumBarColor", "spectrumDotColor", "spectrumFreqMode",
    "spectrumFloorDb", "spectrumShowPeaks",
};

/// Not settings at all, but internal state that happens to live in the same
/// store. Shown, because the generated pane's whole point is that nothing is
/// hidden, but not editable: settingsSchemaVersion drives
/// Settings::applyMigrations(), so typing into it makes migrations re-run or be
/// skipped, and nothing about a spin box suggests that. UserDefaultURLsKey is
/// the Open URL history -- a newline-separated list the dialog maintains, where
/// a hand edit can only produce entries that will not parse.
constexpr std::array kInternalKeys = {"settingsSchemaVersion", "UserDefaultURLsKey"};

[[nodiscard]] bool contains(std::span<const char* const> keys, std::string_view key) {
    return std::ranges::any_of(keys,
                               [key](const char* candidate) { return key == candidate; });
}

/// Adds curated rows to one pane's form.
///
/// A struct rather than a lambda per pane because there are now four panes
/// wanting the same four kinds of row, and four copies of "read the raw value,
/// write it back, announce it" is four chances for one of them to forget the
/// announcement. It reports changes through a callback rather than emitting the
/// dialog's signal itself, which is not something a helper should be reaching
/// into.
class RowBuilder {
public:
    using Announce = std::function<void(const char*)>;

    RowBuilder(Settings& settings, QFormLayout* layout, Announce announce)
        : settings_(&settings), layout_(layout), announce_(std::move(announce)) {}

    void choice(const QString& label, const char* key,
                std::span<const Choice> choices) const {
        auto* box = new QComboBox;
        for (const Choice& option : choices) {
            box->addItem(QCoreApplication::translate("PreferencesDialog", option.label),
                         QString::fromLatin1(option.value));
        }
        // A stored value this build does not offer -- a settings file carried
        // over from a macOS Cog naming an AudioUnit, say -- selects the first
        // entry, which is the same fallback the decoder reading it applies.
        const auto current = QString::fromStdString(settings_->rawValue(key));
        box->setCurrentIndex(std::max(0, box->findData(current)));

        QObject::connect(box, &QComboBox::currentIndexChanged, box,
                         [settings = settings_, announce = announce_, box, key](int) {
                             settings->setRawValue(
                                 key, box->currentData().toString().toStdString());
                             announce(key);
                         });
        layout_->addRow(label, box);
    }

    void toggle(const QString& label, const char* key,
                const QString& hint = {}) const {
        auto* box = new QCheckBox(label);
        box->setChecked(isTrue(settings_->rawValue(key)));
        if (!hint.isEmpty()) {
            box->setToolTip(hint);
        }
        QObject::connect(box, &QCheckBox::toggled, box,
                         [settings = settings_, announce = announce_, key](bool on) {
                             settings->setRawValue(key, on ? "true" : "false");
                             announce(key);
                         });
        layout_->addRow(QString{}, box);
    }

    void number(const QString& label, const char* key, int minimum, int maximum,
                const QString& suffix = {}) const {
        auto* box = new QSpinBox;
        box->setRange(minimum, maximum);
        box->setSuffix(suffix);
        box->setValue(QString::fromStdString(settings_->rawValue(key)).toInt());
        QObject::connect(box, &QSpinBox::valueChanged, box,
                         [settings = settings_, announce = announce_, key](int value) {
                             settings->setRawValue(key, std::to_string(value));
                             announce(key);
                         });
        layout_->addRow(label, box);
    }

    void seconds(const QString& label, const char* key, double maximum) const {
        auto* box = new QDoubleSpinBox;
        box->setRange(0.0, maximum);
        box->setDecimals(1);
        box->setSuffix(QCoreApplication::translate("PreferencesDialog", " s"));
        box->setValue(QString::fromStdString(settings_->rawValue(key)).toDouble());
        QObject::connect(
            box, &QDoubleSpinBox::valueChanged, box,
            [settings = settings_, announce = announce_, key](double value) {
                settings->setRawValue(key, std::to_string(value));
                announce(key);
            });
        layout_->addRow(label, box);
    }

    /// A path the listener has to supply, with the two ways of choosing one.
    ///
    /// Two buttons rather than one because the thing being named may be either
    /// a folder or a file, and no file dialog on any platform offers both at
    /// once. The line edit accepts a typed or pasted path either way.
    /// One file, chosen or typed. For a setting that names a single file and
    /// nothing else -- a SoundFont bank -- where the folder button a path() row
    /// carries would only offer something that cannot be right.
    void file(const QString& label, const char* key, const QString& filter,
              QWidget* parent) const {
        auto*      edit   = makeEdit(key);
        const auto choose = chooser(edit);

        auto* browse = new QPushButton(
            QCoreApplication::translate("PreferencesDialog", "Choose…"));
        QObject::connect(browse, &QPushButton::clicked, browse,
                         [parent, choose, label, filter] {
                             choose(QFileDialog::getOpenFileName(parent, label, {},
                                                                 filter));
                         });

        addRow(label, edit, {browse});
    }

    void path(const QString& label, const char* key, const QString& archiveFilter,
              QWidget* parent) const {
        auto*      edit   = makeEdit(key);
        const auto choose = chooser(edit);

        auto* folder = new QPushButton(
            QCoreApplication::translate("PreferencesDialog", "Folder…"));
        QObject::connect(folder, &QPushButton::clicked, folder, [parent, choose, label] {
            choose(QFileDialog::getExistingDirectory(parent, label));
        });

        auto* archive = new QPushButton(
            QCoreApplication::translate("PreferencesDialog", "Archive…"));
        QObject::connect(archive, &QPushButton::clicked, archive,
                         [parent, choose, label, archiveFilter] {
                             choose(QFileDialog::getOpenFileName(parent, label, {},
                                                                 archiveFilter));
                         });

        addRow(label, edit, {folder, archive});
    }

    /// A line edit that writes its setting when it loses focus or takes Return.
    [[nodiscard]] QLineEdit* makeEdit(const char* key) const {
        auto* edit = new QLineEdit(QString::fromStdString(settings_->rawValue(key)));
        auto* settings = settings_;
        auto  announce = announce_;
        QObject::connect(edit, &QLineEdit::editingFinished, edit,
                         [settings, announce, edit, key] {
                             settings->setRawValue(key, edit->text().toStdString());
                             announce(key);
                         });
        return edit;
    }

    /// What a chooser button does with whatever the dialog returned. Empty is
    /// the cancelled dialog, and leaves the setting alone.
    ///
    /// Spelled out rather than deduced: a member function with an `auto` return
    /// type cannot be called from another member's body, because every body in a
    /// class is parsed after the class is complete and the deduction has not
    /// happened at the point of the call.
    [[nodiscard]] static std::function<void(const QString&)> chooser(QLineEdit* edit) {
        return [edit](const QString& chosen) {
            if (!chosen.isEmpty()) {
                edit->setText(QDir::toNativeSeparators(chosen));
                emit edit->editingFinished();
            }
        };
    }

    void addRow(const QString& label, QLineEdit* edit,
                std::initializer_list<QPushButton*> buttons) const {
        auto* row    = new QWidget;
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(edit, 1);
        for (QPushButton* button : buttons) {
            layout->addWidget(button);
        }
        layout_->addRow(label, row);
    }

    void note(const QString& text) const {
        auto* label = new QLabel(text);
        label->setWordWrap(true);
        label->setEnabled(false);
        layout_->addRow(label);
    }

private:
    // Held by pointer, and every connection captures the pointer rather than
    // `this`: a RowBuilder is a local in the function that builds a pane and is
    // gone by the time anyone clicks anything, while the settings object and the
    // callback both outlive the dialog.
    Settings*    settings_;
    QFormLayout* layout_;
    Announce     announce_;
};

[[nodiscard]] bool hasCuratedRow(std::string_view key) {
    // Every equaliser key -- eqPreamp and the 31 bands -- has a slider of its
    // own in the equaliser dock, so they are matched by prefix rather than
    // listed twice. Still excluded now the dock owns them rather than a pane
    // here: 32 raw spin boxes in Advanced would be a second, worse equaliser.
    if (key.starts_with("eq")) {
        return true;
    }
    return contains(kCuratedKeys, key);
}

}  // namespace

PreferencesDialog::PreferencesDialog(Settings& settings, QWidget* parent)
    : QDialog(parent), settings_(settings) {
    setWindowTitle(tr("Preferences"));
    resize(620, 440);

    auto* panes = new QStackedWidget(this);
    auto* sidebar = new QListWidget(this);
    sidebar->setMaximumWidth(150);

    const auto addPane = [&](const QString& name, QWidget* pane) {
        sidebar->addItem(name);
        panes->addWidget(pane);
    };
    // Cog's order, with its unported panes taken out rather than reshuffled.
    addPane(tr("Playlist"), buildPlaylistPane());
    addPane(tr("Output"), buildOutputPane());
    addPane(tr("Appearance"), buildAppearancePane());
    addPane(tr("MIDI"), buildMidiPane());
    addPane(tr("Spectrum"), buildSpectrumPane());
    addPane(tr("Advanced"), buildAdvancedPane());

    connect(sidebar, &QListWidget::currentRowChanged, panes,
            &QStackedWidget::setCurrentIndex);
    sidebar->setCurrentRow(0);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);

    auto* columns = new QHBoxLayout;
    columns->addWidget(sidebar);
    columns->addWidget(panes, 1);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(columns, 1);
    layout->addWidget(buttons);
}

std::function<void(const char*)> PreferencesDialog::changeNotifier() {
    return [this](const char* key) { emit settingChanged(QString::fromLatin1(key)); };
}

QWidget* PreferencesDialog::buildPlaylistPane() {
    auto* pane   = new QWidget(this);
    auto* layout = new QFormLayout(pane);
    const RowBuilder row{settings_, layout, changeNotifier()};

    row.toggle(tr("Stop after every track"), "alwaysStopAfterCurrent");
    row.toggle(tr("Read cue sheets when adding folders"), "readCueSheetsInFolders");

    return pane;
}

QWidget* PreferencesDialog::buildOutputPane() {
    auto* pane   = new QWidget(this);
    auto* layout = new QFormLayout(pane);
    const RowBuilder row{settings_, layout, changeNotifier()};

    // The device list is read once, when this pane is built. Enumerating spins
    // up the backend's context, and a picker that re-enumerated on every repaint
    // would do that while the listener is dragging a slider next to it.
    auto* deviceBox = new QComboBox;
    deviceBox->addItem(tr("System default"), QString{});
    const std::string chosenId = settings_.rawValue("outputDeviceId");
    for (const DeviceInfo& device : enumerateOutputDevices()) {
        const QString id = QString::fromStdString(device.id);
        deviceBox->addItem(QString::fromStdString(device.name), id);
        if (device.isDefault) {
            // Named rather than left to be guessed at: "System default" and the
            // device it currently resolves to are different rows, and which one
            // is chosen matters when headphones are plugged in later.
            deviceBox->setItemText(deviceBox->count() - 1,
                                   tr("%1 (current default)")
                                       .arg(QString::fromStdString(device.name)));
        }
    }

    // A device that is not here right now is still the choice: the engine falls
    // back to the default until it returns, and the setting is left alone. So
    // the row is shown rather than silently reset to "System default".
    int chosenRow = deviceBox->findData(QString::fromStdString(chosenId));
    if (chosenRow < 0 && !chosenId.empty()) {
        const std::string name = settings_.rawValue("outputDeviceName");
        deviceBox->addItem(tr("%1 (not connected)")
                               .arg(QString::fromStdString(name.empty() ? chosenId : name)),
                           QString::fromStdString(chosenId));
        chosenRow = deviceBox->count() - 1;
    }
    deviceBox->setCurrentIndex(chosenRow < 0 ? 0 : chosenRow);

    {
        auto* settings = &settings_;
        auto  announce = changeNotifier();
        QObject::connect(deviceBox, &QComboBox::currentIndexChanged, deviceBox,
                         [settings, announce, deviceBox](int index) {
                             const QString id = deviceBox->itemData(index).toString();
                             settings->setRawValue("outputDeviceId", id.toStdString());
                             // The name travels with it, as the fallback match
                             // for a device that comes back under a new id.
                             settings->setRawValue(
                                 "outputDeviceName",
                                 id.isEmpty() ? std::string{}
                                              : deviceBox->itemText(index).toStdString());
                             announce("outputDeviceId");
                         });
    }
    layout->addRow(tr("Output device"), deviceBox);

    row.toggle(tr("Take the device exclusively"), "exclusiveOutput",
               tr("Plays at the file's own rate and format instead of the system "
                  "mixer's, and silences everything else on the machine while it "
                  "does. Falls back to sharing when the device or the platform "
                  "will not allow it."));

    row.choice(tr("Volume scaling"), "volumeScaling", kVolumeScalingChoices);
    row.choice(tr("Resampler quality"), "resampling", kResamplingChoices);

    row.toggle(tr("Decode HDCD"), "enableHDCD",
               tr("Only affects 16-bit 44.1 kHz stereo lossless material, and is "
                  "bit-transparent on files carrying no HDCD codes."));
    row.toggle(tr("Halve the volume of DSD"), "halveDSDVolume",
               tr("DSD is converted with a filter whose gain puts half "
                  "modulation — as loud as most SACDs go — at full scale. Turn "
                  "this on if a recording that goes louder is clipping."));
    row.toggle(tr("FreeSurround stereo-to-surround upmix"), "enableFSurround");
    row.toggle(tr("Fade on seek and stop"), "enableFading");
    row.toggle(tr("Release the audio device while paused"), "suspendOutputOnPause");

    row.note(tr("Volume scaling and resampler quality take effect on the next "
                "track. The device and exclusive mode move whatever is playing "
                "on to the new device, with a short break while it opens."));

    return pane;
}

QWidget* PreferencesDialog::buildMidiPane() {
    auto* pane   = new QWidget(this);
    auto* layout = new QFormLayout(pane);
    const RowBuilder row{settings_, layout, changeNotifier()};

    row.choice(tr("Synthesiser"), "midiPlugin", kMidiSynthChoices);
    row.file(tr("SoundFont"), "soundFontPath",
             tr("SoundFont banks (*.sf2 *.sf3 *.sf2pack *.dls *.sflist *.json);;"
                "All files (*)"),
             pane);
    row.path(tr("SC-55 ROMs"), "midiRomPath",
             tr("Archives (*.zip *.rar *.7z);;All files (*)"), pane);

    // Cog's clamps, and its labels. The sample rate is the rate a synthesiser
    // renders at rather than the rate the file plays at -- there is no such
    // thing as the second one for a score.
    row.number(tr("Sample rate"), "synthSampleRate", 8000, 192000, tr(" Hz"));
    row.seconds(tr("Default play time"), "synthDefaultSeconds", 3600.0);
    row.seconds(tr("Default fade time"), "synthDefaultFadeSeconds", 60.0);
    row.number(tr("Default loop count"), "synthDefaultLoopCount", 0, 10);

    row.note(tr("SpessaSynth plays a SoundFont bank, and has no sound of its own "
                "without one — any .sf2, .sf3 or .dls will do. A file that has "
                "a bank of its own beside it is played with that one instead, "
                "whatever is chosen here."));
    row.note(tr("The Roland needs its five ROM files, which are not something "
                "this player can supply. Name either the folder holding them or "
                "the archive they came in — they are recognised by content, so "
                "nothing has to be renamed. Without them a MIDI file still "
                "plays, on the OPL3."));
    row.note(tr("The sample rate and the defaults below it apply to every "
                "synthesised format, not only MIDI — a game music rip has no "
                "length of its own either. The Roland ignores the sample rate: "
                "it renders at the rate its hardware ran at and nothing else."));

    return pane;
}

QWidget* PreferencesDialog::buildAppearancePane() {
    auto* pane   = new QWidget(this);
    auto* layout = new QFormLayout(pane);

    auto* box = new QComboBox;
    for (const appearance::StyleOption& option : appearance::availableStyles()) {
        box->addItem(option.label, option.key);
    }
    // The stored name may be one this platform does not have -- a settings file
    // that has travelled -- in which case findData reports -1 and the box falls
    // back to "System default", which is exactly what applyStyle() will have
    // done with it too.
    const auto current = QString::fromStdString(settings_.WidgetStyle());
    box->setCurrentIndex(std::max(0, box->findData(current)));

    connect(box, &QComboBox::currentIndexChanged, this, [this, box](int) {
        const QString key = box->currentData().toString();
        settings_.setWidgetStyle(key.toStdString());
        // Applied here and now. This is the one setting whose entire purpose is
        // to be looked at, and a label describing a look is no substitute for
        // seeing it -- everything else in this dialog is read by the engine on
        // the next track.
        appearance::applyStyle(key);
        emit settingChanged(QStringLiteral("widgetStyle"));
    });
    layout->addRow(tr("Style"), box);

    // Absent on macOS, where the question it asks does not arise. Closing the
    // window there hides it and leaves XPCog running -- unconditionally, because
    // that is the platform's convention rather than a preference (see
    // MainWindow::closeEvent) -- and there is no tray icon to hide to in any
    // case. A checkbox offering to choose something already chosen is the same
    // fault as one that does nothing; this is that fault, not a missing feature.
    //
    // The key stays in settings.def and in kCuratedKeys, so a settings file that
    // has travelled from Windows keeps its value and Advanced does not offer a
    // raw box for it here.
#ifndef Q_OS_MACOS
    auto* closeToTray = new QCheckBox(tr("Closing the window keeps XPCog running"));
    closeToTray->setChecked(settings_.CloseToTray());
    // Offered only where there is somewhere to hide to. Without a notification area
    // this would hide the window with no way to bring it back, so the setting is
    // ignored at the call site *and* disabled here -- a checkbox that does nothing
    // is worse than an absent one, and this at least says why.
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        closeToTray->setEnabled(false);
        closeToTray->setToolTip(
            tr("This session has no notification area to keep XPCog in."));
    }
    connect(closeToTray, &QCheckBox::toggled, this, [this](bool on) {
        settings_.setCloseToTray(on);
        emit settingChanged(QStringLiteral("closeToTray"));
    });
    layout->addRow(QString{}, closeToTray);
#endif

    auto* note = new QLabel(
        tr("On Windows, \"Windows 11\" is the modern chrome and \"Windows Vista\" "
           "the older look. \"Fusion\" is the same on every platform."),
        pane);
    note->setWordWrap(true);
    note->setEnabled(false);
    layout->addRow(note);

    return pane;
}

QWidget* PreferencesDialog::colorRow(const QString& label, const std::string& stored,
                                     std::function<void(const std::string&)> store) {
    auto* row    = new QWidget(this);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);

    // An unparseable stored value -- notably an imported Cog colour, which is an
    // archived NSColor rather than a string -- shows as the setting's own default
    // rather than as black. Falling back to a default-constructed QColor would put a
    // black bar on a near-black background, which looks like the display is broken.
    QColor initial(QString::fromStdString(stored));
    if (!initial.isValid()) {
        initial = QColor(QStringLiteral("#ff8000"));
    }

    auto* button = new QPushButton(row);
    auto* swatch = new QLabel(label, row);

    const auto show = [button](const QColor& colour) {
        QPixmap pixmap(14, 14);
        pixmap.fill(colour);
        button->setIcon(QIcon(pixmap));
        button->setText(colour.name());
    };
    show(initial);

    connect(button, &QPushButton::clicked, this,
            [this, button, show, store = std::move(store)] {
                const QColor current(button->text());
                const QColor chosen =
                    QColorDialog::getColor(current, this, tr("Choose a colour"));
                if (!chosen.isValid()) {
                    return;  // cancelled
                }
                show(chosen);
                // Lower case and six digits, which is the form the setting
                // documents and what QColor::name() produces.
                store(chosen.name().toStdString());
            });

    layout->addWidget(swatch);
    layout->addStretch(1);
    layout->addWidget(button);
    return row;
}

QWidget* PreferencesDialog::buildSpectrumPane() {
    auto* pane   = new QWidget(this);
    auto* layout = new QFormLayout(pane);

    // Bands. Cog's two analyser modes, stored in its own key: false is the note
    // scale, true the even spacing. A combo rather than a checkbox because
    // "Frequency mode: off" says nothing about what you get instead.
    auto* bands = new QComboBox;
    bands->addItem(tr("Musical notes (one bar per semitone)"), false);
    bands->addItem(tr("Even frequency spacing"), true);
    bands->setCurrentIndex(settings_.SpectrumFreqMode() ? 1 : 0);
    connect(bands, &QComboBox::currentIndexChanged, this, [this, bands](int) {
        settings_.setSpectrumFreqMode(bands->currentData().toBool());
        emit settingChanged(QStringLiteral("spectrumFreqMode"));
    });
    layout->addRow(tr("Bands"), bands);

    layout->addRow(colorRow(tr("Bar colour"), settings_.SpectrumBarColor(),
                            [this](const std::string& hex) {
                                settings_.setSpectrumBarColor(hex);
                                emit settingChanged(
                                    QStringLiteral("spectrumBarColor"));
                            }));
    layout->addRow(colorRow(tr("Peak colour"), settings_.SpectrumDotColor(),
                            [this](const std::string& hex) {
                                settings_.setSpectrumDotColor(hex);
                                emit settingChanged(
                                    QStringLiteral("spectrumDotColor"));
                            }));

    auto* peaks = new QCheckBox(tr("Show peak markers"));
    peaks->setChecked(settings_.SpectrumShowPeaks());
    connect(peaks, &QCheckBox::toggled, this, [this](bool on) {
        settings_.setSpectrumShowPeaks(on);
        emit settingChanged(QStringLiteral("spectrumShowPeaks"));
    });
    layout->addRow(QString{}, peaks);

    // The floor. Cog fixes this at -80; the range here is wide enough to be useful
    // at both ends and stops short of zero, where there would be nothing to draw.
    auto* floorDb = new QSpinBox;
    floorDb->setRange(-120, -20);
    floorDb->setSingleStep(5);
    floorDb->setSuffix(tr(" dB"));
    floorDb->setValue(static_cast<int>(std::lround(settings_.SpectrumFloorDb())));
    connect(floorDb, &QSpinBox::valueChanged, this, [this](int value) {
        settings_.setSpectrumFloorDb(static_cast<double>(value));
        emit settingChanged(QStringLiteral("spectrumFloorDb"));
    });
    layout->addRow(tr("Quietest level shown"), floorDb);

    auto* note = new QLabel(
        tr("Bars sit on semitones from C0, so a spectrum of music lines up with "
           "the notes being played. Below a few hundred hertz several bars share "
           "one analysis bin and move together — that is the resolution of the "
           "window, not a fault."),
        pane);
    note->setWordWrap(true);
    note->setEnabled(false);
    layout->addRow(note);

    return pane;
}

QWidget* PreferencesDialog::buildAdvancedPane() {
    auto* pane   = new QWidget(this);
    auto* layout = new QFormLayout(pane);

    // Generated from settings.def. A setting added there shows up here without
    // any edit to this file, which is what stops a setting existing in the
    // engine but being unreachable from the UI. As settings graduate to a
    // hand-written row in one of the panes above, they leave this list via
    // kCuratedKeys rather than being duplicated by it.
    for (const Settings::Desc& descriptor : Settings::all()) {
        if (hasCuratedRow(descriptor.key)) {
            continue;
        }

        const auto key   = std::string{descriptor.key};
        const auto label = QString::fromUtf8(descriptor.ident.data(),
                                             static_cast<qsizetype>(descriptor.ident.size()));
        const auto value = settings_.rawValue(key);

        QWidget* editor = nullptr;
        if (descriptor.type == "bool") {
            auto* box = new QCheckBox;
            box->setChecked(isTrue(value));
            connect(box, &QCheckBox::toggled, this, [this, key](bool on) {
                settings_.setRawValue(key, on ? "true" : "false");
                emit settingChanged(QString::fromStdString(key));
            });
            editor = box;
        } else if (descriptor.type == "int") {
            auto* box = new QSpinBox;
            box->setRange(-1000000, 1000000);
            box->setValue(QString::fromStdString(value).toInt());
            connect(box, &QSpinBox::valueChanged, this, [this, key](int number) {
                settings_.setRawValue(key, std::to_string(number));
                emit settingChanged(QString::fromStdString(key));
            });
            editor = box;
        } else if (descriptor.type == "double") {
            auto* box = new QDoubleSpinBox;
            box->setDecimals(3);
            box->setRange(-1000000.0, 1000000.0);
            box->setValue(QString::fromStdString(value).toDouble());
            connect(box, &QDoubleSpinBox::valueChanged, this, [this, key](double number) {
                settings_.setRawValue(key, std::to_string(number));
                emit settingChanged(QString::fromStdString(key));
            });
            editor = box;
        } else {
            auto* edit = new QLineEdit(QString::fromStdString(value));
            connect(edit, &QLineEdit::editingFinished, this, [this, edit, key] {
                settings_.setRawValue(key, edit->text().toStdString());
                emit settingChanged(QString::fromStdString(key));
            });
            editor = edit;
        }

        if (contains(kInternalKeys, descriptor.key)) {
            editor->setEnabled(false);
            editor->setToolTip(
                tr("Maintained automatically, and not meant to be edited."));
        }
        layout->addRow(label, editor);
    }

    // Scrolled, because the list grows with every milestone and a fixed pane
    // would quietly start clipping.
    auto* scroller = new QScrollArea(this);
    scroller->setWidget(pane);
    scroller->setWidgetResizable(true);
    return scroller;
}

}  // namespace xpcog::app
