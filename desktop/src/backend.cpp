#include "backend.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <memory>
#include <utility>

namespace {
Q_LOGGING_CATEGORY(metadataLog, "omarchy.spotify.metadata", QtWarningMsg)
constexpr int maxLibraryOffset = 100'000;

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

int LibraryModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : int(m_items.size());
}

QVariant LibraryModel::data(const QModelIndex &index, int role) const {
    return index.isValid() && role == Qt::UserRole ? m_items.value(index.row()) : QVariant{};
}

QHash<int, QByteArray> LibraryModel::roleNames() const {
    static const QHash<int, QByteArray> roles{{Qt::UserRole, "entryData"}};
    return roles;
}

void LibraryModel::replace(QVariantList items) {
    beginResetModel();
    m_items = std::move(items);
    endResetModel();
}

void LibraryModel::append(const QVariantList &items) {
    if (items.isEmpty())
        return;
    const int first = rowCount();
    beginInsertRows({}, first, first + int(items.size()) - 1);
    m_items.append(items);
    endInsertRows();
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
    for (WebJob *job : {&m_web, &m_trackWeb}) {
        job->process.setStandardErrorFile(QProcess::nullDevice());
        connect(&job->process, &QProcess::finished, this, [this, job](int, QProcess::ExitStatus) {
            const QJsonDocument response = QJsonDocument::fromJson(job->process.readAllStandardOutput());
            webFinished(*job, response.isObject() ? response.object() : QJsonObject{});
        });
        connect(&job->process, &QProcess::errorOccurred, this, [this, job](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                webFinished(*job, {});
        });
    }
    m_webCooldown.setSingleShot(true);
    m_webCooldown.setTimerType(Qt::PreciseTimer);
    connect(&m_webCooldown, &QTimer::timeout, this, [this] {
        emit libraryChanged();
        loadPending();
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
    for (QProcess *process : {&m_control, &m_trace, &m_daemon, &m_web.process, &m_trackWeb.process}) {
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
            {"idle", m_playback.status == "idle"},
            {"loadingMetadata", m_playback.needsMetadata()
                && (!m_metadataAttempted || (!m_trackWeb.operation.isEmpty()
                                           && m_webMetadataGeneration == m_metadataGeneration))},
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
    const QString previousUri = m_playback.uri;
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
    refreshMetadata();
    if (previousUri != m_playback.uri) {
        m_checkedTrack.clear();
        m_favorite = -1;
        emit libraryChanged();
    }
    if (!m_selectedUri.isEmpty() && m_playback.uri == m_selectedUri && m_pendingPlay.isEmpty()
        && m_selectionStatus != "inspecting") {
        m_selectionStatus.clear();
        m_lookupSelection = false;
        emit playerChanged();
    }
    playPending();
    loadPending();
    emit playbackChanged();
    emit positionChanged();
    checkFavorite();
}

void Backend::playerStopped(const QString &status) {
    m_startup.stop();
    m_tick.stop();
    if (m_trace.state() != QProcess::NotRunning)
        m_trace.terminate();
    m_playback = {};
    refreshMetadata();
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
            loadLibrary("tracks");
        play(uri);
        if (uri.startsWith("spotify:track:") && m_selection.isEmpty() && m_selectionStatus != "") {
            m_lookupSelection = true;
            loadPending();
        }
    }
}

void Backend::openTrack(const QString &uri) {
    if (!uri.startsWith("spotify:track:") || !Playback::playableUri(uri) || uri == m_playback.uri)
        return;
    m_selectedUri = uri;
    m_selection = m_metadata.contains(uri) ? m_metadata.object(uri)->toVariantMap() : QVariantMap{};
    m_selectionStatus = "inspecting";
    m_lookupSelection = m_selection.isEmpty();
    updateSelection();
    loadPending();
    emit playerChanged();
}

void Backend::refreshMetadata() {
    if (m_metadataUri == m_playback.uri)
        return;
    m_metadataUri = m_playback.uri;
    ++m_metadataGeneration;
    m_metadataAttempted = false;
    m_metadataRetried = false;
    if (const auto *cached = m_metadata.object(m_metadataUri)) {
        m_playback.mergeMetadata(*cached);
        m_metadataAttempted = true;
    }
}

void Backend::playPending() {
    if (m_pendingPlay.isEmpty() || commandBusy() || !m_playback.loggedIn)
        return;
    m_activePlay = std::exchange(m_pendingPlay, {});
    if (m_selectionStatus != "inspecting") {
        m_selectionStatus = "sent";
        emit playerChanged();
    }
    command({"play", m_activePlay});
}

void Backend::controlFinished(bool success) {
    if (!success) {
        setPlayerStatus("player_command");
        if (!m_activePlay.isEmpty() && m_activePlay == m_selectedUri && m_pendingPlay.isEmpty()
            && m_selectionStatus != "inspecting")
            m_selectionStatus = "failed";
    } else if (m_activePlay == m_selectedUri && m_pendingPlay.isEmpty()
            && m_selectionStatus != "inspecting") {
        m_selectionStatus.clear();
    }
    m_activePlay.clear();
    emit playerChanged();
    playPending();
}

void Backend::dismissSelection() {
    if (m_selectionStatus != "inspecting")
        m_pendingPlay.clear();
    m_selectedUri.clear();
    m_selectionStatus.clear();
    m_lookupSelection = false;
    m_selection.clear();
    emit selectionChanged();
    emit playerChanged();
}

bool Backend::selectionLoading() const {
    return m_lookupSelection || (!m_trackWeb.operation.isEmpty()
        && m_selectedUri == "spotify:" + m_trackWeb.operation + ':' + m_trackWeb.arguments.value(0));
}

void Backend::updateSelection() {
    if (m_selectedUri.isEmpty())
        return;
    if (m_selectedUri == m_collectionUri)
        m_selection = m_context.collection;
    for (const QVariant &entry : m_entries.items()) {
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
    WebJob &job = operation == "track" || operation == "episode" ? m_trackWeb : m_web;
    if (!job.operation.isEmpty() || webCoolingDown())
        return false;
    if (m_helper.fileName().isEmpty() || m_helper.isOpen())
        return false;
    job.operation = operation;
    job.arguments = args;
    job.process.start("python3", QStringList{"-B", m_helper.fileName(), operation} + args);
    if (&job == &m_web)
        emit libraryChanged();
    return true;
}

void Backend::loadLibrary(const QString &kind) {
    if (kind == "tracks" || kind == "albums" || kind == "playlists")
        navigate(kind);
}

void Backend::openCollection(const QString &uri, const QVariantMap &metadata) {
    if (!Playback::playableUri(uri))
        return;
    if (uri.startsWith("spotify:album:"))
        navigate("albums", uri, metadata);
    else if (uri.startsWith("spotify:playlist:"))
        navigate("playlists", uri, metadata);
}

void Backend::back() {
    if (!m_collectionUri.isEmpty())
        navigate(m_category);
}

void Backend::loadMore() {
    if (m_context.status != "ok" || m_context.next < 0)
        return;
    m_context.status = "loadingMore";
    m_context.pending = true;
    emit libraryChanged();
    loadPending();
}

void Backend::retryLibrary() {
    if (webCoolingDown() || m_context.next < 0
        || m_context.status == "loading" || m_context.status == "loadingMore" || m_context.status == "ok")
        return;
    m_context.status = m_entries.rowCount() > 0 ? "loadingMore" : "loading";
    m_context.pending = true;
    emit libraryChanged();
    loadPending();
}

void Backend::navigate(const QString &category, const QString &uri, const QVariantMap &metadata) {
    if (category == m_category && uri == m_collectionUri)
        return;
    m_context.items = m_entries.items();
    if (m_collectionUri.isEmpty()) {
        m_roots.insert(m_category, std::move(m_context));
    } else {
        const auto cost = std::max<qsizetype>(80, m_context.items.size());
        m_collections.insert(m_collectionUri, new LibraryContext(std::move(m_context)), cost);
    }

    m_category = category;
    m_collectionUri = uri;
    const std::unique_ptr<LibraryContext> cached(uri.isEmpty() ? nullptr : m_collections.take(uri));
    if (cached) {
        m_context = std::move(*cached);
    } else if (uri.isEmpty() && m_roots.contains(category)) {
        m_context = m_roots.take(category);
    } else {
        m_context = {};
        m_context.generation = ++m_libraryGeneration;
        m_context.collection = metadata;
    }
    m_entries.replace(std::exchange(m_context.items, {}));
    updateSelection();
    emit libraryChanged();
    emit libraryContextChanged();
    loadPending();
}

void Backend::reloadLibrary() {
    const auto collection = std::move(m_context.collection);
    const double scroll = m_context.scroll;
    m_context = {};
    m_context.generation = ++m_libraryGeneration;
    m_context.collection = collection;
    m_context.scroll = scroll;
    m_entries.replace({});
    emit libraryChanged();
    emit libraryContextChanged();
}

void Backend::applyPage(LibraryContext &context, const QJsonObject &result) {
    context.pending = false;
    context.status = result.value("status").toString("unavailable");
    if (result.value("collection").isObject())
        context.collection = result.value("collection").toObject().toVariantMap();
    if (context.status != "ok")
        return;
    if (!result.value("items").isArray()) {
        context.status = "unavailable";
        return;
    }
    const int next = result.value("next").toInt(-1);
    context.next = next > context.next && next <= maxLibraryOffset ? next : -1;
    const auto items = result.value("items").toArray().toVariantList();
    if (&context == &m_context) {
        m_entries.append(items);
        updateSelection();
    } else {
        context.items.append(items);
    }
}

void Backend::loadMetadata() {
    if (webCoolingDown())
        return;
    if (!m_trackWeb.operation.isEmpty()) {
        const QString requested = "spotify:" + m_trackWeb.operation + ':' + m_trackWeb.arguments.value(0);
        if (m_webMetadataGeneration != 0 && requested != m_playback.uri
            && m_playback.needsMetadata() && !m_metadataAttempted && requested != m_selectedUri) {
            qCDebug(metadataLog) << "Cancelled superseded track request" << requested;
            m_trackWeb.process.kill();
        }
        return;
    }
    if (m_playback.needsMetadata() && !m_metadataAttempted) {
        m_metadataAttempted = true;
        m_webMetadataGeneration = m_metadataGeneration;
        const QString kind = m_playback.uri.section(':', 1, 1);
        if (webRequest(kind, {m_playback.uri.section(':', 2, 2)})) {
            qCDebug(metadataLog) << "Requested current track metadata" << m_playback.uri;
            return;
        }
    }
    if (m_lookupSelection && m_selectedUri.startsWith("spotify:track:")) {
        m_webMetadataGeneration = 0;
        m_lookupSelection = false;
        // The same URI is already pending or has been tried for the active track.
        if (m_selectedUri != m_playback.uri)
            webRequest("track", {m_selectedUri.mid(14)});
        emit selectionChanged();
    }
}

void Backend::loadPending() {
    loadMetadata();
    if (webBusy())
        return;
    if (!m_context.pending)
        return;
    const bool detail = !m_collectionUri.isEmpty();
    m_context.pending = false;
    m_webLibraryGeneration = m_context.generation;
    if (webRequest(detail ? "collection" : "library", {libraryContext(), QString::number(m_context.next)}))
        return;
    m_context.status = webCoolingDown() ? "limited" : "unavailable";
    emit libraryChanged();
}

void Backend::connectLibrary() {
    if (webRequest("connect")) {
        m_context.status = "connecting";
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
    if (m_context.next != 0 && !id.isEmpty() && m_checkedTrack != id
        && webRequest("favorite", {id}))
        m_checkedTrack = id;
}

void Backend::webFinished(WebJob &job, const QJsonObject &result) {
    const QString operation = std::exchange(job.operation, {});
    const QStringList arguments = std::exchange(job.arguments, {});
    const QString status = result.value("status").toString("unavailable");
    if (&job == &m_web && (status == "disconnected" || status == "reauthorize")) {
        m_roots.clear();
        m_collections.clear();
    }
    const qint64 retry = result.value("retry").toInteger();
    if (retry > 0 && (status == "limited" || status == "quota")) {
        const int milliseconds = int((std::min)(retry, qint64{86400}) * 1000);
        if (milliseconds > m_backoff.remainingTime()) {
            m_backoff = QDeadlineTimer(milliseconds, Qt::PreciseTimer);
            m_webCooldown.start(milliseconds);
        }
    }
    if (operation == "library" || operation == "collection") {
        const QString key = arguments.value(0);
        LibraryContext *context = nullptr;
        if (key == libraryContext()) {
            context = &m_context;
        } else if (auto root = m_roots.find(key); root != m_roots.end()) {
            context = &root.value();
        } else {
            context = m_collections.object(key);
        }
        if (context && context->generation == m_webLibraryGeneration)
            applyPage(*context, result);
    } else if (operation == "track" || operation == "episode") {
        const QString expected = "spotify:" + operation + ':' + arguments.value(0);
        const QJsonObject metadata = result.value("track").toObject();
        const bool current = expected == m_playback.uri && m_webMetadataGeneration == m_metadataGeneration;
        if (status == "ok" && metadata.value("uri").toString() == expected) {
            m_metadata.insert(expected, new QJsonObject(metadata));
            if (current) {
                m_playback.mergeMetadata(metadata);
            } else {
                qCDebug(metadataLog) << "Discarded response for previous playback generation" << expected;
                // A → B → A can reuse the validated entry without starting a second request for A.
                if (expected == m_playback.uri && m_playback.needsMetadata()) {
                    m_playback.mergeMetadata(metadata);
                    m_metadataAttempted = true;
                }
            }
        } else if (current && retry > 0 && !m_metadataRetried && (status == "limited" || status == "quota")) {
            m_metadataRetried = true;
            m_metadataAttempted = false;
        }
        if (expected == m_selectedUri) {
            if (status == "ok" && metadata.value("uri").toString() == expected)
                m_selection = metadata.toVariantMap();
            m_lookupSelection = false;
            emit selectionChanged();
        }
        m_webMetadataGeneration = 0;
        emit playbackChanged();
    } else if (operation == "connect") {
        if (status == "authorized") {
            m_metadataAttempted = false;
            m_metadataRetried = false;
            m_checkedTrack.clear();
            m_roots.clear();
            m_collections.clear();
            reloadLibrary();
        } else if (!m_context.pending) {
            m_context.status = status;
        }
    } else if (status == "ok") {
        if (arguments.value(0) == trackId(m_playback))
            m_favorite = result.value("saved").toBool() ? 1 : 0;
        if (operation == "save" || operation == "remove") {
            m_roots.remove("tracks");
            if (libraryContext() == "tracks")
                reloadLibrary();
        }
    }
    emit libraryChanged();
    loadPending();
    checkFavorite();
}
