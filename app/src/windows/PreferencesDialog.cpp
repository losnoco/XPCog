#include "PreferencesDialog.hpp"

#include "Appearance.hpp"

#include "xpcog/core/audio/Equalizer.hpp"
#include "xpcog/core/audio/ReplayGain.hpp"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QIcon>
#include <QDialogButtonBox>
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

[[nodiscard]] bool isTrue(const std::string& text) {
    // Cog's plist stores YES/NO; Settings accepts both those and true/false.
    return text == "1" || text == "true" || text == "YES";
}

/// Settings that already have a hand-written row in one of the panes above. The
/// generated pane skips them, so each setting is edited in exactly one place --
/// otherwise a curated combo box and a raw text box for the same key sit two
/// clicks apart, disagreeing about what the value should look like.
constexpr std::array kCuratedKeys = {
    "volumeScaling",   "resampling",             "enableHDCD",
    "enableFading",    "suspendOutputOnPause",   "alwaysStopAfterCurrent",
    "readCueSheetsInFolders", "widgetStyle",           "closeToTray",
    "spectrumBarColor", "spectrumDotColor",     "spectrumFreqMode",
    "spectrumFloorDb",  "spectrumShowPeaks",
};

/// Not settings at all, but internal state that happens to live in the same
/// store. Shown, because the generated pane's whole point is that nothing is
/// hidden, but not editable: settingsSchemaVersion drives
/// Settings::applyMigrations(), so typing into it makes migrations re-run or be
/// skipped, and nothing about a spin box suggests that.
constexpr std::array kInternalKeys = {"settingsSchemaVersion"};

[[nodiscard]] bool contains(std::span<const char* const> keys, std::string_view key) {
    return std::ranges::any_of(keys,
                               [key](const char* candidate) { return key == candidate; });
}

[[nodiscard]] bool hasCuratedRow(std::string_view key) {
    // Every equaliser key -- eqPreamp and the 31 bands -- has a slider of its
    // own, so they are matched by prefix rather than listed twice.
    if (key.starts_with("eq")) {
        return true;
    }
    return contains(kCuratedKeys, key);
}

/// Cog's slider range, in dB, for the bands and the preamp alike.
constexpr int kEqRangeDb = 20;
/// Sliders are integers, so dB is carried in tenths.
constexpr int kEqScale = 10;

