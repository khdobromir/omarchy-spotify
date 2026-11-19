#pragma once

#include <QLocalServer>

#include <functional>
#include <optional>

class Instance : public QLocalServer {
public:
    enum class Action { Activate, OpenTrack, PlayTrack, OpenAlbum, OpenPlaylist };
    struct Request {
        Action action;
        QString uri;
    };

    static std::optional<Request> parse(const QStringList &arguments);
    static bool forward(const QString &name, const QStringList &arguments);
    bool listen(const QString &name, const std::function<void(const Request &)> &received);
};
