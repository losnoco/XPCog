// Settings in the preferences domain, and where the library lives, on macOS.
//
// CFPreferences directly, rather than any config library, and the reason is a
// requirement rather than a preference: what has to be read is a real plist,
// written by a different program. XPCog uses Cog's keys unchanged so that an
// existing setup is inherited rather than reset on first launch, and Cog stores
// typed values -- `repeat` is a plist integer, not a string. A library that wrote
// its own text format beside the plist would read none of it and say nothing.
//
// That is also why the coercion below is explicit. ISettingsStore's currency is
// strings and Settings does the parsing, so a CFNumber or a CFBoolean coming out
// of the domain is converted to its decimal or `true`/`false` spelling on the way
// through. Writes are always CFString, which is what this program has always
// written and what keeps a value it wrote readable by the same branch.
//
// Plain C++ rather than Objective-C++: CoreFoundation is a C API, and there is no
// AppKit or Foundation object in this file. It compiles without ARC because there
// is nothing for ARC to manage.

#include "xpcog/platform/SettingsStore.hpp"

#include "../common/FileSettingsStore.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace xpcog::platform {
namespace {

/// Releases a CoreFoundation object on scope exit. CF has no smart pointer and
/// every path below has at least two exits.
template <typename Ref>
class ScopedRef {
public:
    explicit ScopedRef(Ref ref) : ref_(ref) {}

    ScopedRef(const ScopedRef&)            = delete;
    ScopedRef& operator=(const ScopedRef&) = delete;

    ~ScopedRef() {
        if (ref_ != nullptr) {
            CFRelease(ref_);
        }
    }

    [[nodiscard]] Ref get() const noexcept { return ref_; }
    explicit operator bool() const noexcept { return ref_ != nullptr; }

private:
    Ref ref_;
};

[[nodiscard]] CFStringRef makeCFString(std::string_view text) {
    return CFStringCreateWithBytes(kCFAllocatorDefault,
                                   reinterpret_cast<const UInt8*>(text.data()),
                                   static_cast<CFIndex>(text.size()),
                                   kCFStringEncodingUTF8, false);
}

[[nodiscard]] std::string fromCFString(CFStringRef text) {
    if (text == nullptr) {
        return {};
    }
    const CFIndex length = CFStringGetLength(text);
    const CFIndex bytes  = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;

    std::string out(static_cast<std::size_t>(bytes), '\0');
    if (!CFStringGetCString(text, out.data(), bytes, kCFStringEncodingUTF8)) {
        return {};
    }
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

/// A plist value as the string ISettingsStore deals in, or nullopt when it is a
/// type this cannot sensibly spell -- an array or a dictionary, which nothing in
/// settings.def declares.
[[nodiscard]] std::optional<std::string> coerce(CFPropertyListRef value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    const CFTypeID type = CFGetTypeID(value);

    if (type == CFStringGetTypeID()) {
        return fromCFString(static_cast<CFStringRef>(value));
    }
    if (type == CFBooleanGetTypeID()) {
        // Spelled the way Settings parses it, and the way this store writes it.
        return CFBooleanGetValue(static_cast<CFBooleanRef>(value)) ? "true" : "false";
    }
    if (type == CFNumberGetTypeID()) {
        const auto number = static_cast<CFNumberRef>(value);
        if (CFNumberIsFloatType(number)) {
            double out = 0.0;
            if (CFNumberGetValue(number, kCFNumberDoubleType, &out)) {
                return std::to_string(out);
            }
            return std::nullopt;
        }
        long long out = 0;
        if (CFNumberGetValue(number, kCFNumberLongLongType, &out)) {
            return std::to_string(out);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

class CFPreferencesStore final : public ISettingsStore {
public:
    ~CFPreferencesStore() override { sync(); }

    [[nodiscard]] std::optional<std::string> getRaw(std::string_view key) const override {
        const ScopedRef<CFStringRef> name{makeCFString(key)};
        if (!name) {
            return std::nullopt;
        }
        const ScopedRef<CFPropertyListRef> value{
            CFPreferencesCopyAppValue(name.get(), kCFPreferencesCurrentApplication)};
        return coerce(value.get());
    }

    void setRaw(std::string_view key, std::string_view value) override {
        const ScopedRef<CFStringRef> name{makeCFString(key)};
        const ScopedRef<CFStringRef> text{makeCFString(value)};
        if (!name || !text) {
            return;
        }
        CFPreferencesSetAppValue(name.get(), text.get(), kCFPreferencesCurrentApplication);
    }

    void remove(std::string_view key) override {
        const ScopedRef<CFStringRef> name{makeCFString(key)};
        if (!name) {
            return;
        }
        // A null value is how CFPreferences spells deletion.
        CFPreferencesSetAppValue(name.get(), nullptr, kCFPreferencesCurrentApplication);
    }

    void sync() override {
        CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);
    }
};

}  // namespace

std::unique_ptr<ISettingsStore> makeNativeSettingsStore() {
    return std::make_unique<CFPreferencesStore>();
}

std::unique_ptr<ISettingsStore> makeFileSettingsStore(const std::string& path) {
    return std::make_unique<FileSettingsStore>(path);
}

std::string libraryDatabasePath() {
    const char* home = std::getenv("HOME");
    const std::filesystem::path base =
        (home != nullptr && *home != '\0') ? std::filesystem::path{home}
                                           : std::filesystem::path{"."};

    // Where QStandardPaths::AppDataLocation put it, organisation segment
    // included. An existing installation's library is already there.
    const std::filesystem::path directory =
        base / "Library" / "Application Support" / "LoSnoCo" / "XPCog";

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);

    return (directory / "library.db").string();
}

}  // namespace xpcog::platform
