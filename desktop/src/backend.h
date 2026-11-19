#pragma once

#include "playback.h"

#include <QCache>
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
    Q_PROPERTY(QVariantList entries READ entries NOTIFY entriesChanged)
    Q_PROPERTY(QString libraryStatus READ libraryStatus NOTIFY libraryChanged)
    Q_PROPERTY(QString category READ category NOTIFY libraryChanged)
    Q_PROPERTY(QString collectionUri READ collectionUri NOTIFY libraryChanged)
    Q_PROPERTY(QVariantMap collection READ collection NOTIFY libraryChanged)
    Q_PROPERTY(int offset READ offset NOTIFY libraryChanged)
    Q_PROPERTY(int nextOffset READ nextOffset NOTIFY libraryChanged)
    Q_PROPERTY(bool webBusy READ webBusy NOTIFY libraryChanged)
    Q_PROPERTY(int favorite READ favorite NOTIFY libraryChanged)
    Q_PROPERTY(bool commandBusy READ commandBusy NOTIFY playerChanged)
    Q_PROPERTY(QString selectedUri READ selectedUri NOTIFY playerChanged)
    Q_PROPERTY(QVariantMap selection READ selection NOTIFY selectionChanged)
    Q_PROPERTY(QString selectionStatus READ selectionStatus NOTIFY playerChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    QVariantMap playback() const;
    int position() const;
    QString playerStatus() const { return m_playerStatus; }
    QVariantList entries() const { return m_entries; }
    QString libraryStatus() const { return m_libraryStatus; }
    QString category() const { return m_category; }
    QString collectionUri() const { return m_collectionUri; }
    QVariantMap collection() const { return m_collection; }
    int offset() const { return m_offset; }
    int nextOffset() const { return m_nextOffset; }
    bool webBusy() const { return !m_webOperation.isEmpty(); }
    int favorite() const { return m_favorite; }
    bool commandBusy() const { return m_control.state() != QProcess::NotRunning; }
    QString selectedUri() const { return m_selectedUri; }
    QVariantMap selection() const { return m_selection; }
    QString selectionStatus() const { return m_selectionStatus; }

    void openUri(const QString &uri);
    Q_INVOKABLE void saveKey(const QString &key);
    Q_INVOKABLE void startPlayer();
    Q_INVOKABLE void play(const QString &uri = {});
    Q_INVOKABLE void togglePlayback();
    Q_INVOKABLE void skip(bool next);
    Q_INVOKABLE void seek(int position, const QString &uri);
    Q_INVOKABLE void loadLibrary(const QString &category, int offset);
    Q_INVOKABLE void openCollection(const QString &uri, const QVariantMap &metadata = {});
    Q_INVOKABLE void back();
    Q_INVOKABLE void loadPage(int offset);
    Q_INVOKABLE void retryLibrary();
    Q_INVOKABLE void dismissSelection();
    Q_INVOKABLE void connectLibrary();
    Q_INVOKABLE void toggleFavorite();

signals:
    void playbackChanged();
    void positionChanged();
    void playerChanged();
    void libraryChanged();
    void entriesChanged();
    void selectionChanged();

private:
    struct LibraryPage {
        QDeadlineTimer expires;
        QJsonObject data;
    };

    void setPlayerStatus(const QString &status);
    void tryTrace();
    void readTrace();
    void playerStopped(const QString &status);
    void command(const QStringList &args);
    void playPending();
    void controlFinished(bool success);
    bool webRequest(const QString &operation, const QStringList &args = {});
    void webFinished(const QJsonObject &result);
    void checkFavorite();
    void navigate(int offset, bool refresh = false);
    void loadPending();
    void applyPage(const QJsonObject &result);
    void updateSelection();
    QString pageKey() const;

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
    QString m_pendingPlay;
    QString m_activePlay;
    QString m_selectedUri;
    QString m_selectionStatus;
    QVariantMap m_selection;
    bool m_lookupSelection = false;

    QTemporaryFile m_helper;
    QProcess m_web;
    QDeadlineTimer m_backoff;
    QString m_webOperation;
    QStringList m_webArguments;
    QCache<QString, LibraryPage> m_pages{24};
    QVariantList m_entries;
    QString m_libraryStatus = "loading";
    QString m_category = "tracks";
    QString m_collectionUri;
    QVariantMap m_collection;
    int m_rootOffset = 0;
    int m_offset = 0;
    int m_nextOffset = -1;
    int m_favorite = -1;
    QString m_checkedTrack;
    bool m_loadPending = true;
};
