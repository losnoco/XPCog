#include "midi/SoundFontSynth.hpp"

#include "xpcog/core/FilePath.hpp"

#include <spessasynth/sflist/sflist.h>
#include <spessasynth/soundbank/soundbank.h>
#include <spessasynth/synthesizer/synth.h>
#include <spessasynth/utils/file.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string_view>

namespace xpcog::codecs {
namespace {

/// The engine's own block. Everything it does -- envelope ramps, LFO steps,
/// the effect buses -- happens once per block of this many frames.
constexpr std::size_t kBlockFrames = SS_MAX_SOUND_CHUNK;

/// A render call is bounded by this so a caller holding a std::size_t cannot
/// overflow the uint32_t the engine counts frames in.
constexpr std::size_t kMaxRenderFrames = 65536;

/// Cog's cap (SpessaPlayer::startup). High enough that no General MIDI file
/// reaches it, and it exists so a runaway file cannot allocate without bound.
constexpr std::uint32_t kVoiceCap = 512;

constexpr double kMinSampleRate = 8000.0;
constexpr double kMaxSampleRate = 192000.0;

[[nodiscard]] SS_InterpolationType toEngine(SoundFontInterpolation interpolation) {
    switch (interpolation) {
        case SoundFontInterpolation::Nearest:
            return SS_INTERP_NEAREST;
        case SoundFontInterpolation::Hermite:
            return SS_INTERP_HERMITE;
        case SoundFontInterpolation::Sinc:
            return SS_INTERP_SINC;
        case SoundFontInterpolation::Linear:
            break;
    }
    return SS_INTERP_LINEAR;
}

/// Every extension the engine reads, in the order Cog tries them.
///
/// The lists come first on purpose: a folder holding both `Album.sflist` and
/// the `Album.sf2` the list names should play through the list, which is the
/// arrangement that says something about ranges and offsets.
constexpr std::array<std::string_view, 6> kBankExtensions{
    "sflist", "json", "sf2pack", "sf3", "sf2", "dls",
};

[[nodiscard]] std::string lowerExtension(const std::filesystem::path& path) {
    std::string extension = pathToUtf8(path.extension());
    if (!extension.empty() && extension.front() == '.') {
        extension.erase(0, 1);
    }
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

/// Whether a path names a list of banks rather than a bank.
[[nodiscard]] bool isBankList(const std::filesystem::path& path) {
    const std::string extension = lowerExtension(path);
    return extension == "sflist" || extension == "json";
}

[[nodiscard]] SS_SoundBank* loadBank(const std::filesystem::path& path) {
    const std::string utf8 = pathToUtf8(path);
    SS_File*          file = ss_file_open_from_file(utf8.c_str());
    if (file == nullptr) {
        return nullptr;
    }
    // Closed straight after loading, and the bank still reads from it: the
    // handle is reference-counted and the bank holds slices of it, which is
    // what keeps a gigabyte-and-a-third of samples off the heap.
    SS_SoundBank* bank = ss_soundbank_load(file);
    ss_file_close(file);
    return bank;
}

[[nodiscard]] SS_FilteredBanks* loadBankList(const std::filesystem::path& path) {
    const std::string utf8 = pathToUtf8(path);
    SS_File*          file = ss_file_open_from_file(utf8.c_str());
    if (file == nullptr) {
        return nullptr;
    }
    const std::size_t size = ss_file_size(file);
    std::string       text(size, '\0');
    if (size > 0) {
        ss_file_read_bytes(file, 0, reinterpret_cast<std::uint8_t*>(text.data()), size);
    }
    ss_file_close(file);

    // The base path is what relative names in the list resolve against, so a
    // list and the banks it names travel together.
    const std::string base = pathToUtf8(path.parent_path());
    std::array<char, sflist_max_error> error{};
    return sflist_load(text.data(), text.size(), base.c_str(), error.data());
}

}  // namespace

std::optional<std::filesystem::path> findCompanionBank(
    const std::filesystem::path& midiFile) {
    std::array<std::filesystem::path, 3> stems{};
    std::size_t                          count = 0;

    // `song.mid.sf2`, then `song.sf2`.
    stems[count++] = midiFile;
    if (midiFile.has_extension()) {
        std::filesystem::path stripped = midiFile;
        stems[count++]                 = stripped.replace_extension();
    }
    // `Album/Album.sf2`: one bank for a whole folder, which is how a game rip
    // that ships its instruments is usually laid out.
    const std::filesystem::path parent = midiFile.parent_path();
    if (!parent.empty() && !parent.filename().empty()) {
        stems[count++] = parent / parent.filename();
    }

    for (std::size_t i = 0; i < count; ++i) {
        for (const std::string_view extension : kBankExtensions) {
            std::filesystem::path candidate = stems[i];
            candidate += ".";
            candidate += std::filesystem::path{extension};
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error)) {
                return candidate;
            }
        }
    }
    return std::nullopt;
}

