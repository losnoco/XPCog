// The stand-in for a build configured without XPCOG_WITH_HTTP.
//
// The same arrangement as NullCrashReporter next door in platform/: exactly one
// of the two translation units is compiled, so every caller links the same two
// symbols and none of them carries an #ifdef. What differs is only the answer to
// httpClientAvailable(), which is the question the preferences pane asks before
// it draws a row that could not work.

#include "xpcog/core/net/HttpClient.hpp"

namespace xpcog {

std::unique_ptr<IHttpClient> makeCurlHttpClient() { return nullptr; }

bool httpClientAvailable() noexcept { return false; }

}  // namespace xpcog
