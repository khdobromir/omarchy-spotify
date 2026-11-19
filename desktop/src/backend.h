#pragma once

#include "playback.h"

#include <QDeadlineTimer>
#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QTemporaryFile>
#include <QTimer>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

class Backend : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(QVariantMap playback READ playback NOTIFY playbackChanged)
    Q_PROPERTY(int position READ position NOTIFY positionChanged)
    Q_PROPERTY(QString playerStatus READ playerStatus NOTIFY playerChanged)
    Q_PROPERTY(QVariantList entries READ entries NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryStatus READ libraryStatus NOTIFY libraryChanged)
    Q_PROPERTY(QString category READ category NOTIFY libraryChanged)
    Q_PROPERTY(int offset READ offset NOTIFY libraryChanged)
    Q_PROPERTY(int nextOffset READ nextOffset NOTIFY libraryChanged)
    Q_PROPERTY(bool webBusy READ webBusy NOTIFY libraryChanged)
    Q_PROPERTY(int favorite READ favorite NOTIFY libraryChanged)
    Q_PROPERTY(bool commandBusy READ commandBusy NOTIFY playerChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    QVariantMap playback() const;
    int position() const;
    QString playerStatus() const { return m_playerStatus; }
    QVariantList entries() const { return m_entries; }
    QString libraryStatus() const { return m_libraryStatus; }
    QString category() const { return m_category; }
    int offset() const { return m_offset; }
    int nextOffset() const { return m_nextOffset; }
    bool webBusy() const { return m_web.state() != QProcess::NotRunning; }
    int favorite() const { return m_favorite; }
    bool commandBusy() const { return m_control.state() != QProcess::NotRunning; }

    Q_INVOKABLE void saveKey(const QString &key);
    Q_INVOKABLE void startPlayer();
    Q_INVOKABLE void play(const QString &uri = {});
    Q_INVOKABLE void togglePlayback();
    Q_INVOKABLE void skip(bool next);
    Q_INVOKABLE void seek(int position, const QString &uri);
    Q_INVOKABLE void loadLibrary(const QString &category, int offset);
    Q_INVOKABLE void connectLibrary();
    Q_INVOKABLE void toggleFavorite();

signals:
    void playbackChanged();
    void positionChanged();
    void playerChanged();
    void libraryChanged();

private:
    void setPlayerStatus(const QString &status);
    void tryTrace();
    void readTrace();
    void playerStopped(const QString &status);
    void command(const QStringList &args);
    bool webRequest(const QString &operation, const QStringList &args = {});
    void webFinished(const QJsonObject &result);
    void checkFavorite();

    QString m_home;
    QString m_soloist;
    QString m_dataDir;
    QString m_keyFile;
    QString m_playerStatus = "starting";
    Playback m_playback;
    QProcess m_daemon;
    QProcess m_trace;
    QProcess m_control;
    QFileSystemWatcher m_watch;
    QTimer m_startup;
    QTimer m_traceRetry;
    QTimer m_tick;

    QTemporaryFile m_helper;
    QProcess m_web;
    QDeadlineTimer m_backoff;
    QString m_webOperation;
    QStringList m_webArguments;
    QVariantList m_entries;
    QString m_libraryStatus = "loading";
    QString m_category = "tracks";
    int m_offset = 0;
    int m_nextOffset = -1;
    int m_favorite = -1;
    QString m_checkedTrack;
};
