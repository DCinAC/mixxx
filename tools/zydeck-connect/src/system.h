#pragma once
// The ZyDeck mapping in Mixxx's controllers folder, and opening ZyDeck Connect at login.

#include <QMap>
#include <QString>

namespace zc {

/// The ZyDeck mapping's files by name: the phone's own (res/controllers, built in), named for the desktop.
QMap<QString, QByteArray> mappingFiles();
bool mappingInstalled();
/// Into Mixxx's controllers folder: where they went, or throws QString (why not).
QString installMapping();

bool openAtLogin();
void setOpenAtLogin(bool on);

} // namespace zc
