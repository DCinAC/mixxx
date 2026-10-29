#include "zydecklibraryquery.h"

#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QtDebug>
#include <algorithm>
#include <cmath>

namespace zydeck::library {

const QString kTrackColumns = QStringLiteral(
        "l.id, l.artist, l.title, l.album, l.genre, l.duration, l.bpm, l.key, l.rating, l.color, "
        "l.timesplayed, l.datetime_added, l.filetype, l.bitrate, tl.location, tl.fs_deleted, l.key_id");
const QString kTrackFrom = QStringLiteral("library l JOIN track_locations tl ON tl.id = l.location");
const QString kStemsWhere = QStringLiteral(
        "(lower(tl.location) LIKE '%.stem.mp4' OR lower(tl.location) LIKE '%.stem.m4a')");
// Mixxx's hidden playlists: 1 = Auto DJ queue, 2 = set history.
constexpr int kAutoDj = 1;
constexpr int kHistory = 2;

namespace {

// Camelot (number 1-12, minor = A) of Mixxx's key ids: 1-12 C..B major, 13-24 C..B minor
std::pair<int, bool> camelot(int key) {
    const bool minor = key > 12;
    const int p = (key - 1) % 12;
    const int open = ((minor ? p + 3 : p) * 7) % 12 + 1;   // Open Key: C major = 1d
    return {(open + 6) % 12 + 1, minor};                     // Camelot: C major = 8B, A minor = 8A
}
// Keys that mix with this one: the same, a step round the wheel either way, and its relative major/minor
QList<int> compatibleKeys(int key) {
    QList<int> out;
    if (key < 1 || key > 24) {
        return out;
    }
    const auto [n, minor] = camelot(key);
    for (int other = 1; other <= 24; ++other) {
        const auto [m, otherMinor] = camelot(other);
        const int step = (m - n + 12) % 12;
        if ((otherMinor == minor && (step == 0 || step == 1 || step == 11)) || (otherMinor != minor && step == 0)) {
            out.append(other);
        }
    }
    return out;
}

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
    if (sort == QLatin1String("key")) {
        // Camelot wheel order (1A 1B 2A 2B ... 12B), so harmonically close keys sit together; Mixxx's key_id
        // is its ChromaticKey (1-12 C..B major, 13-24 C..B minor). Unknown keys last.
        return QStringLiteral(
                "CASE l.key_id "
                "WHEN 21 THEN 1 WHEN 12 THEN 2 WHEN 16 THEN 3 WHEN 7 THEN 4 WHEN 23 THEN 5 WHEN 2 THEN 6 "
                "WHEN 18 THEN 7 WHEN 9 THEN 8 WHEN 13 THEN 9 WHEN 4 THEN 10 WHEN 20 THEN 11 WHEN 11 THEN 12 "
                "WHEN 15 THEN 13 WHEN 6 THEN 14 WHEN 22 THEN 15 WHEN 1 THEN 16 WHEN 17 THEN 17 WHEN 8 THEN 18 "
                "WHEN 24 THEN 19 WHEN 3 THEN 20 WHEN 19 THEN 21 WHEN 10 THEN 22 WHEN 14 THEN 23 WHEN 5 THEN 24 "
                "ELSE 99 END, l.bpm, l.artist COLLATE NOCASE");
    }
    if (sort == QLatin1String("played")) {
        return QStringLiteral("l.timesplayed DESC, l.datetime_added DESC");
    }
    return QStringLiteral("l.datetime_added DESC, l.id DESC");
}

QList<QPair<QString, int>> genreFields(const QSqlDatabase& db, const Where& only) {
    QList<QPair<QString, int>> out;
    const QVariantList args = only.args;
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT l.genre, count(*) FROM %1 WHERE l.mixxx_deleted = 0 AND %2 GROUP BY l.genre")
                      .arg(kTrackFrom, only.sql));
    for (const QVariant& a : std::as_const(args)) {
        q.addBindValue(a);
    }
    if (!q.exec()) {
        qWarning() << "Zydek: genre query failed:" << q.lastError().text();
        return out;
    }
    while (q.next()) {
        out.append({q.value(0).toString(), q.value(1).toInt()});
    }
    return out;
}

} // namespace

