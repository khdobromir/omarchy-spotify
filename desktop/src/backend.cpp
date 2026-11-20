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
using namespace std::chrono_literals;
constexpr qint64 coreFrameLimit = 256 * 1024;
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
      m_dataDir(m_home + "/soloist"), m_keyFile(m_home + "/soloist-key") {
    m_clock.start();
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
        if (!m_local)
            return;
        if (code == 10)
            playerStopped("player_expired");
        else if (m_playerStatus == "player_connection" || m_playerStatus == "player_timeout")
            playerStopped(m_playerStatus);
        else
            playerStopped("player_failed");
    });
    connect(&m_daemon, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (m_local && error == QProcess::FailedToStart)
            playerStopped("player_failed");
    });
    connect(&m_trace, &QProcess::readyReadStandardOutput, this, &Backend::readTrace);
    connect(&m_trace, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        if (m_local && m_daemon.state() == QProcess::Running) {
            playerStopped("player_connection");
            m_daemon.terminate();
        }
    });
    connect(&m_control, &QProcess::stateChanged, this, &Backend::playerChanged);
    connect(&m_control, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (m_local)
            controlFinished(code == 0 && status == QProcess::NormalExit);
    });
    connect(&m_control, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (m_local && error == QProcess::FailedToStart)
            controlFinished(false);
    });
    m_webCooldown.setSingleShot(true);
    m_webCooldown.setTimerType(Qt::PreciseTimer);
    connect(&m_webCooldown, &QTimer::timeout, this, [this] {
        emit libraryChanged();
        loadPending();
    });

    m_core.setReadBufferSize(coreFrameLimit);
    m_reconnect.setSingleShot(true);
    connect(&m_reconnect, &QTimer::timeout, this, &Backend::connectCore);
    connect(&m_core, &QLocalSocket::readyRead, this, &Backend::readCore);
    connect(&m_core, &QLocalSocket::connected, this, [this] {
        m_attempts = 0;
        m_revision = -1;
        m_backoff = {};
        m_webCooldown.stop();
        requestCore("subscribe", {}, [this](const QJsonObject &sample) {
            if (!sample.contains("revision")) {
                m_core.abort();
                return;
            }
            m_coreReady = true;
            applySample(sample);
            loadPending();
        }, 5s);
    });
    connect(&m_core, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        m_reconnect.start(1000);
        if (m_attempts == 1) {
            const auto launcher = QStandardPaths::findExecutable("omarchy-spotify-core");
            if (!launcher.isEmpty())
                QProcess::startDetached(launcher, {});
        }
    });
    connect(&m_core, &QLocalSocket::disconnected, this, [this] {
        m_coreReady = false;
        const auto pending = std::exchange(m_replies, {});
        for (const auto &reply : pending)
            reply({{"status", "unavailable"}});
        m_sample = {};
        if (!m_local) {
            m_tick.stop();
            m_playback = {};
            setPlayerStatus("core_unavailable");
            emit playbackChanged();
        }
        m_reconnect.start(1000);
    });
    QTimer::singleShot(0, this, &Backend::connectCore);
}

void Backend::connectCore() {
    if (m_core.state() != QLocalSocket::UnconnectedState)
        return;
    ++m_attempts;
    const auto runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    m_core.connectToServer(QDir(runtime).filePath("omarchy-spotify/core.sock"));
    if (!m_local && !m_coreReady && m_attempts > 1)
        setPlayerStatus("core_unavailable");
}

