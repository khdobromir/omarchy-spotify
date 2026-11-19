#pragma once

#include <QLocalServer>

#include <functional>

class Instance : public QLocalServer {
public:
    static bool validRequest(const QString &uri);
    static bool forward(const QString &name, const QString &uri);
    bool listen(const QString &name, const std::function<void(const QString &)> &received);
};
