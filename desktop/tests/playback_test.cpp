#include <QtTest>

#include "playback.h"

class PlaybackTest : public QObject {
    Q_OBJECT
private slots:
    void remotePauseHoldUsesOnePositionUntilConfirmed() {
        Playback playback;
        QJsonObject sample = {{"sync", "ok"}, {"playing", true}, {"duration", 90'000}, {"progress", 20'000},
                           {"sampleAgeMs", 650}, {"positionFrozen", true}};
        playback.applyRemote(sample, 1000);
        QCOMPARE(playback.position(1600), 20'000);
        sample.insert("playing", false);
        sample.insert("progress", 19'900);
        sample.insert("positionFrozen", false);
        playback.applyRemote(sample, 1600);
        QCOMPARE(playback.position(2000), 19'900);
    }

    void traceUpdatesPlayback() {
        Playback playback;
        QVERIFY(playback.apply("1 {\"type\":\"auth_state\",\"logged_in\":true}", 1000));
        QVERIFY(playback.loggedIn);
        const QByteArray snapshot = "1 {\"type\":\"playback_state\",\"status\":\"playing\",\"item\":{"
            "\"uri\":\"spotify:track:aaaaaaaaaaaaaaaaaaaaaa\",\"decorations\":{\"identity\":{\"name\":\"<Song>\"},"
            "\"visual_identity\":{\"cover\":[{\"url\":\"https://i.scdn.co/image/abcdef\"}]},"
            "\"playback\":{\"duration_ms\":90000}}},\"position\":{\"position_ms\":3000,"
            "\"timestamp_ms\":1000,\"speed\":1}}";
        QVERIFY(playback.apply(snapshot, 1000));
        QCOMPARE(playback.title, "<Song>");
        QCOMPARE(playback.position(2000), 4000);
        QVERIFY(playback.apply("2 {\"type\":\"playback_changed\",\"status\":\"paused\"}", 2000));
        QCOMPARE(playback.position(3000), 4000);
        QVERIFY(playback.apply("3 {\"type\":\"track_changed\",\"item\":{}}", 3000));
        QCOMPARE(playback.title, "<Song>");
        QCOMPARE(playback.position(3000), 4000);
        QVERIFY(playback.apply(R"(4 {"type":"playback_state","status":"paused"})", 3100));
        QCOMPARE(playback.title, "<Song>");
        QCOMPARE(playback.position(3200), 4000);
        QVERIFY(playback.apply(R"(5 {"type":"playback_changed","status":"playing"})", 3200));
        QCOMPARE(playback.position(3300), 4100);
        QVERIFY(playback.apply(R"(6 {"type":"position_sync","position":{"position_ms":18000,"timestamp_ms":3400,"speed":1}})", 3400));
        QCOMPARE(playback.position(3400), 18000);
        QCOMPARE(playback.title, "<Song>");
        QVERIFY(playback.apply(R"(7 {"type":"playback_changed","status":"idle"})", 3500));
        QVERIFY(playback.uri.isEmpty());
        QVERIFY(playback.title.isEmpty());
        QCOMPARE(playback.position(3600), 0);
    }

    void changesIdentityAndRejectsOldPosition() {
        Playback playback;
        QVERIFY(playback.apply(R"(1 {"type":"track_changed","item":{"uri":"spotify:track:aaaaaaaaaaaaaaaaaaaaaa","decorations":{"identity":{"name":"First"}}}})", 2100));
        QVERIFY(playback.apply(R"(2 {"type":"position_sync","position":{"position_ms":9000,"timestamp_ms":2200,"speed":1}})", 2200));
        QCOMPARE(playback.position(2300), 9100);
        QVERIFY(playback.apply(R"(3 {"type":"track_changed","item":{"uri":"spotify:track:bbbbbbbbbbbbbbbbbbbbbb"}})", 2300));
        QVERIFY(playback.title.isEmpty());
        QVERIFY(playback.apply(R"(4 {"type":"position_sync","position":{"position_ms":9900,"timestamp_ms":2190,"speed":1}})", 2400));
        QCOMPARE(playback.position(2400), 0);
        QVERIFY(playback.apply(R"(5 {"type":"track_changed","item":{"uri":"spotify:track:bbbbbbbbbbbbbbbbbbbbbb","decorations":{"identity":{"name":"Second"}}}})", 2500));
        QVERIFY(playback.apply(R"(6 {"type":"track_changed","item":{"uri":"spotify:track:bbbbbbbbbbbbbbbbbbbbbb","decorations":{"playback":{"duration_ms":90000}}}})", 2600));
        QCOMPARE(playback.title, "Second");
        QCOMPARE(playback.duration, 90000);
    }

    void malformedInputAndArtwork() {
        Playback playback;
        QVERIFY(!playback.apply("unexpected line", 0));
        QVERIFY(playback.apply("1 {\"type\":\"track_changed\",\"item\":{\"uri\":\"--unexpected\","
            "\"decorations\":{\"visual_identity\":{\"cover\":[{\"url\":\"file:///etc/passwd\"}]}}}}", 0));
        QVERIFY(playback.uri.isEmpty());
        QVERIFY(playback.art.isEmpty());
        QVERIFY(Playback::playableUri("spotify:playlist:aaaaaaaaaaaaaaaaaaaaaa"));
        QVERIFY(!Playback::playableUri("spotify:artist:aaaaaaaaaaaaaaaaaaaaaa"));
    }

    void coverSizeIsChosenAcrossTrackAndAlbum() {
        Playback playback;
        const QByteArray event = "1 {\"type\":\"track_changed\",\"item\":{\"uri\":\"spotify:track:aaaaaaaaaaaaaaaaaaaaaa\",\"decorations\":{"
            "\"visual_identity\":{\"cover\":["
            "{\"url\":\"https://i.scdn.co/image/111\",\"size\":\"small\"},"
            "{\"url\":\"https://i.scdn.co/image/333\",\"size\":\"large\"},"
            "{\"url\":\"https://i.scdn.co/image/222\",\"size\":\"default\"}]},"
            "\"parent\":{\"entity\":{\"decorations\":{\"visual_identity\":{\"cover\":["
            "{\"url\":\"https://i.scdn.co/image/444\",\"size\":\"xlarge\"}]}}}}"
            "}}}";
        QVERIFY(playback.apply(event, 0));
        QCOMPARE(playback.art, QUrl("https://i.scdn.co/image/222"));
        QCOMPARE(playback.artHigh, QUrl("https://i.scdn.co/image/444"));
    }
};

QTEST_GUILESS_MAIN(PlaybackTest)
#include "playback_test.moc"
