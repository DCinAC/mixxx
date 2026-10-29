#pragma once

#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <atomic>

#include "preferences/usersettings.h"

namespace zydek {

/// Tracks' structure (zydeckstructure.h: sections with letters and labels, as rekordbox's phrases), worked out on a
/// background thread (one track at a time, at low priority: it decodes the whole file) from the track's audio and
/// Mixxx's beat grid, and kept in zydeck-structure/<track id>.json in Mixxx's settings folder until the grid changes.
class StructureCache {
  public:
    explicit StructureCache(UserSettingsPointer pConfig);
    ~StructureCache();

    /// {ok, bar, gridBar, confidence, sections: [{b0, b1 (beats), s, e (seconds), letter, label, energy}]} once it's
    /// known; {ok, pending: true} while it's being worked out (ask again); {ok: false, error} when it can't be (no beat
    /// grid yet, a file that won't decode). fresh: work it out again.
    QJsonObject get(int trackId, bool fresh);

  private:
    QString path(int trackId) const;

    UserSettingsPointer m_pConfig;
    QThreadPool m_pool;
    QMutex m_mutex;
    QSet<int> m_running;
    QHash<int, QJsonObject> m_failed;   // (by track, with the grid it failed on: not tried again until it changes)
    std::atomic<bool> m_cancel{false};
};

} // namespace zydek
