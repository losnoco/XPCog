#pragma once

#include "xpcog/core/Url.hpp"

#include <winrt/base.h>

#include <vector>

namespace xpcog::winui {

/// One player per user. Call first thing after the runtime is bootstrapped,
/// before anything expensive -- a later launch's only job is to hand its
/// files over and go, and every codec registered or database opened first is
/// time spent on a process about to exit.
///
/// True: this is the player, carry on. False: another one is running and has
/// been handed this launch, or the wx player is running and this one has
/// said so; either way, exit.
///
/// The handover is the Windows App SDK's AppInstance: the first instance
/// claims a key, a later one finds it taken and redirects its activation --
/// its command line -- to the holder, whose Activated event opens the files.
[[nodiscard]] bool claimInstance();

/// The files and URLs a launch named, from its command line: local paths that
/// exist as they are, anything else that parses as a URL as one. The
/// executable's own path, which an unpackaged launch's arguments begin with,
/// is not among them.
[[nodiscard]] std::vector<Url> urlsFromCommandLine(const winrt::hstring& commandLine);

}  // namespace xpcog::winui
