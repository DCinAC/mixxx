#include "zydeck/fitstablemodel.h"

#include <QSqlError>
#include <QSqlQuery>

#include "library/dao/trackschema.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "moc_fitstablemodel.cpp"
#include "track/keyutils.h"

namespace zydeck {

namespace {

constexpr double kTempoRange = 0.06;   // within 6 %
const QString kFitColumn = QStringLiteral("fit");

} // namespace

FitsTableModel::FitsTableModel(QObject* pParent, TrackCollectionManager* pTrackCollectionManager, int deck)
        : BaseSqlTableModel(pParent, pTrackCollectionManager, "mixxx.db.model.zydeck.fits"),
          m_viewName(QStringLiteral("zydeck_fits_%1").arg(deck)) {
    fitTo(0, 0, TrackId());
}

FitsTableModel::~FitsTableModel() {
}

void FitsTableModel::fitTo(int key, double bpm, TrackId playing) {
    // A view can't take bound values: the numbers go in as text (all of them numbers made here)
    const auto num = [](double v) { return QString::number(v, 'f', 4); };
    QStringList where{QStringLiteral("library.mixxx_deleted=0"), QStringLiteral("track_locations.fs_deleted=0")};
    QString fit = QStringLiteral("0");
    if (!playing.isValid() || (key <= 0 && bpm <= 0)) {
        where.append(QStringLiteral("0"));   // nothing on the deck (or nothing known about it): nothing fits
    } else {
        where.append(QStringLiteral("library.id<>") + playing.toString());
        if (bpm > 0) {
            QStringList ranges;
            for (const double k : {1.0, 2.0, 0.5}) {
                ranges.append(QStringLiteral("library.bpm BETWEEN %1 AND %2")
                                      .arg(num(bpm * k * (1 - kTempoRange)), num(bpm * k * (1 + kTempoRange))));
            }
            where.append(QStringLiteral("(") + ranges.join(QStringLiteral(" OR ")) + QStringLiteral(")"));
            fit = QStringLiteral("MIN(ABS(library.bpm-%1),ABS(library.bpm-%2),ABS(library.bpm-%3))")
                          .arg(num(bpm), num(bpm * 2), num(bpm / 2));
        }
        if (key > 0) {
            QStringList keys;
            const auto compatible = KeyUtils::getCompatibleKeys(static_cast<mixxx::track::io::key::ChromaticKey>(key));
            for (const auto k : compatible) {
                keys.append(QString::number(static_cast<int>(k)));
            }
            if (!keys.isEmpty()) {
                where.append(QStringLiteral("library.key_id IN (") + keys.join(QLatin1Char(',')) + QStringLiteral(")"));
            }
            fit = QStringLiteral("(CASE WHEN library.key_id=%1 THEN 0 ELSE 1000 END)+%2").arg(key).arg(fit);
        }
    }

    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("DROP VIEW IF EXISTS ") + m_viewName)) {
        qWarning() << "ZyDeck fits:" << query.lastError();
    }
    if (!query.exec(QStringLiteral("CREATE TEMPORARY VIEW ") + m_viewName + QStringLiteral(" AS SELECT library.") +
                LIBRARYTABLE_ID + QStringLiteral(", ") + fit + QStringLiteral(" AS ") + kFitColumn +
                QStringLiteral(" FROM library INNER JOIN track_locations ON library.location=track_locations.id WHERE ") +
                where.join(QStringLiteral(" AND ")))) {
        qWarning() << "ZyDeck fits:" << query.lastError();
    }

    const QString search = currentSearch();
    setTable(m_viewName,
            LIBRARYTABLE_ID,
            QStringList{LIBRARYTABLE_ID, kFitColumn},
            m_pTrackCollectionManager->internalCollection()->getTrackSource());
    if (!m_sorted) {
        m_sorted = true;
        setDefaultSort(fieldIndex(kFitColumn), Qt::AscendingOrder);
    }
    setSearch(search);
    select();
}

bool FitsTableModel::isColumnInternal(int column) {
    return column == fieldIndex(kFitColumn) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ID) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_PLAYED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BPM_LOCK) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BEATS_VERSION) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_MIXXXDELETED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_KEY_ID) ||
            column == fieldIndex(ColumnCache::COLUMN_TRACKLOCATIONSTABLE_DIRECTORY) ||
            column == fieldIndex(ColumnCache::COLUMN_TRACKLOCATIONSTABLE_FSDELETED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_SOURCE) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_TYPE) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_LOCATION) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_COLOR) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_DIGEST) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_HASH);
}

Qt::ItemFlags FitsTableModel::flags(const QModelIndex& index) const {
    return readOnlyFlags(index);
}

TrackModel::Capabilities FitsTableModel::getCapabilities() const {
    return Capability::AddToTrackSet |
            Capability::AddToAutoDJ |
            Capability::EditMetadata |
            Capability::LoadToDeck |
            Capability::LoadToSampler |
            Capability::LoadToPreviewDeck |
            Capability::Analyze |
            Capability::Properties |
            Capability::Sorting;
}

QString FitsTableModel::modelKey(bool noSearch) const {
    if (noSearch) {
        return QStringLiteral("zydeck_fits:") + m_viewName;
    }
    return QStringLiteral("zydeck_fits:") + m_viewName + QStringLiteral("#") + currentSearch();
}

} // namespace zydeck
