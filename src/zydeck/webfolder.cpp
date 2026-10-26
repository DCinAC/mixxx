#include "zydeck/webfolder.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include "analyzer/analyzerscheduledtrack.h"
#include "library/library.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "library/trackset/crate/crate.h"
#include "moc_webfolder.cpp"
#include "sources/soundsourceproxy.h"
#include "track/track.h"
#include "track/trackref.h"
#include "zydeck/stems.h"

namespace zydeck {

namespace {

const QString kGroup = QStringLiteral("[ZyDeck]");
const QString kCrateName = QStringLiteral("ZyDeck Web");
constexpr int kSoonMs = 1500;
constexpr int kPollMs = 15000;

} // namespace

WebFolder::WebFolder(Library* pLibrary, UserSettingsPointer pConfig)
        : QObject(pLibrary),
          m_pLibrary(pLibrary),
          m_set(pConfig->getValueString(ConfigKey(kGroup, QStringLiteral("WebFolder")))),
          m_stems(pConfig->getSettingsPath() + QStringLiteral("/zydeck-stems")) {
    endSession(true);   // (and what a session left: its library entries now, its files if they're still there)
    m_soon.setSingleShot(true);
    m_soon.setInterval(kSoonMs);
    connect(&m_soon, &QTimer::timeout, this, &WebFolder::look);
    m_poll.setInterval(kPollMs);
    connect(&m_poll, &QTimer::timeout, this, &WebFolder::look);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_soon.start(); });
    m_poll.start();
    m_soon.start();
}

WebFolder::~WebFolder() {
    endSession(false);
}

// (the files go as Mixxx closes, the library going too then: their library entries go at the next start)
void WebFolder::endSession(bool library) {
    const QDir session(m_stems + QStringLiteral("/session"));
    if (!session.exists()) {
        return;
    }
    if (library) {
        m_pLibrary->trackCollectionManager()->purgeAllTracks(session);
    }
    for (const QFileInfo& n : QDir(m_stems).entryInfoList({QStringLiteral(".*.json")}, QDir::Files | QDir::Hidden)) {
        QFile f(n.absoluteFilePath());
        if (f.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("session")).toBool()) {
            f.close();
            f.remove();
        }
    }
    for (const QFileInfo& f : session.entryInfoList(QDir::Files | QDir::Hidden)) {
        QFile::remove(f.absoluteFilePath());
    }
}

// The folder: [ZyDeck] WebFolder if it's set and there; else "ZyDeck Web" in whichever music folder has one
// (Connect makes it in Mixxx's first, with the first download; the order Mixxx lists its folders in isn't that).
// "" while there's none: the look every 15 s finds it once it's made
QString WebFolder::folder() const {
    if (!m_set.isEmpty() && QFileInfo(m_set).isDir()) {
        return m_set;
    }
    for (const QString& root : m_pLibrary->trackCollectionManager()->internalCollection()->getRootDirStrings()) {
        const QString f = QDir(root).filePath(kCrateName);
        if (QFileInfo(f).isDir()) {
            return f;
        }
    }
    return {};
}

void WebFolder::watch() {
    QStringList want;
    for (const QString& d : {m_folder, m_stems}) {
        if (!d.isEmpty() && QFileInfo(d).isDir()) {
            want.append(d);
        }
    }
    const QStringList now = m_watcher.directories();
    if (QSet<QString>(now.cbegin(), now.cend()) != QSet<QString>(want.cbegin(), want.cend())) {
        if (!now.isEmpty()) {
            m_watcher.removePaths(now);
        }
        if (!want.isEmpty()) {
            m_watcher.addPaths(want);
        }
    }
}