struct SoundFontSynth::Impl {
    SS_Processor* processor = nullptr;

    ~Impl() {
        if (processor != nullptr) {
            // Frees the banks loaded into it as well; ownership moved on the
            // load call that succeeded.
            ss_processor_free(processor);
        }
    }
};

SoundFontSynth::SoundFontSynth() : impl_(std::make_unique<Impl>()) {}

SoundFontSynth::~SoundFontSynth() = default;

bool SoundFontSynth::start(double sampleRate, SoundFontInterpolation interpolation) {
    impl_ = std::make_unique<Impl>();
    sampleRate_   = 0.0;
    blockFill_    = 0;
    blockTaken_   = 0;
    callerFrames_ = 0;
    embeddedBank_.clear();

    if (!(sampleRate >= kMinSampleRate && sampleRate <= kMaxSampleRate)) {
        return false;
    }
    const auto rate = static_cast<std::uint32_t>(std::lround(sampleRate));

    SS_ProcessorOptions options{};
    options.enable_effects = true;
    options.voice_cap      = kVoiceCap;
    options.interpolation  = toEngine(interpolation);
    // Neither preload. A 1.3 GB bank would otherwise be decoded whole before
    // the first note, and the reason to stream is the same reason the file is
    // that big: almost none of it is reached by any one piece of music.
    options.preload_all_samples = false;
    options.preload_instruments = false;

    SS_Processor* processor = ss_processor_create(rate, &options);
    if (processor == nullptr) {
        return false;
    }
    impl_->processor = processor;

    // The engine took an integer rate, so that is the rate, not what was asked
    // for. Everything above times events against this.
    sampleRate_ = static_cast<double>(rate);
    block_.assign(kBlockFrames * 2, 0.0F);
    return true;
}

bool SoundFontSynth::openEmbedded(std::span<const std::uint8_t> bank, int bankOffset,
                                  double                 sampleRate,
                                  SoundFontInterpolation interpolation) {
    if (!start(sampleRate, interpolation)) {
        return false;
    }
    if (bank.empty()) {
        return false;
    }

    embeddedBank_.assign(bank.begin(), bank.end());

    // `owned` false: the copy above is what stays alive, and freeing it is this
    // object's business rather than the engine's.
    SS_File* file =
        ss_file_open_from_memory(embeddedBank_.data(), embeddedBank_.size(), false);
    if (file == nullptr) {
        return false;
    }
    SS_SoundBank* loaded = ss_soundbank_load(file);
    ss_file_close(file);
    if (loaded == nullptr) {
        return false;
    }
    if (!ss_processor_load_soundbank(impl_->processor, loaded, "embedded", bankOffset,
                                     false)) {
        ss_soundbank_free(loaded);
        return false;
    }

    displayName_ = "SpessaSynth (embedded bank)";
    return true;
}

bool SoundFontSynth::open(const std::filesystem::path& bank, double sampleRate,
                          SoundFontInterpolation interpolation) {
    if (!start(sampleRate, interpolation)) {
        return false;
    }
    SS_Processor* processor = impl_->processor;

    if (isBankList(bank)) {
        SS_FilteredBanks* banks = loadBankList(bank);
        if (banks == nullptr) {
            return false;
        }
        if (!ss_processor_load_filtered_banks(processor, banks, "bank", false)) {
            ss_filtered_banks_free(banks, true);
            return false;
        }
    } else {
        SS_SoundBank* loaded = loadBank(bank);
        if (loaded == nullptr) {
            return false;
        }
        if (!ss_processor_load_soundbank(processor, loaded, "bank", 0, false)) {
            ss_soundbank_free(loaded);
            return false;
        }
    }

    displayName_ = "SpessaSynth (" + pathToUtf8(bank.filename()) + ")";
    return true;
}

