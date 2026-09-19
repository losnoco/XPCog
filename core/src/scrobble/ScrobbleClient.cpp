#include "xpcog/core/scrobble/ScrobbleClient.hpp"

namespace xpcog {

bool ScrobbleError::retryable() const noexcept {
    switch (kind) {
    case Kind::None:
        return false;
    case Kind::Transport:
        // Never reached the server, so the request is still unmade.
        return true;
    case Kind::Transient:
        // The server's own word for "not now".
        return true;
    case Kind::Malformed:
        // The server answered something unreadable. Rare, and more likely a
        // captive portal or a proxy than the service, so worth trying again.
        return true;
    case Kind::NotAuthorized:
        // Retryable in the auth flow's sense -- the listener may yet grant it --
        // but this is never reached from the queue, which only ever holds
        // scrobbles.
        return true;
    case Kind::SessionInvalid:
        // Retrying cannot help; the listener has to authorise again.
        return false;
    case Kind::Api:
        // A statement about the request rather than about the moment: a bad
        // signature, a suspended key or a rejected parameter will be just as
        // bad in an hour. Dropping it is what keeps one poisoned entry from
        // blocking the queue behind it.
        return false;
    }
    return false;
}

}  // namespace xpcog
