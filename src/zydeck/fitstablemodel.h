#pragma once

#include "library/basesqltablemodel.h"
#include "track/trackid.h"

namespace zydeck {

// ZyDeck: the tracks in the library that fit what a deck plays now, as ZyDeck's "Fits deck N" (the phone's library):
// within 6 % of its tempo, or of half or double it, and in a key that mixes with its key as it sounds now (key shift
// included: the same key, a step round the Camelot wheel either way, or its relative major/minor), the track itself
// left out. The same key first, then the nearest tempo (a column of its own, "fit", sorted on and not shown).
class FitsTableModel final : public BaseSqlTableModel {
    Q_OBJECT
  public:
    FitsTableModel(QObject* pParent, TrackCollectionManager* pTrackCollectionManager, int deck);
    ~FitsTableModel() final;

    /// What the deck plays: its key (Mixxx's key id, 0: none), its tempo (0: none) and its track (invalid: nothing,
    /// and nothing fits)
    void fitTo(int key, double bpm, TrackId playing);

    bool isColumnInternal(int column) final;
    Qt::ItemFlags flags(const QModelIndex& index) const final;
    Capabilities getCapabilities() const final;
    QString modelKey(bool noSearch) const final;

  private:
    const QString m_viewName;
    bool m_sorted = false;   // (the default sort set once: a column the user picks stays picked)
};

} // namespace zydeck
