#include "playback.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace {
constexpr int maxDuration = 86'400'000;

QString name(const QJsonObject &decorations) {
    return decorations.value("identity").toObject().value("name").toString().simplified().left(200);
}

int milliseconds(const QJsonValue &value) {
    return int(std::clamp<qint64>(value.toInteger(), 0, maxDuration));
}
}

bool Playback::playableUri(const QString &uri) {
    static const QRegularExpression pattern(R"(^spotify:(track|album|playlist|episode):[a-zA-Z0-9]{22}$)");
    return pattern.match(uri).hasMatch();
}

void Playback::setItem(const QJsonObject &item) {
    const QJsonObject details = item.value("decorations").toObject();
    const QString identifier = item.value("uri").toString();
    uri = playableUri(identifier) ? identifier : QString{};
    title = name(details);
    album = name(details.value("parent").toObject().value("entity").toObject().value("decorations").toObject());
    duration = milliseconds(details.value("playback").toObject().value("duration_ms"));
    QStringList creators;
    for (const QJsonValue &creator : details.value("creators").toArray()) {
        const QString label = name(creator.toObject().value("entity").toObject().value("decorations").toObject());
        if (!label.isEmpty())
            creators.append(label);
        if (creators.size() == 8)
            break;
    }
    artist = creators.join(", ");
    art.clear();
    static const QRegularExpression coverPattern(R"(^https://i\.scdn\.co/image/[a-fA-F0-9]{1,128}$)");
    for (const QJsonValue &cover : details.value("visual_identity").toObject().value("cover").toArray()) {
        const QString url = cover.toObject().value("url").toString();
        if (coverPattern.match(url).hasMatch()) {
            art = QUrl(url);
            break;
        }
    }
}

void Playback::setPosition(const QJsonObject &anchor, qint64 now) {
    m_position = milliseconds(anchor.value("position_ms"));
    m_sampledAt = anchor.value("timestamp_ms").toInteger(now);
    m_speed = std::clamp(anchor.value("speed").toDouble(), 0.0, 4.0);
}

int Playback::position(qint64 now) const {
    const double elapsed = std::clamp(double(now) - double(m_sampledAt), 0.0, double(maxDuration));
    return std::min(duration, m_position + int(elapsed * m_speed));
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
    if (type == "auth_state") {
        loggedIn = event.value("logged_in").toBool();
        if (!loggedIn)
            *this = {};
    } else if (type == "playback_state") {
        status = event.value("status").toString();
        setItem(event.value("item").toObject());
        setPosition(event.value("position").toObject(), now);
    } else if (type == "track_changed") {
        setItem(event.value("item").toObject());
        setPosition({}, now);
    } else if (type == "playback_changed") {
        m_position = position(now);
        m_sampledAt = now;
        status = event.value("status").toString();
        m_speed = status == "playing" ? 1 : 0;
        if (status == "idle")
            setItem({});
    } else if (type == "position_sync") {
        setPosition(event.value("position").toObject(), now);
    } else {
        return false;
    }
    return true;
}
