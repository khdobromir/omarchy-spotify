#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>

struct Playback {
    bool loggedIn = false;
    QString status;
    QString uri;
    QString title;
    QString artist;
    QString album;
    QUrl art;
    QUrl artHigh;
    int duration = 0;

    bool apply(const QByteArray &line, qint64 now);
    int position(qint64 now) const;
    static bool playableUri(const QString &uri);

private:
    void setItem(const QJsonObject &item);
    void setPosition(const QJsonObject &position, qint64 now);

    int m_position = 0;
    qint64 m_sampledAt = 0;
    double m_speed = 0;
};
