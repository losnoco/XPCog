// One XPCog per user, with later launches handing their files to the first.
//
// The reason is not tidiness, it is the audio device and the library. Two
// copies of a player fight over both: the second one opens the same SQLite
// library the first has open, and double-clicking a file in Explorer while
// something is already playing starts a second stream over the top of it rather
// than doing what the user meant, which is "play this".
//
// Cog gets this for free -- macOS launches one instance of an app bundle and
// sends the rest as -application:openFiles: events, so AppController never has
// to think about it. Nothing off macOS works that way: Explorer and every Linux
// file manager run the executable again, so the arbitration has to be ours.
//
// A QLocalServer is the arbitration. The first process to claim the name owns
// it; anything that can connect to that name is a later launch, hands over its
// arguments and exits. On Windows the name is a pipe, on Unix a socket file --
// Qt hides the difference, but not the consequence of a crash, which is handled
// in claim().
//
// Written as "did I get the name" rather than as a lock file so that the answer
// and the delivery channel are the same object. A lock file tells you another
// instance exists and then leaves you with no way to talk to it.

#pragma once

#include <QObject>
#include <QStringList>

class QLocalServer;

namespace xpcog::app {

class SingleInstance : public QObject {
    Q_OBJECT

public:
    /// `name` overrides the socket name, which otherwise identifies "XPCog, for
    /// this user". Tests must pass one: the default deliberately collides with a
    /// running player, so a test using it would claim nothing on a machine with
    /// XPCog open and pass or fail depending on what else is running.
    explicit SingleInstance(QString name = {}, QObject* parent = nullptr);

    /// Tries to become *the* instance.
    ///
    /// Returns true when this process now owns the name and should carry on
    /// starting up. Returns false when another instance is already running, in
    /// which case `arguments` have been handed to it and this process should
    /// exit without opening a window.
    [[nodiscard]] bool claim(const QStringList& arguments);

signals:
    /// A later launch arrived. The list is its file arguments, which may be
    /// empty -- someone launching the app again with no files is asking for the
    /// window they already have, so the receiver should still raise it.
    void launched(const QStringList& arguments);

private:
    QString       name_;
    QLocalServer* server_ = nullptr;
};

}  // namespace xpcog::app
