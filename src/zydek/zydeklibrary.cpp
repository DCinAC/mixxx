#include "zydek/zydeklibrary.h"
#include "zydek/zydeckshared.h"
#include "zydek/zydecklibraryquery.h"
#include "zydek/zydekmain.h"

#include <algorithm>

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QMetaObject>
#include <QMutex>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStringList>

#include "analyzer/analyzerprogress.h"
#include "analyzer/analyzerscheduledtrack.h"
#include "analyzer/plugins/analyzerqueenmarybeats.h"
#include "control/controlobject.h"
#include "library/analysis/analysisfeature.h"
#include "sources/audiosource.h"
#include "sources/soundsourceproxy.h"
#include "track/beats.h"
#include "track/beatutils.h"
#include "track/steminfo.h"
#include "util/samplebuffer.h"
#include <QTimer>
#include "library/library.h"
#include "library/trackcollection.h"
#include "library/trackset/crate/crate.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "qml/qmllibraryproxy.h"
#include "qml/qmlplayermanagerproxy.h"
#include "track/cue.h"
#include "track/track.h"
#include "track/trackref.h"
#include "util/fileinfo.h"

namespace zydek {

using zydeck::library::kTrackFrom;

namespace {

const QString kConnection = QStringLiteral("zydek-library");
constexpr int kMaxSamplers = 64;
const QString kExcludedFile = QStringLiteral("zydek-excluded.json");

const QStringList kAudioSuffixes = {"mp3", "flac", "wav", "aiff", "aif", "m4a", "mp4", "ogg", "opus", "wv", "alac"};

double control(const QString& group, const QString& key) {
    return ControlObject::get(ConfigKey(group, key));
}

QString groupFor(const QString& target, QString* pError) {
    if (target == QLatin1String("preview")) {   // the headphones' preview deck (the beat grid editor)
        return QStringLiteral("[PreviewDeck1]");
    }
    if (target.startsWith(QLatin1String("deck"))) {
        const int n = target.mid(4).toInt();
        if (n >= 1 && n <= 4) {
            return QStringLiteral("[Channel%1]").arg(n);
        }
    } else if (target == QLatin1String("sampler")) {   // first empty sampler
        const int count = std::min(kMaxSamplers, static_cast<int>(control(QStringLiteral("[App]"), QStringLiteral("num_samplers"))));
        for (int n = 1; n <= count; ++n) {
            const QString group = QStringLiteral("[Sampler%1]").arg(n);
            if (control(group, QStringLiteral("track_loaded")) == 0) {
                return group;
            }
        }
        *pError = QStringLiteral("All samplers are loaded");
        return {};
    } else if (target.startsWith(QLatin1String("sampler"))) {
        const int n = target.mid(7).toInt();
        if (n >= 1 && n <= kMaxSamplers) {
            return QStringLiteral("[Sampler%1]").arg(n);
        }
    }
    *pError = QStringLiteral("Unknown target %1").arg(target);
    return {};
}

QJsonObject error(const QString& message) {
    return {{"ok", false}, {"error", message}};
}

} // namespace

Library::Library(UserSettingsPointer pConfig)
        : m_pConfig(pConfig) {
}

Library::~Library() {
    if (m_db.isOpen()) {
        m_db.close();
    }
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(kConnection);
}

bool Library::open() {
    if (m_db.isOpen()) {
        return true;
    }
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), kConnection);
    m_db.setDatabaseName(QDir(m_pConfig->getSettingsPath()).filePath(QStringLiteral("mixxxdb.sqlite")));
    // Mixxx owns and writes the database; this connection only reads.
    m_db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=2000"));
    if (!m_db.open()) {
        qWarning() << "Zydek: can't open the library database:" << m_db.lastError().text();
        return false;
    }
    return true;
}

QJsonObject Library::views() {
    if (!open()) {
        return {{"total", 0}, {"stems", 0}, {"crates", QJsonArray()}, {"playlists", QJsonArray()}, {"history", QJsonArray()}};
    }
    return zydeck::library::views(m_db, only());
}

QJsonObject Library::genres() {
    if (!open()) {
        return {{"genres", QJsonArray()}, {"none", 0}};
    }
    return zydeck::library::genres(m_db, only());
}

QJsonArray Library::tracks(const QString& search, const QString& view, const QString& sort, int limit, int offset,
        const QString& genre) {
    if (!open()) {
        return {};
    }
    if (sort == QLatin1String("loaded")) {
        readLoaded();
    }
    return zydeck::library::tracks(m_db, only(), search, view, sort, limit, offset, genre, m_loaded);
}

QList<int> Library::pick(const zydeck::library::Pool& pool, int count, double after) {
    return open() ? zydeck::library::pick(m_db, only(), pool, count, after) : QList<int>();
}

double Library::autoDjQueueTailBpm() {
    return open() ? zydeck::library::autoDjQueueTailBpm(m_db) : 0;
}

QJsonArray Library::tracksIn(const QList<int>& ids, const QString& search, const QString& sort, int limit, int offset, const QString& genre) {
    return open() ? zydeck::library::tracks(m_db, only(), search, QStringLiteral("ids"), sort, limit, offset, genre, ids) : QJsonArray();
}

int Library::autoDjQueueLength() {
    return open() ? zydeck::library::autoDjQueueLength(m_db) : 0;
}

QJsonObject Library::track(int trackId) {
    if (!open()) {
        return {};
    }
    return zydeck::library::track(m_db, trackId);
}

QString Library::location(int trackId) {
    return track(trackId).value(QStringLiteral("location")).toString();
}

void Library::readLoaded() {
    if (m_loadedRead) {
        return;
    }
    m_loadedRead = true;
    QFile file(QDir(m_pConfig->getSettingsPath()).filePath(QStringLiteral("zydek-loaded.json")));
    if (file.open(QIODevice::ReadOnly)) {
        for (const QJsonValue& v : QJsonDocument::fromJson(file.readAll()).array()) {
            if (v.toInt() > 0) {
                m_loaded.append(v.toInt());
            }
        }
    }
}

