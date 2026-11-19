#include "window.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScreen>
#include <QTimer>
#include <QWindow>

namespace {
bool hyprland() {
    return QGuiApplication::platformName().startsWith("wayland")
           && qEnvironmentVariableIsSet("HYPRLAND_INSTANCE_SIGNATURE");
}

QByteArray hyprctl(const QStringList &arguments) {
    QProcess process;
    process.start("hyprctl", arguments);
    if (!process.waitForFinished(500)) {
        process.kill();
        process.waitForFinished(200);
        return {};
    }
    return process.readAllStandardOutput();
}

QSize availableSize(QWindow &window) {
    if (hyprland()) {
        const auto document = QJsonDocument::fromJson(hyprctl({"-j", "monitors"}));
        const auto monitors = document.isArray() ? document.array() : QJsonArray{};
        for (const auto &entry : monitors) {
            const auto monitor = entry.toObject();
            if (!monitor.value("focused").toBool())
                continue;
            const double scale = monitor.value("scale").toDouble();
            const auto reserved = monitor.value("reserved").toArray();
            if (scale <= 0 || reserved.size() != 4)
                break;
            QSize pixels(monitor.value("width").toInt(), monitor.value("height").toInt());
            if (monitor.value("transform").toInt() % 2 != 0)
                pixels.transpose();
            return pixels / scale - QSize(reserved[0].toInt() + reserved[2].toInt(),
                                          reserved[1].toInt() + reserved[3].toInt());
        }
    }
    return window.screen() ? window.screen()->availableSize() : QSize(874, 624);
}
}

QSize configureWindow(QWindow &window) {
    if (hyprland()) {
        // Named runtime rule survives subsequent launches without changing config files.
        const QByteArray result = hyprctl({"eval", R"(
            hl.window_rule({
                name = "omarchy-spotify-desktop",
                enabled = true,
                match = { class = "^omarchy-spotify$" },
                float = true, center = true, focus_on_activate = true
            })
        )"});
        if (result.trimmed() != "ok")
            qWarning("Could not configure the Omarchy Spotify window rule");
    }
    const QSize available = availableSize(window) - QSize(24, 24);
    const QSize size = QSize(850, 600).boundedTo(available.expandedTo(QSize(1, 1)));
    window.setMinimumSize(QSize(500, 360).boundedTo(size));
    return size;
}

void activateWindow(QWindow &window, bool revisit) {
    if (window.visibility() == QWindow::Minimized)
        window.showNormal();
    else if (!window.isVisible())
        window.show();
    if (QGuiApplication::platformName() == "xcb")
        window.raise();
    if (!revisit || !hyprland()) {
        window.requestActivate();
    } else {
        // The selector combines the desktop ID with this instance's PID.
        const QString script = QString(R"(
            local w = hl.get_window("pid:%1")
            if w and w.class == "omarchy-spotify" then
                local ws = hl.get_active_special_workspace() or hl.get_active_workspace()
                if ws and w.workspace and w.workspace.id ~= ws.id then
                    hl.dispatch(hl.dsp.window.move({window = w, workspace = ws, follow = true}))
                    if w.floating and (w.fullscreen or 0) == 0 then
                        hl.dispatch(hl.dsp.window.center({window = w}))
                    end
                end
                hl.dispatch(hl.dsp.focus({window = w}))
                if w.floating then
                    hl.dispatch(hl.dsp.window.alter_zorder({window = w, mode = "top"}))
                end
            end
        )").arg(QCoreApplication::applicationPid());
        auto *process = new QProcess(&window);
        QObject::connect(process, &QProcess::finished, process, [process, &window] {
            if (process->readAllStandardOutput().trimmed() != "ok") {
                qWarning("Could not focus the Omarchy Spotify window through Hyprland");
                window.requestActivate();
            }
            process->deleteLater();
        });
        QObject::connect(process, &QProcess::errorOccurred, process, [process, &window](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                window.requestActivate();
                process->deleteLater();
            }
        });
        QTimer::singleShot(600, process, &QProcess::kill);
        process->start("hyprctl", {"eval", script});
    }
}
