#include "instance.h"

#include "playback.h"

#include <QDeadlineTimer>
#include <QLocalSocket>
#include <QThread>
#include <QTimer>

#include <chrono>

namespace {
constexpr int frameLimit = 64;
constexpr auto timeout = std::chrono::milliseconds(2500);
}

bool Instance::validRequest(const QString &uri) {
    return uri.isEmpty() || (Playback::playableUri(uri) && !uri.startsWith("spotify:episode:"));
}

bool Instance::forward(const QString &name, const QString &uri) {
    QLocalSocket socket;
    QDeadlineTimer deadline(timeout);
    for (;;) {
        socket.connectToServer(name);
        if (socket.waitForConnected(100))
            break;
        if (deadline.hasExpired())
            return false;
        socket.abort();
        QThread::msleep(25);
    }
    const QByteArray frame = uri.toLatin1() + '\0';
    if (socket.write(frame) != frame.size())
        return false;
    while (socket.bytesAvailable() < 1) {
        if (deadline.hasExpired() || !socket.waitForReadyRead(int(deadline.remainingTime())))
            return false;
    }
    return socket.read(1) == "1";
}

bool Instance::listen(const QString &name, const std::function<void(const QString &)> &received) {
    // Only the process holding the instance lock may remove a stale socket.
    removeServer(name);
    setSocketOptions(QLocalServer::UserAccessOption);
    setMaxPendingConnections(16);
    if (!QLocalServer::listen(name))
        return false;
    connect(this, &QLocalServer::newConnection, this, [this, received] {
        auto *socket = nextPendingConnection();
        if (!socket)
            return;
        socket->setReadBufferSize(frameLimit);
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        QTimer::singleShot(timeout, socket, [socket] {
            socket->abort();
            socket->deleteLater();
        });
        const auto read = [socket, received] {
            const QByteArray frame = socket->peek(frameLimit);
            const auto end = frame.indexOf('\0');
            if (end < 0 && frame.size() < frameLimit)
                return;
            socket->readAll();
            const QString uri = QString::fromLatin1(frame.first(end < 0 ? 0 : end));
            if (end >= 0 && end == frame.size() - 1 && Instance::validRequest(uri)) {
                received(uri);
                socket->write("1", 1);
            } else {
                socket->write("0", 1);
            }
            socket->disconnectFromServer();
        };
        connect(socket, &QLocalSocket::readyRead, socket, read);
        read();
    });
    return true;
}
