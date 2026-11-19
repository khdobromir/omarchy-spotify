#include <QtTest>

#include "playback.h"

class PlaybackTest : public QObject {
    Q_OBJECT
private slots:
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
        QVERIFY(playback.uri.isEmpty());
        QCOMPARE(playback.position(3000), 0);
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
        const QByteArray event = "1 {\"type\":\"track_changed\",\"item\":{\"decorations\":{"
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
