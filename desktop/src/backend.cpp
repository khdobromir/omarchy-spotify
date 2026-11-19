#include "backend.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <chrono>
#include <utility>

namespace {
QByteArray readSmall(const QString &path, qint64 limit = 8192) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = file.read(limit + 1);
    return data.size() <= limit ? data.trimmed() : QByteArray{};
}

QString trackId(const Playback &playback) {
    return playback.uri.startsWith("spotify:track:") ? playback.uri.mid(14) : QString{};
}
}

Backend::Backend(QObject *parent)
    : QObject(parent), m_home(QStandardPaths::writableLocation(QStandardPaths::StateLocation)),
      m_dataDir(m_home + "/soloist"), m_keyFile(m_home + "/soloist-key"),
      m_helper(QDir::temp().filePath("omarchy-spotify-XXXXXX")) {
    m_daemon.setStandardOutputFile(QProcess::nullDevice());
    m_daemon.setStandardErrorFile(QProcess::nullDevice());
    m_trace.setStandardErrorFile(QProcess::nullDevice());
    m_control.setStandardOutputFile(QProcess::nullDevice());
    m_control.setStandardErrorFile(QProcess::nullDevice());
    m_web.setStandardErrorFile(QProcess::nullDevice());

    m_startup.setSingleShot(true);
    m_startup.setInterval(15000);
    connect(&m_startup, &QTimer::timeout, this, [this] {
        if (m_playerStatus == "starting") {
            setPlayerStatus("player_timeout");
            m_daemon.terminate();
        }
    });
    m_tick.setInterval(250);
    connect(&m_tick, &QTimer::timeout, this, &Backend::positionChanged);
    m_traceRetry.setSingleShot(true);
    m_traceRetry.setInterval(100);
    connect(&m_traceRetry, &QTimer::timeout, this, &Backend::tryTrace);
    connect(&m_watch, &QFileSystemWatcher::directoryChanged, this, &Backend::tryTrace);
    connect(&m_daemon, &QProcess::started, this, &Backend::tryTrace);
    connect(&m_daemon, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        if (code == 10)
            playerStopped("player_expired");
        else if (m_playerStatus == "player_connection" || m_playerStatus == "player_timeout")
            playerStopped(m_playerStatus);
        else
            playerStopped("player_failed");
    });
    connect(&m_daemon, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            playerStopped("player_failed");
    });
    connect(&m_trace, &QProcess::readyReadStandardOutput, this, &Backend::readTrace);
    connect(&m_trace, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        if (m_daemon.state() == QProcess::Running) {
            playerStopped("player_connection");
            m_daemon.terminate();
        }
    });
    connect(&m_control, &QProcess::stateChanged, this, &Backend::playerChanged);
    connect(&m_control, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        controlFinished(code == 0 && status == QProcess::NormalExit);
    });
    connect(&m_control, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            controlFinished(false);
    });
    connect(&m_web, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        const QJsonDocument response = QJsonDocument::fromJson(m_web.readAllStandardOutput());
        webFinished(response.isObject() ? response.object() : QJsonObject{});
    });
    connect(&m_web, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            webFinished({});
    });

    QFile source(":/spotify.py");
    if (source.open(QIODevice::ReadOnly) && m_helper.open()) {
        const QByteArray contents = source.readAll();
        if (m_helper.write(contents) == contents.size() && m_helper.flush())
            m_helper.close();
    }
    QTimer::singleShot(0, this, [this] {
        startPlayer();
        loadPending();
    });
}

Backend::~Backend() {
    for (QProcess *process : {&m_control, &m_trace, &m_daemon, &m_web}) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->terminate();
            if (!process->waitForFinished(400)) {
                process->kill();
                process->waitForFinished(400);
            }
        }
    }
}

QVariantMap Backend::playback() const {
    return {{"ready", m_playback.loggedIn}, {"playing", m_playback.status == "playing"},
            {"uri", m_playback.uri}, {"title", m_playback.title}, {"artist", m_playback.artist}, {"album", m_playback.album},
            {"art", m_playback.art}, {"artHigh", m_playback.artHigh}, {"duration", m_playback.duration}};
}

int Backend::position() const {
    return m_playback.position(QDateTime::currentMSecsSinceEpoch());
}

