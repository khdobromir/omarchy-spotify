#include "playback.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace {
constexpr int maxDuration = 86'400'000;
Q_LOGGING_CATEGORY(traceLog, "omarchy.spotify.soloist", QtWarningMsg)
const QRegularExpression coverPattern(
    R"(\Ahttps://(?:i\.scdn\.co/image|image-cdn-(?:ak|fa)\.spotifycdn\.com/image|mosaic\.scdn\.co/(?:60|300|640))/[a-fA-F0-9]{1,200}\z)");

QString name(const QJsonObject &decorations) {
    return decorations.value("identity").toObject().value("name").toString().simplified().left(200);
}

int milliseconds(const QJsonValue &value) {
    return int(std::clamp<qint64>(value.toInteger(), 0, maxDuration));
}
}

bool Playback::playableUri(const QString &uri) {
    static const QRegularExpression pattern(R"(\Aspotify:(track|album|playlist|episode):[a-zA-Z0-9]{22}\z)");
    return pattern.match(uri).hasMatch();
}

void Playback::setItem(const QJsonObject &item, qint64 now) {
    const QJsonObject details = item.value("decorations").toObject();
    const QString identifier = item.value("uri").toString();
    if (!playableUri(identifier))
        return;
    if (uri != identifier) {
        const auto anchor = m_lastAnchor;
        const auto transport = status;
        const double speed = status == "playing" ? m_speed : 0;
        const bool connected = loggedIn;
        *this = {};
        loggedIn = connected;
        status = transport == "idle" ? QString{} : transport;
        uri = identifier;
        m_lastAnchor = anchor;
        m_sampledAt = now;
        m_speed = speed;
        qCDebug(traceLog) << "Track URI changed" << uri;
    }
    if (const QString titleFromEvent = name(details); !titleFromEvent.isEmpty())
        title = titleFromEvent;
    const QJsonObject parent = details.value("parent").toObject().value("entity").toObject().value("decorations").toObject();
    if (const QString albumFromEvent = name(parent); !albumFromEvent.isEmpty())
        album = albumFromEvent;
    if (const int length = milliseconds(details.value("playback").toObject().value("duration_ms")); length > 0)
        duration = length;
    QStringList creators;
    for (const QJsonValue &creator : details.value("creators").toArray()) {
        const QString label = name(creator.toObject().value("entity").toObject().value("decorations").toObject());
        if (!label.isEmpty())
            creators.append(label);
        if (creators.size() == 8)
            break;
    }
    if (!creators.isEmpty())
        artist = creators.join(", ");
    const QStringList sizes{"small", "default", "large", "xlarge"};
    for (const auto &entity : {details, parent}) {
        for (const QJsonValue &cover : entity.value("visual_identity").toObject().value("cover").toArray()) {
            const QJsonObject image = cover.toObject();
            const QString url = image.value("url").toString();
            if (!coverPattern.match(url).hasMatch())
                continue;
            const int size = int(sizes.indexOf(image.value("size").toString())) + 1;
            if (size > m_artSize) {
                m_artSize = size;
                artHigh = QUrl(url);
            }
            const int fit = size >= 2 ? 6 - size : size;
            if (fit > m_artFit) {
                m_artFit = fit;
                art = QUrl(url);
            }
        }
    }
}

bool Playback::needsMetadata() const {
    return (uri.startsWith("spotify:track:") || uri.startsWith("spotify:episode:"))
           && (title.isEmpty() || artist.isEmpty() || album.isEmpty() || m_artSize < 3 || duration == 0);
}

bool Playback::mergeMetadata(const QJsonObject &track) {
    if (uri.isEmpty() || track.value("uri").toString() != uri)
        return false;
    if (title.isEmpty())
        title = track.value("title").toString();
    if (artist.isEmpty())
        artist = track.value("subtitle").toString();
    if (album.isEmpty())
        album = track.value("album").toString();
    if (duration == 0)
        duration = milliseconds(track.value("duration"));
    const QString small = track.value("art").toString();
    const QString large = track.value("artHigh").toString();
    if (coverPattern.match(small).hasMatch() && m_artFit < 4) {
        art = QUrl(small);
        m_artFit = 4;
    }
    if (coverPattern.match(large).hasMatch() && m_artSize < 3) {
        artHigh = QUrl(large);
        // Keep accepting better covers from later Soloist events.
        m_artSize = 2;
    }
    qCDebug(traceLog) << "Merged Web API metadata" << uri;
    return true;
}

void Playback::setPosition(const QJsonObject &anchor, qint64 now) {
    if (!anchor.value("position_ms").isDouble())
        return;
    const qint64 timestamp = anchor.value("timestamp_ms").toInteger(now);
    if (timestamp < m_lastAnchor)
        return;
    m_lastAnchor = timestamp;
    m_position = milliseconds(anchor.value("position_ms"));
    m_sampledAt = (std::min)(timestamp, now);
    if (anchor.value("speed").isDouble())
        m_speed = std::clamp(anchor.value("speed").toDouble(), 0.0, 4.0);
}

int Playback::position(qint64 now) const {
    const double elapsed = std::clamp(double(now) - double(m_sampledAt), 0.0, double(maxDuration));
    return (std::min)(duration > 0 ? duration : maxDuration,
                      m_position + int(elapsed * (status == "paused" || status == "idle" ? 0 : m_speed)));
}

void Playback::setStatus(const QString &next, qint64 now) {
    if (next == "idle") {
        if (!uri.isEmpty())
            qCDebug(traceLog) << "Cleared track on idle";
        const bool connected = loggedIn;
        const auto anchor = m_lastAnchor;
        *this = {};
        loggedIn = connected;
        m_lastAnchor = anchor;
        status = next;
    } else if ((next == "playing" || next == "paused") && next != status) {
        m_position = position(now);
        m_sampledAt = now;
        m_speed = next == "playing" ? 1 : 0;
        status = next;
    }
}

bool Playback::apply(const QByteArray &line, qint64 now) {
    const auto separator = line.indexOf(' ');
    if (separator < 0 || line.size() > 256 * 1024)
        return false;
    const QJsonDocument message = QJsonDocument::fromJson(line.mid(separator + 1));
    if (!message.isObject())
        return false;
    const QJsonObject event = message.object();
    const QString type = event.value("type").toString();
    const auto item = event.value("item").toObject();
    const QString transport = event.value("status").toString();
    qCDebug(traceLog) << "Event" << type << "item present" << !item.isEmpty()
                     << "URI present" << !item.value("uri").toString().isEmpty();
    if (type == "auth_state") {
        if (!event.value("logged_in").isBool())
            return false;
        loggedIn = event.value("logged_in").toBool();
        if (!loggedIn) {
            qCDebug(traceLog) << "Cleared track on logout";
            *this = {};
        }
    } else if (type == "playback_state") {
        if (transport != "idle")
            setItem(item, now);
        setStatus(transport, now);
        if (status != "idle")
            setPosition(event.value("position").toObject(), now);
    } else if (type == "track_changed") {
        setItem(item, now);
    } else if (type == "playback_changed") {
        setStatus(transport, now);
    } else if (type == "position_sync") {
        if (status != "idle")
            setPosition(event.value("position").toObject(), now);
    } else {
        return false;
    }
    return true;
}
