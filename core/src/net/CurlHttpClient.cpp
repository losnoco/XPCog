// The libcurl half of HttpClient.hpp. Compiled only when XPCOG_WITH_HTTP is on;
// NullHttpClient.cpp stands in otherwise, so no caller needs an #ifdef.
//
// One handle per request rather than a reused one. The reuse would buy a kept-
// alive connection, and Last.fm calls arrive minutes apart -- far past any
// keep-alive window -- so what it would actually buy is shared mutable state
// between two threads for no saving.

#include "xpcog/core/net/HttpClient.hpp"

#include <curl/curl.h>

#include <array>
#include <cstddef>

namespace xpcog {
namespace {

/// Long enough for a slow mobile link, short enough that a submission worker
/// stuck on a dead host is not stuck for a whole track.
constexpr long kTimeoutSeconds = 20;

/// A response body that large is not one of ours: Last.fm's largest reply here
/// is a few hundred bytes. Bounded so a misdirected request at a radio stream
/// cannot fill memory.
constexpr std::size_t kMaxBody = 1U << 20;

std::size_t appendBody(char* data, std::size_t size, std::size_t count, void* user) {
    auto*             body  = static_cast<std::string*>(user);
    const std::size_t bytes = size * count;
    if (body->size() + bytes > kMaxBody) {
        // Returning short is libcurl's abort signal, which surfaces as
        // CURLE_WRITE_ERROR and therefore as a transport failure. Correct: a
        // truncated body is not an answer.
        return 0;
    }
    body->append(data, bytes);
    return bytes;
}

class CurlHttpClient final : public IHttpClient {
public:
    HttpResponse post(std::string_view url, const HttpParams& params) override {
        const std::string body = formEncode(params);
        return perform(std::string{url}, &body);
    }

    HttpResponse get(std::string_view url, const HttpParams& params) override {
        std::string target{url};
        if (!params.empty()) {
            target += (target.find('?') == std::string::npos) ? '?' : '&';
            target += formEncode(params);
        }
        return perform(target, nullptr);
    }

private:
    static HttpResponse perform(const std::string& url, const std::string* body) {
        HttpResponse response;

        CURL* handle = curl_easy_init();
        if (handle == nullptr) {
            response.error = "could not initialise libcurl";
            return response;
        }

        std::array<char, CURL_ERROR_SIZE> errorBuffer{};

        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &appendBody);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);
        curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer.data());
        curl_easy_setopt(handle, CURLOPT_TIMEOUT, kTimeoutSeconds);
        curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, kTimeoutSeconds);
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
        // Without this a redirect to http:// would be followed silently, which
        // for a request carrying a session key is a downgrade nobody asked for.
        curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");
        curl_easy_setopt(handle, CURLOPT_USERAGENT, "XPCog");
        // libcurl is not thread-safe about its own signal handling, and this is
        // called from a worker; without it a DNS timeout can longjmp out of
        // another thread's alarm handler.
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);

        if (body != nullptr) {
            curl_easy_setopt(handle, CURLOPT_POST, 1L);
            curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body->c_str());
            curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(body->size()));
        }

        const CURLcode result = curl_easy_perform(handle);
        if (result == CURLE_OK) {
            curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status);
        } else {
            const char* detail = (errorBuffer[0] != '\0') ? errorBuffer.data()
                                                          : curl_easy_strerror(result);
            response.error = detail;
            // A failed request has no body worth reading, and leaving a partial
            // one would let a caller parse half a reply.
            response.body.clear();
        }

        curl_easy_cleanup(handle);
        return response;
    }
};

}  // namespace

std::unique_ptr<IHttpClient> makeCurlHttpClient() {
    return std::make_unique<CurlHttpClient>();
}

bool httpClientAvailable() noexcept { return true; }

}  // namespace xpcog
