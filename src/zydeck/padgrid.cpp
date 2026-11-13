#include "zydeck/padgrid.h"

#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "control/controlobject.h"
#include "mixer/playerinfo.h"
#include "track/cue.h"
#include "track/track.h"
#include "zydeck/padfx.h"

namespace zydeck {

namespace {

constexpr int kDecks = 4;
enum Mode { HotCue = 0, PadFx1 = 1, PadFx2 = 2, Sampler = 3, Stems = 4, kModes = 5 };
const char* const kModeNames[kModes] = {"HOT CUE", "PAD FX 1", "PAD FX 2", "SAMPLER", "STEMS"};
const char* const kModeShort[kModes] = {"CUE", "FX 1", "FX 2", "SMPL", "STEM"};

// LateNight's look (PaleMoon): square buttons, a 1 px near-black edge round a face of #1e1e20 with a faint light on it
// (btn_embedded_square.svg: #dcdcdc at 7 %), pressed a shade darker; its FX 3/4 teal for PAD FX (units 3 and 4)
const QColor kFace(0x1e, 0x1e, 0x20);
const QColor kEdge(0x00, 0x00, 0x01);
const QColor kSheen(0xdc, 0xdc, 0xdc, 18);
const QColor kPress(0x00, 0x00, 0x00, 80);
const QColor kTabOn(0x3a, 0x3a, 0x3c);
const QColor kText(0xbb, 0xbb, 0xbb);
const QColor kDim(0x66, 0x66, 0x66);
const QColor kFx(0x14, 0x66, 0x74);
const QColor kFxOn(0x25, 0x7b, 0x82);
const QColor kSampler(0xb2, 0x4c, 0x12);
const QColor kStem(0x39, 0x55, 0x79);
constexpr int kGap = 2;    // (as LateNight's buttons sit)
constexpr int kTabH = 18;

QFont labelFont(int px, bool bold) {   // (LateNight's Open Sans, its own spacing, hinted to the pixel)
    QFont f(QStringLiteral("Open Sans"));
    f.setPixelSize(px);
    f.setWeight(bold ? QFont::DemiBold : QFont::Normal);
    f.setKerning(true);
    f.setHintingPreference(QFont::PreferFullHinting);
    f.setStyleStrategy(QFont::PreferAntialias);
    return f;
}

// One LateNight button: its edge, its face (colour: a set pad's, else the plain face), its sheen, pressed darker
void button(QPainter& p, const QRectF& r, const QColor& face, bool pressed) {
    const QRect box = r.toAlignedRect();
    p.fillRect(box, kEdge);
    const QRect inner = box.adjusted(1, 1, -1, -1);
    p.fillRect(inner, face);
    p.fillRect(inner, kSheen);
    if (pressed) {
        p.fillRect(inner, kPress);
    }
}

double get(const QString& group, const QString& key) {
    const ControlObject* pCo = ControlObject::getControl(ConfigKey(group, key), ControlFlag::NoWarnIfMissing);
    return pCo ? pCo->get() : 0;
}
void set(const QString& group, const QString& key, double v) {
    ControlObject* pCo = ControlObject::getControl(ConfigKey(group, key), ControlFlag::NoWarnIfMissing);
    if (pCo) {
        pCo->set(v);
    }
}

struct Pad {
    QString top;      // what it does
    QString bottom;   // its detail (beats, number)
    QColor color;     // its colour (invalid: an empty pad)
    bool lit = false;
};

class PadGrid : public QWidget {
  public:
    PadGrid(const QString& group, QWidget* pParent)
            : QWidget(pParent),
              m_group(group),
              m_deck(group.mid(8, 1).toInt()) {
        setMinimumSize(160, 96);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(false);
        auto* pTimer = new QTimer(this);   // (what the pads show, looked at 25 times a second: a repaint only on a change)
        connect(pTimer, &QTimer::timeout, this, [this] {
            const QString now = signature();
            if (now != m_shown) {
                m_shown = now;
                update();
            }
        });
        pTimer->start(40);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const int mode = this->mode();
        const QFont small = labelFont(10, true);
        p.setFont(small);
        // the modes' row
        for (int m = 0; m < kModes; ++m) {
            const QRectF r = tabRect(m);
            const bool on = m == mode;
            button(p, r, on ? kTabOn : kFace, false);
            p.setPen(on ? QColor(0xee, 0xee, 0xee) : kDim);
            p.drawText(r, Qt::AlignCenter, QString::fromLatin1(width() > 300 ? kModeNames[m] : kModeShort[m]));
        }
        // the pads
        const QList<Pad> pads = this->pads();
        const QFont big = labelFont(std::clamp(static_cast<int>(padRect(0).height() / 3.2), 11, 14), true);
        for (int k = 0; k < 8; ++k) {
            const QRectF r = padRect(k);
            const Pad& pad = pads.value(k);
            const bool there = pad.color.isValid();
            const QColor face = there ? pad.color : kFace;   // (as Mixxx's: a set hot cue in its colour)
            button(p, r, face, k == m_down);
            const QColor ink = !there ? kDim : face.lightness() > 150 ? QColor(0x14, 0x14, 0x14) : QColor(0xee, 0xee, 0xee);
            p.setPen(ink);
            p.setFont(big);
            // its name in the middle, its detail under it (the two together centred when there's room)
            const QRectF inner = r.adjusted(4, 2, -4, -2);
            const int lineH = QFontMetrics(big).height(), detailH = pad.bottom.isEmpty() ? 0 : QFontMetrics(small).height();
            const int blockH = std::min<int>(static_cast<int>(inner.height()), lineH + detailH);
            const QRectF name(inner.left(), std::round(inner.center().y() - blockH / 2.0), inner.width(), lineH);
            p.drawText(name, Qt::AlignHCenter | Qt::AlignVCenter,
                    QFontMetrics(big).elidedText(pad.top, Qt::ElideRight, static_cast<int>(inner.width())));
            if (detailH) {
                p.setFont(small);
                p.drawText(QRectF(inner.left(), name.bottom(), inner.width(), detailH), Qt::AlignHCenter | Qt::AlignTop, pad.bottom);
            }
        }
        if (shifted()) {   // (SHIFT held: its layer)
            p.setPen(kDim);
            p.setFont(small);
            p.drawText(QRectF(0, 0, width() - 2, kTabH), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("SHIFT"));
        }
    }