void Library::noteLoaded(int trackId) {
    if (trackId <= 0) {
        return;
    }
    readLoaded();
    if (!m_loaded.isEmpty() && m_loaded.first() == trackId) {
        return;
    }
    m_loaded.removeAll(trackId);
    m_loaded.prepend(trackId);
    while (m_loaded.size() > 300) {
        m_loaded.removeLast();
    }
    QJsonArray ids;
    for (int id : std::as_const(m_loaded)) {
        ids.append(id);
    }
    QSaveFile file(QDir(m_pConfig->getSettingsPath()).filePath(QStringLiteral("zydek-loaded.json")));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(ids).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

QJsonObject Library::load(int trackId, const QString& target) {
    const QJsonObject t = track(trackId);
    if (t.isEmpty()) {
        return error(QStringLiteral("Track not in the library"));
    }
    if (t.value(QStringLiteral("missing")).toBool()) {
        return error(QStringLiteral("The file isn't available"));
    }
    QJsonObject result = loadLocation(t.value(QStringLiteral("location")).toString(), target);
    result.insert(QStringLiteral("title"), t.value(QStringLiteral("title")));
    return result;
}

QJsonObject Library::loadLocation(const QString& location, const QString& target, bool evenIfPlaying) {
    QString err;
    const QString group = groupFor(target, &err);
    if (group.isEmpty()) {
        return error(err);
    }
    if (!evenIfPlaying && group.startsWith(QLatin1String("[Channel")) && control(group, QStringLiteral("play")) > 0) {
        return error(QStringLiteral("Deck %1 is playing").arg(group.mid(8, 1)));
    }
    PlayerManager* pPlayerManager = mixxx::qml::QmlPlayerManagerProxy::get();
    if (!pPlayerManager) {
        return error(QStringLiteral("Mixxx isn't ready"));
    }
    QMetaObject::invokeMethod(
            pPlayerManager,
            [pPlayerManager, location, group] {
                pPlayerManager->slotLoadLocationToPlayer(location, group, false);
            },
            Qt::QueuedConnection);
    return {{"ok", true}, {"group", group}};
}

QJsonObject Library::preview(int trackId) {
    const QString loc = location(trackId);
    if (loc.isEmpty() || !QFileInfo::exists(loc)) {
        return error(QStringLiteral("The file isn't available"));
    }
    PlayerManager* pPlayerManager = mixxx::qml::QmlPlayerManagerProxy::get();
    if (!pPlayerManager || control(QStringLiteral("[App]"), QStringLiteral("num_preview_decks")) < 1) {
        return error(QStringLiteral("No preview deck"));
    }
    // Mixxx's preview deck plays on the headphone (cue) output, not the main mix.
    QMetaObject::invokeMethod(
            pPlayerManager,
            [pPlayerManager, loc] {
                pPlayerManager->slotLoadLocationToPlayer(loc, QStringLiteral("[PreviewDeck1]"), true);
            },
            Qt::QueuedConnection);
    return {{"ok", true}};
}

void Library::stopPreview() {
    ControlObject::set(ConfigKey(QStringLiteral("[PreviewDeck1]"), QStringLiteral("play")), 0);
}

QJsonObject Library::decks() const {
    QJsonArray decks;
    for (int n = 1; n <= 4; ++n) {
        const QString group = QStringLiteral("[Channel%1]").arg(n);
        const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(group);
        decks.append(QJsonObject{
                {"n", n},
                {"loaded", pTrack != nullptr},
                {"playing", control(group, QStringLiteral("play")) > 0},
                {"title", pTrack ? QJsonValue(pTrack->getTitle()) : QJsonValue()},
                {"artist", pTrack ? QJsonValue(pTrack->getArtist()) : QJsonValue()},
                {"id", pTrack ? QJsonValue(pTrack->getId().toVariant().toInt()) : QJsonValue()},
        });
    }
    int emptySamplers = 0;
    const int count = std::min(kMaxSamplers, static_cast<int>(control(QStringLiteral("[App]"), QStringLiteral("num_samplers"))));
    for (int n = 1; n <= count; ++n) {
        emptySamplers += control(QStringLiteral("[Sampler%1]").arg(n), QStringLiteral("track_loaded")) == 0;
    }
    return {{"decks", decks}, {"emptySamplers", emptySamplers}, {"scanning", scanning()}};
}

QJsonArray Library::folders() {
    QJsonArray out;
    if (!open()) {
        return out;
    }
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT directory FROM directories ORDER BY directory"));
    while (q.next()) {
        const QString dir = q.value(0).toString();
        out.append(QJsonObject{{"path", dir}, {"available", QFileInfo(dir).isDir()}});
    }
    return out;
}

QJsonObject Library::createCrate(const QString& name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        return error(QStringLiteral("Give the crate a name"));
    }
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    if (!pLibrary) {
        return error(QStringLiteral("Mixxx isn't ready"));
    }
    TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
    bool ok = false;
    CrateId id;
    runOnMain(
            pCollection,
            [pCollection, trimmed, &ok, &id] {
                Crate crate;
                crate.setName(trimmed);
                ok = pCollection->internalCollection()->insertCrate(crate, &id);
            });
    if (!ok) {
        return error(QStringLiteral("Couldn't make the crate: is there one with that name already?"));
    }
    return {{"ok", true}, {"id", id.toVariant().toInt()}};
}

QJsonObject Library::setCrateTrack(int crateId, int trackId, bool member) {
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    if (!pLibrary) {
        return error(QStringLiteral("Mixxx isn't ready"));
    }
    TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
    bool ok = false;
    runOnMain(
            pCollection,
            [pCollection, crateId, trackId, member, &ok] {
                TrackCollection* pTracks = pCollection->internalCollection();
                const CrateId crate{QVariant(crateId)};
                const QList<TrackId> tracks{TrackId(QVariant(trackId))};
                ok = member ? pTracks->addCrateTracks(crate, tracks) : pTracks->removeCrateTracks(crate, tracks);
            });
    if (!ok) {
        return error(QStringLiteral("Couldn't change the crate"));
    }
    return {{"ok", true}};
}

QJsonArray Library::cratesOf(int trackId) {
    QJsonArray out;
    if (!open()) {
        return out;
    }
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT crate_id FROM crate_tracks WHERE track_id = ?"));
    q.addBindValue(trackId);
    if (q.exec()) {
        while (q.next()) {
            out.append(q.value(0).toInt());
        }
    }
    return out;
}

