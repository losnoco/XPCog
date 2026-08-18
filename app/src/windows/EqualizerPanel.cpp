#include "EqualizerPanel.hpp"

#include "xpcog/core/audio/Equalizer.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <cmath>
#include <string>

namespace xpcog::app {
namespace {

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

EqualizerPanel::EqualizerPanel(Settings& settings, QWidget* parent)
    : QWidget(parent), settings_(settings) {
    auto* layout = new QVBoxLayout(this);

    // One column per band, plus the preamp on its own at the left, matching the
    // shape of Cog's Equalizer window. Sliders are deliberately narrow: 31 bands
    // is a lot of screen, and the alternative -- a scroll area the user has to
    // pan to reach 20 kHz -- makes a curve impossible to see as a curve.
    auto* columns = new QHBoxLayout;
    columns->setSpacing(2);

    sliders_.append(addBand(columns, tr("Pre"), "eqPreamp"));

    auto* separator = new QFrame;
    separator->setFrameShape(QFrame::VLine);
    separator->setFrameShadow(QFrame::Sunken);
    columns->addWidget(separator);

    const auto frequencies = Equalizer::bandFrequencies();
    const auto keys        = Equalizer::bandSettingsKeys();
    for (std::size_t band = 0; band < keys.size(); ++band) {
        sliders_.append(addBand(columns, frequencyLabel(frequencies[band]), keys[band]));
    }

    layout->addLayout(columns, 1);

    auto* flat = new QPushButton(tr("Flat"), this);
    flat->setToolTip(tr("Returns every band and the preamp to 0 dB, which makes the "
                        "equaliser bit-transparent again."));
    connect(flat, &QPushButton::clicked, this, &EqualizerPanel::flatten);

    auto* note = new QLabel(
        tr("31 bands at Cog's frequencies and Q, ±20 dB. Changes apply to the "
           "track already playing. A boost can clip; the preamp is the headroom "
           "for it."),
        this);
    note->setWordWrap(true);
    note->setEnabled(false);

    auto* footer = new QHBoxLayout;
    footer->addWidget(flat);
    footer->addWidget(note, 1);
    layout->addLayout(footer);
}

QSlider* EqualizerPanel::addBand(QHBoxLayout* columns, const QString& caption,
                                 const char* key) {
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
}

void EqualizerPanel::flatten() {
    // Each setValue emits valueChanged, so the settings and the engine follow
    // without this needing to know about either.
    for (QSlider* slider : sliders_) {
        slider->setValue(0);
    }
}

}  // namespace xpcog::app