void Backend::setPlayerStatus(const QString &status) {
    if (m_playerStatus != status) {
        m_playerStatus = status;
        emit playerChanged();
    }
}

void Backend::saveKey(const QString &input) {
    const QByteArray key = input.trimmed().toUtf8();
    if (key.isEmpty() || key.size() > 8192 || key.contains('\n')
        || m_daemon.state() != QProcess::NotRunning)
        return;
    QSaveFile file(m_keyFile);
    if (!QDir().mkpath(m_home) || !file.open(QIODevice::WriteOnly)
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(key) != key.size() || !file.commit()) {
        setPlayerStatus("key_write");
        return;
    }
    startPlayer();
}

void Backend::startPlayer() {
    if (m_daemon.state() != QProcess::NotRunning)
        return;
    m_soloist = QStandardPaths::findExecutable("soloist");
    const QString local = QDir::home().filePath(".local/bin/soloist");
    if (m_soloist.isEmpty() && QFileInfo(local).isExecutable())
        m_soloist = local;
    if (m_soloist.isEmpty()) {
        setPlayerStatus("player_missing");
        return;
    }
    const QByteArray key = readSmall(m_keyFile);
    if (key.isEmpty()) {
        setPlayerStatus("key_required");
        return;
    }
    if (!QDir().mkpath(m_dataDir)
        || !QFile::setPermissions(m_dataDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        setPlayerStatus("player_failed");
        return;
    }
    if (!m_watch.directories().contains(m_dataDir) && !m_watch.addPath(m_dataDir)) {
        setPlayerStatus("player_connection");
        return;
    }
    setPlayerStatus("starting");
    m_startup.start();
    m_daemon.start(m_soloist, {"--device-name", "Omarchy Spotify", "--api-key", QString::fromUtf8(key),
                              "--data-dir", m_dataDir, "--ws", "127.0.0.1:0"});
}

void Backend::tryTrace() {
    if (m_trace.state() != QProcess::NotRunning || m_daemon.state() != QProcess::Running)
        return;
    bool pidValid = false;
    const auto pid = readSmall(m_dataDir + "/soloist.pid", 24).toLongLong(&pidValid);
    bool portValid = false;
    const auto port = readSmall(m_dataDir + "/ws.port", 6).toUShort(&portValid);
    if (pidValid && pid == m_daemon.processId() && portValid && port
        && readSmall(m_dataDir + "/ws.addr", 32) == "127.0.0.1") {
        m_traceRetry.stop();
        m_trace.start(m_soloist, {"ctl", "trace", "--data-dir", m_dataDir});
    } else if (m_startup.isActive() && !m_traceRetry.isActive()) {
        m_traceRetry.start();
    }
}

void Backend::readTrace() {
    bool changed = false;
    while (m_trace.canReadLine())
        changed |= m_playback.apply(m_trace.readLine(), QDateTime::currentMSecsSinceEpoch());
    if (!changed)
        return;
    m_startup.stop();
    m_traceRetry.stop();
    setPlayerStatus(m_playback.loggedIn ? "ready" : "pairing");
    if (m_playback.status == "playing") {
        if (!m_tick.isActive())
            m_tick.start();
    } else {
        m_tick.stop();
    }
    if (m_checkedTrack != trackId(m_playback)) {
        m_favorite = -1;
        emit libraryChanged();
    }
    emit playbackChanged();
    emit positionChanged();
    if (!m_selectedUri.isEmpty() && m_playback.uri == m_selectedUri && m_pendingPlay.isEmpty()) {
        m_selectionStatus.clear();
        m_lookupSelection = false;
        emit playerChanged();
    }
    playPending();
    checkFavorite();
}

void Backend::playerStopped(const QString &status) {
    m_startup.stop();
    m_tick.stop();
    if (m_trace.state() != QProcess::NotRunning)
        m_trace.terminate();
    m_playback = {};
    m_checkedTrack.clear();
    m_favorite = -1;
    setPlayerStatus(status);
    emit playbackChanged();
    emit positionChanged();
    emit libraryChanged();
}

void Backend::command(const QStringList &args) {
    if (!m_playback.loggedIn || commandBusy())
        return;
    setPlayerStatus("ready");
    m_control.start(m_soloist, QStringList{"ctl"} + args + QStringList{"--data-dir", m_dataDir});
}

void Backend::play(const QString &uri) {
    if (uri.isEmpty())
        command({"play"});
    else if (Playback::playableUri(uri)) {
        m_selectedUri = m_pendingPlay = uri;
        m_selection.clear();
        m_lookupSelection = false;
        updateSelection();
        m_selectionStatus = "waiting";
        emit playerChanged();
        playPending();
    }
}

void Backend::openUri(const QString &uri) {
    if (!Playback::playableUri(uri))
        return;
    if (uri.startsWith("spotify:album:") || uri.startsWith("spotify:playlist:")) {
        openCollection(uri);
    } else {
        if (m_category != "tracks" || !m_collectionUri.isEmpty())
            loadLibrary("tracks", 0);
        play(uri);
        if (uri.startsWith("spotify:track:") && m_selection.isEmpty() && m_selectionStatus != "") {
            m_lookupSelection = true;
            loadPending();
        }
    }
}

void Backend::playPending() {
    if (m_pendingPlay.isEmpty() || commandBusy() || !m_playback.loggedIn)
        return;
    m_activePlay = std::exchange(m_pendingPlay, {});
    m_selectionStatus = "sent";
    emit playerChanged();
    command({"play", m_activePlay});
}

void Backend::controlFinished(bool success) {
    if (!success) {
        setPlayerStatus("player_command");
        if (!m_activePlay.isEmpty() && m_activePlay == m_selectedUri && m_pendingPlay.isEmpty())
            m_selectionStatus = "failed";
    } else if (m_activePlay == m_selectedUri && m_pendingPlay.isEmpty()) {
        m_selectionStatus.clear();
    }
    m_activePlay.clear();
    emit playerChanged();
    playPending();
}

void Backend::dismissSelection() {
    m_pendingPlay.clear();
    m_selectedUri.clear();
    m_selectionStatus.clear();
    m_lookupSelection = false;
    m_selection.clear();
    emit selectionChanged();
    emit playerChanged();
}

void Backend::updateSelection() {
    if (m_selectedUri == m_collectionUri)
        m_selection = m_collection;
    for (const QVariant &entry : m_entries) {
        const QVariantMap metadata = entry.toMap();
        if (metadata.value("uri") == m_selectedUri) {
            m_selection = metadata;
            m_lookupSelection = false;
            break;
        }
    }
    emit selectionChanged();
}

void Backend::togglePlayback() {
    if (m_playback.status == "playing")
        command({"pause"});
    else
        play();
}

void Backend::skip(bool next) {
    command({next ? "next" : "prev"});
}

void Backend::seek(int at, const QString &uri) {
    if (!uri.isEmpty() && m_playback.uri == uri && m_playback.duration > 0)
        command({"seek", QString::number(std::clamp(at, 0, m_playback.duration - 1))});
}

bool Backend::webRequest(const QString &operation, const QStringList &args) {
    if (webBusy() || !m_backoff.hasExpired())
        return false;
    if (m_helper.fileName().isEmpty() || m_helper.isOpen()) {
        m_libraryStatus = "unavailable";
        emit libraryChanged();
        return false;
    }
    m_webOperation = operation;
    m_webArguments = args;
    m_web.start("python3", QStringList{"-B", m_helper.fileName(), operation} + args);
    emit libraryChanged();
    return true;
}

void Backend::loadLibrary(const QString &kind, int page) {
    if (kind != "tracks" && kind != "albums" && kind != "playlists")
        return;
    m_collectionUri.clear();
    m_collection.clear();
    m_category = kind;
    navigate(page);
}

void Backend::openCollection(const QString &uri, const QVariantMap &metadata) {
    if (!Playback::playableUri(uri))
        return;
    const bool album = uri.startsWith("spotify:album:");
    if (!album && !uri.startsWith("spotify:playlist:"))
        return;
    if (m_collectionUri == uri && m_offset == 0 && m_libraryStatus == "ok")
        return;
    const QString category = album ? "albums" : "playlists";
    if (m_category != category)
        m_rootOffset = 0;
    m_category = category;
    m_collectionUri = uri;
    m_collection = metadata;
    navigate(0);
}

void Backend::back() {
    if (!m_collectionUri.isEmpty())
        loadLibrary(m_category, m_rootOffset);
}

void Backend::loadPage(int offset) {
    navigate(offset);
}

void Backend::retryLibrary() {
    navigate(m_offset, true);
}

QString Backend::pageKey() const {
    return (m_collectionUri.isEmpty() ? m_category : m_collectionUri) + '/' + QString::number(m_offset);
}

void Backend::navigate(int offset, bool refresh) {
    if (offset < 0 || offset > 100'000 || offset % 20 != 0)
        return;
    m_offset = offset;
    if (m_collectionUri.isEmpty())
        m_rootOffset = offset;
    const QString key = pageKey();
    if (const auto *cached = m_pages.object(key); !refresh && cached && !cached->expires.hasExpired()) {
        m_loadPending = false;
        applyPage(cached->data);
        return;
    }
    m_pages.remove(key);
    m_loadPending = true;
    m_nextOffset = -1;
    m_entries.clear();
    m_libraryStatus = "loading";
    emit libraryChanged();
    emit entriesChanged();
    loadPending();
}

void Backend::applyPage(const QJsonObject &result) {
    m_libraryStatus = result.value("status").toString("unavailable");
    m_entries = result.value("items").toArray().toVariantList();
    m_nextOffset = result.value("next").toInt(-1);
    if (result.value("collection").isObject())
        m_collection = result.value("collection").toObject().toVariantMap();
    updateSelection();
    emit libraryChanged();
    emit entriesChanged();
}

void Backend::loadPending() {
    if (m_loadPending) {
        if (webBusy())
            return;
        const bool detail = !m_collectionUri.isEmpty();
        m_loadPending = false;
        if (webRequest(detail ? "collection" : "library",
                       {detail ? m_collectionUri : m_category, QString::number(m_offset)}))
            return;
        m_libraryStatus = m_backoff.hasExpired() ? "unavailable" : "limited";
        emit libraryChanged();
    }
    if (m_lookupSelection && webRequest("track", {m_selectedUri.mid(14)}))
        m_lookupSelection = false;
}

void Backend::connectLibrary() {
    if (webRequest("connect")) {
        m_libraryStatus = "connecting";
        emit libraryChanged();
    }
}

void Backend::toggleFavorite() {
    const QString id = trackId(m_playback);
    if (!id.isEmpty() && m_favorite >= 0)
        webRequest(m_favorite == 0 ? "save" : "remove", {id});
}

void Backend::checkFavorite() {
    const QString id = trackId(m_playback);
    if (m_libraryStatus == "ok" && !id.isEmpty() && m_checkedTrack != id
        && webRequest("favorite", {id}))
        m_checkedTrack = id;
}

void Backend::webFinished(const QJsonObject &result) {
    const QString operation = std::exchange(m_webOperation, {});
    const QStringList arguments = std::exchange(m_webArguments, {});
    const QString status = result.value("status").toString("unavailable");
    if (status == "disconnected" || status == "reauthorize")
        m_pages.clear();
    const qint64 retry = result.value("retry").toInteger();
    if (retry > 0)
        m_backoff = QDeadlineTimer(std::min(retry, qint64{86400}) * 1000);
    if (operation == "library" || operation == "collection") {
        const QString key = arguments.value(0) + '/' + arguments.value(1);
        if (status == "ok")
            m_pages.insert(key, new LibraryPage{QDeadlineTimer(std::chrono::minutes(3)), result});
        if (key == pageKey()) {
            m_loadPending = false;
            applyPage(result);
        }
    } else if (operation == "track") {
        const QVariantMap metadata = result.value("track").toObject().toVariantMap();
        if (metadata.value("uri") == m_selectedUri) {
            m_selection = metadata;
            emit selectionChanged();
        }
    } else if (operation == "connect") {
        if (status == "authorized") {
            m_checkedTrack.clear();
            m_pages.clear();
            navigate(m_offset);
        } else if (!m_loadPending) {
            m_libraryStatus = status;
        }
    } else if (status == "ok") {
        if (arguments.value(0) == trackId(m_playback))
            m_favorite = result.value("saved").toBool() ? 1 : 0;
        if (operation == "save" || operation == "remove") {
            m_pages.clear();
            if (m_category == "tracks" && m_collectionUri.isEmpty())
                navigate(m_offset);
        }
    } else if (operation == "save" || operation == "remove") {
        if (!m_loadPending)
            m_libraryStatus = status;
    }
    emit libraryChanged();
    loadPending();
    checkFavorite();
}
