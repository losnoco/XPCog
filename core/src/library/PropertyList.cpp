#include "PropertyList.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace xpcog::plist {
namespace {

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] bool isSpace(char character) {
    return character == ' ' || character == '\t' || character == '\n' ||
           character == '\r';
}

/// UTF-8 encodes one code point from a `&#nn;` or `&#xnn;` reference. Numeric
/// references are rare in a playlist but legal, and dropping them silently
/// corrupts a title.
void appendCodePoint(std::string& out, std::string_view entity) {
    const bool  hex  = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
    std::string text = std::string{entity.substr(hex ? 2 : 1)};
    char*       end  = nullptr;
    const long  code = std::strtol(text.c_str(), &end, hex ? 16 : 10);
    if (end == text.c_str() || code < 0) {
        return;
    }

    const auto value = static_cast<unsigned long>(code);
    if (value < 0x80) {
        out.push_back(static_cast<char>(value));
    } else if (value < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (value >> 6)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    } else if (value < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (value >> 12)));
        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (value >> 18)));
        out.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    }
}

/// The subset of XML a property list uses. Deliberately not a general parser:
/// no entity declarations and no DTD fetching, because resolving the DOCTYPE
/// Foundation emits would turn opening a playlist into a network request.
class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    [[nodiscard]] std::optional<Value> parseDocument() {
        skipProlog();
        if (!expectTag("plist")) {
            // A bare root element is tolerated: some tools omit the wrapper.
            position_ = 0;
            skipProlog();
        }
        auto value = parseValue();
        return value;
    }

