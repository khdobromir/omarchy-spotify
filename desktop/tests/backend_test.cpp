#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"

namespace {
const QString trackA = "spotify:track:" + QString(22, 'a');
const QString trackC = "spotify:track:" + QString(22, 'c');
const QString playlist = "spotify:playlist:" + QString(22, 'p');

bool writeFile(const QString &path, const QByteArray &bytes, bool append = false) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | (append ? QIODevice::Append : QIODevice::Truncate))
           && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
}

class BackendTest : public QObject {
    Q_OBJECT

    QProcess m_core;
    QByteArray originalPath = qgetenv("PATH");

    bool configure(const QTemporaryDir &directory) {
        if (!directory.isValid())
            return false;
        const QString base = directory.path();
        qputenv("PATH", base.toUtf8() + ':' + originalPath);
        qputenv("XDG_STATE_HOME", base.toUtf8());
        qputenv("XDG_RUNTIME_DIR", base.toUtf8());
        if (!QDir(base).mkdir("backend-test")
            || !writeFile(base + "/backend-test/soloist-key", "fixture")
            || !writeFile(base + "/events", {})
            || !writeFile(base + "/library-fixture", {}))
            return false;
        const QByteArray fixture = "#!" PYTHON_EXECUTABLE "\n" + readFile(FIXTURE_PATH);
        const auto path = base + "/soloist";
        if (!writeFile(path, fixture) || !QFile::setPermissions(path, QFile::ReadOwner | QFile::ExeOwner))
            return false;
        m_core.start(PYTHON_EXECUTABLE, {FIXTURE_PATH});
        return m_core.waitForStarted();
    }

private slots:
    void cleanup() {
        m_core.terminate();
        if (!m_core.waitForFinished(1000)) {
            m_core.kill();
            m_core.waitForFinished();
        }
    }

    void usesPhoneByDefault() {
        QTemporaryDir directory;
        QVERIFY(configure(directory));
        Backend backend;
        QTRY_COMPARE(backend.playback().value("device").toString(), "Phone");
        backend.togglePlayback();
        QTRY_VERIFY(readFile(directory.filePath("played")).contains("[\"pause\", null, \"phone\"]"));
        QVERIFY(!QFile::exists(directory.filePath("daemon")));
    }

    void appendsAndRetriesWithoutReplacingRows() {
        QTemporaryDir directory;
        QVERIFY(configure(directory));
        Backend backend;
        auto *model = backend.entries();
        backend.openCollection(playlist);
        QTRY_COMPARE(model->rowCount(), 20);
        QSignalSpy reset(model, &QAbstractItemModel::modelReset);
        QSignalSpy inserts(model, &QAbstractItemModel::rowsInserted);
        backend.rememberScroll(12.5);
        backend.loadMore();
        backend.loadMore();
        QTRY_COMPARE(backend.libraryStatus(), "limited");
        QCOMPARE(model->rowCount(), 20);
        QVERIFY(backend.webCoolingDown());
        backend.retryLibrary();
        QCOMPARE(backend.libraryStatus(), "limited");
        QTRY_VERIFY(!backend.webCoolingDown());
        backend.retryLibrary();
        QTRY_COMPARE(model->rowCount(), 23);
        QCOMPARE(reset.size(), 0);
        QCOMPARE(inserts.size(), 1);
        QCOMPARE(backend.nextOffset(), -1);
        QCOMPARE(backend.entryAt(20).value("uri"), backend.entryAt(0).value("uri"));
        QCOMPARE(backend.entryAt(22).value("title").toString(), playlist + " 22");
        backend.loadMore();
        backend.back();
        QTRY_COMPARE(backend.libraryStatus(), "ok");
        backend.openCollection(playlist);
        QCOMPARE(backend.libraryScroll(), 12.5);
        QCOMPARE(model->rowCount(), 23);
        QCOMPARE(readFile(directory.filePath("web")).count(QByteArray("\"collection\", \"") + playlist.toUtf8() + "\", \"20\""), 2);
    }