    void mousePressEvent(QMouseEvent* e) override {
        const QPointF at = e->position();
        for (int m = 0; m < kModes; ++m) {
            if (tabRect(m).contains(at)) {
                set(m_group, QStringLiteral("zydeck_pad_mode"), m);
                update();
                return;
            }
        }
        for (int k = 0; k < 8; ++k) {
            if (padRect(k).contains(at)) {
                m_down = k;
                m_downMode = mode();
                m_downShift = shifted() || e->modifiers().testFlag(Qt::ShiftModifier);
                press(k, true, e->button() == Qt::RightButton);
                update();
                return;
            }
        }
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (m_down >= 0) {
            const int k = m_down;
            press(k, false, e->button() == Qt::RightButton);
            m_down = -1;
            update();
        }
    }

  private:
    int mode() const {
        return std::clamp(static_cast<int>(get(m_group, QStringLiteral("zydeck_pad_mode"))), 0, kModes - 1);
    }
    bool shifted() const {   // (the controller's SHIFT: the keyboard's only with a click, not on show)
        return get(m_group, QStringLiteral("zydeck_pad_shift")) > 0;
    }
    // (whole pixels: n cells of a width w with 2 px between, the remainder spread over the first ones)
    static QRect cell(int i, int n, int w, int y, int h) {
        const int room = w - (n - 1) * kGap, base = room / n, extra = room % n;
        const int x = i * (base + kGap) + std::min(i, extra);
        return {x, y, base + (i < extra ? 1 : 0), h};
    }
    QRectF tabRect(int m) const {
        return cell(m, kModes, width(), 0, kTabH);
    }
    // (the height it's shown in: a deck row with less room than the pads ask for cuts them, so they fit what shows)
    double shownHeight() const {
        const QRect seen = visibleRegion().boundingRect();
        return seen.isEmpty() ? height() : std::min(height(), seen.bottom() + 1);
    }
    QRectF padRect(int k) const {
        const int top = kTabH + kGap, rows = static_cast<int>(shownHeight()) - top;
        const int h1 = (rows - kGap) / 2, h2 = rows - kGap - h1;
        return cell(k % 4, 4, width(), k < 4 ? top : top + h1 + kGap, k < 4 ? h1 : h2);
    }
    QString samplerGroup(int k) const {
        // decks 1, 3: samplers 1-8; decks 2, 4: 9-16 (as ZyDeck's pads; Mixxx with fewer: the ones there are)
        return QStringLiteral("[Sampler%1]").arg((m_deck % 2 ? 0 : 8) + k + 1);
    }