// A track's genres from its genre field: "House; Tech House", "Disco, Funk", "Pop / Rock", "Classical/Trance" (; , |
// and /), except the store genres that are one name with a / in it ("Hip-Hop/Rap": as phone.html's splitGenres)
QStringList splitGenres(const QString& field) {
    static const QRegularExpression oneName(
            QStringLiteral(R"((hip[ -]?hop|r&b|singer)\s*/\s*(rap|soul|songwriter))"),
            QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression separator(QStringLiteral(R"(\s*[;,|/]\s*)"));
    const QChar keep(0x2215);   // (∕, kept apart from a separator while splitting)
    QString f = field;
    f.replace(oneName, QStringLiteral("\\1") + keep + QStringLiteral("\\2"));
    QStringList out;
    for (QString g : f.split(separator, Qt::SkipEmptyParts)) {
        g = g.simplified().replace(keep, QLatin1Char('/'));
        if (!g.isEmpty()) {
            out.append(g);
        }
    }
    return out;
}

// What makes two genre names one genre: case, spaces, hyphens and dots aside ("J-Trance", "jtrance", "J Trance")
QString genreKey(const QString& name) {
    static const QRegularExpression ignored(QStringLiteral(R"([\s\-_.'’])"));
    return name.toLower().remove(ignored);
}

QJsonObject trackJson(const QSqlQuery& q) {
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
            {"keyId", q.value(16).toInt()},   // Mixxx's ChromaticKey: 1-12 C..B major, 13-24 minor, 0 none
            {"rating", q.value(8).toInt()},
            {"color", q.value(9).isNull() ? QJsonValue() : QJsonValue(q.value(9).toLongLong())},
            {"played", q.value(10).toInt()},
            {"added", q.value(11).toString()},
            {"type", q.value(12).toString().toLower()},
            {"bitrate", q.value(13).toInt()},
            {"stems", location.endsWith(QLatin1String(".stem.mp4"), Qt::CaseInsensitive) ||
                            location.endsWith(QLatin1String(".stem.m4a"), Qt::CaseInsensitive) ||
                            location.endsWith(QLatin1String(".stem.m4a"), Qt::CaseInsensitive)},
            // fs_deleted: Mixxx's last scan didn't find it; exists(): e.g. the SD card isn't there now
            {"missing", q.value(15).toBool() || !QFileInfo::exists(location)},
    };
}

QJsonObject views(const QSqlDatabase& db, const Where& only) {
    QSqlQuery q(db);
    const auto count = [&q, &only](const QString& where) {
        const QVariantList args = only.args;
        q.prepare(QStringLiteral("SELECT count(*) FROM %1 WHERE l.mixxx_deleted = 0 AND %2 AND %3")
                          .arg(kTrackFrom, where, only.sql));
        for (const QVariant& a : std::as_const(args)) {
            q.addBindValue(a);
        }
        return q.exec() && q.next() ? q.value(0).toInt() : 0;
    };
    const int total = count(QStringLiteral("1"));
    const int stems = count(kStemsWhere);
    const int recent = count(QStringLiteral("l.last_played_at IS NOT NULL"));

    QJsonArray crates;
    q.exec(QStringLiteral(
            "SELECT c.id, c.name, count(ct.track_id), c.autodj_source FROM crates c LEFT JOIN crate_tracks ct ON ct.crate_id = c.id "
            "WHERE c.show = 1 GROUP BY c.id ORDER BY c.name COLLATE NOCASE"));
    while (q.next()) {   // (source: one of the crates Auto DJ takes random tracks from)
        crates.append(QJsonObject{{"id", q.value(0).toInt()}, {"name", q.value(1).toString()}, {"count", q.value(2).toInt()},
                {"source", q.value(3).toInt() == 1}});
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
    return {{"total", total}, {"stems", stems}, {"recent", recent}, {"crates", crates}, {"playlists", playlists}, {"autodj", autodj}, {"history", history}};
}

QJsonObject genres(const QSqlDatabase& db, const Where& only) {
    struct Genre {   // (its spellings: the most used one is shown)
        int count = 0;
        QHash<QString, int> spellings;
    };
    QHash<QString, Genre> found;   // by its genreKey
    int none = 0;
    for (const auto& [field, n] : genreFields(db, only)) {
        const QStringList names = splitGenres(field);
        if (names.isEmpty()) {
            none += n;
            continue;
        }
        QSet<QString> seen;   // ("House; house": once)
        for (const QString& name : std::as_const(names)) {
            const QString k = genreKey(name);
            if (seen.contains(k)) {
                continue;
            }
            seen.insert(k);
            Genre& g = found[k];
            g.count += n;
            g.spellings[name] += n;
        }
    }
    QList<QPair<QString, int>> list;
    for (auto it = found.cbegin(); it != found.cend(); ++it) {
        QString shown;
        int most = -1;
        for (auto s = it->spellings.cbegin(); s != it->spellings.cend(); ++s) {
            if (s.value() > most || (s.value() == most && s.key() < shown)) {
                shown = s.key();
                most = s.value();
            }
        }
        list.append({shown, it->count});
    }
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first.compare(b.first, Qt::CaseInsensitive) < 0;
    });
    QJsonArray out;
    for (const auto& [name, count] : std::as_const(list)) {
        out.append(QJsonObject{{"name", name}, {"count", count}});
    }
    return {{"genres", out}, {"none", none}};
}