private:
    void skipSpace() {
        while (position_ < text_.size() && isSpace(text_[position_])) {
            ++position_;
        }
    }

    /// Skips the XML declaration, the DOCTYPE, comments and processing
    /// instructions -- everything before the first real element.
    void skipProlog() {
        for (;;) {
            skipSpace();
            if (text_.compare(position_, 4, "<!--") == 0) {
                const std::size_t end = text_.find("-->", position_);
                if (end == std::string_view::npos) {
                    position_ = text_.size();
                    return;
                }
                position_ = end + 3;
            } else if (text_.compare(position_, 2, "<?") == 0 ||
                       text_.compare(position_, 2, "<!") == 0) {
                const std::size_t end = text_.find('>', position_);
                if (end == std::string_view::npos) {
                    position_ = text_.size();
                    return;
                }
                position_ = end + 1;
            } else {
                return;
            }
        }
    }

    /// Consumes `<name ...>` if that is what comes next.
    [[nodiscard]] bool expectTag(std::string_view name) {
        const std::size_t saved = position_;
        skipSpace();
        if (position_ >= text_.size() || text_[position_] != '<') {
            position_ = saved;
            return false;
        }
        const std::size_t close = text_.find('>', position_);
        if (close == std::string_view::npos) {
            position_ = saved;
            return false;
        }
        std::string_view tag = text_.substr(position_ + 1, close - position_ - 1);
        if (!tag.starts_with(name) ||
            (tag.size() > name.size() && !isSpace(tag[name.size()]) &&
             tag[name.size()] != '/')) {
            position_ = saved;
            return false;
        }
        position_ = close + 1;
        return true;
    }

    /// The name of the next element, plus whether it is self-closing. Does not
    /// consume.
    [[nodiscard]] std::optional<std::pair<std::string_view, bool>> peekElement() {
        const std::size_t saved = position_;
        skipProlog();
        if (position_ >= text_.size() || text_[position_] != '<') {
            position_ = saved;
            return std::nullopt;
        }
        const std::size_t close = text_.find('>', position_);
        if (close == std::string_view::npos) {
            position_ = saved;
            return std::nullopt;
        }
        std::string_view tag = text_.substr(position_ + 1, close - position_ - 1);
        const bool       selfClosing = tag.ends_with('/');
        if (selfClosing) {
            tag.remove_suffix(1);
        }
        std::size_t nameEnd = 0;
        while (nameEnd < tag.size() && !isSpace(tag[nameEnd])) {
            ++nameEnd;
        }
        return std::pair{tag.substr(0, nameEnd), selfClosing};
    }

    /// Text up to `</name>`, with entities decoded. Consumes the closing tag.
    [[nodiscard]] std::string readTextUntilClose(std::string_view name) {
        const std::string closing = "</" + std::string{name};
        const std::size_t end     = text_.find(closing, position_);
        if (end == std::string_view::npos) {
            position_ = text_.size();
            return {};
        }
        const std::string_view raw = text_.substr(position_, end - position_);
        const std::size_t      after = text_.find('>', end);
        position_ = (after == std::string_view::npos) ? text_.size() : after + 1;
        return decodeEntities(raw);
    }

    [[nodiscard]] static std::string decodeEntities(std::string_view raw) {
        return xmlDecodeEntities(raw);
    }

    [[nodiscard]] std::optional<Value> parseValue() {
        const auto element = peekElement();
        if (!element) {
            return std::nullopt;
        }
        const auto [name, selfClosing] = *element;

        if (name == "true" || name == "false") {
            const bool flag = (name == "true");
            static_cast<void>(expectTag(name));
            if (!selfClosing) {
                static_cast<void>(readTextUntilClose(name));
            }
            return Value::ofBool(flag);
        }
        if (name == "dict") {
            return parseDict(selfClosing);
        }
        if (name == "array") {
            return parseArray(selfClosing);
        }
        if (name == "string" || name == "key" || name == "date") {
            const std::string tagName{name};
            static_cast<void>(expectTag(tagName));
            if (selfClosing) {
                return Value::ofString({});
            }
            return Value::ofString(readTextUntilClose(tagName));
        }
        if (name == "integer") {
            static_cast<void>(expectTag("integer"));
            const std::string text = selfClosing ? "" : readTextUntilClose("integer");
            return Value::ofInteger(std::strtoll(text.c_str(), nullptr, 10));
        }
        if (name == "real") {
            static_cast<void>(expectTag("real"));
            const std::string text = selfClosing ? "" : readTextUntilClose("real");
            return Value::ofReal(std::strtod(text.c_str(), nullptr));
        }
        if (name == "data") {
            static_cast<void>(expectTag("data"));
            const std::string text = selfClosing ? "" : readTextUntilClose("data");
            return Value::ofData(base64Decode(text));
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<Value> parseDict(bool selfClosing) {
        static_cast<void>(expectTag("dict"));
        Value result;
        result.type = Value::Type::Dict;
        if (selfClosing) {
            return result;
        }

        for (;;) {
            skipProlog();
            if (expectTag("/dict")) {
                return result;
            }
            const auto element = peekElement();
            if (!element) {
                return std::nullopt;
            }
            if (element->first != "key") {
                return std::nullopt;
            }
            static_cast<void>(expectTag("key"));
            const std::string key =
                element->second ? std::string{} : readTextUntilClose("key");

            auto value = parseValue();
            if (!value) {
                return std::nullopt;
            }
            result.dict.emplace_back(key, std::move(*value));
        }
    }

    [[nodiscard]] std::optional<Value> parseArray(bool selfClosing) {
        static_cast<void>(expectTag("array"));
        Value result;
        result.type = Value::Type::Array;
        if (selfClosing) {
            return result;
        }

        for (;;) {
            skipProlog();
            if (expectTag("/array")) {
                return result;
            }
            auto value = parseValue();
            if (!value) {
                return std::nullopt;
            }
            result.array.push_back(std::move(*value));
        }
    }

    std::string_view text_;
    std::size_t      position_ = 0;
};

void escapeInto(std::string& out, std::string_view text) { out += xmlEscape(text); }

void writeInto(std::string& out, const Value& value, int depth);

void indent(std::string& out, int depth) { out.append(static_cast<std::size_t>(depth), '\t'); }

void writeInto(std::string& out, const Value& value, int depth) {
    switch (value.type) {
        case Value::Type::Null:
            indent(out, depth);
            out += "<string></string>\n";
            break;
        case Value::Type::Bool:
            indent(out, depth);
            out += value.boolean ? "<true/>\n" : "<false/>\n";
            break;
        case Value::Type::Integer:
            indent(out, depth);
            out += "<integer>" + std::to_string(value.integer) + "</integer>\n";
            break;
        case Value::Type::Real: {
            indent(out, depth);
            // %.17g round-trips a double exactly, which matters for a stored
            // playback position.
            std::array<char, 64> buffer{};
            const int written =
                std::snprintf(buffer.data(), buffer.size(), "%.17g", value.real);
            out += "<real>";
            out.append(buffer.data(), static_cast<std::size_t>(std::max(written, 0)));
            out += "</real>\n";
            break;
        }
        case Value::Type::String:
            indent(out, depth);
            out += "<string>";
            escapeInto(out, value.string);
            out += "</string>\n";
            break;
        case Value::Type::Data:
            indent(out, depth);
            out += "<data>\n";
            indent(out, depth);
            out += base64Encode(value.data);
            out += "\n";
            indent(out, depth);
            out += "</data>\n";
            break;
        case Value::Type::Array:
            indent(out, depth);
            if (value.array.empty()) {
                out += "<array/>\n";
                break;
            }
            out += "<array>\n";
            for (const Value& item : value.array) {
                writeInto(out, item, depth + 1);
            }
            indent(out, depth);
            out += "</array>\n";
            break;
        case Value::Type::Dict:
            indent(out, depth);
            if (value.dict.empty()) {
                out += "<dict/>\n";
                break;
            }
            out += "<dict>\n";
            for (const auto& [key, item] : value.dict) {
                indent(out, depth + 1);
                out += "<key>";
                escapeInto(out, key);
                out += "</key>\n";
                writeInto(out, item, depth + 1);
            }
            indent(out, depth);
            out += "</dict>\n";
            break;
    }
}

}  // namespace

