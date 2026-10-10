#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <memory>

#include "preferences/usersettings.h"
#include "track/track_decl.h"

namespace zydek {

/// What the phone's library page needs from Mixxx: browsing and searching the library, loading a track
/// into a deck or sampler by file path, previewing it, and managing the music folders.
///
/// Reads go through its own read-only connection to Mixxx's database (it lives on the controller thread);
/// actions are handed to Mixxx's objects on the main thread.
class Library {
  public:
    explicit Library(UserSettingsPointer pConfig);
    ~Library();

    QJsonObject views();
    /// view: all | stems | crate:<id> | playlist:<id>. Every word of q must appear in artist/title/album/genre.
    QJsonArray tracks(const QString& q, const QString& view, const QString& sort, int limit, int offset);
    QJsonObject track(int trackId);
    QString location(int trackId);

    /// target: deck1..deck4, sampler (first empty), samplerN. Returns {ok, group} or {error}.
    QJsonObject load(int trackId, const QString& target);
    QJsonObject loadLocation(const QString& location, const QString& target);
    QJsonObject preview(int trackId);
    void stopPreview();
    QJsonObject decks() const;

    QJsonArray folders();
    QJsonObject addFolder(const QString& path);
    // crates (changes go through Mixxx, so its own views update too)
    QJsonObject createCrate(const QString& name);
    QJsonObject setCrateTrack(int crateId, int trackId, bool member);
    QJsonArray cratesOf(int trackId);
    /// Adds a file to Mixxx's library (a Web tab download), returning its track id.
    QJsonObject addTrack(const QString& path);
    QJsonObject listDirectory(const QString& path) const;
    void startScan();
    bool scanning() const;

    /// Folders the phone hides, and "Analyze all" skips. Mixxx has no such setting, so Zydek keeps its own
    /// list (zydek-excluded.json in Mixxx's settings folder).
    QJsonArray excludedFolders();
    QJsonObject setExcluded(const QString& path, bool excluded);
    /// Mixxx's own analysis (BPM, key, beat grid, waveform) of every track the phone shows that has no BPM
    /// or key yet, in the background. dryRun: only count them.
    QJsonObject analyzeAll(bool dryRun);
    QJsonObject analysisStatus();
    void stopAnalysis();

    /// Sampler capture: the loop on a deck, with only the stems that deck plays (all of them for a normal
    /// track), cut straight from the file into a WAV in Music/ZyDeck Samples, added to the library with the
    /// source's BPM, and loaded into sampler `slot` set to loop. sync: it follows the master tempo.
    QJsonObject captureSample(int deck, int slot, bool sync);
    /// The sample editor's cut: [start, end) seconds of what's loaded in `group` (the preview deck), with the
    /// stems in stemMask (bit 0 = stem 1; 0 = the whole track), into sampler `slot`.
    QJsonObject cutSample(const QString& group, double start, double end, uint stemMask, int slot, bool sync);
    /// A sampler's playing options: keep its key, follow the master tempo (or not), loop the whole clip (or not).
    static void setSamplerOptions(const QString& group, bool sync, bool repeat);

  private:
    QJsonObject renderSample(const TrackPointer& pTrack, double firstFrame, double lastFrame, uint mask, int slot, bool sync);
    bool open();
    QJsonObject trackJson(const class QSqlQuery& query) const;
    /// SQL condition leaving out the excluded folders; its values are appended to pArgs.
    QString notExcluded(QVariantList* pArgs);
    void loadExcluded();
    void saveExcluded();

    QStringList m_excluded;
    bool m_excludedLoaded = false;
    struct Analysis;
    std::shared_ptr<Analysis> m_pAnalysis;

    UserSettingsPointer m_pConfig;
    QSqlDatabase m_db;
};

} // namespace zydek