QJsonObject Library::tracklist() {
    if (!open()) {
        return {{"ok", false}, {"error", "Can't read the library"}};
    }
    QSqlQuery list(m_db);   // Mixxx keeps a History list per session (hidden = 2, PlaylistDAO::PLHT_SET_LOG)
    if (!list.exec(QStringLiteral("SELECT id, name FROM Playlists WHERE hidden = 2 ORDER BY id DESC LIMIT 1")) || !list.next()) {
        return {{"ok", true}, {"name", QString()}, {"tracks", QJsonArray()}, {"text", QString()}};
    }
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT l.artist, l.title, pt.pl_datetime_added, tl.location FROM PlaylistTracks pt "
                             "JOIN library l ON l.id = pt.track_id JOIN track_locations tl ON tl.id = l.location "
                             "WHERE pt.playlist_id = ? ORDER BY pt.position"));
    q.addBindValue(list.value(0));
    QJsonArray tracks;
    QStringList lines;
    QString last;
    if (q.exec()) {
        while (q.next()) {
            const QString artist = q.value(0).toString();
            const QString title = q.value(1).toString().isEmpty() ? QFileInfo(q.value(3).toString()).completeBaseName()
                                                                   : q.value(1).toString();
            const QString line = artist.isEmpty() ? title : artist + QStringLiteral(" - ") + title;
            if (line == last) {
                continue;   // the same track again (its live stems took over): once
            }
            last = line;
            const QString at = QDateTime::fromString(q.value(2).toString(), Qt::ISODate).toLocalTime().toString(QStringLiteral("HH:mm"));
            tracks.append(QJsonObject{{"artist", artist}, {"title", title}, {"at", at}});
            lines.append(QStringLiteral("%1. %2").arg(tracks.size()).arg(line));
        }
    }
    return {{"ok", true}, {"name", list.value(1).toString()}, {"tracks", tracks}, {"text", lines.join(QLatin1Char('\n'))}};
}

QList<int> Library::crateTrackIds(const QString& name, int* pCrateId) {
    QList<int> ids;
    *pCrateId = 0;
    if (!open()) {
        return ids;
    }
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT id FROM crates WHERE name = ?"));
    q.addBindValue(name);
    if (!q.exec() || !q.next()) {
        return ids;
    }
    *pCrateId = q.value(0).toInt();
    QSqlQuery t(m_db);
    t.prepare(QStringLiteral("SELECT track_id FROM crate_tracks WHERE crate_id = ?"));
    t.addBindValue(*pCrateId);
    if (t.exec()) {
        while (t.next()) {
            ids.append(t.value(0).toInt());
        }
    }
    return ids;
}

QJsonObject Library::addTrack(const QString& path) {
    if (!QFileInfo(path).isFile()) {
        return error(QStringLiteral("File not found"));
    }
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    if (!pLibrary) {
        return error(QStringLiteral("Mixxx isn't ready"));
    }
    TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
    int id = -1;
    runOnMain(
            pCollection,
            [pCollection, path, &id] {
                const TrackPointer pTrack = pCollection->getOrAddTrack(TrackRef::fromFilePath(path));
                if (pTrack) {
                    id = pTrack->getId().toVariant().toInt();
                }
            });
    if (id < 0) {
        return error(QStringLiteral("Mixxx couldn't add the file"));
    }
    return {{"ok", true}, {"id", id}};
}

namespace {
/// A whole storage (the phone's, or a card's) rather than a folder on it: it holds Android/ with every app's
/// data, and scanning it all is slow and fills the library with ringtones and app sounds.
bool isStorageRoot(const QString& path) {
    const QString p = QDir::cleanPath(path);
    static const QRegularExpression card(QStringLiteral("^/storage/[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}$"));
    return p == QLatin1String("/") || p == QLatin1String("/storage") || p == QLatin1String("/storage/emulated") ||
            p == QLatin1String("/storage/emulated/0") || p == QLatin1String("/sdcard") ||
            p == QLatin1String("/storage/self/primary") || card.match(p).hasMatch();
}
} // namespace

QJsonObject Library::addFolder(const QString& requested) {
    const QString path = QDir::cleanPath(requested);
    if (isStorageRoot(path)) {
        return error(QStringLiteral("Pick a folder inside the storage, like Music or Download: the whole storage "
                                    "holds every app's files too"));
    }
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    if (!pLibrary) {
        return error(QStringLiteral("Mixxx isn't ready"));
    }
    // Mixxx won't watch a folder inside one it watches already. When that one is a whole storage (added
    // before ZyDeck refused them), the new folder replaces it: the tracks outside it leave the library.
    QString replaces;
    for (const QJsonValue& f : folders()) {
        const QString dir = f.toObject().value(QStringLiteral("path")).toString();
        if (isStorageRoot(dir) && path.startsWith(QDir::cleanPath(dir) + QLatin1Char('/'))) {
            replaces = QDir::cleanPath(dir);
        }
    }
    const QList<TrackId> outside = replaces.isEmpty() ? QList<TrackId>() : trackIdsUnder(replaces, path, false);
    const QList<TrackId> hidden = trackIdsUnder(path, QString(), true);   // removed before: back with their cues
    TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
    DirectoryDAO::AddResult result = DirectoryDAO::AddResult::SqlError;
    // The collection belongs to the main thread; wait for it there (adding a folder is quick).
    runOnMain(
            pCollection,
            [pCollection, path, replaces, outside, hidden, &result] {
                if (!replaces.isEmpty()) {
                    pCollection->removeDirectory(mixxx::FileInfo(replaces));
                }
                result = pCollection->addDirectory(mixxx::FileInfo(path));
                if (result != DirectoryDAO::AddResult::Ok) {
                    if (!replaces.isEmpty()) {
                        pCollection->addDirectory(mixxx::FileInfo(replaces));   // put it back
                    }
                    return;
                }
                if (!outside.isEmpty()) {
                    pCollection->hideTracks(outside);
                }
                if (!hidden.isEmpty()) {
                    pCollection->unhideTracks(hidden);
                }
            });
    switch (result) {
    case DirectoryDAO::AddResult::Ok:
        startScan();
        return {{"ok", true}, {"replaced", replaces}};
    case DirectoryDAO::AddResult::AlreadyWatching:
        return error(QStringLiteral("This folder (or one containing it) is already in the library"));
    case DirectoryDAO::AddResult::InvalidOrMissingDirectory:
        return error(QStringLiteral("That folder doesn't exist"));
    case DirectoryDAO::AddResult::UnreadableDirectory:
        return error(QStringLiteral("Can't read that folder: allow ZyDeck to read your music (Folders tab)"));
    default:
        return error(QStringLiteral("Couldn't add the folder"));
    }
}

