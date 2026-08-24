#include "xpcog/core/audio/EqualizerPresets.hpp"

#include "xpcog/core/AssetPath.hpp"
#include "xpcog/core/Settings.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <utility>

namespace xpcog {
namespace {

/// The ten centres a preset stores. Cog's `cog_equalizer_bands`.
constexpr double kPointFrequencies[EqualizerPreset::kPoints] = {
    32.0, 64.0, 128.0, 256.0, 512.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};

/// The member names, in the order Cog's `_cog_equalizer_items()` lists them:
/// the name first, then the ten points, then the preamp. All twelve are
/// required, and a preset missing any of them is dropped rather than defaulted.
constexpr const char* kRequiredMembers[] = {
    "name",   "hz32",   "hz64",   "hz128",  "hz256",   "hz512",
    "hz1000", "hz2000", "hz4000", "hz8000", "hz16000", "preamp"};
constexpr std::size_t kRequiredCount = std::size(kRequiredMembers);

static_assert(kRequiredCount == EqualizerPreset::kPoints + 2,
              "a name, ten points and a preamp");

constexpr const char* kAltGenresMember = "altGenres";
constexpr std::string_view kDocumentType = "Cog EQ library file v1.0";

/// The stored form of a gain: an integer 1..401 standing for -20.0..+20.0 dB in
/// tenths, offset by 201 so that 201 is 0 dB.
///
/// Anything outside that range becomes 0 dB rather than being clamped, which is
/// Cog's rule and the safer of the two: a value that far out is a file written
/// against a different scale, and flattening one band is a smaller lie than
/// pinning it to the rail.
[[nodiscard]] double gainFromStored(std::int64_t value) {
    if (value < 1 || value > 401) {
        return 0.0;
    }
    return static_cast<double>(value - 201) / 10.0;
}

// --- just enough JSON -----------------------------------------------------
//
// One file format, parsed here rather than by a dependency. The library file is
// a fixed shape -- an object of strings, integers and one optional array of
// strings -- and adding a JSON library to core to read 6 KB of it would put a
// dependency in front of every consumer of the engine, including the headless
// builds that have no equaliser interface at all. PropertyList.cpp sets the
// precedent: a format XPCog reads but does not define is read by a reader
// scoped to it.
//
// The one place this has to be exact rather than merely sufficient is the
// integer/real distinction. Cog accepts a gain only when the parser reports
// `json_integer`, so `"hz32": 251.0` is not 5.0 dB there -- it is a member that
// fails the type check, which drops the whole preset. Recording which tokens
// carried a decimal point or an exponent is what lets that stay true here.

constexpr int kMaxDepth = 32;

struct JsonValue {
    enum class Kind { Null, Bool, Integer, Real, String, Array, Object };

    Kind         kind    = Kind::Null;
    bool         boolean = false;
    std::int64_t integer = 0;
    double       real    = 0.0;
    std::string  text;
    std::vector<JsonValue>                          array;
    std::vector<std::pair<std::string, JsonValue>>  object;

