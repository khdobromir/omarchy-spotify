#pragma once

#include "playback.h"

#include <QAbstractListModel>
#include <QCache>
#include <QHash>
#include <QDeadlineTimer>
#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QTemporaryFile>
#include <QTimer>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

class LibraryModel final : public QAbstractListModel {
public:
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    const QVariantList &items() const { return m_items; }
    void replace(QVariantList items);
    void append(const QVariantList &items);

private:
    QVariantList m_items;
};

class Backend : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(QVariantMap playback READ playback NOTIFY playbackChanged)
    Q_PROPERTY(int position READ position NOTIFY positionChanged)
    Q_PROPERTY(QString playerStatus READ playerStatus NOTIFY playerChanged)
    Q_PROPERTY(QAbstractItemModel *entries READ entries CONSTANT)
    Q_PROPERTY(QString libraryStatus READ libraryStatus NOTIFY libraryChanged)
    Q_PROPERTY(QString category READ category NOTIFY libraryChanged)
    Q_PROPERTY(QString collectionUri READ collectionUri NOTIFY libraryChanged)
    Q_PROPERTY(QVariantMap collection READ collection NOTIFY libraryChanged)
    Q_PROPERTY(QString libraryContext READ libraryContext NOTIFY libraryChanged)
    Q_PROPERTY(int nextOffset READ nextOffset NOTIFY libraryChanged)
    Q_PROPERTY(bool webBusy READ webBusy NOTIFY libraryChanged)
    Q_PROPERTY(bool webCoolingDown READ webCoolingDown NOTIFY libraryChanged)
    Q_PROPERTY(int favorite READ favorite NOTIFY libraryChanged)
    Q_PROPERTY(bool commandBusy READ commandBusy NOTIFY playerChanged)
    Q_PROPERTY(QString selectedUri READ selectedUri NOTIFY playerChanged)
    Q_PROPERTY(QVariantMap selection READ selection NOTIFY selectionChanged)
    Q_PROPERTY(bool selectionLoading READ selectionLoading NOTIFY selectionChanged)
    Q_PROPERTY(QString selectionStatus READ selectionStatus NOTIFY playerChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    QVariantMap playback() const;
    int position() const;
    QString playerStatus() const { return m_playerStatus; }
    QAbstractItemModel *entries() { return &m_entries; }
    QString libraryStatus() const { return m_context.status; }
    QString category() const { return m_category; }
    QString collectionUri() const { return m_collectionUri; }
    QVariantMap collection() const { return m_context.collection; }
    QString libraryContext() const { return m_collectionUri.isEmpty() ? m_category : m_collectionUri; }
    int nextOffset() const { return m_context.next; }
    bool webBusy() const { return !m_web.operation.isEmpty(); }
    bool webCoolingDown() const { return !m_backoff.hasExpired(); }
    int favorite() const { return m_favorite; }
    bool commandBusy() const { return m_control.state() != QProcess::NotRunning; }
    QString selectedUri() const { return m_selectedUri; }
    QVariantMap selection() const { return m_selection; }
    bool selectionLoading() const;
    QString selectionStatus() const { return m_selectionStatus; }

    void openUri(const QString &uri);
    void openTrack(const QString &uri);
    Q_INVOKABLE void saveKey(const QString &key);
    Q_INVOKABLE void startPlayer();
    Q_INVOKABLE void play(const QString &uri = {});
    Q_INVOKABLE void togglePlayback();
    Q_INVOKABLE void skip(bool next);
    Q_INVOKABLE void seek(int position, const QString &uri);
    Q_INVOKABLE void loadLibrary(const QString &category);
    Q_INVOKABLE void openCollection(const QString &uri, const QVariantMap &metadata = {});
    Q_INVOKABLE void back();
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE QVariantMap entryAt(int row) const { return m_entries.items().value(row).toMap(); }
    Q_INVOKABLE void rememberScroll(double position) { m_context.scroll = position; }
    Q_INVOKABLE double libraryScroll() const { return m_context.scroll; }
    Q_INVOKABLE void retryLibrary();
    Q_INVOKABLE void dismissSelection();
    Q_INVOKABLE void connectLibrary();
    Q_INVOKABLE void toggleFavorite();

signals:
    void playbackChanged();
    void positionChanged();
    void playerChanged();
    void libraryChanged();
    void libraryContextChanged();
    void selectionChanged();

private:
    struct LibraryContext {
        quint64 generation = 1;
        QVariantList items;
        QVariantMap collection;
        QString status = "loading";
        int next = 0;
        double scroll = 0;
        bool pending = true;
    };

    struct WebJob {
        QProcess process;
        QString operation;
        QStringList arguments;
    };

    void setPlayerStatus(const QString &status);
    void tryTrace();
    void readTrace();
    void playerStopped(const QString &status);
    void command(const QStringList &args);
    void playPending();
    void controlFinished(bool success);
    bool webRequest(const QString &operation, const QStringList &args = {});
    void webFinished(WebJob &job, const QJsonObject &result);
    void checkFavorite();
    void navigate(const QString &category, const QString &uri = {}, const QVariantMap &metadata = {});
    void reloadLibrary();
    void loadPending();
    void applyPage(LibraryContext &context, const QJsonObject &result);
    void updateSelection();
    void loadMetadata();
    void refreshMetadata();


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
    WebJob m_web;
    WebJob m_trackWeb;
    QDeadlineTimer m_backoff;
    QTimer m_webCooldown;
    QCache<QString, QJsonObject> m_metadata{48};
    QString m_metadataUri;
    quint64 m_metadataGeneration = 0;
    quint64 m_webMetadataGeneration = 0;
    bool m_metadataAttempted = false;
    bool m_metadataRetried = false;
    LibraryModel m_entries;
    LibraryContext m_context;
    QHash<QString, LibraryContext> m_roots;
    QCache<QString, LibraryContext> m_collections{3000};
    quint64 m_libraryGeneration = 1;
    quint64 m_webLibraryGeneration = 0;
    QString m_category = "tracks";
    QString m_collectionUri;
    int m_favorite = -1;
    QString m_checkedTrack;
};