/// Tracks whose file is under dir (and not under except); hidden: the ones hidden from the library instead.
QList<TrackId> Library::trackIdsUnder(const QString& dir, const QString& except, bool hidden) {
    QList<TrackId> ids;
    if (!open()) {
        return ids;
    }
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT l.id FROM %1 WHERE l.mixxx_deleted = ? AND substr(tl.location, 1, ?) = ?%2")
                      .arg(kTrackFrom, except.isEmpty() ? QString() : QStringLiteral(" AND substr(tl.location, 1, ?) <> ?")));
    const QString prefix = dir + QLatin1Char('/');
    q.addBindValue(hidden ? 1 : 0);
    q.addBindValue(prefix.size());
    q.addBindValue(prefix);
    if (!except.isEmpty()) {
        const QString exceptPrefix = except + QLatin1Char('/');
        q.addBindValue(exceptPrefix.size());
        q.addBindValue(exceptPrefix);
    }
    if (q.exec()) {
        while (q.next()) {
            ids.append(TrackId(q.value(0)));
        }
    }
    return ids;
}

QJsonObject Library::removeFolder(const QString& path) {
    if (folders().size() < 2) {
        return error(QStringLiteral("That's the only music folder: add another one first"));
    }
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    if (!pLibrary) {
        return error(QStringLiteral("Mixxx isn't ready"));
    }
    TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
    bool ok = false;
    // Like Mixxx's "Hide tracks": its tracks leave the library, but keep their cues and beat grids in case
    // the folder comes back
    runOnMain(
            pCollection,
            [pCollection, path, &ok] {
                ok = pCollection->removeDirectory(mixxx::FileInfo(path)) == DirectoryDAO::RemoveResult::Ok;
                if (ok) {
                    pCollection->hideAllTracks(QDir(path));
                }
            });
    return ok ? QJsonObject{{"ok", true}} : error(QStringLiteral("Couldn't remove that folder"));
}

QJsonObject Library::listDirectory(const QString& requested) const {
    // Start somewhere useful: the shared storage on Android, the Music folder elsewhere.
    QString path = requested;
    if (path.isEmpty()) {
#ifdef __ANDROID__
        path = QStringLiteral("/storage/emulated/0");
#else
        path = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
#endif
    }
    QDir dir(path);
    QJsonArray dirs;
    int audio = 0;
    const auto entries = dir.entryInfoList(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable,
            QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& e : entries) {
        if (e.isDir()) {
            if (!e.fileName().startsWith(QLatin1Char('.'))) {
                dirs.append(e.fileName());
            }
        } else if (kAudioSuffixes.contains(e.suffix().toLower())) {
            ++audio;
        }
    }
    QJsonArray roots;
#ifdef __ANDROID__
    roots.append(QStringLiteral("/storage/emulated/0"));
    const auto volumes = QDir(QStringLiteral("/storage")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& v : volumes) {   // SD cards and USB drives show up as /storage/XXXX-XXXX
        if (v != QLatin1String("emulated") && v != QLatin1String("self")) {
            roots.append(QString(QStringLiteral("/storage/") + v));
        }
    }
#endif
    return {{"path", dir.absolutePath()},
            {"parent", dir.isRoot() ? QJsonValue() : QJsonValue(QFileInfo(dir.absolutePath()).absolutePath())},
            {"readable", dir.isReadable()},
            {"dirs", dirs},
            {"audioFiles", audio},
            {"roots", roots}};
}

void Library::startScan() {
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    if (!pLibrary) {
        return;
    }
    TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
    QMetaObject::invokeMethod(pCollection, &TrackCollectionManager::startLibraryScan, Qt::QueuedConnection);
}

bool Library::scanning() const {
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    return pLibrary && pLibrary->trackCollectionManager()->isLibraryScanActive();
}

} // namespace zydek

namespace zydek {

// ---- excluded folders ---------------------------------------------------------------------------------

void Library::loadExcluded() {
    if (m_excludedLoaded) {
        return;
    }
    m_excludedLoaded = true;
    QFile f(QDir(m_pConfig->getSettingsPath()).filePath(kExcludedFile));
    if (f.open(QIODevice::ReadOnly)) {
        for (const QJsonValue& v : QJsonDocument::fromJson(f.readAll()).array()) {
            m_excluded.append(v.toString());
        }
    }
}

void Library::saveExcluded() {
    QSaveFile f(QDir(m_pConfig->getSettingsPath()).filePath(kExcludedFile));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonArray::fromStringList(m_excluded)).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

zydeck::library::Where Library::only() {
    zydeck::library::Where w;
    w.sql = notExcluded(&w.args);
    return w;
}

QString Library::notExcluded(QVariantList* pArgs) {
    loadExcluded();
    // Live stems files stand in for their tracks on the decks; the library shows the originals
    QStringList parts{QStringLiteral("tl.location NOT LIKE '%/ZyDeck Stems/%'")};
    for (const QString& dir : std::as_const(m_excluded)) {
        // a plain prefix compare: LIKE would treat _ and % in folder names as wildcards
        const QString prefix = dir + QLatin1Char('/');
        parts.append(QStringLiteral("substr(tl.location, 1, ?) <> ?"));
        pArgs->append(prefix.size());
        pArgs->append(prefix);
    }
    return parts.join(QStringLiteral(" AND "));
}

QJsonArray Library::excludedFolders() {
    loadExcluded();
    QJsonArray out;
    for (const QString& dir : std::as_const(m_excluded)) {
        out.append(dir);
    }
    return out;
}

QJsonObject Library::setExcluded(const QString& path, bool excluded) {
    loadExcluded();
    QString dir = QDir::cleanPath(path);
    if (dir.isEmpty() || dir == QLatin1String("/")) {
        return error(QStringLiteral("Pick a folder"));
    }
    if (excluded) {
        if (!m_excluded.contains(dir)) {
            m_excluded.append(dir);
            m_excluded.sort();
        }
    } else {
        m_excluded.removeAll(dir);
    }
    saveExcluded();
    return {{"ok", true}, {"excluded", excludedFolders()}};
}

// ---- analysis -----------------------------------------------------------------------------------------

/// Progress of an "Analyze all" run, updated from Mixxx's analysis (main thread), read by the hub.
struct Library::Analysis {
    QMutex mutex;
    QSet<int> pending;
    int total = 0;
    bool active = false;
    bool connected = false;
    QElapsedTimer timer;
};

QJsonObject Library::analyzeAll(bool dryRun) {
    if (!open()) {
        return error(QStringLiteral("Can't read the library"));
    }
    QVariantList args;
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral(
            "SELECT l.id FROM %1 WHERE l.mixxx_deleted = 0 AND tl.fs_deleted = 0 "
            "AND (l.bpm IS NULL OR l.bpm <= 0 OR l.key_id IS NULL OR l.key_id = 0) AND %2")
                      .arg(kTrackFrom, notExcluded(&args)));
    for (const QVariant& a : std::as_const(args)) {
        q.addBindValue(a);
    }
    if (!q.exec()) {
        return error(QStringLiteral("Can't read the library"));
    }
    QList<int> ids;
    while (q.next()) {
        ids.append(q.value(0).toInt());
    }
    if (dryRun || ids.isEmpty()) {
        return {{"ok", true}, {"count", ids.size()}};
    }
    return schedule(ids, false);
}