void WebFolder::look() {
    m_folder = folder();
    watch();
    const QDir dir(m_folder.isEmpty() ? m_stems : m_folder);   // (no folder yet: stems can still come, to the NAS)
    QStringList settled;
    QHash<QString, qint64> sizes;
    const QFileInfoList files = m_folder.isEmpty() ? QFileInfoList()
                                                   : dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);   // (no hidden ones: .incoming)
    for (const QFileInfo& f : files) {
        const QString path = f.absoluteFilePath();
        if (m_done.contains(path) || !SoundSourceProxy::isFileNameSupported(f.fileName())) {
            continue;
        }
        // the same size as the last look (and a moment old): it's finished
        if (m_sizes.value(path, -1) == f.size() && f.lastModified().secsTo(QDateTime::currentDateTime()) >= 1) {
            settled.append(path);
        } else {
            sizes.insert(path, f.size());
        }
    }
    m_sizes = sizes;
    // Connect's notes on stems it made (in m_stems; older ones in the folder): each names its stem file, which is in
    // a "stem" folder beside its original (one on the NAS), in m_stems/session, or here in the folder (found above)
    m_notes.clear();
    m_session.clear();
    QList<QDir> notes{QDir(m_stems)};
    if (!m_folder.isEmpty()) {
        notes.prepend(dir);
    }
    for (const QDir& in : std::as_const(notes)) {
        for (const QFileInfo& n : in.entryInfoList({QStringLiteral(".*.json")}, QDir::Files | QDir::Hidden)) {
            QFile f(n.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly)) {
                continue;
            }
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            const QString stem = o.value(QStringLiteral("stem")).toString();
            const QString path = stem.isEmpty() ? in.filePath(n.fileName().mid(1).chopped(5)) : stem;   // (older notes: no "stem")
            if (!QFileInfo::exists(path)) {
                if (n.lastModified().secsTo(QDateTime::currentDateTime()) > 60) {
                    f.remove();   // (its stem never came: Connect couldn't save it there)
                }
                continue;   // (Connect writes the note just before the stem)
            }
            m_notes.insert(path, n.absoluteFilePath());
            if (o.value(QStringLiteral("session")).toBool()) {
                m_session.insert(path);
            }
            if ((m_folder.isEmpty() || QFileInfo(path).dir() != dir) && !m_done.contains(path)) {
                settled.append(path);   // (complete: Connect gives it its name finished)
            }
        }
    }
    if (!m_sizes.isEmpty()) {
        m_soon.start();   // (look again in a moment for the ones still settling)
    }
    if (!settled.isEmpty()) {
        add(settled);
    }
}

// In the library (if not there yet), in the crate, and analysed if it's new
void WebFolder::add(const QStringList& paths) {
    TrackCollectionManager* const pManager = m_pLibrary->trackCollectionManager();
    TrackCollection* const pCollection = pManager->internalCollection();
    QList<TrackId> ids;
    QList<AnalyzerScheduledTrack> analyse;
    for (const QString& path : paths) {
        m_done.insert(path);
        bool known = false;
        const TrackPointer pTrack = pManager->getOrAddTrack(TrackRef::fromFilePath(path), &known);
        if (!pTrack || !pTrack->getId().isValid()) {
            qWarning() << "ZyDeck Web: couldn't add" << path;
            continue;
        }
        ids.append(pTrack->getId());
        // stems from ZyDeck Connect: its note names the original, whose cues, beat grid and the rest they take over
        // (once: the note goes)
        QFile note(m_notes.value(path));
        if (m_notes.contains(path) && note.open(QIODevice::ReadOnly)) {
            const QString original = QJsonDocument::fromJson(note.readAll()).object().value(QStringLiteral("original")).toString();
            note.close();
            if (const TrackPointer pOriginal = pManager->getTrackByRef(TrackRef::fromFilePath(original))) {
                takeOver(pManager, pTrack, pOriginal, m_session.contains(path));
            }
            note.remove();
        }
        if (!known) {
            analyse.append(AnalyzerScheduledTrack(pTrack->getId()));
        }
    }
    if (ids.isEmpty()) {
        return;
    }
    Crate crate;
    CrateId crateId;
    if (pCollection->crates().readCrateByName(kCrateName, &crate)) {
        crateId = crate.getId();
    } else {
        crate.setName(kCrateName);
        if (!pCollection->insertCrate(crate, &crateId)) {
            qWarning() << "ZyDeck Web: couldn't make the crate";
        }
    }
    if (crateId.isValid()) {
        pCollection->addCrateTracks(crateId, ids);   // (already in it: ignored)
    }
    if (!analyse.isEmpty()) {
        emit m_pLibrary->analyzeTracks(analyse);
    }
}

} // namespace zydeck
