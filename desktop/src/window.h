#pragma once

#include <QSize>

class QWindow;

QSize configureWindow(QWindow &window);
void activateWindow(QWindow &window, bool revisit);
