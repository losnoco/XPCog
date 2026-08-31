#include "RemoteJobs.hpp"

#include <algorithm>
#include <utility>

namespace xpcog::app {

std::string RemoteJobs::start() {
    remote::JobStatus job;
    job.id    = "scan-" + std::to_string(nextId_++);
    // "queued" rather than "running", and it is true rather than cautious:
    // MainFrame runs one scan at a time because the PluginCache is not
    // synchronised, so a job really can sit waiting for the one before it.
    job.state = "queued";

    const std::string id = job.id;
    jobs_.push_back(std::move(job));
    while (jobs_.size() > kMaxJobs) {
        jobs_.pop_front();
    }
    return id;
}

remote::JobStatus* RemoteJobs::lookup(std::string_view id) {
    const auto found = std::find_if(jobs_.begin(), jobs_.end(),
                                    [id](const remote::JobStatus& job) {
                                        return job.id == id;
                                    });
    return found == jobs_.end() ? nullptr : &*found;
}

void RemoteJobs::setRunning(const std::string& id, int done, int total) {
    if (remote::JobStatus* job = lookup(id); job != nullptr) {
        job->state = "running";
        job->done  = done;
        job->total = total;
    }
}

void RemoteJobs::finish(const std::string& id, std::size_t added) {
    if (remote::JobStatus* job = lookup(id); job != nullptr) {
        job->state = "done";
        job->added = added;
        // So a client polling sees 900/900 rather than whatever the last
        // progress report happened to be.
        if (job->total > 0) {
            job->done = job->total;
        }
    }
}

void RemoteJobs::fail(const std::string& id, std::string error) {
    if (remote::JobStatus* job = lookup(id); job != nullptr) {
        job->state = "failed";
        job->error = std::move(error);
    }
}

std::optional<remote::JobStatus> RemoteJobs::find(std::string_view id) const {
    const auto found = std::find_if(jobs_.begin(), jobs_.end(),
                                    [id](const remote::JobStatus& job) {
                                        return job.id == id;
                                    });
    if (found == jobs_.end()) {
        return std::nullopt;
    }
    return *found;
}

}  // namespace xpcog::app
