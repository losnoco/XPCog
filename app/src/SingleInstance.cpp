#include "SingleInstance.hpp"

#include <QDataStream>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>

#include <memory>
#include <utility>

namespace xpcog::app {
namespace {

/// Per user, not per machine.
///
/// On Unix the socket lives in a shared directory, so a bare "XPCog" would mean
/// the first user to log in owns the name and the second one's player silently
/// hands its files to a session it cannot see. Windows pipes are already
/// session-scoped, but the name costs nothing there and keeping one form avoids
/// two behaviours to reason about.
QString serverName() {
    QString user = qEnvironmentVariable("USER");
    if (user.isEmpty()) {
        user = qEnvironmentVariable("USERNAME");
    }
    if (user.isEmpty()) {
        user = QStringLiteral("default");
    }
    // The name ends up as a path component on Unix and a pipe name on Windows,
    // and a Windows username can carry a domain separator. Keep the characters
    // both forms agree on rather than guessing which need escaping.
    QString sanitised;
    sanitised.reserve(user.size());
    for (const QChar character : user) {
        if (character.isLetterOrNumber()) {
            sanitised.append(character);
        }
    }
    return QStringLiteral("XPCog-") + sanitised;
}

/// How long to wait for the running instance. Short: this runs before the
/// window appears, so every millisecond here is launch latency the user sees on
/// the *normal* path, where there is no other instance and the connection fails
/// immediately anyway.
constexpr int kConnectTimeoutMs = 500;
constexpr int kWriteTimeoutMs   = 1000;

}  // namespace

SingleInstance::SingleInstance(QString name, QObject* parent)
    : QObject(parent), name_(name.isEmpty() ? serverName() : std::move(name)) {}

bool SingleInstance::claim(const QStringList& arguments) {
    const QString& name = name_;

    // Ask first, claim second. Connecting is also how a stale name is told from
    // a live one below: a name that refuses connections is left over from a
    // process that died, and removing it is safe. Removing it because listen()
    // failed, without having tried to connect, would evict a running instance.
    {
        QLocalSocket socket;
        socket.connectToServer(name);
        if (socket.waitForConnected(kConnectTimeoutMs)) {
            // Serialised twice on purpose: the arguments into `inner`, then
            // `inner` into the wire. Writing a QByteArray through a QDataStream
            // prefixes it with its own length, and that length is the frame the
            // receiver needs -- see the read side for why hanging up is not one.
            QByteArray  inner;
            QDataStream packer(&inner, QIODevice::WriteOnly);
            packer.setVersion(QDataStream::Qt_6_0);
            packer << arguments;

            QByteArray  wire;
            QDataStream stream(&wire, QIODevice::WriteOnly);
            stream.setVersion(QDataStream::Qt_6_0);
            stream << inner;

            socket.write(wire);
            socket.waitForBytesWritten(kWriteTimeoutMs);
            // Wait for the receiver to close rather than closing first. This
            // process is about to exit, and exiting with bytes still in the pipe
            // drops them: the running instance raises its window having been told
            // nothing, so the file the user double-clicked never opens.
            socket.waitForDisconnected(kWriteTimeoutMs);
            return false;
        }
    }

    server_ = new QLocalServer(this);
    if (!server_->listen(name)) {
        // Unix leaves the socket file behind when a process is killed, and the
        // connect above already established that nothing is listening on it.
        QLocalServer::removeServer(name);
        if (!server_->listen(name)) {
            // Still no. Rather than refuse to start over a feature, run as a
            // second instance: worse than single-instance, better than a player
            // that will not open.
            delete server_;
            server_ = nullptr;
            return true;
        }
    }

    connect(server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = server_->nextPendingConnection()) {
            // Framed by the length prefix, and read as it arrives.
            //
            // The obvious alternative -- one message per connection, read whole
            // on disconnected() -- does not work on Windows, and its failure is
            // the quiet kind: readAll() there returns nothing once the peer has
            // gone, so every handover deserialised to an empty list. That is
            // indistinguishable from a relaunch carrying no files, so the window
            // came forward and the file was silently dropped. A test caught it;
            // by hand it would have looked like a decoder problem.
            auto pending = std::make_shared<QByteArray>();
            connect(socket, &QLocalSocket::readyRead, this, [this, socket, pending] {
                pending->append(socket->readAll());

                constexpr qsizetype kHeader = sizeof(quint32);
                if (pending->size() < kHeader) {
                    return;
                }
                QDataStream header(*pending);
                header.setVersion(QDataStream::Qt_6_0);
                quint32 length = 0;
                header >> length;
                if (pending->size() < kHeader + static_cast<qsizetype>(length)) {
                    return;
                }

                QDataStream stream(*pending);
                stream.setVersion(QDataStream::Qt_6_0);
                QByteArray inner;
                stream >> inner;

                QDataStream unpacker(inner);
                unpacker.setVersion(QDataStream::Qt_6_0);
                // Not `arguments`: that is claim()'s parameter, meaning *our*
                // command line, and these are someone else's. GCC caught the
                // shadowing; the confusion it invites is the actual reason to
                // rename rather than silence it.
                QStringList received;
                unpacker >> received;

                // Closing from this end is what releases the sender, which is
                // waiting to be sure the message landed before it exits.
                socket->disconnectFromServer();
                emit launched(received);
            });
            connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);
        }
    });

    return true;
}

}  // namespace xpcog::app
