#pragma once
// ZyDeck: the library page's queries on Mixxx's database (mixxxdb.sqlite), one copy for the hub (the device's own
// Mixxx) and ZyDeck Connect (the computer's, in controller mode): its views (crates, playlists, the Auto DJ queue,
// set history), tracks by view, search, sort and genre, and its genres. Qt Core and Sql only.

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVariantList>

class QSqlQuery;

namespace zydeck::library {

/// A condition every query adds, on l (library) and tl (track_locations), with its ? values: the hub's leaves out
/// the folders the phone hides; "1" for none.
struct Where {
    QString sql = QStringLiteral("1");
    QVariantList args;
};

extern const QString kTrackColumns;   // a track's row (trackJson reads them), from kTrackFrom
extern const QString kTrackFrom;
/// tracks(genre): the tracks with no genre
inline const QString kNoGenre = QStringLiteral(":none");

/// {total, stems, recent, crates: [{id, name, count, source (an Auto DJ source)}], playlists, autodj: {id, name,
/// count}, history: [the last 20 sets with tracks]}
QJsonObject views(const QSqlDatabase& db, const Where& only);
/// view: all | stems | recent | ids (the tracks in loaded, in its order) | crate:<id> | playlist:<id> |
/// fits:<key id>:<bpm>:<track to leave out> (keys that mix
/// with it, within 6 % of the tempo or at half or double). Every word of search must be in artist, title, album, genre
/// or key. sort: added | artist | title | bpm | key (Camelot order) | played | loaded (loaded's order first).
/// genre: tracks with it among theirs (genres()), or kNoGenre.
QJsonArray tracks(const QSqlDatabase& db,
        const Where& only,
        const QString& search,
        const QString& view,
        const QString& sort,
        int limit,
        int offset,
        const QString& genre = QString(),
        const QList<int>& loaded = {});
/// One track, with its file's "location"; {} when there's none
QJsonObject track(const QSqlDatabase& db, int trackId);
/// The genres, most tracks first: {genres: [{name, count}], none: tracks without one}. A genre field can hold several
/// ("House; Tech House", "Disco, Funk", "Classical/Trance": ; , | and /), and counts under each; spelt differently
/// ("J-Trance", "jtrance"): one genre, under its most used spelling.
QJsonObject genres(const QSqlDatabase& db, const Where& only);

/// Autoplay's pool: the whole library, or what fits these (any of them empty or 0: no limit there)
struct Pool {
    QStringList genres;       // any of these (as genres() names them)
    QStringList skipGenres;   // none of these
    double bpmMin = 0;
    double bpmMax = 0;
    double tempo = 8;         // each next track within this many % of the one before it (half / double too); 0: any
    static Pool fromJson(const QJsonObject& o);
    QJsonObject toJson() const;
};
/// Up to count random tracks from the pool, ids: not in the Auto DJ queue, not played this session (Mixxx's newest
/// history list), their files there. Every track of the library in turn, at random, as it plays. after: the tempo
/// they follow (the queue's last track's, or the playing one's; 0: unknown): with the pool's tempo limit, each is
/// within it of the one before (the closest tempo there is, when none is).
QList<int> pick(const QSqlDatabase& db, const Where& only, const Pool& pool, int count, double after = 0);
/// The Auto DJ queue's length, and its last track's tempo (0: none)
int autoDjQueueLength(const QSqlDatabase& db);
double autoDjQueueTailBpm(const QSqlDatabase& db);

/// A row of kTrackColumns as the pages read a track
QJsonObject trackJson(const QSqlQuery& query);
/// A genre field's genres, and what makes two names one genre (as phone.html's splitGenres and genreKey)
QStringList splitGenres(const QString& field);
QString genreKey(const QString& name);

} // namespace zydeck::library
