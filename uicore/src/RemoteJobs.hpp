// Scans started over the API, and how they are getting on.
//
// POST /playlist/tracks answers 202 and a job id rather than doing the work: a
// directory of ten thousand files takes minutes, and the gate would give up on
// the request long before the scan finished. This is what the id then refers to.
//
// Owned by the window and touched only on the interface thread, like everything
// else the remote control reaches. Bounded, because a peer can start jobs and an
// unbounded record of them is a slow leak with a network interface on it -- the
// oldest are dropped once there are more than a few, which is right for
// something whose whole purpose is to be polled for a minute and forgotten.

#pragma once

#include "xpcog/core/remote/PlayerControl.hpp"

#include <cstddef>
#include <deque>
#include <optional>
#include <string>

namespace xpcog::app {

class RemoteJobs {
public:
    /// A new job, queued. Answers the id the client will ask about.
    [[nodiscard]] std::string start();

    void setRunning(const std::string& id, int done, int total);
    void finish(const std::string& id, std::size_t added);
    void fail(const std::string& id, std::string error);

    [[nodiscard]] std::optional<remote::JobStatus> find(std::string_view id) const;

private:
    static constexpr std::size_t kMaxJobs = 32;

    [[nodiscard]] remote::JobStatus* lookup(std::string_view id);

    std::deque<remote::JobStatus> jobs_;
    std::size_t                   nextId_ = 1;
};

}  // namespace xpcog::app
