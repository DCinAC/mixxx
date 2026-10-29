#include "library.h"
#include "zydecklibraryquery.h"

#include <QCache>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThread>
#include <QThreadPool>
#include <QtEndian>
#include <atomic>
#include <cmath>
#include <cstring>
#include <optional>

namespace zc {

QStringList mixxxDirCandidates() {
    const QString home = QDir::homePath();
#if defined(Q_OS_MACOS)
    return {home + "/Library/Containers/org.mixxx.mixxx/Data/Library/Application Support/Mixxx",
            home + "/Library/Application Support/Mixxx"};
#elif defined(Q_OS_WIN)
    QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (local.isEmpty()) {
        local = home + "/AppData/Local";
    }
    return {local + "/Mixxx"};
#else
    return {home + "/.mixxx", home + "/.local/share/mixxx", home + "/.var/app/org.mixxx.Mixxx/.mixxx"};
#endif
}

namespace {
QString lookForMixxxDir() {
    for (const QString& d : mixxxDirCandidates()) {
        if (QFileInfo(d + "/mixxxdb.sqlite").isFile()) {
            return d;
        }
    }
    return {};
}
QMutex g_dirMutex;
QString g_dir;
bool g_dirKnown = false;
qint64 g_dirAt = 0;
std::atomic<bool> g_dirLooking{false};
} // namespace

QString mixxxDir(bool* known) {
    // (the window's thread, once it runs, mustn't wait; a worker, or the start before the event loop (the command
    // line's --install-mapping), looks now)
    const bool waitable = !QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread() ||
            QThread::currentThread()->loopLevel() == 0;
    QMutexLocker lock(&g_dirMutex);
    if (!g_dirKnown && waitable) {
        lock.unlock();
        const QString dir = lookForMixxxDir();
        lock.relock();
        g_dir = dir;
        g_dirKnown = true;
        g_dirAt = QDateTime::currentMSecsSinceEpoch();
    }
    if (QDateTime::currentMSecsSinceEpoch() - g_dirAt > 3000 && !g_dirLooking.exchange(true)) {
        QThreadPool::globalInstance()->start([] {
            const QString dir = lookForMixxxDir();
            QMutexLocker l(&g_dirMutex);
            g_dir = dir;
            g_dirKnown = true;
            g_dirAt = QDateTime::currentMSecsSinceEpoch();
            g_dirLooking = false;
        });
    }
    if (known) {
        *known = g_dirKnown;
    }
    return g_dir;
}

namespace {

/// A read-only connection to Mixxx's database for this thread (Qt's connections belong to the thread that made
/// them), or an invalid one when there's no library.
QSqlDatabase db() {
    const QString dir = mixxxDir();
    if (dir.isEmpty()) {
        return {};
    }
    const QString name = QStringLiteral("zc-%1").arg(reinterpret_cast<quintptr>(QThread::currentThreadId()));
    QSqlDatabase d = QSqlDatabase::contains(name) ? QSqlDatabase::database(name, false)
                                                  : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
    const QString path = dir + "/mixxxdb.sqlite";
    if (d.databaseName() != path) {
        d.close();
        d.setDatabaseName(path);
        d.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=2000"));
    }
    if (!d.isOpen() && !d.open()) {
        return {};
    }
    return d;
}

// ---- protobuf, just enough for Mixxx's waveforms and beats ----
struct Field {
    quint32 number;
    int type;            // 0 varint, 1 64-bit, 2 bytes, 5 32-bit
    quint64 varint = 0;
    double f64 = 0;
    float f32 = 0;
    QByteArray bytes;
};

std::optional<quint64> varint(const QByteArray& b, int& i) {
    quint64 v = 0;
    for (int shift = 0; i < b.size() && shift < 64; shift += 7) {
        const auto x = static_cast<quint8>(b[i++]);
        v |= static_cast<quint64>(x & 0x7F) << shift;
        if (x < 0x80) {
            return v;
        }
    }
    return std::nullopt;
}

/// The message's fields, or nullopt if it isn't one.
std::optional<QVector<Field>> fields(const QByteArray& b) {
    QVector<Field> out;
    int i = 0;
    while (i < b.size()) {
        const auto key = varint(b, i);
        if (!key) {
            return std::nullopt;
        }
        Field f{static_cast<quint32>(*key >> 3), static_cast<int>(*key & 7)};
        if (f.type == 0) {
            const auto v = varint(b, i);
            if (!v) {
                return std::nullopt;
            }
            f.varint = *v;
        } else if (f.type == 1) {
            if (i + 8 > b.size()) {
                return std::nullopt;
            }
            std::memcpy(&f.f64, b.constData() + i, 8);
            f.f64 = qFromLittleEndian(f.f64);
            i += 8;
        } else if (f.type == 2) {
            const auto n = varint(b, i);
            if (!n || i + static_cast<qint64>(*n) > b.size()) {
                return std::nullopt;
            }
            f.bytes = b.mid(i, static_cast<int>(*n));
            i += static_cast<int>(*n);
        } else if (f.type == 5) {
            if (i + 4 > b.size()) {
                return std::nullopt;
            }
            std::memcpy(&f.f32, b.constData() + i, 4);
            f.f32 = qFromLittleEndian(f.f32);
            i += 4;
        } else {
            return std::nullopt;
        }
        out.append(f);
    }
    return out;
}

/// A waveform.proto Signal: its values (field 1, packed or not), each capped at 255.
QByteArray signal(const QByteArray& b) {
    QByteArray out;
    const auto fs = fields(b);
    if (!fs) {
        return out;
    }
    for (const Field& f : *fs) {
        if (f.number != 1) {
            continue;
        }
        if (f.type == 0) {
            out.append(static_cast<char>(qMin<quint64>(255, f.varint)));
        } else if (f.type == 2) {
            for (int j = 0; j < f.bytes.size();) {
                const auto x = varint(f.bytes, j);
                if (!x) {
                    break;
                }
                out.append(static_cast<char>(qMin<quint64>(255, *x)));
            }
        }
    }
    return out;
}

/// The first value of field n in message b (a double field, or a varint one read as a number)
std::optional<double> number(const QByteArray& b, quint32 n) {
    const auto fs = fields(b);
    if (!fs) {
        return std::nullopt;
    }
    for (const Field& f : *fs) {
        if (f.number == n) {
            return f.type == 1 ? f.f64 : f.type == 5 ? f.f32 : static_cast<double>(f.varint);
        }
    }
    return std::nullopt;
}

} // namespace

QByteArray packWaveform(const QByteArray& file) {
    // qCompress'd (a big-endian length first) or not
    QByteArray data = file;
    if (file.size() > 4 && qFromBigEndian<quint32>(file.constData()) != 0) {
        const QByteArray u = qUncompress(file);
        if (!u.isEmpty()) {
            data = u;
        }
    }
    double rate = 441;
    QByteArray all, band[3];
    const auto fs = fields(data);
    if (!fs) {
        return {};
    }
    for (const Field& f : *fs) {
        if (f.number == 1) {
            rate = f.type == 1 ? f.f64 : f.type == 5 ? f.f32 : static_cast<double>(f.varint);
        } else if (f.number == 3 && f.type == 2) {
            all = signal(f.bytes);
        } else if (f.number == 4 && f.type == 2) {
            const auto filtered = fields(f.bytes);
            if (filtered) {
                for (const Field& g : *filtered) {
                    if (g.number >= 1 && g.number <= 3 && g.type == 2) {
                        band[g.number - 1] = signal(g.bytes);
                    }
                }
            }
        }
    }
    // left and right alternate: the louder of each pair
    const auto merge = [](const QByteArray& s) {
        QByteArray m(s.size() / 2, 0);
        for (int i = 0; i < m.size(); ++i) {
            m[i] = static_cast<char>(qMax(static_cast<quint8>(s[2 * i]), static_cast<quint8>(s[2 * i + 1])));
        }
        return m;
    };
    const QByteArray a = merge(all), lo = merge(band[0]), mi = merge(band[1]), hi = merge(band[2]);
    const int n = static_cast<int>(qMin(qMin(a.size(), lo.size()), qMin(mi.size(), hi.size())));
    QByteArray out("MXWF", 4);
    const float r = static_cast<float>(rate);
    const quint32 count = static_cast<quint32>(n);
    char head[8];
    qToLittleEndian(r, head);
    qToLittleEndian(count, head + 4);
    out.append(head, 8);
    out.resize(12 + n * 4);
    char* p = out.data() + 12;
    for (int i = 0; i < n; ++i) {
        p[4 * i] = a[i];
        p[4 * i + 1] = lo[i];
        p[4 * i + 2] = mi[i];
        p[4 * i + 3] = hi[i];
    }
    return out;
}

QJsonObject decodeBeats(const QByteArray& blob, const QString& version, int rate, double duration, bool locked) {
    const double end = duration * rate;
    QVector<double> frames;
    int bar = 0;
    const auto fs = fields(blob);
    if (!fs || rate <= 0) {
        return {};
    }
    if (version.startsWith(QLatin1String("BeatGrid"))) {
        std::optional<double> first, bpm;
        for (const Field& f : *fs) {
            if (f.number == 1 && f.type == 2) {
                bpm = number(f.bytes, 1);
            } else if (f.number == 2 && f.type == 2) {
                first = number(f.bytes, 1).value_or(0);
            }
        }
        if (!bpm || *bpm <= 0 || !first) {
            return {};
        }
        const double step = 60.0 * rate / *bpm;
        const auto k = static_cast<qint64>(std::floor(*first / step));   // beats before the marker, back to the start
        for (double x = *first - k * step; x < end && frames.size() < 20000; x += step) {
            frames.append(x);
        }
        bar = static_cast<int>(((-k) % 4 + 4) % 4);
    } else if (version.startsWith(QLatin1String("BeatMap"))) {
        for (const Field& f : *fs) {
            if (f.number == 1 && f.type == 2) {
                const double x = number(f.bytes, 1).value_or(0);
                if (x < end && frames.size() < 20000) {
                    frames.append(x);
                }
            }
        }
    } else {
        return {};
    }
    QJsonArray beats;
    for (double x : frames) {
        beats.append(std::round(x / rate * 10000) / 10000);
    }
    return {{"beats", beats}, {"bar", bar}, {"locked", locked}};
}

void Library::load() {
    QSqlDatabase d = db();
    if (!d.isValid()) {
        return;
    }
    QSqlQuery q(d);
    if (!q.exec(QStringLiteral("SELECT l.duration, l.title, l.artist, tl.location, l.id FROM library l "
                               "JOIN track_locations tl ON tl.id = l.location WHERE l.duration > 0 ORDER BY l.mixxx_deleted"))) {
        return;
    }
    QVector<Row> rows;
    while (q.next()) {
        QString title = q.value(1).toString().trimmed();
        if (title.isEmpty()) {
            title = QFileInfo(q.value(3).toString()).completeBaseName();
        }
        rows.append({q.value(0).toDouble(), title, q.value(2).toString().trimmed(), q.value(4).toInt()});
    }
    m_rows = rows;
    m_loadedMs = QDateTime::currentMSecsSinceEpoch();
}

int Library::trackCount() {
    QMutexLocker lock(&m_mutex);
    if (QDateTime::currentMSecsSinceEpoch() - m_loadedMs > 5000) {
        load();
    }
    return static_cast<int>(m_rows.size());
}

QJsonObject Library::name(qint64 ms) {
    for (int attempt = 0; attempt < 6; ++attempt) {
        const Row* best = nullptr;
        Row found;
        {
            QMutexLocker lock(&m_mutex);
            if (attempt || QDateTime::currentMSecsSinceEpoch() - m_loadedMs > 5000) {
                load();   // a track added a moment ago
            }
            // Mixxx tells the length to the millisecond, as the library has it: a track within 60 ms, the nearest;
            // further off (a file whose length Mixxx measured again), only one that nothing else comes near. Better
            // no name than another track's (and its waveform)
            double gap = 1e18;
            int near = 0;
            for (const Row& r : m_rows) {
                const double g = std::abs(r.seconds * 1000 - ms);
                if (g <= 1000) {
                    ++near;
                }
                if (g < gap) {
                    gap = g;
                    best = &r;
                }
            }
            if (best && (gap <= 60 || (gap <= 1000 && near == 1))) {
                found = *best;
            } else {
                best = nullptr;
            }
        }
        if (best) {
            return {{"t", found.title}, {"a", found.artist}, {"id", found.id}};
        }
        QThread::msleep(500);
    }
    return {};
}

QByteArray Library::waveform(int trackId) {
    static QMutex cacheMutex;
    static QCache<QString, QByteArray> cache(16);
    QString path;
    {
        QSqlDatabase d = db();
        if (!d.isValid()) {
            return {};
        }
        QSqlQuery q(d);
        q.prepare(QStringLiteral("SELECT id FROM track_analysis WHERE track_id = ? AND type = 1 ORDER BY id DESC LIMIT 1"));
        q.addBindValue(trackId);
        if (!q.exec() || !q.next()) {
            return {};
        }
        path = mixxxDir() + "/analysis/" + q.value(0).toString();
    }
    const QFileInfo info(path);
    if (!info.isFile()) {
        return {};
    }
    const QString key = path + '@' + QString::number(info.lastModified().toMSecsSinceEpoch());
    {
        QMutexLocker lock(&cacheMutex);
        if (const QByteArray* hit = cache.object(key)) {
            return *hit;
        }
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray packed = packWaveform(f.readAll());
    QMutexLocker lock(&cacheMutex);
    cache.insert(key, new QByteArray(packed));
    return packed;
}

QStringList Library::rootDirs() {
    QStringList out;
    QSqlDatabase d = db();
    if (!d.isValid()) {
        return out;
    }
    QSqlQuery q(d);
    if (q.exec(QStringLiteral("SELECT directory FROM directories ORDER BY rowid"))) {
        while (q.next()) {
            out.append(q.value(0).toString());
        }
    }
    return out;
}

QString Library::location(int trackId) {
    QSqlDatabase d = db();
    if (!d.isValid()) {
        return {};
    }
    QSqlQuery q(d);
    q.prepare(QStringLiteral("SELECT tl.location FROM library l JOIN track_locations tl ON tl.id = l.location "
                             "WHERE l.id = ? AND l.mixxx_deleted = 0 AND tl.fs_deleted = 0"));
    q.addBindValue(trackId);
    return q.exec() && q.next() ? q.value(0).toString() : QString();
}

QList<QPair<int, QString>> Library::likelyTracks(int count) {
    QList<QPair<int, QString>> out;
    QSqlDatabase d = db();
    if (!d.isValid()) {
        return out;
    }
    QSet<int> seen;
    for (const char* order : {"l.last_played_at DESC", "l.datetime_added DESC", "l.timesplayed DESC"}) {
        QSqlQuery q(d);
        if (!q.exec(QStringLiteral("SELECT l.id, tl.location FROM library l JOIN track_locations tl ON tl.id = l.location "
                                   "WHERE l.mixxx_deleted = 0 AND tl.fs_deleted = 0 ORDER BY %1 LIMIT %2")
                            .arg(QLatin1String(order))
                            .arg(count / 3))) {
            continue;   // (an older Mixxx without last_played_at)
        }
        while (q.next()) {
            const int id = q.value(0).toInt();
            if (!seen.contains(id)) {
                seen.insert(id);
                out.append({id, q.value(1).toString()});
            }
        }
    }
    return out;
}

QJsonObject Library::beats(int trackId) {
    QSqlDatabase d = db();
    if (!d.isValid()) {
        return {};
    }
    QSqlQuery q(d);
    q.prepare(QStringLiteral("SELECT beats, beats_version, samplerate, duration, bpm_lock FROM library WHERE id = ?"));
    q.addBindValue(trackId);
    if (!q.exec() || !q.next() || q.value(0).toByteArray().isEmpty() || q.value(2).toInt() <= 0) {
        return {};
    }
    return decodeBeats(q.value(0).toByteArray(), q.value(1).toString(), q.value(2).toInt(), q.value(3).toDouble(),
            q.value(4).toBool());
}

QJsonObject Library::views() {
    QSqlDatabase d = db();
    return d.isValid() ? zydeck::library::views(d, {}) : QJsonObject{{"total", 0}, {"crates", QJsonArray()}, {"playlists", QJsonArray()}, {"history", QJsonArray()}};
}

QJsonArray Library::tracks(const QString& search, const QString& view, const QString& sort, int limit, int offset, const QString& genre) {
    QSqlDatabase d = db();
    return d.isValid() ? zydeck::library::tracks(d, {}, search, view, sort, limit, offset, genre) : QJsonArray();
}

QJsonObject Library::genres() {
    QSqlDatabase d = db();
    return d.isValid() ? zydeck::library::genres(d, {}) : QJsonObject{{"genres", QJsonArray()}, {"none", 0}};
}

QJsonObject Library::track(int trackId) {
    QSqlDatabase d = db();
    return d.isValid() ? zydeck::library::track(d, trackId) : QJsonObject();
}

QList<int> Library::pick(const zydeck::library::Pool& pool, int count) {
    QSqlDatabase d = db();
    return d.isValid() ? zydeck::library::pick(d, {}, pool, count) : QList<int>();
}

int Library::autoDjQueueLength() {
    QSqlDatabase d = db();
    return d.isValid() ? zydeck::library::autoDjQueueLength(d) : 0;
}

} // namespace zc
