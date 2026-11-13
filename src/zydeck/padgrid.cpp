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

const QColor kBack(0x1a, 0x1a, 0x1a);
const QColor kPad(0x2b, 0x2b, 0x2b);
const QColor kEdge(0x3c, 0x3c, 0x3c);
const QColor kText(0xd6, 0xd6, 0xd6);
const QColor kDim(0x77, 0x77, 0x77);
const QColor kFx(0xb0, 0x6c, 0xff);
const QColor kSampler(0xff, 0x9a, 0x2e);
const QColor kStem(0x3c, 0xc8, 0xff);

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
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), kBack);
        const int mode = this->mode();
        // the modes' row
        QFont small = font();
        small.setPixelSize(9);
        small.setBold(true);
        p.setFont(small);
        for (int m = 0; m < kModes; ++m) {
            const QRectF r = tabRect(m);
            const bool on = m == mode;
            p.setPen(Qt::NoPen);
            p.setBrush(on ? QColor(0x44, 0x44, 0x44) : kPad);
            p.drawRoundedRect(r, 2, 2);
            p.setPen(on ? (m == PadFx1 || m == PadFx2 ? kFx : kText) : kDim);
            p.drawText(r, Qt::AlignCenter, QString::fromLatin1(width() > 260 ? kModeNames[m] : kModeShort[m]));
        }
        // the pads
        const QList<Pad> pads = this->pads();
        QFont big = font();
        big.setPixelSize(std::max(9, std::min(13, padRect(0).toRect().height() / 4)));
        big.setBold(true);
        for (int k = 0; k < 8; ++k) {
            const QRectF r = padRect(k);
            const Pad& pad = pads.value(k);
            const bool there = pad.color.isValid();
            QColor fill = !there ? kPad : pad.lit ? pad.color : pad.color.darker(330);
            p.setPen(QPen(there ? pad.color.darker(pad.lit ? 100 : 180) : kEdge, 1));
            p.setBrush(fill);
            p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
            if (k == m_down) {
                p.setPen(QPen(Qt::white, 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 3, 3);
            }
            const QColor ink = pad.lit && there ? (pad.color.lightness() > 140 ? QColor(0x14, 0x14, 0x14) : Qt::white)
                    : there                    ? kText
                                               : kDim;
            p.setPen(ink);
            p.setFont(big);
            const QRectF inner = r.adjusted(3, 2, -3, -2);
            p.drawText(inner, Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextWordWrap,
                    QFontMetrics(big).elidedText(pad.top, Qt::ElideRight, static_cast<int>(inner.width() * 2)));
            if (!pad.bottom.isEmpty()) {
                p.setFont(small);
                p.drawText(inner, Qt::AlignHCenter | Qt::AlignBottom, pad.bottom);
            }
        }
        if (shifted()) {   // (SHIFT's layer)
            p.setPen(kDim);
            p.setFont(small);
            p.drawText(rect().adjusted(0, 0, -2, 0), Qt::AlignRight | Qt::AlignBottom, QStringLiteral("SHIFT"));
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
    bool shifted() const {
        return get(m_group, QStringLiteral("zydeck_pad_shift")) > 0 ||
                QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
    }
    QRectF tabRect(int m) const {
        constexpr int modes = kModes;
        const double w = (width() - (modes - 1) * 2.0) / modes;
        return {m * (w + 2), 0, w, 14};
    }
    QRectF padRect(int k) const {
        const double top = 17, gap = 3;
        const double w = (width() - 3 * gap) / 4, h = (height() - top - gap) / 2;
        return {(k % 4) * (w + gap), top + (k / 4) * (h + gap), w, h};
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
                const int p = (mode == PadFx2 ? 16 : 0) + (shift ? 8 : 0) + k;
                const PadFxLabel l = padFxLabel(p);
                Pad pad;
                if (!l.name.isEmpty()) {
                    pad.top = l.name.toUpper();
                    pad.bottom = l.beats;
                    pad.color = kFx;
                    pad.lit = get(m_group, QStringLiteral("zydeck_padfx_%1").arg(p + 1)) > 0;
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
                        pad.color = kSampler;
                        pad.lit = get(g, QStringLiteral("play")) > 0;
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
                    pad.color = c > 0 ? QColor(static_cast<QRgb>(c)) : kStem;
                    pad.lit = get(g, QStringLiteral("mute")) <= 0;
                }
                out.append(pad);
            }
            break;
        }
        }
        return out;
    }

    QString signature() const {
        QString s = QString::number(mode()) + (shifted() ? "s" : "") + QString::number(m_down) + QString::number(width());
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
        case PadFx2:
            set(m_group, QStringLiteral("zydeck_padfx_%1").arg((mode == PadFx2 ? 16 : 0) + (shift ? 8 : 0) + k + 1), down ? 1 : 0);
            return;
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
