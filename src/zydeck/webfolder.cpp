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
          m_pLibrary(pLibrary) {
    const ConfigKey key(kGroup, QStringLiteral("WebFolder"));
    m_folder = pConfig->getValueString(key);
    if (m_folder.isEmpty()) {
        const QStringList roots = pLibrary->trackCollectionManager()->internalCollection()->getRootDirStrings();
        if (roots.isEmpty()) {
            return;   // no music folder yet: nothing to watch
        }
        m_folder = QDir(roots.first()).filePath(kCrateName);
        pConfig->setValue(key, m_folder);
    }
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
}

// The folder itself (once it exists: Connect makes it with the first download) and, until then, the folder it goes in
void WebFolder::watch() {
    const QStringList now = m_watcher.directories();
    const QString parent = QFileInfo(m_folder).absolutePath();
    const QString want = QFileInfo::exists(m_folder) ? m_folder : parent;
    if (now != QStringList{want}) {
        if (!now.isEmpty()) {
            m_watcher.removePaths(now);
        }
        if (QFileInfo::exists(want)) {
            m_watcher.addPath(want);
        }
    }
}

void WebFolder::look() {
    watch();
    const QDir dir(m_folder);
    if (!dir.exists()) {
        return;
    }
    QStringList settled;
    QHash<QString, qint64> sizes;
    const QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);   // (no hidden ones: .incoming)
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
        // stems from ZyDeck Connect: its note beside the file names the original, whose cues, beat grid and the rest
        // they take over (once: the note goes)
        const QFileInfo fi(path);
        QFile note(fi.dir().filePath(QLatin1Char('.') + fi.fileName() + QStringLiteral(".json")));
        if (note.open(QIODevice::ReadOnly)) {
            const QString original = QJsonDocument::fromJson(note.readAll()).object().value(QStringLiteral("original")).toString();
            note.close();
            if (const TrackPointer pOriginal = pManager->getTrackByRef(TrackRef::fromFilePath(original))) {
                takeOver(pManager, pTrack, pOriginal);
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