// --- Value --------------------------------------------------------------

Value Value::ofString(std::string text) {
    Value value;
    value.type   = Type::String;
    value.string = std::move(text);
    return value;
}

Value Value::ofInteger(std::int64_t number) {
    Value value;
    value.type    = Type::Integer;
    value.integer = number;
    return value;
}

Value Value::ofReal(double number) {
    Value value;
    value.type = Type::Real;
    value.real = number;
    return value;
}

Value Value::ofBool(bool flag) {
    Value value;
    value.type    = Type::Bool;
    value.boolean = flag;
    return value;
}

Value Value::ofData(std::vector<std::byte> bytes) {
    Value value;
    value.type = Type::Data;
    value.data = std::move(bytes);
    return value;
}

Value Value::ofArray(std::vector<Value> items) {
    Value value;
    value.type  = Type::Array;
    value.array = std::move(items);
    return value;
}

Value Value::ofDict(std::vector<std::pair<std::string, Value>> items) {
    Value value;
    value.type = Type::Dict;
    value.dict = std::move(items);
    return value;
}

const Value* Value::find(std::string_view key) const {
    for (const auto& [name, item] : dict) {
        if (name == key) {
            return &item;
        }
    }
    return nullptr;
}

std::string Value::stringValue(std::string_view key, std::string_view fallback) const {
    const Value* item = find(key);
    if (item == nullptr) {
        return std::string{fallback};
    }
    switch (item->type) {
        case Type::String: return item->string;
        case Type::Integer: return std::to_string(item->integer);
        case Type::Bool: return item->boolean ? "1" : "0";
        default: return std::string{fallback};
    }
}

std::int64_t Value::integerValue(std::string_view key, std::int64_t fallback) const {
    const Value* item = find(key);
    if (item == nullptr) {
        return fallback;
    }
    switch (item->type) {
        case Type::Integer: return item->integer;
        case Type::Real: return static_cast<std::int64_t>(item->real);
        case Type::Bool: return item->boolean ? 1 : 0;
        case Type::String: return std::strtoll(item->string.c_str(), nullptr, 10);
        default: return fallback;
    }
}

