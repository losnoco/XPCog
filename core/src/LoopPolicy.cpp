#include "xpcog/core/LoopPolicy.hpp"

#include "xpcog/core/Plugin.hpp"
#include "xpcog/core/Settings.hpp"
#include "xpcog/core/library/Playlist.hpp"

namespace xpcog {

bool loopsForever(const Settings* settings, LoopPolicy policy) {
    if (policy == LoopPolicy::Never || settings == nullptr) {
        return false;
    }
    return settings->RepeatMode() == static_cast<int>(RepeatMode::One);
}

bool IDecoder::loopForever(const Settings* settings) const {
    return loopsForever(settings, loopPolicy_);
}

}  // namespace xpcog