QJsonArray tracks(const QSqlDatabase& db, const Where& only, const QString& search, const QString& view, const QString& sort,
        int limit, int offset, const QString& genre, const QList<int>& loaded) {
    QJsonArray out;
    QStringList where{QStringLiteral("l.mixxx_deleted = 0")};
    QVariantList args;
    QString join;
    QString order = orderFor(sort);
    if (sort == QLatin1String("loaded")) {
        if (!loaded.isEmpty()) {
            QString rank = QStringLiteral("CASE l.id");
            for (int i = 0; i < loaded.size(); ++i) {
                rank += QStringLiteral(" WHEN %1 THEN %2").arg(loaded[i]).arg(i);
            }
            order = rank + QStringLiteral(" ELSE 1000000 END, l.datetime_added DESC, l.id DESC");
        }
    }
    const QString kind = view.section(QLatin1Char(':'), 0, 0);
    const int viewId = view.section(QLatin1Char(':'), 1).toInt();
    if (kind == QLatin1String("stems")) {
        where.append(kStemsWhere);
    } else if (kind == QLatin1String("recent")) {   // played on a deck, newest first
        where.append(QStringLiteral("l.last_played_at IS NOT NULL"));
        if (sort == QLatin1String("added")) {
            order = QStringLiteral("l.last_played_at DESC");
        }
    } else if (kind == QLatin1String("fits")) {   // fits:<key id>:<bpm>:<track to leave out> (the hub's, for a deck)
        const QStringList f = view.split(QLatin1Char(':'));
        const int key = f.value(1).toInt();
        const double bpm = f.value(2).toDouble();
        if (bpm > 0) {   // within 6 %, or at half or double time
            where.append(QStringLiteral("(l.bpm BETWEEN ? AND ? OR l.bpm BETWEEN ? AND ? OR l.bpm BETWEEN ? AND ?)"));
            for (const double k : {1.0, 2.0, 0.5}) {
                args.append(bpm * k * 0.94);
                args.append(bpm * k * 1.06);
            }
        }
        const QList<int> keys = compatibleKeys(key);
        if (!keys.isEmpty()) {
            QStringList marks;
            for (int k : keys) {
                marks.append(QStringLiteral("?"));
                args.append(k);
            }
            where.append(QStringLiteral("l.key_id IN (%1)").arg(marks.join(QLatin1Char(','))));
        }
        where.append(QStringLiteral("l.id <> ?"));
        args.append(f.value(3).toInt());
        if (sort == QLatin1String("added")) {   // the same key first, then the nearest tempo
            order = QStringLiteral("CASE WHEN l.key_id = %1 THEN 0 ELSE 1 END, ABS(l.bpm - %2), l.artist COLLATE NOCASE")
                            .arg(key)
                            .arg(bpm > 0 ? bpm : 0);
        }
    } else if (kind == QLatin1String("ids")) {   // the tracks in loaded, in its order (autoplay's history)
        if (loaded.isEmpty()) {
            return out;
        }
        QStringList marks;
        QString rank = QStringLiteral("CASE l.id");
        for (int i = 0; i < loaded.size(); ++i) {
            marks.append(QString::number(loaded[i]));
            rank += QStringLiteral(" WHEN %1 THEN %2").arg(loaded[i]).arg(i);
        }
        where.append(QStringLiteral("l.id IN (%1)").arg(marks.join(QLatin1Char(','))));
        if (sort == QLatin1String("added")) {
            order = rank + QStringLiteral(" END");
        }
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
    where.append(only.sql);
    args += only.args;
    if (!genre.trimmed().isEmpty()) {
        // the genre fields that hold it, as genres() splits them (SQLite's lower() is only for A-Z: not in SQL)
        const QString wanted = genreKey(genre);
        QStringList marks;
        for (const auto& [field, n] : genreFields(db, only)) {
            const QStringList names = splitGenres(field);
            if (genre == kNoGenre ? names.isEmpty()
                                  : std::any_of(names.cbegin(), names.cend(), [&](const QString& g) {
                                        return genreKey(g) == wanted;
                                    })) {
                marks.append(QStringLiteral("?"));
                args.append(field);
            }
        }
        const QString in = marks.isEmpty() ? QStringLiteral("0") : QStringLiteral("l.genre IN (%1)").arg(marks.join(QLatin1Char(',')));
        where.append(genre == kNoGenre ? QStringLiteral("(l.genre IS NULL OR %1)").arg(in) : in);
    }
    const QStringList words = search.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString& word : words) {
        where.append(QStringLiteral(
                "(l.artist LIKE ? OR l.title LIKE ? OR l.album LIKE ? OR l.genre LIKE ? OR l.album_artist LIKE ? "
                "OR l.key LIKE ?)"));
        for (int i = 0; i < 6; ++i) {
            args.append(QString(QStringLiteral("%") + word + QStringLiteral("%")));
        }
    }
    QSqlQuery q(db);
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

QJsonObject track(const QSqlDatabase& db, int trackId) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT %1 FROM %2 WHERE l.id = ?").arg(kTrackColumns, kTrackFrom));
    q.addBindValue(trackId);
    if (!q.exec() || !q.next()) {
        return {};
    }
    QJsonObject t = trackJson(q);
    t.insert(QStringLiteral("location"), q.value(14).toString());
    return t;
}

