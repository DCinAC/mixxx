#include "zydeck/fitsfeature.h"

#include <algorithm>
#include <cmath>

#include "control/controlproxy.h"
#include "library/library.h"
#include "library/treeitem.h"
#include "mixer/playerinfo.h"
#include "moc_fitsfeature.cpp"
#include "track/track.h"
#include "zydeck/fitstablemodel.h"

namespace zydeck {

namespace {

constexpr int kSettleMs = 400;
constexpr double kTempoStep = 0.005;   // a tempo change under 0.5 % keeps the list as it is

QString deckGroup(int deck) {
    return QStringLiteral("[Channel%1]").arg(deck);
}

} // namespace

FitsFeature::FitsFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : LibraryFeature(pLibrary, pConfig, QStringLiteral("promotracks")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)),
          m_pNumDecks(new ControlProxy(QStringLiteral("[App]"), QStringLiteral("num_decks"), this)) {
    m_pSidebarModel->setRootItem(TreeItem::newRoot(this));
    m_settle.setSingleShot(true);
    m_settle.setInterval(kSettleMs);
    connect(&m_settle, &QTimer::timeout, this, &FitsFeature::refresh);
    m_pNumDecks->connectValueChanged(this, [this] { m_settle.start(); });
    connect(&PlayerInfo::instance(), &PlayerInfo::trackChanged, this, [this] { m_settle.start(); });
    m_settle.start();
}

FitsFeature::~FitsFeature() {
}

QVariant FitsFeature::title() {
    return tr("Fits deck");
}

TreeItemModel* FitsFeature::sidebarModel() const {
    return m_pSidebarModel;
}

FitsTableModel* FitsFeature::model(int deck) {
    Deck& d = m_decks[deck - 1];
    if (!d.pModel) {
        d.pModel = new FitsTableModel(this, m_pLibrary->trackCollectionManager(), deck);
        d.pModel->fitTo(d.key, d.bpm, d.track);
    }
    return d.pModel;
}

// What each deck plays now: a list that's made gets the new fit; the sidebar lists the decks with a track
void FitsFeature::refresh() {
    const int decks = std::clamp(static_cast<int>(m_pNumDecks->get()), 0, kMaxDecks);
    QList<int> listed;
    for (int i = 0; i < kMaxDecks; ++i) {
        Deck& d = m_decks[i];
        if (!d.pKey && i < decks) {   // (a deck's controls exist once the deck does: decks 3 and 4 can come later)
            d.pKey = new ControlProxy(deckGroup(i + 1), QStringLiteral("key"), this, ControlFlag::NoAssertIfMissing);
            d.pBpm = new ControlProxy(deckGroup(i + 1), QStringLiteral("bpm"), this, ControlFlag::NoAssertIfMissing);
            d.pKey->connectValueChanged(this, [this] { m_settle.start(); });
            d.pBpm->connectValueChanged(this, [this] { m_settle.start(); });
        }
        const TrackPointer pTrack = i < decks && d.pKey->valid()
                ? PlayerInfo::instance().getTrackInfo(deckGroup(i + 1))
                : TrackPointer();
        const TrackId track = pTrack ? pTrack->getId() : TrackId();
        const int key = pTrack ? static_cast<int>(std::lround(d.pKey->get())) : 0;
        const double bpm = pTrack ? d.pBpm->get() : 0;
        const bool tempoMoved = d.bpm > 0 ? std::abs(bpm - d.bpm) / d.bpm >= kTempoStep : bpm > 0;
        if (track != d.track || key != d.key || tempoMoved || (bpm <= 0) != (d.bpm <= 0)) {
            d.track = track;
            d.key = key;
            d.bpm = bpm;
            if (d.pModel) {
                d.pModel->fitTo(key, bpm, track);
            }
        }
        if (track.isValid()) {
            listed.append(i + 1);
        }
    }
    if (listed != m_listed) {
        m_listed = listed;
        std::unique_ptr<TreeItem> pRoot = TreeItem::newRoot(this);
        for (const int deck : std::as_const(m_listed)) {
            pRoot->appendChild(tr("Deck %1").arg(deck), deck);
        }
        m_pSidebarModel->setRootItem(std::move(pRoot));
    }
}

void FitsFeature::show(int deck) {
    emit saveModelState();
    emit showTrackModel(model(deck));
    emit enableCoverArtDisplay(true);
}

// "Fits deck" itself: the first deck with a track (or deck 1's empty list)
void FitsFeature::activate() {
    show(m_listed.isEmpty() ? 1 : m_listed.first());
}

void FitsFeature::activateChild(const QModelIndex& index) {
    const int deck = index.data(TreeItemModel::kDataRole).toInt();
    if (deck >= 1 && deck <= kMaxDecks) {
        show(deck);
    }
}

} // namespace zydeck
