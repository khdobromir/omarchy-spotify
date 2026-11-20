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

    void applyRemote(const QJsonObject &sample, qint64 now);
    bool apply(const QByteArray &line, qint64 now);
    bool mergeMetadata(const QJsonObject &track);
    bool needsMetadata() const;
    int position(qint64 now) const;
    static bool playableUri(const QString &uri);

private:
    void setItem(const QJsonObject &item, qint64 now);
    void setStatus(const QString &next, qint64 now);
    void setPosition(const QJsonObject &position, qint64 now);

    int m_position = 0;
    qint64 m_sampledAt = 0;
    qint64 m_lastAnchor = -1;
    double m_speed = 0;
    int m_artSize = -1;
    int m_artFit = -1;
};