void SoundFontSynth::submit(const std::uint8_t* data, std::size_t length) {
    if (impl_->processor == nullptr || length == 0) {
        return;
    }
    ss_processor_process_message(impl_->processor, data, length, 0,
                                 static_cast<double>(callerFrames_) / sampleRate_);
}

void SoundFontSynth::write(std::uint32_t message) {
    const auto status = static_cast<std::uint8_t>(message & 0xFFU);
    if (status < 0x80U) {
        return;  // not a status byte: nothing to deliver
    }

    // How many of the packed bytes are actually part of the message. Getting
    // this wrong would hand the engine a stale third byte from the word above.
    std::size_t      length = 3;
    const std::uint8_t high = status & 0xF0U;
    if (high == 0xC0U || high == 0xD0U) {
        length = 2;  // program change, channel pressure
    } else if (high == 0xF0U) {
        switch (status) {
            case 0xF1U:  // MIDI time code quarter frame
            case 0xF3U:  // song select
                length = 2;
                break;
            case 0xF2U:  // song position pointer
                length = 3;
                break;
            default:
                length = 1;  // clock, start, stop, active sensing, reset
                break;
        }
    }

    const std::array<std::uint8_t, 3> bytes{
        status,
        static_cast<std::uint8_t>((message >> 8) & 0xFFU),
        static_cast<std::uint8_t>((message >> 16) & 0xFFU),
    };
    submit(bytes.data(), length);
}

void SoundFontSynth::writeSysex(std::span<const std::uint8_t> bytes) {
    // Whole, from 0xF0 to 0xF7. ss_processor_sysex() wants the inner bytes
    // instead, but process_message takes the message as it appears in the file
    // and does that itself.
    submit(bytes.data(), bytes.size());
}

void SoundFontSynth::render(float* out, std::size_t frames) {
    if (frames == 0) {
        return;
    }
    if (impl_->processor == nullptr) {
        std::fill_n(out, frames * 2, 0.0F);
        return;
    }

    while (frames > 0) {
        const std::size_t buffered = blockFill_ - blockTaken_;
        if (buffered > 0) {
            const std::size_t take = std::min(frames, buffered);
            std::memcpy(out, block_.data() + blockTaken_ * 2,
                        take * 2 * sizeof(float));
            blockTaken_ += take;
            out += take * 2;
            frames -= take;
            callerFrames_ += take;
            continue;
        }

        // A whole number of blocks goes straight to the caller's buffer; the
        // engine mixes in blocks either way, so the copy would buy nothing.
        if (frames >= kBlockFrames) {
            const std::size_t whole =
                std::min(frames - (frames % kBlockFrames), kMaxRenderFrames);
            ss_processor_render_interleaved(impl_->processor, out,
                                            static_cast<std::uint32_t>(whole));
            out += whole * 2;
            frames -= whole;
            callerFrames_ += whole;
            continue;
        }

        // Less than a block asked for: render one anyway and keep the rest.
        // Handing the engine a short count instead would run a whole block's
        // worth of parameter ramping into those few samples.
        ss_processor_render_interleaved(impl_->processor, block_.data(),
                                        static_cast<std::uint32_t>(kBlockFrames));
        blockFill_  = kBlockFrames;
        blockTaken_ = 0;
    }
}

void SoundFontSynth::reset() {
    if (impl_->processor == nullptr) {
        return;
    }
    ss_processor_system_reset(impl_->processor);
    // Whatever is still buffered was rendered before the reset and belongs to
    // wherever the track used to be. Counting it as taken keeps this clock
    // level with the engine's own, which a reset deliberately does not rewind.
    callerFrames_ += blockFill_ - blockTaken_;
    blockFill_  = 0;
    blockTaken_ = 0;
}

}  // namespace xpcog::codecs
