// Finding the files XPCog ships beside itself.
//
// Not the same question as "where does application data live", which
// Library.hpp says core has no business answering -- that one is per-OS policy
// about the *user's* directories, and the path is injected. This one is a fact
// about the running process: the bank, and whatever ships next to it later, sit
// at a known place relative to the executable, and the executable can say where
// it is without asking anyone.
//
// Cog answers it with `[[NSBundle mainBundle] pathForResource:]`. There is no
// portable equivalent, so this is the executable's own path plus the layout
// each platform puts resources in:
//
//   Windows   <exe dir>/<name>
//   Linux     <exe dir>/<name>, then <exe dir>/../share/xpcog/<name>
//   macOS     <exe dir>/../Resources/<name>, then <exe dir>/<name>
//
// The build tree needs no special case. CMake copies the assets next to the
// binaries it builds, so a test, the CLI and the app all find them the same way
// they will once installed -- which is worth more than it costs, because a
// lookup that only works when installed is one nobody exercises until then.

#pragma once

#include <filesystem>
#include <string_view>

namespace xpcog {

/// The directory the running executable itself is in.
///
/// Not the same answer as assetDirectory() and deliberately so: inside a macOS
/// bundle this is Contents/MacOS, where a *program* shipped beside XPCog lives,
/// while its data lives one level over in Contents/Resources. The one thing
/// that needs this rather than the other is the crash handler -- crashpad ships
/// a helper executable, and a helper in Resources is in the wrong place for
/// both the loader and the code signature.
///
/// Empty when the platform will not say where the executable is.
[[nodiscard]] std::filesystem::path executableDirectory();

/// The directory this executable's shipped assets are in. Empty when the
/// platform will not say where the executable is, which is not expected but is
/// not worth aborting over either -- every caller has something to do without
/// the asset.
[[nodiscard]] std::filesystem::path assetDirectory();

/// `name` resolved inside assetDirectory(), or empty when no such file exists.
///
/// Checked rather than merely joined, because the answer feeds decisions --
/// whether to default to the SoundFont synthesiser at all -- and a path that
/// names nothing would turn "no bank shipped" into "a bank that fails to open".
[[nodiscard]] std::filesystem::path assetPath(std::string_view name);

}  // namespace xpcog
