#pragma once

#include <QFileSystemWatcher>
#include <QHash>
#include <QSet>
#include <QString>
#include <QTimer>

#include "preferences/usersettings.h"

class Library;

namespace zydeck {

// ZyDeck: the downloads folder, watched (ZyDeck Connect's YouTube and Bilibili downloads; [ZyDeck] WebFolder, by
// default "ZyDeck Web" in the first music folder). A file that lands there joins the library at once, goes into the
// crate "ZyDeck Web" and is analysed: no rescan. Connect writes into the hidden ".incoming" folder inside it and
// moves a file in when it's complete, so what shows up is finished; a file copied in some other way waits until its
// size has stopped changing. Files that arrived while Mixxx was closed join at start.
class WebFolder final : public QObject {
    Q_OBJECT
  public:
    WebFolder(Library* pLibrary, UserSettingsPointer pConfig);
    ~WebFolder() final;

  private:
    void look();                   // the folder's files: the settled ones in, the rest kept an eye on
    void add(const QStringList& paths);
    void watch();

    Library* const m_pLibrary;
    QString m_folder;
    QFileSystemWatcher m_watcher;
    QTimer m_soon;                     // a burst of changes looked at once, a moment after
    QTimer m_poll;                     // a network share doesn't always say when something changed: looked at now and then
    QSet<QString> m_done;              // files already taken in (or found in the library)
    QHash<QString, qint64> m_sizes;    // files not settled yet: their size when last looked at
};

} // namespace zydeck