    void routesLatePageToItsOwnContext() {
        QTemporaryDir directory;
        QVERIFY(configure(directory));
        Backend backend;
        auto *model = backend.entries();
        QTRY_COMPARE(model->rowCount(), 20);
        backend.rememberScroll(8);
        backend.loadMore();
        backend.loadLibrary("albums");
        QTRY_COMPARE(model->rowCount(), 20);
        QCOMPARE(backend.entryAt(0).value("title").toString(), "albums 0");
        backend.loadLibrary("tracks");
        QCOMPARE(backend.libraryScroll(), 8.0);
        QCOMPARE(model->rowCount(), 23);
        QCOMPARE(backend.entryAt(20).value("title").toString(), "tracks 20");
        QCOMPARE(readFile(directory.filePath("web")).count("\"library\", \"tracks\", \"20\""), 1);
    }

    void resolvesSparseEventsAndRejectsObsoleteMetadata() {
        QTemporaryDir directory;
        QVERIFY(configure(directory));
        Backend backend;
        backend.setLocalMode(true);
        const QString events = directory.filePath("events");
        const QString log = directory.filePath("web");
        const auto trackChanged = [&](char letter) {
            return writeFile(events, "0 {\"type\":\"track_changed\",\"item\":{\"uri\":\"spotify:track:"
                + QByteArray(22, letter) + "\"}}\n", true);
        };
        QTRY_VERIFY(backend.playback().value("ready").toBool());
        QVERIFY(trackChanged('a'));
        QTRY_COMPARE(backend.playback().value("title").toString(), "API a");
        QCOMPARE(backend.playback().value("artHigh").toUrl(), QUrl("https://i.scdn.co/image/456"));
        QVERIFY(trackChanged('b'));
        QTRY_VERIFY(readFile(log).contains(QByteArray(22, 'b')));
        QVERIFY(trackChanged('c'));
        QTRY_COMPARE(backend.playback().value("title").toString(), "API c");
        QCOMPARE(backend.playback().value("uri").toString(), trackC);
        QVERIFY(trackChanged('e'));
        QTRY_VERIFY(readFile(log).contains(QByteArray(22, 'e')));
        QTRY_VERIFY(!backend.playback().value("loadingMetadata").toBool());
        QVERIFY(backend.playback().value("title").toString().isEmpty());
        QVERIFY(trackChanged('d'));
        QTRY_VERIFY(readFile(log).contains(QByteArray(22, 'd')));
        QTRY_VERIFY(!backend.playback().value("loadingMetadata").toBool());
        QVERIFY(backend.playback().value("title").toString().isEmpty());
        QVERIFY(trackChanged('a'));
        QTRY_COMPARE(backend.playback().value("title").toString(), "API a");
        QCOMPARE(backend.playback().value("uri").toString(), trackA);
        QCOMPARE(readFile(log).count(QByteArray("\"track\", \"") + QByteArray(22, 'a')), 1);
    }

    void sendsTransportCommandsAndRejectsSeekForPreviousTrack() {
        QTemporaryDir directory;
        QVERIFY(configure(directory));
        Backend backend;
        backend.setLocalMode(true);
        const QString events = directory.filePath("events");
        const QString commands = directory.filePath("played");
        QTRY_VERIFY(backend.playback().value("ready").toBool());
        QVERIFY(writeFile(events, "0 {\"type\":\"playback_state\",\"status\":\"playing\",\"item\":{\"uri\":\""
            + trackA.toUtf8() + "\"}}\n", true));
        QTRY_COMPARE(backend.playback().value("title").toString(), "API a");
        backend.seek(5000, trackC);
        QVERIFY(!QFile::exists(commands));
        backend.seek(5000, trackA);
        QTRY_VERIFY(readFile(commands).contains("[\"seek\", \"5000\"]"));
        QTRY_VERIFY(!backend.commandBusy());
        backend.togglePlayback();
        QTRY_VERIFY(readFile(commands).contains("[\"pause\"]"));
        QTRY_VERIFY(!backend.commandBusy());
        QVERIFY(writeFile(events, "0 {\"type\":\"playback_changed\",\"status\":\"paused\"}\n", true));
        QTRY_VERIFY(!backend.playback().value("playing").toBool());
        QCOMPARE(backend.playback().value("title").toString(), "API a");
        backend.togglePlayback();
        QTRY_VERIFY(readFile(commands).contains("[\"play\"]"));
        QTRY_VERIFY(!backend.commandBusy());
        backend.skip(true);
        QTRY_VERIFY(readFile(commands).contains("[\"next\"]"));
        QTRY_VERIFY(!backend.commandBusy());
        backend.skip(false);
        QTRY_VERIFY(readFile(commands).contains("[\"prev\"]"));
    }
};

QTEST_GUILESS_MAIN(BackendTest)
#include "backend_test.moc"