QJsonObject Library::analyzeTrack(int trackId, bool fresh) {
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    if (trackId <= 0 || !pLibrary) {
        return error(QStringLiteral("No such track"));
    }
    if (fresh) {
        TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
        QString problem = QStringLiteral("Mixxx is busy: try again");
        runOnMain(
                pLibrary,
                [pCollection, trackId, &problem] {
                    const TrackPointer pTrack = pCollection->getTrackById(TrackId(QVariant(trackId)));
                    if (!pTrack) {
                        problem = QStringLiteral("No such track");
                    } else if (pTrack->isBpmLocked()) {
                        problem = QStringLiteral("The beat grid is locked: unlock it in the beat grid editor first");
                    } else {
                        pTrack->trySetBeats(mixxx::BeatsPointer());
                        pTrack->resetKeys();
                        problem.clear();
                    }
                });
        if (!problem.isEmpty()) {
            return error(problem);
        }
    }
    return schedule({trackId}, true);
}

/// Hands ids to Mixxx's analysis. add: join an analysis that's already running instead of refusing.
QJsonObject Library::schedule(const QList<int>& ids, bool add) {
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    AnalysisFeature* pFeature = pLibrary ? pLibrary->findChild<AnalysisFeature*>() : nullptr;
    if (!pFeature) {
        return error(QStringLiteral("Mixxx isn't ready"));
    }
    if (!m_pAnalysis) {
        m_pAnalysis = std::make_shared<Analysis>();
    }
    std::shared_ptr<Analysis> a = m_pAnalysis;
    {
        QMutexLocker lock(&a->mutex);
        if (a->active && !add) {
            return error(QStringLiteral("Already analyzing"));
        }
        if (a->active) {
            for (int id : ids) {
                a->total += a->pending.contains(id) ? 0 : 1;
                a->pending.insert(id);
            }
        } else {
            a->pending = QSet<int>(ids.begin(), ids.end());
            a->total = ids.size();
            a->timer.start();
            a->active = true;
        }
    }
    QList<AnalyzerScheduledTrack> tracks;
    for (int id : std::as_const(ids)) {
        tracks.append(AnalyzerScheduledTrack(TrackId(QVariant(id))));
    }
    QMetaObject::invokeMethod(
            pLibrary,
            [pLibrary, pFeature, a, tracks] {
                if (!a->connected) {
                    a->connected = true;
                    QObject::connect(pFeature, &AnalysisFeature::trackProgress, pFeature, [a](TrackId id, AnalyzerProgress progress) {
                        if (progress >= kAnalyzerProgressDone) {
                            QMutexLocker lock(&a->mutex);
                            a->pending.remove(id.toVariant().toInt());
                        }
                    });
                    QObject::connect(pFeature, &AnalysisFeature::analysisActive, pFeature, [a](bool active) {
                        if (!active) {   // finished or stopped; what's still pending wasn't analyzed
                            QMutexLocker lock(&a->mutex);
                            a->active = false;
                        }
                    });
                }
                emit pLibrary->analyzeTracks(tracks);
            },
            Qt::QueuedConnection);
    return {{"ok", true}, {"count", ids.size()}};
}

QJsonObject Library::analysisStatus() {
    if (!m_pAnalysis) {
        return {{"active", false}};
    }
    QMutexLocker lock(&m_pAnalysis->mutex);
    const int done = m_pAnalysis->total - m_pAnalysis->pending.size();
    return {{"active", m_pAnalysis->active},
            {"total", m_pAnalysis->total},
            {"done", done},
            {"seconds", m_pAnalysis->active ? m_pAnalysis->timer.elapsed() / 1000.0 : 0.0}};
}

QJsonObject Library::removeTrack(int trackId, bool deleteFile) {
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    const QString path = location(trackId);
    if (!pLibrary || path.isEmpty()) {
        return error(QStringLiteral("No such track"));
    }
    for (const TrackPointer& pLoaded : PlayerInfo::instance().getLoadedTracks()) {
        if (pLoaded && pLoaded->getId().toVariant().toInt() == trackId) {
            return error(QStringLiteral("It's loaded on a deck or pad: eject it first"));
        }
    }
    TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
    bool ok = false;
    runOnMain(
            pLibrary,
            [pCollection, trackId, path, deleteFile, &ok] {
                if (deleteFile) {   // forgotten by the library (cues, grid, plays), then gone from the storage
                    pCollection->purgeTracks({TrackRef::fromFilePath(path, TrackId(QVariant(trackId)))});
                    ok = !QFileInfo::exists(path) || QFile::remove(path);
                } else {   // like Mixxx's "Hide": out of the library, back if its folder is added again
                    ok = pCollection->hideTracks({TrackId(QVariant(trackId))});
                }
            });
    if (!ok) {
        return error(deleteFile ? QStringLiteral("Couldn't delete the file (it may need All files access)")
                                : QStringLiteral("Couldn't remove it"));
    }
    return {{"ok", true}};
}