double Value::realValue(std::string_view key, double fallback) const {
    const Value* item = find(key);
    if (item == nullptr) {
        return fallback;
    }
    switch (item->type) {
        case Type::Real: return item->real;
        case Type::Integer: return static_cast<double>(item->integer);
        case Type::String: return std::strtod(item->string.c_str(), nullptr);
        default: return fallback;
    }
}

bool Value::boolValue(std::string_view key, bool fallback) const {
    const Value* item = find(key);
    if (item == nullptr) {
        return fallback;
    }
    switch (item->type) {
        case Type::Bool: return item->boolean;
        case Type::Integer: return item->integer != 0;
        case Type::String: return item->string == "1" || item->string == "true";
        default: return fallback;
    }
}

// --- parse and write ----------------------------------------------------

std::optional<Value> parse(std::string_view text) {
    Parser parser{text};
    return parser.parseDocument();
}

std::string write(const Value& root) {
    std::string out =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n";
    writeInto(out, root, 0);
    out += "</plist>\n";
    return out;
}

// --- XML text -----------------------------------------------------------

std::string xmlEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char character : text) {
        switch (character) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out.push_back(character); break;
        }
    }
    return out;
}

std::string xmlDecodeEntities(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '&') {
            out.push_back(raw[i]);
            continue;
        }
        const std::size_t semicolon = raw.find(';', i);
        if (semicolon == std::string_view::npos) {
            out.push_back(raw[i]);
            continue;
        }
        const std::string_view entity = raw.substr(i + 1, semicolon - i - 1);
        if (entity == "amp") {
            out.push_back('&');
        } else if (entity == "lt") {
            out.push_back('<');
        } else if (entity == "gt") {
            out.push_back('>');
        } else if (entity == "quot") {
            out.push_back('"');
        } else if (entity == "apos") {
            out.push_back('\'');
        } else if (entity.starts_with('#')) {
            appendCodePoint(out, entity);
        } else {
            // Unknown entity: keep it literally rather than dropping data.
            out.push_back('&');
            out.append(entity);
            out.push_back(';');
        }
        i = semicolon;
    }
    return out;
}

// --- base64 -------------------------------------------------------------

std::string base64Encode(const std::vector<std::byte>& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const auto a = static_cast<unsigned>(data[i]);
        const auto b = static_cast<unsigned>(data[i + 1]);
        const auto c = static_cast<unsigned>(data[i + 2]);
        out.push_back(kBase64Alphabet[a >> 2]);
        out.push_back(kBase64Alphabet[((a & 0x03) << 4) | (b >> 4)]);
        out.push_back(kBase64Alphabet[((b & 0x0F) << 2) | (c >> 6)]);
        out.push_back(kBase64Alphabet[c & 0x3F]);
    }

    if (i < data.size()) {
        const auto a = static_cast<unsigned>(data[i]);
        out.push_back(kBase64Alphabet[a >> 2]);
        if (i + 1 < data.size()) {
            const auto b = static_cast<unsigned>(data[i + 1]);
            out.push_back(kBase64Alphabet[((a & 0x03) << 4) | (b >> 4)]);
            out.push_back(kBase64Alphabet[(b & 0x0F) << 2]);
        } else {
            out.push_back(kBase64Alphabet[(a & 0x03) << 4]);
            out.push_back('=');
        }
        out.push_back('=');
    }
    return out;
}

std::vector<std::byte> base64Decode(std::string_view text) {
    std::array<int, 256> reverse{};
    reverse.fill(-1);
    for (std::size_t i = 0; i < kBase64Alphabet.size(); ++i) {
        reverse[static_cast<unsigned char>(kBase64Alphabet[i])] = static_cast<int>(i);
    }

    std::vector<std::byte> out;
    out.reserve(text.size() * 3 / 4);

    unsigned buffer = 0;
    int      bits   = 0;
    for (const char character : text) {
        // Foundation wraps <data> across lines and indents it; skipping anything
        // that is not alphabet is what makes that round-trip.
        const int value = reverse[static_cast<unsigned char>(character)];
        if (value < 0) {
            continue;
        }
        buffer = (buffer << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::byte>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace xpcog::plist
