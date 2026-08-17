// Handing the foreground to another process.
//
// Windows does not let a process raise its own window whenever it likes. Only a
// process that already owns the foreground -- or one just launched by the process
// that does -- may call SetForegroundWindow and be obeyed; for anyone else the
// call is silently refused and the taskbar button flashes orange instead. Qt's
// QWidget::activateWindow() is SetForegroundWindow, so it inherits the rule.
//
// That is exactly the situation the single-instance handover is in, and it is
// backwards from what it looks like. The process that *wants* the foreground is
// the one already running, which does not have the right. The process that *has*
// the right is the one Explorer just started, which is about to exit and does not
// want it. So the transient process gives its claim away before it goes, which is
// what AllowSetForegroundWindow exists for.
//
// Discovered by hand: opening a file with an already-running XPCog added the
// track and highlighted the taskbar button instead of coming forward. Nothing
// fails and nothing logs, so the symptom is a window that simply does not appear.
//
// A no-op everywhere else. macOS and Linux both let an application activate
// itself, and macOS never reaches this path at all -- LaunchServices delivers a
// second open as an event rather than a second process.

#pragma once

namespace xpcog::platform {

/// Lets another process take the foreground, when this one is entitled to it.
///
/// Call from a process that is about to hand work to a different one and exit.
/// The permission is short-lived and belongs to the calling process, so there is
/// nothing to undo.
void permitForegroundHandover();

}  // namespace xpcog::platform
