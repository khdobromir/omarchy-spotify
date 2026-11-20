#include <QGuiApplication>
#include <QIcon>
#include <QKeySequence>
#include <QLockFile>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QQmlNetworkAccessManagerFactory>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <cstdio>

#include "backend.h"
#include "instance.h"
#include "window.h"
#include "theme.h"
#include "translator.h"

namespace {
class ArtworkNetwork : public QQmlNetworkAccessManagerFactory {
    QNetworkAccessManager *create(QObject *parent) override {
        auto *network = new QNetworkAccessManager(parent);
        auto *cache = new QNetworkDiskCache(network);
        cache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/artwork");
        cache->setMaximumCacheSize(64 * 1024 * 1024);
        network->setCache(cache);
        return network;
    }
};
}

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("omarchy-spotify");
    app.setApplicationVersion("0.1.0");

    app.setDesktopFileName("omarchy-spotify");
    app.setWindowIcon(QIcon::fromTheme("multimedia-player"));
    const auto args = app.arguments().mid(1);
    const auto request = Instance::parse(args);
    if (!request) {
        qWarning("Usage: omarchy-spotify [--activate | [--open-track|--play-track|--open-album|--open-playlist] Spotify-URI]");
        return 2;
    }
    const auto reportReady = [] {
        if (qEnvironmentVariableIntValue("OMARCHY_SPOTIFY_REPORT_READY") == 1) {
            std::puts("1");
            std::fflush(stdout);
        }
    };
    const QString runtimePath = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtimePath.isEmpty())
        return 1;
    const QDir runtime(runtimePath);
    const QString socket = runtime.filePath("omarchy-spotify.sock");
    QLockFile lock(runtime.filePath("omarchy-spotify.lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        if (lock.error() == QLockFile::LockFailedError && Instance::forward(socket, args)) {
            reportReady();
            return 0;
        }
        qWarning("Could not contact the running Omarchy Spotify instance");
        return 1;
    }

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    Backend backend;
    ArtworkNetwork artwork;

    QQmlApplicationEngine engine;
    engine.setNetworkAccessManagerFactory(&artwork);
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    const auto preferences = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath("omarchy-spotify/preferences.ini");
    engine.setInitialProperties({{"theme", QVariant::fromValue(&theme)}, {"backend", QVariant::fromValue(&backend)},
                                 {"preferences", QUrl::fromLocalFile(preferences)}});
    engine.loadFromModule("OmarchySpotify", "Main");
    auto *window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window)
        return 1;
    const auto route = [window, &backend](const Instance::Request &incoming) {
        using Action = Instance::Action;
        if (incoming.action == Action::Activate)
            return;
        window->setProperty("nowPlayingExpanded", incoming.action == Action::OpenTrack
                            && backend.playback().value("uri").toString() == incoming.uri);
        if (incoming.action == Action::OpenTrack)
            backend.openTrack(incoming.uri);
        else {
            if (incoming.action == Action::PlayTrack && backend.localMode())
                backend.setLocalMode(false);
            backend.openUri(incoming.uri);
        }
    };
    Instance instance;
    if (!instance.listen(socket, [window, &route](const Instance::Request &incoming) {
        activateWindow(*window, true);
        route(incoming);
    })) {
        qWarning() << "Could not open the Omarchy Spotify activation socket:" << instance.errorString();
        return 1;
    }
    window->resize(configureWindow(*window));
    activateWindow(*window, false);
    route(*request);
    reportReady();

    // `omarchy agent look app` runs the app offscreen with these set, to see
    // the window without a display: size it to OMARCHY_LOOK_SIZE when given,
    // press the keys in OMARCHY_LOOK_KEYS through the window's Shortcuts,
    // save it once it has settled, and quit.
    if (const QString shot = qEnvironmentVariable("OMARCHY_LOOK_SCREENSHOT"); !shot.isEmpty()) {
        const QStringList size = qEnvironmentVariable("OMARCHY_LOOK_SIZE").split('x');
        if (size.size() == 2)
            window->resize(size[0].toInt(), size[1].toInt());
        const QStringList keys = qEnvironmentVariable("OMARCHY_LOOK_KEYS").split(' ', Qt::SkipEmptyParts);
        for (int i = 0; i < keys.size(); ++i) {
            QTimer::singleShot(800 + i * 300, &app, [&engine, key = QKeySequence(keys[i])] {
                for (QObject *shortcut : engine.rootObjects().constFirst()->findChildren<QObject *>()) {
                    if (qstrcmp(shortcut->metaObject()->className(), "QQuickShortcut") == 0
                        && shortcut->property("enabled").toBool()
                        && QKeySequence(shortcut->property("sequence").toString()) == key) {
                        QMetaObject::invokeMethod(shortcut, "activated");
                        break;
                    }
                }
            });
        }
        const int delay = qEnvironmentVariableIsSet("OMARCHY_LOOK_DELAY_MS") ? qEnvironmentVariableIntValue("OMARCHY_LOOK_DELAY_MS") : 1500;
        QTimer::singleShot(800 + keys.size() * 300 + delay, &app, [window, shot] {
            window->grabWindow().save(shot);
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
