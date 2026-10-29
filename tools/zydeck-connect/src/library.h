#pragma once
// Mixxx's library, read only: tracks by their length (Mixxx's mapping only sees numbers), their files (for the
// waveforms made here, waveforms.h), and each track's analysed waveform and beat grid, as the phone's hub serves them.
// Safe from any thread (a database connection per thread).

#include <QByteArray>
#include <QJsonArray>

#include "zydecklibraryquery.h"
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

namespace zc {

/// Where Mixxx keeps its settings and library on this computer (the folder with mixxxdb.sqlite), or "". Looked for on
/// a worker thread every few seconds and kept: on macOS the first look into Mixxx's folder (another app's data) waits
/// until the user allows it, and the window mustn't wait with it. known: false until the first look is done (a worker
/// asking then looks itself).
QString mixxxDir(bool* known = nullptr);
/// Where to look for it, in order (the first is where the mapping goes when Mixxx hasn't run yet).
QStringList mixxxDirCandidates();

class Library {
  public:
    /// {"t": title, "a": artist, "id": id} of the track ms long (Mixxx keeps lengths to the millisecond), or
    /// empty. Tries again for a few seconds: a track added a moment ago.
    QJsonObject name(qint64 ms);
    /// "MXWF" + rate (float) + count (uint32) + all/low/mid/high per visual sample (the louder of left and
    /// right), from Mixxx's analysis file; empty if there's none.
    QByteArray waveform(int trackId);
    /// {"beats": [seconds], "bar": k, "locked": bool}: beat i starts a bar when (i + k) % 4 == 0; empty if none.
    QJsonObject beats(int trackId);
    int trackCount();
    /// Mixxx's music folders (Preferences › Library), in the order they were added.
    QStringList rootDirs();
    /// Track id's file, or "" (not in the library, or removed from it).
    QString location(int trackId);
    /// The tracks most likely to be loaded next, {id, file}: played last, added last, played most (a third each).
    QList<QPair<int, QString>> likelyTracks(int count);
    // The phone's library page in controller mode (/web/library/...): the same answers as its hub's own, from the
    // same queries (zydecklibraryquery.h), on this computer's library
    QJsonObject views();
    QJsonArray tracks(const QString& search, const QString& view, const QString& sort, int limit, int offset, const QString& genre);
    QJsonObject genres();
    QJsonObject track(int trackId);
    /// Autoplay's (zydecklibraryquery.h): random tracks of the pool, and the Auto DJ queue's length
    QList<int> pick(const zydeck::library::Pool& pool, int count);
    int autoDjQueueLength();

  private:
    struct Row {
        double seconds;
        QString title, artist;
        int id;
    };
    void load();   // with m_mutex held
    QMutex m_mutex;
    QVector<Row> m_rows;
    qint64 m_loadedMs = 0;
};

/// Decoding Mixxx's own files (exposed for tests)
QByteArray packWaveform(const QByteArray& analysisFile);
QJsonObject decodeBeats(const QByteArray& blob, const QString& version, int sampleRate, double duration, bool locked);

} // namespace zc