QJsonObject Library::trackCues(int trackId) {
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    if (!pLibrary || trackId <= 0) {
        return error(QStringLiteral("No such track"));
    }
    TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
    QJsonObject out;
    runOnMain(
            pLibrary,
            [pCollection, trackId, &out] {
                const TrackPointer pTrack = pCollection->getTrackById(TrackId(QVariant(trackId)));
                if (!pTrack) {
                    return;
                }
                const double rate = pTrack->getSampleRate().toDouble();
                QJsonArray cues;
                for (const CuePointer& pCue : pTrack->getCuePoints()) {
                    if (pCue->getType() != mixxx::CueType::HotCue || pCue->getHotCue() < 0) {
                        continue;
                    }
                    cues.append(QJsonObject{{"n", pCue->getHotCue() + 1},
                            {"seconds", rate > 0 ? pCue->getPosition().value() / rate : 0},
                            {"label", pCue->getLabel()},
                            {"color", QStringLiteral("#%1").arg(static_cast<uint>(pCue->getColor()), 6, 16, QLatin1Char('0'))}});
                }
                out = {{"ok", true}, {"title", pTrack->getTitle()}, {"duration", pTrack->getDuration()}, {"cues", cues}};
            });
    return out.isEmpty() ? error(QStringLiteral("No such track")) : out;
}

QJsonObject Library::editTrackCue(int trackId, int n, const QJsonObject& change) {
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    if (!pLibrary || trackId <= 0 || n < 1) {
        return error(QStringLiteral("No such hot cue"));
    }
    TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
    bool found = false;
    runOnMain(
            pLibrary,
            [pCollection, trackId, n, change, &found] {
                const TrackPointer pTrack = pCollection->getTrackById(TrackId(QVariant(trackId)));
                if (!pTrack) {
                    return;
                }
                for (const CuePointer& pCue : pTrack->getCuePoints()) {
                    if (pCue->getType() != mixxx::CueType::HotCue || pCue->getHotCue() != n - 1) {
                        continue;
                    }
                    found = true;
                    if (change.value(QStringLiteral("clear")).toBool()) {
                        pTrack->removeCue(pCue);
                    } else {
                        if (change.contains(QStringLiteral("label"))) {
                            pCue->setLabel(change.value(QStringLiteral("label")).toString().trimmed().left(60));
                        }
                        const QString color = change.value(QStringLiteral("color")).toString();
                        bool ok = false;
                        const uint rgb = QStringView(color).mid(color.startsWith(QLatin1Char('#')) ? 1 : 0).toUInt(&ok, 16);
                        if (ok && rgb <= 0xFFFFFF) {
                            pCue->setColor(mixxx::RgbColor(rgb));
                        }
                    }
                    pCollection->saveTrack(pTrack);
                    break;
                }
            });
    return found ? QJsonObject{{"ok", true}} : error(QStringLiteral("No such hot cue"));
}

QJsonObject Library::renameTrack(int trackId, const QString& title) {
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    const QString name = title.trimmed().left(200);
    if (!pLibrary || trackId <= 0 || name.isEmpty()) {
        return error(QStringLiteral("Give it a name"));
    }
    TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
    bool ok = false;
    runOnMain(
            pLibrary,
            [pCollection, trackId, name, &ok] {
                const TrackPointer pTrack = pCollection->getTrackById(TrackId(QVariant(trackId)));
                if (pTrack) {
                    pTrack->setTitle(name);
                    ok = pCollection->saveTrack(pTrack) != TrackCollectionManager::SaveTrackResult::Failed;
                }
            });
    return ok ? QJsonObject{{"ok", true}} : error(QStringLiteral("Couldn't rename it"));
}

QJsonObject Library::saveLoaded(const QString& group) {
    const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(group);
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    if (!pTrack || !pLibrary) {
        return error(QStringLiteral("Nothing loaded there"));
    }
    TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
    bool saved = false;
    runOnMain(
            pLibrary,
            [pCollection, pTrack, &saved] {
                saved = pCollection->saveTrack(pTrack) != TrackCollectionManager::SaveTrackResult::Failed;
            });
    return saved ? QJsonObject{{"ok", true}, {"bpm", pTrack->getBpm()}} : error(QStringLiteral("Couldn't save the track"));
}

QJsonObject Library::analyzeRange(const TrackPointer& pTrack, double start, double end, const std::atomic<bool>* pCancel) {
    if (!pTrack) {
        return error(QStringLiteral("Nothing loaded there"));
    }
    if (pTrack->isBpmLocked()) {
        return error(QStringLiteral("The grid is locked: unlock it first"));
    }
    if (!(end - start >= kRangeMinSeconds)) {
        return error(QStringLiteral("Pick at least %1 seconds").arg(kRangeMinSeconds));
    }
    mixxx::AudioSource::OpenParams params;
    params.setChannelCount(mixxx::audio::ChannelCount::stereo());   // (a stems file: its mix)
    const mixxx::AudioSourcePointer pSource = SoundSourceProxy(pTrack).openAudioSource(params);
    if (!pSource) {
        return error(QStringLiteral("Couldn't read the track"));
    }
    const mixxx::audio::SampleRate rate = pSource->getSignalInfo().getSampleRate();
    if (pSource->getSignalInfo().getChannelCount() != mixxx::audio::ChannelCount::stereo()) {
        return error(QStringLiteral("Couldn't read the track in stereo"));
    }
    const mixxx::IndexRange frames = pSource->frameIndexRange();
    const SINT first = std::clamp(static_cast<SINT>(start * rate.toDouble()), frames.start(), frames.end());
    const SINT last = std::clamp(static_cast<SINT>(end * rate.toDouble()), first, frames.end());
    mixxx::AnalyzerQueenMaryBeats detector;   // Mixxx's own beat tracker, on that stretch alone
    if (last - first < kRangeMinSeconds * rate.toDouble() || !detector.initialize(rate)) {
        return error(QStringLiteral("That stretch is too short"));
    }
    constexpr SINT kChunk = 65536;   // frames read and fed at a time (the stretch may be the whole track)
    mixxx::SampleBuffer buffer(kChunk * 2);
    for (SINT at = first; at < last;) {
        if (pCancel && pCancel->load()) {
            return error(QStringLiteral("Cancelled"));
        }
        const mixxx::ReadableSampleFrames read = pSource->readSampleFrames(mixxx::WritableSampleFrames(
                mixxx::IndexRange::between(at, std::min(at + kChunk, last)), mixxx::SampleBuffer::WritableSlice(buffer)));
        if (read.readableLength() <= 0) {
            break;
        }
        detector.processSamples(read.readableData(), read.readableLength());
        at = read.frameIndexRange().end();
    }
    if (!detector.finalize()) {
        return error(QStringLiteral("Couldn't find the beat there"));
    }
    // its beats are counted from the stretch's start: a steady grid through them, as Mixxx makes one
    QVector<mixxx::audio::FramePos> beats = detector.getBeats();
    for (mixxx::audio::FramePos& b : beats) {
        b += static_cast<double>(first);
    }
    const QVector<BeatUtils::ConstRegion> regions = BeatUtils::retrieveConstRegions(beats, rate);
    mixxx::audio::FramePos firstBeat = mixxx::audio::kStartFramePos;
    const mixxx::Bpm bpm = regions.isEmpty() ? mixxx::Bpm() : BeatUtils::makeConstBpm(regions, rate, &firstBeat);
    if (beats.size() < 8 || !bpm.isValid() || !firstBeat.isValid()) {
        return error(QStringLiteral("Couldn't find a steady beat there"));
    }
    firstBeat = BeatUtils::adjustPhase(firstBeat, bpm, rate, beats);
    // anchored there, and from the track's start on (whole beats back from the stretch)
    const double beatLength = 60.0 * rate.toDouble() / bpm.value();
    firstBeat = mixxx::audio::FramePos(firstBeat.value() - std::floor(firstBeat.value() / beatLength) * beatLength);
    const mixxx::BeatsPointer pBeats = mixxx::Beats::fromConstTempo(rate, firstBeat.toNearestFrameBoundary(), bpm);
    if (pCancel && pCancel->load()) {   // (the editor closed meanwhile: its grid stays as it left it)
        return error(QStringLiteral("Cancelled"));
    }
    if (!pBeats || !pTrack->trySetBeats(pBeats)) {
        return error(QStringLiteral("Couldn't set the grid"));
    }
    return {{"ok", true}, {"bpm", pTrack->getBpm()}};
}

