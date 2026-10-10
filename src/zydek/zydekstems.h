#pragma once

#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QWaitCondition>
#include <deque>
#include <thread>

#include "preferences/usersettings.h"
#include "track/track_decl.h"

namespace zydek {

/// Live stems: splits a track into drums, bass, other and vocals on the phone itself, like Serato's stems,
/// so a deck can mute them a few seconds after the track loads.
///
/// The model is Open-Unmix UMX-HQ (sigsep, MIT), four small BiLSTM networks quantized to 8 bits and run with
/// ONNX Runtime on the CPU: about 0.2 s for 12 s of music on a recent phone, ~5 s for a whole track. Each one
/// estimates its instrument's spectrum; their shares of the mixture (soft masks) make the four stems, which add
/// up to the original. The result is an NI Stems file (.stem.m4a: the mix and four AAC stems, ~40 MB for four
/// minutes) in Music/ZyDeck Stems, added to the library with the original's beat grid, key and hot cues.
/// Separation runs on its own thread, one track at a time.
class LiveStems : public QObject {
    Q_OBJECT
  public:
    LiveStems(UserSettingsPointer pConfig, QObject* pParent);
    ~LiveStems() override;

    /// Built with ONNX Runtime and the models.
    static bool available();
    /// Queues the track (once). Results come as progress() and finished().
    void request(const TrackPointer& pTrack);
    /// The stems file made from this track before, or empty.
    QString stemFileFor(int trackId) const;
    /// {"busy": track id or 0, "progress": 0..1, "queued": n}
    QJsonObject status() const;

  signals:
    void progress(int trackId, double fraction);
    /// path: the stems file, or empty with error
    void finished(int trackId, const QString& path, const QString& error);

  private:
    void run();
    QString separate(const TrackPointer& pTrack, QString* pError);
    void remember(int trackId, const QString& path);

    UserSettingsPointer m_pConfig;
    mutable QMutex m_mutex;
    QWaitCondition m_wake;
    std::deque<TrackPointer> m_queue;
    int m_busyId = 0;
    double m_progress = 0;
    bool m_stop = false;
    QHash<int, QString> m_files;   // track id -> stems file, kept in zydek-livestems.json
    std::thread m_thread;
};

} // namespace zydek
