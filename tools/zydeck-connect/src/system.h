#pragma once
// The ZyDeck mapping in Mixxx's controllers folder, and opening ZyDeck Connect at login.

#include <QMap>
#include <QString>

namespace zc {

/// The ZyDeck mapping's files by name: the phone's own (res/controllers, built in), named for the desktop.
QMap<QString, QByteArray> mappingFiles();
bool mappingInstalled();
/// Installed and the same as the one built in (an update of ZyDeck Connect can bring a newer one).
bool mappingCurrent();
/// Into Mixxx's controllers folder: where they went, or throws QString (why not).
QString installMapping();

/// Mixxx's settings (mixxx.cfg) have the ZyDeck mapping on, for the ZyDeck Connect port.
bool controllerOn();
/// Turns it on in mixxx.cfg (backed up first, the first time, as mixxx.cfg.before-zydeck). Only with Mixxx closed:
/// it reads the file when it starts and writes it when it quits. Throws QString (why not).
void turnControllerOn();

bool openAtLogin();
void setOpenAtLogin(bool on);

} // namespace zc