void Library::stopAnalysis() {
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    AnalysisFeature* pFeature = pLibrary ? pLibrary->findChild<AnalysisFeature*>() : nullptr;
    if (pFeature) {
        QMetaObject::invokeMethod(pFeature, &AnalysisFeature::stopAnalysis, Qt::QueuedConnection);
    }
}

} // namespace zydek

namespace zydek {

// ---- sampler capture ------------------------------------------------------------------------------------

namespace {

/// Where ZyDeck keeps its samples: Music/ZyDeck Samples on the shared storage when it can write there.
QString samplesFolder() {
    const QString shared = QStringLiteral("/storage/emulated/0/Music");
    const QString music = QFileInfo(shared).isWritable() ? shared : QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    const QString dir = music + QStringLiteral("/ZyDeck Samples");
    QDir().mkpath(dir);
    return dir;
}

using zydeck::shared::safeFileName;   // (ZyDeck Connect names its files the same way)

/// A 32-bit float WAV.
bool writeWav(const QString& path, const CSAMPLE* data, SINT samples, int channels, int sampleRate) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    const quint32 dataBytes = static_cast<quint32>(samples * sizeof(float));
    QByteArray h;
    const auto u32 = [&h](quint32 v) { for (int i = 0; i < 4; ++i) h.append(static_cast<char>(v >> (8 * i))); };
    const auto u16 = [&h](quint16 v) { h.append(static_cast<char>(v)); h.append(static_cast<char>(v >> 8)); };
    h.append("RIFF"); u32(36 + dataBytes); h.append("WAVE");
    h.append("fmt "); u32(16); u16(3 /* IEEE float */); u16(static_cast<quint16>(channels)); u32(static_cast<quint32>(sampleRate));
    u32(static_cast<quint32>(sampleRate * channels * 4)); u16(static_cast<quint16>(channels * 4)); u16(32);
    h.append("data"); u32(dataBytes);
    f.write(h);
    f.write(reinterpret_cast<const char*>(data), dataBytes);
    return f.error() == QFileDevice::NoError;
}

} // namespace

void Library::setSamplerOptions(const QString& group, bool sync, bool repeat, bool keylock) {
    const auto apply = [group, sync, repeat, keylock] {
        ControlObject::set(ConfigKey(group, QStringLiteral("repeat")), repeat ? 1 : 0);
        ControlObject::set(ConfigKey(group, QStringLiteral("keylock")), keylock ? 1 : 0);
        ControlObject::set(ConfigKey(group, QStringLiteral("quantize")), 1);
        ControlObject::set(ConfigKey(group, QStringLiteral("sync_enabled")), sync ? 1 : 0);
    };
    apply();
    QTimer::singleShot(1500, apply);   // again once the clip has loaded (loading resets some of them)
}

QJsonObject Library::captureSample(int deck, int slot, bool sync, bool keylock) {
    if (deck < 1 || deck > 4 || slot < 1 || slot > kMaxSamplers) {
        return error(QStringLiteral("No such deck or sampler"));
    }
    const QString group = QStringLiteral("[Channel%1]").arg(deck);
    const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(group);
    if (!pTrack) {
        return error(QStringLiteral("Deck %1 is empty").arg(deck));
    }
    // Mixxx's positions count samples of the stereo signal: two per frame
    const double loopIn = control(group, QStringLiteral("loop_start_position"));
    const double loopOut = control(group, QStringLiteral("loop_end_position"));
    if (loopIn < 0 || loopOut <= loopIn) {
        return error(QStringLiteral("Set a loop on deck %1 first").arg(deck));
    }
    // the stems this deck plays
    uint mask = 0;
    const int stemCount = static_cast<int>(control(group, QStringLiteral("stem_count")));
    for (int s = 0; s < stemCount && s < 4; ++s) {
        const QString sg = QStringLiteral("[Channel%1_Stem%2]").arg(deck).arg(s + 1);
        if (control(sg, QStringLiteral("mute")) == 0 && control(sg, QStringLiteral("volume")) > 0) {
            mask |= 1u << s;
        }
    }
    if (stemCount > 0 && !mask) {
        return error(QStringLiteral("Every stem on deck %1 is muted").arg(deck));
    }
    if (stemCount > 0 && mask == (1u << stemCount) - 1) {
        mask = 0;   // all of them: just the track
    }
    return renderSample(pTrack, loopIn / 2, loopOut / 2, mask, slot, sync, keylock);
}

