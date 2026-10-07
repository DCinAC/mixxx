#include "zydek/zydeklibrary.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QStringList>

#include "control/controlobject.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "mixer/playermanager.h"
#include "qml/qmllibraryproxy.h"
#include "qml/qmlplayermanagerproxy.h"
#include "track/track.h"
#include "util/fileinfo.h"

namespace zydek {

namespace {

const QString kConnection = QStringLiteral("zydek-library");
const QString kTrackColumns = QStringLiteral(
        "l.id, l.artist, l.title, l.album, l.genre, l.duration, l.bpm, l.key, l.rating, l.color, "
        "l.timesplayed, l.datetime_added, l.filetype, l.bitrate, tl.location, tl.fs_deleted");
const QString kTrackFrom = QStringLiteral("library l JOIN track_locations tl ON tl.id = l.location");
const QString kStemsWhere = QStringLiteral(
        "(lower(tl.location) LIKE '%.stem.mp4' OR lower(tl.location) LIKE '%.stem.m4a')");
// Mixxx's hidden playlists: 1 = Auto DJ queue, 2 = set history.
constexpr int kAutoDj = 1;
constexpr int kHistory = 2;
constexpr int kMaxSamplers = 64;

const QStringList kAudioSuffixes = {"mp3", "flac", "wav", "aiff", "aif", "m4a", "mp4", "ogg", "opus", "wv", "alac"};

QString orderFor(const QString& sort) {
    if (sort == QLatin1String("artist")) {
        return QStringLiteral("l.artist COLLATE NOCASE, l.title COLLATE NOCASE");
    }
    if (sort == QLatin1String("title")) {
        return QStringLiteral("l.title COLLATE NOCASE, l.artist COLLATE NOCASE");
    }
    if (sort == QLatin1String("bpm")) {
        return QStringLiteral("l.bpm, l.artist COLLATE NOCASE");
    }
    if (sort == QLatin1String("played")) {
        return QStringLiteral("l.timesplayed DESC, l.datetime_added DESC");
    }
    return QStringLiteral("l.datetime_added DESC, l.id DESC");
}

double control(const QString& group, const QString& key) {
    return ControlObject::get(ConfigKey(group, key));
}

QString groupFor(const QString& target, QString* pError) {
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

QJsonObject Library::trackJson(const QSqlQuery& q) const {
    const QString location = q.value(14).toString();
    const double bpm = q.value(6).toDouble();
    return {
            {"id", q.value(0).toInt()},
            {"artist", q.value(1).toString()},
            {"title", q.value(2).toString().isEmpty() ? QFileInfo(location).completeBaseName() : q.value(2).toString()},
            {"album", q.value(3).toString()},
            {"genre", q.value(4).toString()},
            {"duration", q.value(5).toDouble()},
            {"bpm", bpm > 0 ? QJsonValue(std::round(bpm * 10) / 10) : QJsonValue()},
            {"key", q.value(7).toString()},
            {"rating", q.value(8).toInt()},
            {"color", q.value(9).isNull() ? QJsonValue() : QJsonValue(q.value(9).toLongLong())},
            {"played", q.value(10).toInt()},
            {"added", q.value(11).toString()},
            {"type", q.value(12).toString().toLower()},
            {"bitrate", q.value(13).toInt()},
            {"stems", location.endsWith(QLatin1String(".stem.mp4"), Qt::CaseInsensitive) ||
                            location.endsWith(QLatin1String(".stem.m4a"), Qt::CaseInsensitive)},
            // fs_deleted: Mixxx's last scan didn't find it; exists(): e.g. the SD card isn't there now
            {"missing", q.value(15).toBool() || !QFileInfo::exists(location)},
    };
}

QJsonObject Library::views() {
    if (!open()) {
        return {{"total", 0}, {"stems", 0}, {"crates", QJsonArray()}, {"playlists", QJsonArray()}, {"history", QJsonArray()}};
    }
    QSqlQuery q(m_db);
    q.exec(QStringLiteral("SELECT count(*) FROM %1 WHERE l.mixxx_deleted = 0").arg(kTrackFrom));
    const int total = q.next() ? q.value(0).toInt() : 0;
    q.exec(QStringLiteral("SELECT count(*) FROM %1 WHERE l.mixxx_deleted = 0 AND %2").arg(kTrackFrom, kStemsWhere));
    const int stems = q.next() ? q.value(0).toInt() : 0;

    QJsonArray crates;
    q.exec(QStringLiteral(
            "SELECT c.id, c.name, count(ct.track_id) FROM crates c LEFT JOIN crate_tracks ct ON ct.crate_id = c.id "
            "WHERE c.show = 1 GROUP BY c.id ORDER BY c.name COLLATE NOCASE"));
    while (q.next()) {
        crates.append(QJsonObject{{"id", q.value(0).toInt()}, {"name", q.value(1).toString()}, {"count", q.value(2).toInt()}});
    }
    QJsonArray playlists, history;
    QJsonValue autodj;
    q.exec(QStringLiteral(
            "SELECT p.id, p.name, count(pt.id), p.hidden FROM Playlists p LEFT JOIN PlaylistTracks pt ON pt.playlist_id = p.id "
            "WHERE p.hidden IN (0, %1, %2) GROUP BY p.id ORDER BY p.hidden, p.position, p.id DESC")
                    .arg(kAutoDj)
                    .arg(kHistory));
    while (q.next()) {
        const QJsonObject p{{"id", q.value(0).toInt()}, {"name", q.value(1).toString()}, {"count", q.value(2).toInt()}};
        const int hidden = q.value(3).toInt();
        if (hidden == 0) {
            playlists.append(p);
        } else if (hidden == kAutoDj) {
            autodj = p;
        } else if (p.value(QStringLiteral("count")).toInt() > 0 && history.size() < 20) {
            history.append(p);
        }
    }
    return {{"total", total}, {"stems", stems}, {"crates", crates}, {"playlists", playlists}, {"autodj", autodj}, {"history", history}};
}

QJsonArray Library::tracks(const QString& search, const QString& view, const QString& sort, int limit, int offset) {
    QJsonArray out;
    if (!open()) {
        return out;
    }
    QStringList where{QStringLiteral("l.mixxx_deleted = 0")};
    QVariantList args;
    QString join;
    QString order = orderFor(sort);
    const QString kind = view.section(QLatin1Char(':'), 0, 0);
    const int viewId = view.section(QLatin1Char(':'), 1).toInt();
    if (kind == QLatin1String("stems")) {
        where.append(kStemsWhere);
    } else if (kind == QLatin1String("crate") && viewId > 0) {
        join = QStringLiteral("JOIN crate_tracks ct ON ct.track_id = l.id");
        where.append(QStringLiteral("ct.crate_id = ?"));
        args.append(viewId);
    } else if (kind == QLatin1String("playlist") && viewId > 0) {
        join = QStringLiteral("JOIN PlaylistTracks pt ON pt.track_id = l.id");
        where.append(QStringLiteral("pt.playlist_id = ?"));
        args.append(viewId);
        if (sort == QLatin1String("added")) {
            order = QStringLiteral("pt.position");   // a playlist's own order
        }
    }
    const QStringList words = search.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& word : words) {
        where.append(QStringLiteral(
                "(l.artist LIKE ? OR l.title LIKE ? OR l.album LIKE ? OR l.genre LIKE ? OR l.album_artist LIKE ?)"));
        for (int i = 0; i < 5; ++i) {
            args.append(QString(QStringLiteral("%") + word + QStringLiteral("%")));
        }
    }
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM %2 %3 WHERE %4 ORDER BY %5 LIMIT ? OFFSET ?")
                      .arg(kTrackColumns, kTrackFrom, join, where.join(QStringLiteral(" AND ")), order));
    for (const QVariant& a : std::as_const(args)) {
        q.addBindValue(a);
    }
    q.addBindValue(std::clamp(limit, 1, 500));
    q.addBindValue(std::max(0, offset));
    if (!q.exec()) {
        qWarning() << "Zydek: library query failed:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        out.append(trackJson(q));
    }
    return out;
}

QJsonObject Library::track(int trackId) {
    if (!open()) {
        return {};
    }
    QSqlQuery q(m_db);
    q.prepare(QStringLiteral("SELECT %1 FROM %2 WHERE l.id = ?").arg(kTrackColumns, kTrackFrom));
    q.addBindValue(trackId);
    if (!q.exec() || !q.next()) {
        return {};
    }
    QJsonObject t = trackJson(q);
    t.insert(QStringLiteral("location"), q.value(14).toString());
    return t;
}

QString Library::location(int trackId) {
    return track(trackId).value(QStringLiteral("location")).toString();
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

QJsonObject Library::loadLocation(const QString& location, const QString& target) {
    QString err;
    const QString group = groupFor(target, &err);
    if (group.isEmpty()) {
        return error(err);
    }
    if (group.startsWith(QLatin1String("[Channel")) && control(group, QStringLiteral("play")) > 0) {
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

QJsonObject Library::addFolder(const QString& path) {
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    if (!pLibrary) {
        return error(QStringLiteral("Mixxx isn't ready"));
    }
    TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
    DirectoryDAO::AddResult result = DirectoryDAO::AddResult::SqlError;
    // The collection belongs to the main thread; wait for it there (adding a folder is quick).
    QMetaObject::invokeMethod(
            pCollection,
            [pCollection, path, &result] { result = pCollection->addDirectory(mixxx::FileInfo(path)); },
            Qt::BlockingQueuedConnection);
    switch (result) {
    case DirectoryDAO::AddResult::Ok:
        startScan();
        return {{"ok", true}};
    case DirectoryDAO::AddResult::AlreadyWatching:
        return error(QStringLiteral("This folder (or one containing it) is already in the library"));
    case DirectoryDAO::AddResult::InvalidOrMissingDirectory:
        return error(QStringLiteral("That folder doesn't exist"));
    case DirectoryDAO::AddResult::UnreadableDirectory:
        return error(QStringLiteral("Can't read that folder: allow Zydek (Mixxx) \"All files access\" in Android's settings"));
    default:
        return error(QStringLiteral("Couldn't add the folder"));
    }
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