    QList<Pad> pads() const {
        QList<Pad> out;
        const int mode = this->mode();
        const bool shift = shifted();
        switch (mode) {
        case HotCue: {
            const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(m_group);
            QHash<int, CuePointer> cues;
            if (pTrack) {
                for (const CuePointer& pCue : pTrack->getCuePoints()) {
                    if (pCue->getHotCue() >= 0) {
                        cues.insert(pCue->getHotCue(), pCue);
                    }
                }
            }
            for (int k = 0; k < 8; ++k) {
                const int n = (shift ? 8 : 0) + k;   // (SHIFT: hot cues 9-16)
                Pad pad;
                const CuePointer pCue = cues.value(n);
                if (pCue) {
                    pad.color = QColor(static_cast<QRgb>(pCue->getColor()));
                    pad.top = pCue->getLabel().isEmpty() ? QString::number(n + 1) : pCue->getLabel();
                    pad.bottom = pCue->getLabel().isEmpty() ? QString() : QString::number(n + 1);
                    if (pCue->getType() == mixxx::CueType::Loop) {
                        pad.bottom = QStringLiteral("↻ ") + pad.bottom;
                    }
                    pad.lit = get(m_group, QStringLiteral("hotcue_%1_status").arg(n + 1)) >= 2 ||
                            (m_down == k && m_downMode == HotCue);
                } else {
                    pad.top = QString::number(n + 1);
                }
                out.append(pad);
            }
            break;
        }
        case PadFx1:
        case PadFx2:
            for (int k = 0; k < 8; ++k) {
                const int p = (mode == PadFx2 ? 8 : 0) + k;
                const PadFxLabel l = padFxLabel(p);
                Pad pad;
                if (!l.name.isEmpty()) {
                    pad.top = l.name.toUpper();
                    pad.bottom = l.beats;
                    pad.lit = get(m_group, QStringLiteral("zydeck_padfx_%1").arg(p + 1)) > 0;
                    pad.color = pad.lit ? kFxOn : kFx;
                }
                out.append(pad);
            }
            break;
        case Sampler:
            for (int k = 0; k < 8; ++k) {
                const QString g = samplerGroup(k);
                Pad pad;
                if (ControlObject::getControl(ConfigKey(g, QStringLiteral("play")), ControlFlag::NoWarnIfMissing)) {
                    const TrackPointer pTrack = PlayerInfo::instance().getTrackInfo(g);
                    pad.top = pTrack ? pTrack->getTitle() : QString();
                    pad.bottom = QString::number((m_deck % 2 ? 0 : 8) + k + 1);
                    if (pTrack) {
                        pad.lit = get(g, QStringLiteral("play")) > 0;
                        pad.color = pad.lit ? kSampler : QColor(0x2e, 0x2e, 0x30);
                    }
                }
                out.append(pad);
            }
            break;
        case Stems: {
            const int stems = static_cast<int>(get(m_group, QStringLiteral("stem_count")));
            for (int k = 0; k < 8; ++k) {
                Pad pad;
                if (k < stems) {
                    const QString g = QStringLiteral("[Channel%1_Stem%2]").arg(m_deck).arg(k + 1);
                    pad.top = QStringLiteral("STEM %1").arg(k + 1);
                    pad.bottom = get(g, QStringLiteral("mute")) > 0 ? QStringLiteral("muted") : QString();
                    const double c = get(g, QStringLiteral("color"));
                    pad.lit = get(g, QStringLiteral("mute")) <= 0;
                    pad.color = !pad.lit ? QColor(0x2e, 0x2e, 0x30) : c > 0 ? QColor(static_cast<QRgb>(c)) : kStem;
                }
                out.append(pad);
            }
            break;
        }
        }
        return out;
    }