QJsonObject Library::cutSample(const QString& group, double start, double end, uint stemMask, int slot, bool sync, bool keylock) {
    if (slot < 1 || slot > kMaxSamplers) {
        return error(QStringLiteral("No such sampler"));
    }
    const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(group);
    if (!pTrack) {
        return error(QStringLiteral("Nothing is loaded to cut from"));
    }
    const double rate = pTrack->getSampleRate().toDouble();
    if (!(end > start) || start < 0 || rate <= 0) {
        return error(QStringLiteral("Pick a stretch of the track first"));
    }
    return renderSample(pTrack, start * rate, end * rate, stemMask & 15, slot, sync, keylock);
}

QJsonObject Library::renderSample(const TrackPointer& pTrack, double firstFrame, double lastFrame, uint mask, int slot, bool sync, bool keylock) {
    QStringList stems;
    if (mask) {
        const QList<StemInfo> info = pTrack->getStemInfo();
        for (int s = 0; s < 4; ++s) {
            if (mask & (1u << s)) {
                stems.append(s < info.size() && !info[s].getLabel().isEmpty() ? info[s].getLabel() : QStringLiteral("Stem %1").arg(s + 1));
            }
        }
    }
    mixxx::AudioSource::OpenParams params;
    params.setChannelCount(mixxx::audio::ChannelCount::stereo());
#ifdef __STEM__
    if (mask) {
        params.setStemMask(mixxx::StemChannelSelection::fromInt(mask));
    }
#endif
    const mixxx::AudioSourcePointer pSource = SoundSourceProxy(pTrack).openAudioSource(params);
    if (!pSource) {
        return error(QStringLiteral("Couldn't read the track"));
    }
    const int channels = pSource->getSignalInfo().getChannelCount();
    const int rate = pSource->getSignalInfo().getSampleRate();
    // A sample is read into memory whole (8 bytes a frame): two minutes at most (~45 MB at 48 kHz), and
    // only what the track has.
    const mixxx::IndexRange frames = pSource->frameIndexRange();
    const SINT first = std::clamp(static_cast<SINT>(firstFrame), frames.start(), frames.end());
    const SINT last = std::clamp(static_cast<SINT>(lastFrame), first, std::min<SINT>(frames.end(), first + 120 * rate));
    if (last <= first) {
        return error(QStringLiteral("Pick a stretch of the track first"));
    }
    mixxx::SampleBuffer buffer((last - first) * channels);
    const mixxx::ReadableSampleFrames read = pSource->readSampleFrames(mixxx::WritableSampleFrames(
            mixxx::IndexRange::between(first, last), mixxx::SampleBuffer::WritableSlice(buffer)));
    if (read.readableLength() <= 0) {
        return error(QStringLiteral("Couldn't read the loop"));
    }

    const double bpm = pTrack->getBpm();
    const double seconds = static_cast<double>(read.frameIndexRange().length()) / rate;
    const double beats = bpm > 0 ? seconds * bpm / 60 : 0;
    const QString beatsText = beats > 0 ? QStringLiteral("%1 beats").arg(beats >= 1 ? QString::number(std::round(beats * 100) / 100)
                                                                                    : QStringLiteral("1/%1").arg(std::round(1 / beats)))
                                        : QStringLiteral("%1 s").arg(QString::number(seconds, 'f', 1));
    const QString title = pTrack->getTitle().isEmpty() ? QFileInfo(pTrack->getLocation()).completeBaseName() : pTrack->getTitle();
    const QString what = beatsText + (stems.isEmpty() ? QString() : QStringLiteral(", ") + stems.join(QStringLiteral(" + ")));
    const QString base = safeFileName((pTrack->getArtist().isEmpty() ? QString() : pTrack->getArtist() + QStringLiteral(" - ")) +
            title + QStringLiteral(" (") + what + QStringLiteral(")"));
    const QString dir = samplesFolder();
    QString path = dir + QLatin1Char('/') + base + QStringLiteral(".wav");
    for (int n = 2; QFileInfo::exists(path); ++n) {
        path = dir + QLatin1Char('/') + base + QStringLiteral(" %1.wav").arg(n);
    }
    if (!writeWav(path, read.readableData(), read.readableLength(), channels, rate)) {
        return error(QStringLiteral("Couldn't save the sample"));
    }

    // into the library, with the source's tempo (a beat grid from its start) so it can follow the master
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    if (!pLibrary) {
        return error(QStringLiteral("Mixxx isn't ready"));
    }
    TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
    const QString artist = pTrack->getArtist(), sampleTitle = title + QStringLiteral(" · ") + what;
    const QString comment = QStringLiteral("ZyDeck sample of %1, %2–%3 s").arg(pTrack->getLocation())
            .arg(first / static_cast<double>(rate), 0, 'f', 2).arg(last / static_cast<double>(rate), 0, 'f', 2);
    bool added = false;
    runOnMain(
            pCollection,
            [pCollection, path, artist, sampleTitle, comment, bpm, &added] {
                const TrackPointer pSample = pCollection->getOrAddTrack(TrackRef::fromFilePath(path));
                if (!pSample) {
                    return;
                }
                pSample->setArtist(artist);
                pSample->setTitle(sampleTitle);
                pSample->setComment(comment);
                if (bpm > 0) {
                    pSample->trySetBpm(bpm);
                    pSample->setBpmLocked(true);
                }
                added = true;
            });
    if (!added) {
        return error(QStringLiteral("Mixxx couldn't add the sample"));
    }
    const QJsonObject loaded = loadLocation(path, QStringLiteral("sampler%1").arg(slot));
    if (!loaded.value(QStringLiteral("ok")).toBool()) {
        return loaded;
    }
    setSamplerOptions(QStringLiteral("[Sampler%1]").arg(slot), sync, true, keylock);   // a captured loop loops
    QJsonArray stemList;
    for (const QString& s : std::as_const(stems)) {
        stemList.append(s);
    }
    return {{"ok", true}, {"slot", slot}, {"path", path}, {"name", sampleTitle}, {"beats", beats}, {"stems", stemList}};
}

} // namespace zydek
