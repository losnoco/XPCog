// The route table: one list that both dispatches and describes itself.
//
// The reason it is one list rather than two is drift. A hand-written OpenAPI
// document beside a hand-written router is two descriptions of the same thing,
// and they agree exactly until the first change that touches one of them.
// Generating the document by walking the table the router walks means the spec
// cannot describe an endpoint that does not exist, or miss one that does --
// and tests/core/test_remote_openapi.cpp asserts the rest.
//
// Route is a literal type with a function pointer for a handler, so the table is
// a static array with no construction order to think about.

#pragma once

#include "CallGate.hpp"
#include "Query.hpp"

#include "xpcog/core/remote/RemoteServer.hpp"

#include <nlohmann/json.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xpcog::remote {

enum class Method { Get, Post, Put, Patch, Delete };

/// Where a parameter arrives.
enum class In { Path, Query };

struct Param {
    std::string_view name;
    In               in            = In::Query;
    std::string_view type          = "string";  ///< An OpenAPI scalar type.
    bool             required      = false;
    std::string_view description;
};

/// What a handler is given.
struct Ctx {
    CallGate&         gate;
    const RawRequest& request;
    const Query&      query;

    /// The values captured from `{name}` segments of the path pattern.
    const std::vector<std::pair<std::string, std::string>>& path;

    /// The parsed request body, or null when there was none. Already checked to
    /// be an object where the route declares a schema.
    const nlohmann::json& body;

    [[nodiscard]] std::string pathParam(std::string_view name) const;
};

using Handler = RawResponse (*)(const Ctx&);

struct Route {
    Method           method;
    std::string_view path;         ///< "/api/v1/playlist/{id}"
    std::string_view operationId;  ///< Unique; the name a generated client gets.
    std::string_view summary;      ///< One line, English, untranslated.
    std::span<const Param> params;
    std::string_view requestSchema;   ///< A name in schemas(), or empty.
    std::string_view responseSchema;  ///< Likewise.
    std::string_view responseMedia = "application/json";
    /// Refused with 403 when the server is configured read-only.
    bool             writes   = false;
    /// Answers 202 and a job id rather than doing the work inline.
    bool             deferred = false;
    Handler          handler  = nullptr;
};

/// One schema, by name, as an OpenAPI schema object.
struct Schema {
    std::string_view name;
    nlohmann::json (*build)();
};

[[nodiscard]] std::span<const Route>  routes();
[[nodiscard]] std::span<const Schema> schemas();

[[nodiscard]] std::string_view methodName(Method method);

// --- shared helpers, used by the handlers and by the router ----------------

[[nodiscard]] RawResponse jsonResponse(const nlohmann::json& body, int status = 200);
[[nodiscard]] RawResponse jsonError(int status, std::string_view code,
                                    std::string_view message,
                                    std::string_view field = {});

/// The one place an Outcome becomes a status code, so every route answers a
/// declined command the same way.
[[nodiscard]] RawResponse outcomeResponse(Outcome outcome, const nlohmann::json& body);

/// 503 with Retry-After, for a gate that did not answer. Not 504: nothing here
/// is a gateway, the interface is busy.
[[nodiscard]] RawResponse interfaceBusy();

}  // namespace xpcog::remote
