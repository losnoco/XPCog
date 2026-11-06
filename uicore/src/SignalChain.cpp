#include "SignalChain.hpp"

#include "Translations.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace xpcog::app {

namespace {

constexpr const char* kSeparator = " \xC2\xB7 ";  // a middle dot

/// The catalogue's substitution knows no floats, on purpose; see Translations.hpp.
[[nodiscard]] std::string number(double value, int places, bool withSign = false) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), withSign ? "%+.*f" : "%.*f", places, value);
    return buffer;
}

[[nodiscard]] std::string rateText(double hertz) {
    return trf("%s kHz", number(hertz / 1000.0, 1));
}

[[nodiscard]] std::string channelsText(std::uint32_t channels) {
    return fmt(trn("%u channel", "%u channels", channels), static_cast<unsigned>(channels));
}

[[nodiscard]] std::string formatText(const AudioFormat& format) {
    std::string text;
    if (format.bitsPerSample > 0) {
        text = fmt(tr("%u-bit"), static_cast<unsigned>(format.bitsPerSample)) + kSeparator;
    }
    return text + rateText(format.sampleRate) + kSeparator + channelsText(format.channels);
}

}  // namespace

SignalChain describeSignal(const SignalReport& report) {
    SignalChain chain;
    if (!report.active) {
        chain.summary = tr("Nothing playing");
        return chain;
    }
    chain.active = true;

    using State = SignalStage::State;
    const auto add = [&chain](std::string name, State state, std::string detail) {
        chain.stages.push_back({std::move(name), state, std::move(detail)});
    };

    // The decoder.
    {
        std::string detail = formatText(report.source.format);
        if (!report.source.codec.empty()) {
            detail = report.source.codec + kSeparator + detail;
        }
        add(tr("Decoder"), State::Passing, std::move(detail));
    }

    if (report.hdcdDecoded) {
        add(tr("HDCD"), State::Changing, tr("Peak extension and gain decoded"));
    } else {
        add(tr("HDCD"), State::Off, tr("Not in this track"));
    }

    const bool resamples = std::abs(report.source.format.sampleRate - report.device.sampleRate) > 0.5;
    if (resamples) {
        add(tr("Resampler"), State::Changing,
            rateText(report.source.format.sampleRate) + " \xE2\x86\x92 " +
                rateText(report.device.sampleRate) + kSeparator + report.resampleQuality);
    } else {
        add(tr("Resampler"), State::Off, tr("The device runs at the track's rate"));
    }

    const bool gained = report.source.trackGain != 1.0F;
    if (gained) {
        add(tr("Track gain"), State::Changing,
            trf("%s dB", number(20.0 * std::log10(static_cast<double>(report.source.trackGain)), 2,
                                /*withSign=*/true)));
    } else {
        add(tr("Track gain"), State::Off, tr("None for this track"));
    }

    if (report.freeSurround) {
        add(tr("FreeSurround"), State::Changing, tr("Stereo upmixed to 5.1"));
    } else {
        add(tr("FreeSurround"), State::Off, tr("Off"));
    }

    const bool fitted = !report.freeSurround &&
                        report.source.format.channels != report.device.channels;
    if (fitted) {
        add(tr("Channels"), State::Changing,
            channelsText(report.source.format.channels) + " \xE2\x86\x92 " +
                channelsText(report.device.channels));
    } else {
        add(tr("Channels"), State::Passing, channelsText(report.device.channels));
    }

    if (report.dsp.stretch) {
        add(tr("Pitch & Tempo"), State::Changing,
            trf("Tempo %s%%, pitch %s%%", number(report.dsp.tempo * 100.0, 0),
                number(report.dsp.pitch * 100.0, 0)));
    } else {
        add(tr("Pitch & Tempo"), State::Off, tr("At normal speed"));
    }

    if (report.dsp.equalizer) {
        add(tr("Equalizer"), State::Changing, trf("Preamp %s dB", number(report.dsp.preamp, 1, /*withSign=*/true)));
    } else {
        add(tr("Equalizer"), State::Off, tr("Off or flat"));
    }

    const bool attenuated = report.volume != 1.0F;
    if (attenuated) {
        add(tr("Volume"), State::Changing, trf("%s%%", number(static_cast<double>(report.volume) * 100.0, 0)));
    } else {
        add(tr("Volume"), State::Passing, tr("Full"));
    }

    {
        std::string detail = formatText(report.device);
        if (report.exclusive) {
            detail += kSeparator + tr("Exclusive");
        }
        add(tr("Output"), State::Passing, std::move(detail));
    }

    chain.unchanged = !report.hdcdDecoded && !resamples && !gained && !report.freeSurround &&
                      !fitted && !report.dsp.stretch && !report.dsp.equalizer && !attenuated;

    chain.summary = (chain.unchanged ? tr("Unchanged") : tr("Processed")) + kSeparator +
                    rateText(report.device.sampleRate) + kSeparator +
                    channelsText(report.device.channels);

    chain.live.push_back(trf("Output latency: %s ms", number(report.latencySeconds * 1000.0, 0)));
    chain.live.push_back(trf("Queued: %s ms", number(report.bufferedSeconds * 1000.0, 0)));
    chain.live.push_back(trf("Underruns: %zu", static_cast<std::size_t>(report.underruns)));
    return chain;
}

}  // namespace xpcog::app
