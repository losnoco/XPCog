// Shelling out to the fixture encoders, portably.
//
// Forty-one tests build their fixtures by invoking a command-line encoder
// through std::system, which runs /bin/sh on POSIX and cmd.exe on Windows. The
// two share no syntax for any of the things those call sites need: discarding
// the encoder's diagnostics, asking whether a tool exists, and naming a program
// by an absolute path.
//
// Neither failure is loud. `2>/dev/null` under cmd.exe is parsed as a
// redirection to a file called \dev\null; that directory does not exist, so the
// redirection fails, the encoder never runs, and std::system returns non-zero --
// which every call site reads as "encoder not installed" and turns into a skip.
// `command -v` is a POSIX shell builtin cmd.exe does not have, so the probe that
// guards the scanner fixtures answered "no tool" for the same reason.
//
// The effect was that installing the encoders on Windows changed nothing: the
// tests skipped anyway, and a skip is not a failure. That is precisely the trap
// docs/PORTING.md warns about -- "check the skip count, not just the pass rate"
// -- wearing a platform disguise, so it is worth one shared header rather than
// an #ifdef at each of the seven sites.

#pragma once

#include <cstdlib>
#include <string>

namespace xpcog::test {

/// Append to a command to discard its diagnostics.
inline constexpr const char* kSilenceStderr =
#ifdef _WIN32
    " 2>NUL";
#else
    " 2>/dev/null";
#endif

/// True when `name` is runnable, so the caller can skip rather than fail.
[[nodiscard]] inline bool haveTool(const char* name) {
#ifdef _WIN32
    // `where` prints the resolved path and exits non-zero when there is none.
    const std::string probe = std::string{"where "} + name + " >NUL 2>NUL";
#else
    const std::string probe = std::string{"command -v "} + name + " >/dev/null 2>&1";
#endif
    return std::system(probe.c_str()) == 0;
}

/// A command line, made safe to hand to std::system().
///
/// Only does anything on Windows, and only to a command whose *program* is a
/// quoted path -- which is any encoder that is not on PATH and has to be named
/// in full. cmd.exe strips the first and last quote character of the line it is
/// given, so
///
///     "C:/.../mpcenc.exe" --silent "in.wav" "out.mpc"
///
/// arrives at the program as something that names no file it can find, and
/// std::system returns 1 with nothing written. Wrapping the whole line in one
/// more pair leaves the intended command after that strip.
///
/// Guarded on the leading quote rather than applied to everything: cmd's rule
/// depends on how many quotes the line has, so wrapping a command that does not
/// need it is not reliably a no-op.
[[nodiscard]] inline std::string shellCommand(std::string command) {
#ifdef _WIN32
    if (!command.empty() && command.front() == '"') {
        return "\"" + command + "\"";
    }
#endif
    return command;
}

}  // namespace xpcog::test