/// "20", "31.5", "1k", "20k" -- short enough to sit under a narrow slider.
[[nodiscard]] QString frequencyLabel(double hertz) {
    if (hertz < 1000.0) {
        return QString::number(hertz, 'g', 3);
    }
    const double kilohertz = hertz / 1000.0;
    return QString::number(kilohertz, 'g', 2) + QStringLiteral("k");
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
    addPane(tr("Playback"), buildPlaybackPane());
    addPane(tr("Equalizer"), buildEqualizerPane());
    addPane(tr("Appearance"), buildAppearancePane());
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

QWidget* PreferencesDialog::buildPlaybackPane() {
    auto* pane   = new QWidget(this);
    auto* layout = new QFormLayout(pane);

    const auto addChoice = [this, layout](const QString& label, const char* key,
                                          std::span<const Choice> choices) {
        auto* box = new QComboBox;
        for (const Choice& choice : choices) {
            box->addItem(tr(choice.label), QString::fromLatin1(choice.value));
        }
        const auto current = QString::fromStdString(settings_.rawValue(key));
        box->setCurrentIndex(std::max(0, box->findData(current)));

        connect(box, &QComboBox::currentIndexChanged, this, [this, box, key](int) {
            settings_.setRawValue(key, box->currentData().toString().toStdString());
            emit settingChanged(QString::fromLatin1(key));
        });
        layout->addRow(label, box);
    };

    addChoice(tr("Volume scaling"), "volumeScaling", kVolumeScalingChoices);
    addChoice(tr("Resampler quality"), "resampling", kResamplingChoices);

    const auto addToggle = [this, layout](const QString& label, const char* key,
                                          const QString& hint = {}) {
        auto* box = new QCheckBox(label);
        box->setChecked(isTrue(settings_.rawValue(key)));
        if (!hint.isEmpty()) {
            box->setToolTip(hint);
        }
        connect(box, &QCheckBox::toggled, this, [this, key](bool on) {
            settings_.setRawValue(key, on ? "true" : "false");
            emit settingChanged(QString::fromLatin1(key));
        });
        layout->addRow(QString{}, box);
    };

    addToggle(tr("Decode HDCD"), "enableHDCD",
              tr("Only affects 16-bit 44.1 kHz stereo lossless material, and is "
                 "bit-transparent on files carrying no HDCD codes."));
    addToggle(tr("Fade on seek and stop"), "enableFading");
    addToggle(tr("Release the audio device while paused"), "suspendOutputOnPause");
    addToggle(tr("Stop after every track"), "alwaysStopAfterCurrent");
    addToggle(tr("Read cue sheets when adding folders"), "readCueSheetsInFolders");

    auto* note = new QLabel(
        tr("Volume scaling and resampler quality take effect on the next track."),
        pane);
    note->setWordWrap(true);
    note->setEnabled(false);
    layout->addRow(note);

    return pane;
}

QWidget* PreferencesDialog::buildEqualizerPane() {
    auto* pane   = new QWidget(this);
    auto* layout = new QVBoxLayout(pane);

    // One column per band, plus the preamp on its own at the left, matching the
    // shape of Cog's Equalizer window. Sliders are deliberately narrow: 31 bands
    // is a lot of screen, and the alternative -- a scroll area the user has to
    // pan to reach 20 kHz -- makes a curve impossible to see as a curve.
    auto* columns = new QHBoxLayout;
    columns->setSpacing(2);

    // Every slider reads and writes settings by key, so nothing here has to know
    // 31 accessor names, and the band-to-key pairing stays where the DSP defines
    // it.
    const auto addSlider = [this, columns](const QString& caption, const char* key) {
        auto* column = new QVBoxLayout;
        column->setSpacing(2);

        auto* readout = new QLabel;
        readout->setAlignment(Qt::AlignHCenter);

        auto* slider = new QSlider(Qt::Vertical);
        slider->setRange(-kEqRangeDb * kEqScale, kEqRangeDb * kEqScale);
        slider->setFixedWidth(18);
        slider->setToolTip(caption);

        const double stored = QString::fromStdString(settings_.rawValue(key)).toDouble();
        slider->setValue(static_cast<int>(std::lround(stored * kEqScale)));

        const auto show = [readout](int value) {
            readout->setText(QString::number(static_cast<double>(value) / kEqScale, 'f', 1));
        };
        show(slider->value());

        connect(slider, &QSlider::valueChanged, this, [this, key, show](int value) {
            show(value);
            const double decibels = static_cast<double>(value) / kEqScale;
            settings_.setRawValue(key, std::to_string(decibels));
            // Named so MainWindow can tell an equaliser change from any other and
            // ask the engine to re-read the chain mid-track.
            emit settingChanged(QString::fromLatin1(key));
        });

        auto* label = new QLabel(caption);
        label->setAlignment(Qt::AlignHCenter);

        column->addWidget(readout);
        column->addWidget(slider, 1, Qt::AlignHCenter);
        column->addWidget(label);
        columns->addLayout(column);
        return slider;
    };

    QList<QSlider*> sliders;
    sliders.append(addSlider(tr("Pre"), "eqPreamp"));

    auto* separator = new QFrame;
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Sunken);
    columns->addWidget(separator);

    const auto frequencies = Equalizer::bandFrequencies();
    const auto keys        = Equalizer::bandSettingsKeys();
    for (std::size_t band = 0; band < keys.size(); ++band) {
        sliders.append(addSlider(frequencyLabel(frequencies[band]), keys[band]));
    }

    layout->addLayout(columns, 1);

    auto* flat = new QPushButton(tr("Flat"), pane);
    flat->setToolTip(tr("Returns every band and the preamp to 0 dB, which makes the "
                        "equaliser bit-transparent again."));
    connect(flat, &QPushButton::clicked, this, [sliders] {
        // Each setValue emits valueChanged, so the settings and the engine follow
        // without this needing to know about either.
        for (QSlider* slider : sliders) {
            slider->setValue(0);
        }
    });

    auto* note = new QLabel(
        tr("31 bands at Cog's frequencies and Q, ±20 dB. Changes apply to the "
           "track already playing. A boost can clip; the preamp is the headroom "
           "for it."),
        pane);
    note->setWordWrap(true);
    note->setEnabled(false);

    auto* footer = new QHBoxLayout;
    footer->addWidget(flat);
    footer->addStretch(1);
    layout->addLayout(footer);
    layout->addWidget(note);

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

    auto* note = new QLabel(
        tr("The list is what this build of Qt offers. On Windows, \"Windows 11\" "
           "is the WinUI-style chrome and \"Windows Vista\" the older Win32 look; "
           "\"Fusion\" is Qt's own, and identical on every platform."),
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
        tr("The note scale is Cog's own: bars sit on semitones from C0, so a "
           "spectrum of music lines up with the notes being played. Below a few "
           "hundred hertz several bars share one analysis bin and move together — "
           "that is the resolution of the window, not a fault.\n\n"
           "Colours import from a Cog preferences file by key but not by value: "
           "Cog stores them in an archived Mac format, so an import leaves these "
           "at Cog's own defaults."),
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
                tr("Maintained by XPCog. Changing it would make the settings "
                   "migrations re-run or be skipped."));
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