Pool Pool::fromJson(const QJsonObject& o) {
    Pool p;
    for (const QJsonValue& g : o.value(QStringLiteral("genres")).toArray()) {
        p.genres.append(g.toString());
    }
    for (const QJsonValue& g : o.value(QStringLiteral("skip")).toArray()) {
        p.skipGenres.append(g.toString());
    }
    p.bpmMin = std::max(0.0, o.value(QStringLiteral("bpmMin")).toDouble());
    p.bpmMax = std::max(0.0, o.value(QStringLiteral("bpmMax")).toDouble());
    return p;
}

QJsonObject Pool::toJson() const {
    return {{"genres", QJsonArray::fromStringList(genres)},
            {"skip", QJsonArray::fromStringList(skipGenres)},
            {"bpmMin", bpmMin},
            {"bpmMax", bpmMax}};
}

QList<int> pick(const QSqlDatabase& db, const Where& only, const Pool& pool, int count) {
    QList<int> out;
    if (count <= 0) {
        return out;
    }
    QStringList where{QStringLiteral("l.mixxx_deleted = 0"), QStringLiteral("tl.fs_deleted = 0"), only.sql,
            // not queued already, not played this session (Mixxx's newest set history: hidden = 2)
            QStringLiteral("l.id NOT IN (SELECT pt.track_id FROM PlaylistTracks pt JOIN Playlists p ON p.id = pt.playlist_id "
                           "WHERE p.hidden = 1 OR p.id = (SELECT max(id) FROM Playlists WHERE hidden = 2))")};
    QVariantList args = only.args;
    // genres: the genre fields that hold one of them (as tracks() matches a genre)
    const auto fieldsWith = [&db, &only](const QStringList& genres, QVariantList* pArgs) {
        QSet<QString> keys;
        for (const QString& g : genres) {
            keys.insert(genreKey(g));
        }
        QStringList marks;
        for (const auto& [field, n] : genreFields(db, only)) {
            const QStringList names = splitGenres(field);
            if (std::any_of(names.cbegin(), names.cend(), [&keys](const QString& g) { return keys.contains(genreKey(g)); })) {
                marks.append(QStringLiteral("?"));
                pArgs->append(field);
            }
        }
        return marks;
    };
    if (!pool.genres.isEmpty()) {
        const QStringList marks = fieldsWith(pool.genres, &args);
        if (marks.isEmpty()) {
            return out;   // (none of those genres in the library)
        }
        where.append(QStringLiteral("l.genre IN (%1)").arg(marks.join(QLatin1Char(','))));
    }
    if (!pool.skipGenres.isEmpty()) {
        const QStringList marks = fieldsWith(pool.skipGenres, &args);
        if (!marks.isEmpty()) {
            where.append(QStringLiteral("(l.genre IS NULL OR l.genre NOT IN (%1))").arg(marks.join(QLatin1Char(','))));
        }
    }
    if (pool.bpmMin > 0) {
        where.append(QStringLiteral("l.bpm >= ?"));
        args.append(pool.bpmMin);
    }
    if (pool.bpmMax > 0) {
        where.append(QStringLiteral("l.bpm > 0 AND l.bpm <= ?"));
        args.append(pool.bpmMax);
    }
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT l.id, tl.location FROM %1 WHERE %2 ORDER BY random() LIMIT ?")
                      .arg(kTrackFrom, where.join(QStringLiteral(" AND "))));
    for (const QVariant& a : std::as_const(args)) {
        q.addBindValue(a);
    }
    q.addBindValue(count * 4 + 4);   // (more than needed: a file can be gone, a network drive away)
    if (!q.exec()) {
        qWarning() << "ZyDeck: autoplay's query failed:" << q.lastError().text();
        return out;
    }
    while (q.next() && out.size() < count) {
        if (QFileInfo::exists(q.value(1).toString())) {
            out.append(q.value(0).toInt());
        }
    }
    return out;
}

int autoDjQueueLength(const QSqlDatabase& db) {
    QSqlQuery q(db);
    return q.exec(QStringLiteral("SELECT count(*) FROM PlaylistTracks pt JOIN Playlists p ON p.id = pt.playlist_id WHERE p.hidden = 1")) &&
                    q.next()
            ? q.value(0).toInt()
            : 0;
}

} // namespace zydeck::library