QString Backend::requestCore(const QString &method, const QJsonObject &params,
                             std::function<void(const QJsonObject &)> done, std::chrono::milliseconds timeout) {
    if (m_core.state() != QLocalSocket::ConnectedState || m_replies.size() >= 16)
        return {};
    const QString id = QString::number(++m_requestId);
    const QJsonObject frame{{"version", 1}, {"id", id}, {"method", method}, {"params", params}};
    if (m_core.write(QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n') < 0)
        return {};
    m_replies.insert(id, std::move(done));
    QTimer::singleShot(timeout, this, [this, id] {
        if (auto reply = m_replies.take(id))
            reply({{"status", "unavailable"}});
    });
    return id;
}

void Backend::readCore() {
    while (m_core.canReadLine()) {
        const auto document = QJsonDocument::fromJson(m_core.readLine(coreFrameLimit));
        const auto message = document.object();
        if (!document.isObject() || message.value("version") != 1) {
            m_core.abort();
            return;
        }
        const auto event = message.value("event").toString();
        if (event == "playback.changed") {
            applySample(message.value("data").toObject());
        } else if (event == "library.changed") {
            const auto change = message.value("data").toObject();
            m_roots.remove("tracks");
            if (libraryContext() == "tracks")
                reloadLibrary();
            if (change.value("trackId").toString() == trackId(m_playback))
                m_favorite = change.value("saved").toBool() ? 1 : 0;
            emit libraryChanged();
            loadPending();
        } else if (auto reply = m_replies.take(message.value("id").toString())) {
            const auto result = message.value("ok").toBool()
                ? message.value("result").toObject()
                : QJsonObject{{"status", message.value("error").toObject().value("code")}};
            reply(result);
        }
    }
    if (m_core.bytesAvailable() >= coreFrameLimit - 1)
        m_core.abort();
}

void Backend::clearAccount() {
    m_roots.clear();
    m_collections.clear();
    m_metadata.clear();
    m_checkedTrack.clear();
    m_favorite = -1;
    m_devices.clear();
    m_queue.clear();
    dismissSelection();
    reloadLibrary();
}

void Backend::applySample(const QJsonObject &sample) {
    const auto revision = sample.value("revision").toInteger(-1);
    if (revision < 0 || revision <= m_revision)
        return;
    const bool restarted = m_revision < 0 && m_accountRevision >= 0;
    m_revision = revision;
    const auto account = sample.value("accountRevision").toInteger();
    if (restarted || (m_accountRevision >= 0 && account != m_accountRevision))
        clearAccount();
    m_accountRevision = account;
    m_sample = sample;
    if (!m_local) {
        const auto oldTrack = m_playback.uri;
        m_playback.applyRemote(sample, m_clock.elapsed());
        const auto sync = sample.value("sync").toString();
        setPlayerStatus(sync == "ok" ? (m_playback.loggedIn ? "ready" : "no_device") : sync);
        if (m_playback.status == "playing")
            m_tick.start();
        else
            m_tick.stop();
        if (oldTrack != m_playback.uri) {
            m_checkedTrack.clear();
            m_favorite = -1;
            emit libraryChanged();
        }
        emit playbackChanged();
        emit positionChanged();
        emit playerChanged();
        playPending();
    }
    loadPending();
    checkFavorite();
}

void Backend::remoteCommand(const QString &method, QJsonObject params) {
    if (m_remoteBusy || !m_coreReady)
        return;
    if (method != "transfer") {
        if (!m_playback.loggedIn) {
            setPlayerStatus("no_device");
            return;
        }
        params.insert("deviceId", m_sample.value("deviceId"));
    }
    m_remoteBusy = !requestCore("playback." + method, params, [this, method](const QJsonObject &result) {
        m_remoteBusy = false;
        if (!m_local) {
            if (!result.value("accepted").toBool())
                setPlayerStatus(result.value("status").toString("action_failed"));
            if (method == "play" && m_selectionStatus == "sent")
                m_selectionStatus = result.value("accepted").toBool() ? QString{} : QString("failed");
        }
        emit playerChanged();
    }).isEmpty();
    emit playerChanged();
}

void Backend::refreshDevices() {
    requestCore("devices", {}, [this](const QJsonObject &result) {
        m_devices = result.value("devices").toArray().toVariantList();
        if (result.value("status") != "ok")
            setPlayerStatus(result.value("status").toString("unavailable"));
        emit playerChanged();
    });
}

void Backend::refreshQueue() {
    m_queue.clear();
    emit playerChanged();
    requestCore("queue", {}, [this](const QJsonObject &result) {
        m_queue = result.value("items").toArray().toVariantList();
        if (result.value("status") != "ok")
            setPlayerStatus(result.value("status").toString("unavailable"));
        emit playerChanged();
    });
}

void Backend::setShuffle(bool state) {
    if (!m_local)
        remoteCommand("shuffle", {{"state", state}});
}

void Backend::setRepeat(const QString &state) {
    if (!m_local)
        remoteCommand("repeat", {{"state", state}});
}

void Backend::setVolume(int volume) {
    if (!m_local)
        remoteCommand("volume", {{"volume", volume}});
}

void Backend::transfer(const QString &device) {
    if (!m_local)
        remoteCommand("transfer", {{"deviceId", device}});
}

void Backend::preferDevice(const QString &device) {
    requestCore("preferDevice", {{"deviceId", device}}, [this](const QJsonObject &result) {
        if (result.value("status") == "ok")
            refreshDevices();
        else
            setPlayerStatus(result.value("status").toString("unavailable"));
    });
}

void Backend::disconnectAccount() {
    webRequest("disconnect");
}

void Backend::setLocalMode(bool enabled) {
    if (m_local == enabled || (enabled && m_daemon.state() != QProcess::NotRunning))
        return;
    m_local = enabled;
    dismissSelection();
    m_tick.stop();
    m_playback = {};
    m_checkedTrack.clear();
    m_favorite = -1;
    if (enabled) {
        startPlayer();
    } else {
        for (auto *process : {&m_control, &m_trace, &m_daemon})
            process->terminate();
        const auto sample = m_sample;
        m_revision = -1;
        m_accountRevision = -1;
        if (sample.isEmpty())
            setPlayerStatus("core_unavailable");
        else
            applySample(sample);
    }
    emit playbackChanged();
    emit playerChanged();
}

Backend::~Backend() {
    m_core.disconnect(this);
    for (QProcess *process : {&m_control, &m_trace, &m_daemon}) {
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
            {"device", m_local ? (m_playback.loggedIn ? QString("Omarchy Spotify") : QString{})
                               : m_sample.value("device").toString()},
            {"pending", m_local ? QString{} : m_sample.value("pending").toString()},
            {"shuffle", m_sample.value("shuffle").toVariant()},
            {"repeat", m_sample.value("repeat").toString()},
            {"volume", m_sample.value("capabilities").toObject().value("volume").toVariant()},
            {"supportsVolume", !m_local && m_sample.value("capabilities").toObject().value("supportsVolume").toBool()},
            {"disallows", m_local ? QVariantList{} : m_sample.value("disallows").toArray().toVariantList()},
            {"art", m_playback.art}, {"artHigh", m_playback.artHigh}, {"duration", m_playback.duration}};
}

int Backend::position() const {
    return m_playback.position(m_local ? QDateTime::currentMSecsSinceEpoch() : m_clock.elapsed());
}

void Backend::setPlayerStatus(const QString &status) {
    if (m_playerStatus != status) {
        m_playerStatus = status;
        emit playerChanged();
    }
}

void Backend::saveKey(const QString &input) {
    const QByteArray key = input.trimmed().toUtf8();
    if (!m_local || key.isEmpty() || key.size() > 8192 || key.contains('\n')
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
    if (!m_local || m_daemon.state() != QProcess::NotRunning)
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

void Backend::play(const QString &uri, int offset) {
    if (uri.isEmpty()) {
        if (m_local)
            command({"play"});
        else
            remoteCommand("resume");
    } else if (!m_local && offset >= 0 && !m_collectionUri.isEmpty()) {
        remoteCommand("play", {{"uri", uri}, {"context", m_collectionUri}, {"offset", offset}});
    }
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
        if (m_selectionStatus == "inspecting")
            dismissSelection();
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
    if (m_pendingPlay.isEmpty() || commandBusy())
        return;
    if (!m_local) {
        if (!m_coreReady || m_sample.value("sync") == "loading" || m_sample.isEmpty())
            return;
        if (!m_playback.loggedIn) {
            m_pendingPlay.clear();
            m_selectionStatus = "failed";
            emit playerChanged();
            return;
        }
        m_selectionStatus = "sent";
        remoteCommand("play", {{"uri", std::exchange(m_pendingPlay, {})}});
        return;
    }
    if (!m_playback.loggedIn)
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
    if (!m_local)
        remoteCommand(m_playback.status == "playing" ? "pause" : "resume");
    else if (m_playback.status == "playing")
        command({"pause"});
    else
        play();
}

void Backend::skip(bool next) {
    if (m_local)
        command({next ? "next" : "prev"});
    else
        remoteCommand(next ? "next" : "previous");
}

void Backend::seek(int at, const QString &uri) {
    if (uri.isEmpty() || m_playback.uri != uri || m_playback.duration <= 0)
        return;
    const int position = std::clamp(at, 0, m_playback.duration - 1);
    if (m_local)
        command({"seek", QString::number(position)});
    else
        remoteCommand("seek", {{"position", position}, {"uri", uri}});
}

bool Backend::webRequest(const QString &operation, const QStringList &args) {
    WebJob &job = operation == "track" || operation == "episode" ? m_trackWeb : m_web;
    if (!job.operation.isEmpty() || webCoolingDown())
        return false;
    if (!m_coreReady)
        return false;
    job.id = requestCore(operation, {{"args", QJsonArray::fromStringList(args)}},
                         [this, job = &job](const QJsonObject &result) { webFinished(*job, result); },
                         operation == "connect" ? 195s : 20s);
    if (job.id.isEmpty())
        return false;
    job.operation = operation;
    job.arguments = args;
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
            m_replies.remove(m_trackWeb.id);
            m_trackWeb.operation.clear();
        } else {
            return;
        }
    }
    if (m_local && m_playback.needsMetadata() && !m_metadataAttempted) {
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
        if (!m_local || m_selectedUri != m_playback.uri)
            webRequest("track", {m_selectedUri.mid(14)});
        emit selectionChanged();
    }
}

void Backend::loadPending() {
    if (!m_coreReady)
        return;
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
    if (!id.isEmpty() && m_checkedTrack != id
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

    }
    emit libraryChanged();
    loadPending();
    checkFavorite();
}
