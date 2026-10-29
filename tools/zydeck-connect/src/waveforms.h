#pragma once
// ZyDeck Connect's own waveforms, for any track in Mixxx's library, analysed by Mixxx or not (a big library has few
// analysis files, and none at all with Mixxx's waveform cache off). Made as Mixxx makes its own
// (analyzer/analyzerwaveform.cpp): 441 points a second, each the loudest |sample| of the whole signal and of three
// bands (4th-order Bessel filters at 600 and 4000 Hz) as 0-255, the louder channel. Checked against Mixxx's analysis
// files (FLAC, AAC, Opus, MP3): the same, to the point and the level (an MP3 is put on Mixxx's timeline: its decoder
// trims a file's start differently from FFmpeg's, see mp3Shift). Decoded by Qt Multimedia (FFmpeg) at 44.1 kHz on a
// thread of its own, one track at a time, and kept in the cache folder (deflated, ~250 KB a track; the oldest go past
// 2 GB). A track asked for goes first; with nothing asked, the tracks most likely to be loaded (added, played or
// played most lately) are made ahead, one a second or so.

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>
#include <functional>
#include <memory>

class QAudioDecoder;
#include <QJsonObject>
#include <vector>

namespace zc {

class Library;

class Waveforms : public QObject {
    Q_OBJECT
  public:
    explicit Waveforms(Library* library, QObject* parent = nullptr);
    ~Waveforms() override;

    using Done = std::function<void(const QByteArray&)>;
    /// Track id's waveform, packed as the pages read it ("MXWF", see packWaveform in library.h): from the cache, or
    /// made now, ahead of everything else. done runs on any thread, with an empty array when the file can't be read
    /// (or this build has no decoder). Safe from any thread.
    void get(int trackId, Done done);
    /// Track id's structure (zydeckstructure.h: its sections, for the pages and Mixxx ZyDeck's Auto DJ), as the hub's
    /// /api/structure answers: the file (zydeck-structure/<id>.json in Mixxx's folder) when it's up to date with the
    /// track's beat grid, else {ok, pending} while it's worked out (from the same decoding as a waveform, ahead of the
    /// tracks made ahead), or {ok: false, error}. Safe from any thread (it reads the database: not the window's).
    QJsonObject structure(int trackId);

  private:
    struct Job {
        int id = 0;
        QString file, cacheFile;
        QList<Done> done;   // empty: made ahead, nobody waiting
        bool structure = false;   // its structure wanted (decoded for it even when its waveform's cached)
    };
    struct Analysis;

    QString cacheFileFor(int trackId, const QString& file) const;   // "" if the file isn't there
    // (the rest only on m_thread)
    void enqueue(Job job);
    void next();
    void decoded();
    void finish(const QByteArray& packed);
    void planAhead();
    void trimCache();
    void writeStructure(int trackId, const std::vector<float>& mono);   // (on m_thread)

    Library* m_library;
    QString m_cacheDir;
    QThread m_thread;
    QObject* m_context = nullptr;   // lives on m_thread: what runs there runs in its name
    QAudioDecoder* m_decoder = nullptr;
    QList<Job> m_queue;
    std::unique_ptr<Job> m_current;
    std::unique_ptr<Analysis> m_analysis;
    QTimer* m_aheadTimer = nullptr;   // the pause between tracks made ahead
    QTimer* m_planTimer = nullptr;    // what to make ahead, looked at again now and then
    QTimer* m_stuckTimer = nullptr;   // a file that never finishes decoding (a network drive gone)
};

} // namespace zc
