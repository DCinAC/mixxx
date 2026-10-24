#pragma once

#include <QTimer>
#include <array>

#include "library/libraryfeature.h"
#include "library/treeitemmodel.h"
#include "preferences/usersettings.h"
#include "track/trackid.h"
#include "util/parented_ptr.h"

class ControlProxy;

namespace zydeck {

class FitsTableModel;

// ZyDeck: "Fits deck" in the library's sidebar, ZyDeck's "what fits next": under it, each deck that has a track, and
// a click shows the tracks that mix with what that deck plays now (see FitsTableModel). The lists follow the decks: a
// new track, a key shift or a tempo change shows the list for it, once it has settled (a moving tempo fader doesn't
// redo it at every step).
class FitsFeature final : public LibraryFeature {
    Q_OBJECT
  public:
    FitsFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~FitsFeature() final;

    QVariant title() final;
    TreeItemModel* sidebarModel() const final;

  public slots:
    void activate() final;
    void activateChild(const QModelIndex& index) final;

  private:
    static constexpr int kMaxDecks = 4;
    struct Deck {
        ControlProxy* pKey = nullptr;
        ControlProxy* pBpm = nullptr;
        FitsTableModel* pModel = nullptr;   // made the first time it's shown
        int key = 0;
        double bpm = 0;
        TrackId track;
    };

    void refresh();
    void show(int deck);
    FitsTableModel* model(int deck);

    parented_ptr<TreeItemModel> m_pSidebarModel;
    ControlProxy* m_pNumDecks;
    std::array<Deck, kMaxDecks> m_decks;
    QTimer m_settle;               // a change is looked at once things have been still for a moment
    QList<int> m_listed;           // the decks under "Fits deck" now
};

} // namespace zydeck
