#include "xpcog/platform/PropertyListFile.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <string>
#include <vector>

namespace xpcog::platform {
namespace {

/// A CF object released when it goes out of scope. CFPropertyListCreate* hands
/// back +1 references and there are four early returns below.
template <typename T>
class Owned {
public:
    explicit Owned(T ref) : ref_(ref) {}
    ~Owned() {
        if (ref_ != nullptr) {
            CFRelease(ref_);
        }
    }
    Owned(const Owned&)            = delete;
    Owned& operator=(const Owned&) = delete;

    [[nodiscard]] T get() const noexcept { return ref_; }
    explicit       operator bool() const noexcept { return ref_ != nullptr; }

private:
    T ref_;
};

}  // namespace

std::optional<std::string> propertyListToXml(const std::filesystem::path& path) {
    // Read the file ourselves rather than through CFURL: the path is already a
    // std::filesystem::path, and CFURLCreateFromFileSystemRepresentation would
    // mean converting to bytes and back for nothing.
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return std::nullopt;
    }
    std::vector<unsigned char> bytes;
    unsigned char              buffer[8192];
    while (const std::size_t got = std::fread(buffer, 1, sizeof(buffer), file)) {
        bytes.insert(bytes.end(), buffer, buffer + got);
    }
    std::fclose(file);
    if (bytes.empty()) {
        return std::nullopt;
    }

    Owned<CFDataRef> data{CFDataCreate(kCFAllocatorDefault, bytes.data(),
                                       static_cast<CFIndex>(bytes.size()))};
    if (!data) {
        return std::nullopt;
    }

    // Any format, which is the point: this is handed a binary plist in practice
    // and an XML one in tests, and neither caller should have to know which.
    Owned<CFPropertyListRef> plist{CFPropertyListCreateWithData(
        kCFAllocatorDefault, data.get(), kCFPropertyListImmutable, nullptr,
        nullptr)};
    if (!plist) {
        return std::nullopt;
    }

    Owned<CFDataRef> xml{CFPropertyListCreateData(kCFAllocatorDefault, plist.get(),
                                                  kCFPropertyListXMLFormat_v1_0, 0,
                                                  nullptr)};
    if (!xml) {
        return std::nullopt;
    }

    const auto* start = CFDataGetBytePtr(xml.get());
    return std::string{reinterpret_cast<const char*>(start),
                       static_cast<std::size_t>(CFDataGetLength(xml.get()))};
}

}  // namespace xpcog::platform
