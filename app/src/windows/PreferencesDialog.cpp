#include "PreferencesDialog.hpp"

#include "Appearance.hpp"

#include "xpcog/core/audio/ReplayGain.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
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
    "readCueSheetsInFolders", "widgetStyle",
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
    addPane(tr("Appearance"), buildAppearancePane());
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

QWidget* PreferencesDialog::buildAdvancedPane() {
    auto* pane   = new QWidget(this);
    auto* layout = new QFormLayout(pane);

    // Generated from settings.def. A setting added there shows up here without
    // any edit to this file, which is what stops a setting existing in the
    // engine but being unreachable from the UI. As settings graduate to a
    // hand-written row in one of the panes above, they leave this list via
    // kCuratedKeys rather than being duplicated by it.
    for (const Settings::Desc& descriptor : Settings::all()) {
        if (contains(kCuratedKeys, descriptor.key)) {
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