    [[nodiscard]] const JsonValue* member(std::string_view name) const {
        for (const auto& entry : object) {
            if (entry.first == name) {
                return &entry.second;
            }
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    /// The whole document, or nullopt when it is not one. Trailing content
    /// other than whitespace fails, so a truncated file cannot parse as its
    /// first valid fragment.
    [[nodiscard]] std::optional<JsonValue> parseDocument() {
        auto value = parseValue(0);
        if (!value) {
            return std::nullopt;
        }
        skipWhitespace();
        return atEnd() ? std::move(value) : std::nullopt;
    }

private:
    [[nodiscard]] bool atEnd() const { return position_ >= text_.size(); }
    [[nodiscard]] char peek() const { return atEnd() ? '\0' : text_[position_]; }

    void skipWhitespace() {
        while (!atEnd()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++position_;
        }
    }

    [[nodiscard]] bool consume(char expected) {
        skipWhitespace();
        if (peek() != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    [[nodiscard]] bool consumeLiteral(std::string_view literal) {
        if (text_.compare(position_, literal.size(), literal) != 0) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    [[nodiscard]] std::optional<JsonValue> parseValue(int depth) {
        // Depth-limited because the parser recurses and the input is a file the
        // user can replace. Without it a few thousand open brackets are a stack
        // overflow rather than a rejected file.
        if (depth >= kMaxDepth) {
            return std::nullopt;
        }
        skipWhitespace();
        switch (peek()) {
            case '{': return parseObject(depth);
            case '[': return parseArray(depth);
            case '"': {
                auto text = parseString();
                if (!text) {
                    return std::nullopt;
                }
                JsonValue value;
                value.kind = JsonValue::Kind::String;
                value.text = std::move(*text);
                return value;
            }
            case 't': {
                if (!consumeLiteral("true")) return std::nullopt;
                JsonValue value;
                value.kind    = JsonValue::Kind::Bool;
                value.boolean = true;
                return value;
            }
            case 'f': {
                if (!consumeLiteral("false")) return std::nullopt;
                JsonValue value;
                value.kind = JsonValue::Kind::Bool;
                return value;
            }
            case 'n': {
                if (!consumeLiteral("null")) return std::nullopt;
                return JsonValue{};
            }
            default: return parseNumber();
        }
    }

    [[nodiscard]] std::optional<JsonValue> parseObject(int depth) {
        if (!consume('{')) {
            return std::nullopt;
        }
        JsonValue value;
        value.kind = JsonValue::Kind::Object;
        skipWhitespace();
        if (consume('}')) {
            return value;
        }
        for (;;) {
            skipWhitespace();
            auto name = parseString();
            if (!name || !consume(':')) {
                return std::nullopt;
            }
            auto member = parseValue(depth + 1);
            if (!member) {
                return std::nullopt;
            }
            value.object.emplace_back(std::move(*name), std::move(*member));
            if (consume(',')) {
                continue;
            }
            return consume('}') ? std::optional{std::move(value)} : std::nullopt;
        }
    }

    [[nodiscard]] std::optional<JsonValue> parseArray(int depth) {
        if (!consume('[')) {
            return std::nullopt;
        }
        JsonValue value;
        value.kind = JsonValue::Kind::Array;
        skipWhitespace();
        if (consume(']')) {
            return value;
        }
        for (;;) {
            auto element = parseValue(depth + 1);
            if (!element) {
                return std::nullopt;
            }
            value.array.push_back(std::move(*element));
            if (consume(',')) {
                continue;
            }
            return consume(']') ? std::optional{std::move(value)} : std::nullopt;
        }
    }

    /// Appends one code point as UTF-8. The library file is ASCII, but a preset
    /// name a user writes need not be, and a `\u` escape dropped on the floor
    /// would corrupt exactly the names that are hardest to retype.
    static void appendUtf8(std::string& out, std::uint32_t code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    [[nodiscard]] std::optional<std::uint32_t> parseHex4() {
        if (position_ + 4 > text_.size()) {
            return std::nullopt;
        }
        std::uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            const char digit = text_[position_ + static_cast<std::size_t>(i)];
            code <<= 4;
            if (digit >= '0' && digit <= '9') {
                code |= static_cast<std::uint32_t>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                code |= static_cast<std::uint32_t>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
                code |= static_cast<std::uint32_t>(digit - 'A' + 10);
            } else {
                return std::nullopt;
            }
        }
        position_ += 4;
        return code;
    }

    [[nodiscard]] std::optional<std::string> parseString() {
        skipWhitespace();
        if (peek() != '"') {
            return std::nullopt;
        }
        ++position_;

        std::string out;
        while (!atEnd()) {
            const char c = text_[position_++];
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (atEnd()) {
                return std::nullopt;
            }
            switch (const char escape = text_[position_++]) {
                case '"':
                case '\\':
                case '/': out.push_back(escape); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    auto code = parseHex4();
                    if (!code) {
                        return std::nullopt;
                    }
                    // A high surrogate is only half a code point; the low half
                    // follows as a second escape. Left unpaired it becomes the
                    // replacement character rather than an invalid encoding,
                    // because a mangled name should still be a legible string.
                    if (*code >= 0xD800 && *code <= 0xDBFF) {
                        const std::size_t mark = position_;
                        if (consumeLiteral("\\u")) {
                            if (auto low = parseHex4();
                                low && *low >= 0xDC00 && *low <= 0xDFFF) {
                                appendUtf8(out, 0x10000 + ((*code - 0xD800) << 10) +
                                                    (*low - 0xDC00));
                                break;
                            }
                        }
                        position_ = mark;
                        appendUtf8(out, 0xFFFD);
                        break;
                    }
                    if (*code >= 0xDC00 && *code <= 0xDFFF) {
                        appendUtf8(out, 0xFFFD);
                        break;
                    }
                    appendUtf8(out, *code);
                    break;
                }
                default: return std::nullopt;
            }
        }
        return std::nullopt;  // unterminated
    }

    [[nodiscard]] std::optional<JsonValue> parseNumber() {
        skipWhitespace();
        const std::size_t start = position_;
        bool              real  = false;

        if (peek() == '-') {
            ++position_;
        }
        const std::size_t digitsStart = position_;
        while (!atEnd() && (std::isdigit(static_cast<unsigned char>(peek())) != 0)) {
            ++position_;
        }
        if (position_ == digitsStart) {
            return std::nullopt;  // a sign, or nothing, is not a number
        }
        if (peek() == '.') {
            real = true;
            ++position_;
            while (!atEnd() && (std::isdigit(static_cast<unsigned char>(peek())) != 0)) {
                ++position_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            real = true;
            ++position_;
            if (peek() == '+' || peek() == '-') {
                ++position_;
            }
            while (!atEnd() && (std::isdigit(static_cast<unsigned char>(peek())) != 0)) {
                ++position_;
            }
        }

        const std::string token{text_.substr(start, position_ - start)};
        JsonValue         value;
        if (real) {
            value.kind = JsonValue::Kind::Real;
            value.real = std::strtod(token.c_str(), nullptr);
        } else {
            value.kind = JsonValue::Kind::Integer;
            // strtoll rather than from_chars only because the token is already a
            // NUL-terminated copy; an integer too large for the type saturates,
            // and gainFromStored() rejects it on range like any other.
            value.integer = std::strtoll(token.c_str(), nullptr, 10);
        }
        return value;
    }

    std::string_view text_;
    std::size_t      position_ = 0;
};

// --- interpolation --------------------------------------------------------

/// Cog's `interpolatePoint()`, term for term.
///
/// The extrapolated arms are the interesting half. Beyond either end of the
/// stored range Cog builds four more points by repeating the last step -- in
/// gain and in frequency both -- scaled by 1.05, and then interpolates inside
/// that extension exactly as it would inside the real data. The frequency steps
/// go negative almost immediately at the bottom (32 Hz followed by 32 + (32-64)
/// * 1.05 = -1.6 Hz), which sounds wrong and is not: the extension only has to
/// bracket the target, and 20, 25 and 31.5 Hz all fall inside that first
/// synthetic interval, so what actually reaches the filter is a linear
/// continuation of the slope between the 64 Hz and 32 Hz points.
[[nodiscard]] double interpolatePoint(const EqualizerPreset& preset, double target) {
    constexpr int kPoints   = EqualizerPreset::kPoints;
    constexpr int kExtended = kPoints + 4;
    constexpr double kStep  = 1.05;

    if (target < kPointFrequencies[0]) {
        // Reversed, so index 0 is the highest frequency and the extension grows
        // downwards off the end of the array.
        double gains[kExtended]{};
        double freqs[kExtended]{};
        for (int i = 0; i < kPoints; ++i) {
            gains[kPoints - 1 - i] = preset.gainsDb[static_cast<std::size_t>(i)];
            freqs[kPoints - 1 - i] = kPointFrequencies[i];
        }
        for (int i = kPoints; i < kExtended; ++i) {
            gains[i] = gains[i - 1] + (gains[i - 1] - gains[i - 2]) * kStep;
            freqs[i] = freqs[i - 1] + (freqs[i - 1] - freqs[i - 2]) * kStep;
        }
        for (int i = 0; i < kExtended - 1; ++i) {
            const int low  = kExtended - 1 - i;
            const int high = kExtended - 2 - i;
            if (target >= freqs[low] && target < freqs[high]) {
                const double delta = (target - freqs[low]) / (freqs[high] - freqs[low]);
                return gains[low] + (gains[high] - gains[low]) * delta;
            }
        }
        return gains[kExtended - 1];
    }

    if (target > kPointFrequencies[kPoints - 1]) {
        double gains[kExtended]{};
        double freqs[kExtended]{};
        for (int i = 0; i < kPoints; ++i) {
            gains[i] = preset.gainsDb[static_cast<std::size_t>(i)];
            freqs[i] = kPointFrequencies[i];
        }
        for (int i = kPoints; i < kExtended; ++i) {
            gains[i] = gains[i - 1] + (gains[i - 1] - gains[i - 2]) * kStep;
            freqs[i] = freqs[i - 1] + (freqs[i - 1] - freqs[i - 2]) * kStep;
        }
        for (int i = 0; i < kExtended - 1; ++i) {
            if (target >= freqs[i] && target < freqs[i + 1]) {
                const double delta = (target - freqs[i]) / (freqs[i + 1] - freqs[i]);
                return gains[i] + (gains[i + 1] - gains[i]) * delta;
            }
        }
        return gains[kExtended - 1];
    }

    // The two ends land exactly on a stored point, and are taken directly rather
    // than through a division that would be by zero at the top end.
    if (target == kPointFrequencies[0]) {
        return preset.gainsDb[0];
    }
    if (target == kPointFrequencies[kPoints - 1]) {
        return preset.gainsDb[kPoints - 1];
    }

    for (int i = 0; i < kPoints - 1; ++i) {
        if (target >= kPointFrequencies[i] && target < kPointFrequencies[i + 1]) {
            const double delta = (target - kPointFrequencies[i]) /
                                 (kPointFrequencies[i + 1] - kPointFrequencies[i]);
            return preset.gainsDb[static_cast<std::size_t>(i)] +
                   (preset.gainsDb[static_cast<std::size_t>(i) + 1] -
                    preset.gainsDb[static_cast<std::size_t>(i)]) *
                       delta;
        }
    }
    return 0.0;
}

[[nodiscard]] std::string lowercased(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// One preset object, or nullopt when it is not one.
[[nodiscard]] std::optional<EqualizerPreset> readPreset(const JsonValue& value) {
    if (value.kind != JsonValue::Kind::Object) {
        return std::nullopt;
    }

    EqualizerPreset preset;

    // Every required member, by name and by the type its position implies: the
    // first is the name and must be a string, the other eleven are gains and
    // must be integers. A member of the wrong type is a missing member, which
    // drops the preset -- Cog counts the same way.
    for (std::size_t i = 0; i < kRequiredCount; ++i) {
        const JsonValue* member = value.member(kRequiredMembers[i]);
        if (member == nullptr) {
            return std::nullopt;
        }
        if (i == 0) {
            if (member->kind != JsonValue::Kind::String || member->text.empty()) {
                return std::nullopt;
            }
            preset.name = member->text;
            continue;
        }
        if (member->kind != JsonValue::Kind::Integer) {
            return std::nullopt;
        }
        const double gain = gainFromStored(member->integer);
        if (i <= EqualizerPreset::kPoints) {
            preset.gainsDb[i - 1] = gain;
        } else {
            preset.preampDb = gain;
        }
    }

    if (const JsonValue* aliases = value.member(kAltGenresMember);
        aliases != nullptr && aliases->kind == JsonValue::Kind::Array) {
        for (const JsonValue& alias : aliases->array) {
            if (alias.kind == JsonValue::Kind::String && !alias.text.empty()) {
                preset.altGenres.push_back(alias.text);
            }
        }
    }

    return preset;
}

}  // namespace

std::span<const double> EqualizerPresetLibrary::frequencies() noexcept {
    return std::span<const double>{kPointFrequencies, EqualizerPreset::kPoints};
}

std::string_view EqualizerPresetLibrary::documentType() noexcept { return kDocumentType; }

EqualizerPresetLibrary EqualizerPresetLibrary::parse(std::string_view document) {
    EqualizerPresetLibrary library;

    JsonParser parser{document};
    const auto root = parser.parseDocument();
    if (!root || root->kind != JsonValue::Kind::Object) {
        return library;
    }

    const JsonValue* type = root->member("type");
    if (type == nullptr || type->kind != JsonValue::Kind::String ||
        type->text != kDocumentType) {
        return library;
    }

    const JsonValue* presets = root->member("presets");
    if (presets == nullptr || presets->kind != JsonValue::Kind::Array) {
        return library;
    }

    for (const JsonValue& entry : presets->array) {
        if (auto preset = readPreset(entry)) {
            library.presets_.push_back(std::move(*preset));
        }
    }
    return library;
}

EqualizerPresetLibrary EqualizerPresetLibrary::loadShipped() {
    const std::filesystem::path path = assetPath("equalizer/Cog.q1.json");
    if (path.empty()) {
        return {};
    }
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return {};
    }
    std::ostringstream text;
    text << file.rdbuf();
    return parse(text.str());
}

const EqualizerPreset* EqualizerPresetLibrary::at(int index) const noexcept {
    if (index < 0 || static_cast<std::size_t>(index) >= presets_.size()) {
        return nullptr;
    }
    return &presets_[static_cast<std::size_t>(index)];
}

int EqualizerPresetLibrary::indexOf(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < presets_.size(); ++i) {
        if (presets_[i].name == name) {
            return static_cast<int>(i);
        }
        for (const std::string& alias : presets_[i].altGenres) {
            if (alias == name) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

int EqualizerPresetLibrary::matchGenre(std::string_view genre) const noexcept {
    if (const int exact = indexOf(genre); exact >= 0) {
        return exact;
    }

    if (!genre.empty()) {
        const std::string haystack = lowercased(genre);

        // Longest wins, so a preset whose name is a suffix of another's cannot
        // shadow it: "Rock" and "Punk Rock" both sit inside "Punk Rock", and
        // the specific one is the answer.
        int         best       = -1;
        std::size_t bestLength = 0;
        for (std::size_t i = 0; i < presets_.size(); ++i) {
            const auto consider = [&](const std::string& candidate) {
                if (candidate.size() <= bestLength) {
                    return;
                }
                if (haystack.find(lowercased(candidate)) != std::string::npos) {
                    best       = static_cast<int>(i);
                    bestLength = candidate.size();
                }
            };
            consider(presets_[i].name);
            for (const std::string& alias : presets_[i].altGenres) {
                consider(alias);
            }
        }
        if (best >= 0) {
            return best;
        }
    }

    return indexOf("Flat");
}

const EqualizerPresetLibrary& shippedEqualizerPresets() {
    static const EqualizerPresetLibrary library = EqualizerPresetLibrary::loadShipped();
    return library;
}

std::array<double, Equalizer::kBands> interpolateEqualizerPreset(
    const EqualizerPreset& preset) {
    const auto                            centres = Equalizer::bandFrequencies();
    std::array<double, Equalizer::kBands> gains{};
    for (std::size_t band = 0; band < gains.size(); ++band) {
        gains[band] = interpolatePoint(preset, centres[band]);
    }
    return gains;
}

void applyEqualizerPreset(Settings& settings, const EqualizerPreset& preset) {
    const auto gains = interpolateEqualizerPreset(preset);
    const auto keys  = Equalizer::bandSettingsKeys();

    settings.setEqPreamp(preset.preampDb);
    for (std::size_t band = 0; band < keys.size(); ++band) {
        // By key rather than by generated accessor, for the reason
        // AudioEngine::applyDspSettings() gives: the band-to-key pairing lives in
        // one table and naming 31 accessors here would be a second one.
        settings.setRawValue(keys[band], std::to_string(gains[band]));
    }
}

}  // namespace xpcog
