// No spatial path on this platform -- see SpatialStream.hpp for why none is
// needed here.

#include "SpatialStream.hpp"

namespace xpcog {
namespace detail {

bool spatialStreamWanted(std::uint32_t /*channels*/, const std::string& /*deviceUid*/) {
    return false;
}

std::unique_ptr<SpatialStream> openSpatialStream(double /*sampleRate*/,
                                                 std::uint32_t /*channels*/,
                                                 std::uint32_t /*channelConfig*/,
                                                 const std::string& /*deviceUid*/,
                                                 SpatialSource /*source*/) {
    return nullptr;
}

}  // namespace detail
}  // namespace xpcog