    QString signature() const {
        QString s = QString::number(mode()) + (shifted() ? "s" : "") + QString::number(m_down) + QString::number(width()) + 'x' +
                QString::number(shownHeight());
        s += QString::number(get(QStringLiteral("[ZyDeck]"), QStringLiteral("padfx_version")));
        for (const Pad& pad : pads()) {
            s += '|' + pad.top + '/' + pad.bottom + '/' + pad.color.name() + (pad.lit ? "!" : "");
        }
        return s;
    }

    void press(int k, bool down, bool right) {
        const int mode = down ? this->mode() : m_downMode;
        const bool shift = m_downShift;
        switch (mode) {
        case HotCue: {
            const int n = (shift ? 8 : 0) + k + 1;
            if (right) {   // (the right button: clear it, as Mixxx's own hot cue buttons)
                if (down) {
                    set(m_group, QStringLiteral("hotcue_%1_clear").arg(n), 1);
                }
                return;
            }
            set(m_group, QStringLiteral("hotcue_%1_activate").arg(n), down ? 1 : 0);
            return;
        }
        case PadFx1:
        case PadFx2: {
            const int p = (mode == PadFx2 ? 8 : 0) + k;
            if (shift || right) {   // SHIFT (or the right button): the pad's editor, as a sampler pad's menu
                if (down) {
                    editPadFx(p, window());
                }
                return;
            }
            set(m_group, QStringLiteral("zydeck_padfx_%1").arg(p + 1), down ? 1 : 0);
            return;
        }
        case Sampler: {
            if (!down) {
                return;
            }
            const QString g = samplerGroup(k);
            set(g, right || shift ? QStringLiteral("cue_gotoandstop") : QStringLiteral("cue_gotoandplay"), 1);
            return;
        }
        case Stems:
            if (down && k < get(m_group, QStringLiteral("stem_count"))) {
                const QString g = QStringLiteral("[Channel%1_Stem%2]").arg(m_deck).arg(k + 1);
                set(g, QStringLiteral("mute"), get(g, QStringLiteral("mute")) > 0 ? 0 : 1);
            }
            return;
        }
    }

    QString m_group;
    int m_deck;
    int m_down = -1;
    int m_downMode = 0;
    bool m_downShift = false;
    QString m_shown;
};

// A skin widget named ZyDeckPads_[ChannelN], as it's first shown: the pads go in it
class Installer : public QObject {
  public:
    using QObject::QObject;
    bool eventFilter(QObject* pObject, QEvent* pEvent) override {
        if (pEvent->type() == QEvent::Polish && pObject->isWidgetType() &&
                pObject->objectName().startsWith(QLatin1String("ZyDeckPads_"))) {
            auto* pHost = static_cast<QWidget*>(pObject);
            if (!pHost->property("zydeckPads").toBool()) {
                pHost->setProperty("zydeckPads", true);
                const QString group = pHost->objectName().mid(11);
                auto* pGrid = new PadGrid(group, pHost);
                if (!pHost->layout()) {
                    auto* pLayout = new QVBoxLayout(pHost);
                    pLayout->setContentsMargins(0, 0, 0, 0);
                }
                pHost->layout()->addWidget(pGrid);
            }
        }
        return false;
    }
};

} // namespace

void watchPadGrids() {
    for (int d = 1; d <= kDecks; ++d) {
        const QString group = QStringLiteral("[Channel%1]").arg(d);
        new ControlObject(ConfigKey(group, QStringLiteral("zydeck_pad_mode")));
        new ControlObject(ConfigKey(group, QStringLiteral("zydeck_pad_shift")));
    }
    qApp->installEventFilter(new Installer(qApp));
    // LateNight's hot cues show as its gear menu says, but not where the pads are
    auto* pShown = new ControlObject(ConfigKey(QStringLiteral("[Skin]"), QStringLiteral("zydeck_hotcues_shown")));
    auto* pTimer = new QTimer(qApp);
    QObject::connect(pTimer, &QTimer::timeout, pTimer, [pShown] {
        const double v = get(QStringLiteral("[Skin]"), QStringLiteral("show_hotcues")) > 0 &&
                        get(QStringLiteral("[Skin]"), QStringLiteral("zydeck_pads")) <= 0
                ? 1
                : 0;
        if (pShown->get() != v) {
            pShown->set(v);
        }
    });
    pTimer->start(200);
}

} // namespace zydeck
