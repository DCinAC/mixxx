#include "window.h"

#include <QAction>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSettings>
#include <QLineEdit>
#include <QLabel>
#include <QFormLayout>
#include <QFile>
#include <QDialogButtonBox>
#include <QDialog>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QDesktopServices>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QPointer>
#include <QSystemTrayIcon>
#include <QUrl>
#include <QThreadPool>
#include <QTimer>
#include <QWindow>
#include <QSet>
#include <algorithm>
#include <cmath>

#include "core.h"
#include "discovery.h"
#include "setup.h"
#include "library.h"
#include "qrcodegen.hpp"
#include "system.h"

namespace zc {

namespace {
// The unit, in logical pixels: the ears, the face, the tag under it. Its width follows the device names (layoutWidth).
constexpr int kMinW = 700, kMaxW = 1000, kH = 236, kEar = 30;
// The window: see-through, nothing round the unit but a title bar on top of it (drawn here, in the system's manner: it
// moves the window, closes and minimizes it), room round both for their drop shadow on what's behind, and below for
// the tag
constexpr int kPad = 18, kBar = 30;
constexpr int kTagW = 196, kTagH = 254, kTagX = 66;   // under the left column
int windowHeight(bool tagOut) { return kPad + kBar + kH + (tagOut ? kTagH + 24 : kPad); }
const QPointF kUnit(kPad, kPad + kBar);   // where the unit is in the window
// how it comes out (ms): pulled until its inner corner catches and it slants (to 300), held there (to 500), pushed back
// a little (to 700), pulled again, straight at 1000; back in, a plain slide
constexpr double kTravel = kTagH - 6, kCatchMs = 300, kHoldMs = 500, kBackMs = 700, kOutMs = 1000, kInMs = 400;
constexpr double kSnag = 14, kPushBack = 12, kSlant = -7.5;   // px short of out when it catches; px back; degrees
double easeOut(double u) { return 1 - std::pow(1 - u, 3); }
double easeInOut(double u) { return u < .5 ? 4 * u * u * u : 1 - std::pow(-2 * u + 2, 3) / 2; }
double smooth(double a, double b, double u) { const double t = std::clamp((u - a) / (b - a), 0.0, 1.0); return t * t * (3 - 2 * t); }
const QRectF kVfd(54, 58, 220, 74);
// MIXXX: a plate standing out of the panel with Mixxx's red rocker and a cable plugged in beside it (what MAP was);
// the unit's own POWER rocker in the corner (for show: it says which switch is Mixxx's); SCAN: round, its word under it
const QRectF kMixxx(84, 142, 106, 62), kPowerKey(40, 178, 22, 22), kScan(213, 149, 40, 40);
const QRectF kMixxxRocker(kMixxx.x() + 13, kMixxx.y() + 20, 24, 34);   // (only it is pressed, not the plate)
// the slots: bays from kBayX to the right edge; in each, the name, then 30 px clear, then the cluster: PAIR (3:2)
// and the IN and OUT lamps
constexpr double kBayX = 298, kBayRight = 22, kBayH = 46, kBayGap = 8, kBayY = 16;
constexpr double kNameX = 64, kNameGap = 30, kCluster = 96;   // the cluster: from PAIR's left edge to the bay's right

const QColor kLamp(0xff, 0xc2, 0x3d), kGreen(0x3d, 0xdc, 0x84), kRed(0xff, 0x4a, 0x3d), kVfdInk(0x4f, 0xf0, 0xd0);

QFont pxFont(int px, int weight = QFont::Normal, double spacing = 0, bool mono = false) {
    QFont f = mono ? QFontDatabase::systemFont(QFontDatabase::FixedFont) : QApplication::font();
    f.setPixelSize(px);
    f.setWeight(static_cast<QFont::Weight>(weight));
    if (spacing != 0) {
        f.setLetterSpacing(QFont::AbsoluteSpacing, spacing);
    }
    return f;
}

QPixmap qrPixmap(const QString& text) {
    const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.toUtf8().constData(), qrcodegen::QrCode::Ecc::LOW);
    const int n = qr.getSize(), px = 6;
    QPixmap pm(n * px, n * px);
    pm.fill(Qt::white);
    QPainter p(&pm);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x14, 0x14, 0x16));
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            if (qr.getModule(x, y)) {
                p.drawRect(x * px, y * px, px, px);
            }
        }
    }
    return pm;
}

/// A lamp: dark when off; lit, brightest in the middle with a little glow
void lamp(QPainter& p, const QRectF& r, const QColor& c, bool on, bool round = true) {
    p.save();
    p.setPen(Qt::NoPen);
    if (on) {
        QRadialGradient glow(r.center(), r.width() * 1.4);
        QColor g = c;
        g.setAlpha(90);
        glow.setColorAt(0, g);
        glow.setColorAt(1, Qt::transparent);
        p.setBrush(glow);
        p.drawEllipse(r.center(), r.width() * 1.4, r.height() * 1.4);
        QRadialGradient face(r.center(), r.width() * .7);
        face.setColorAt(0, c.lighter(160));
        face.setColorAt(.55, c);
        face.setColorAt(1, c.darker(140));
        p.setBrush(face);
    } else {
        p.setBrush(QColor(0x26, 0x28, 0x2c));
        p.setPen(QPen(QColor(0x3a, 0x3c, 0x40), 1));
    }
    if (round) {
        p.drawEllipse(r);
    } else {
        p.drawRect(r);
    }
    p.restore();
}

/// The deck's key: a black frame, a grey face lit from the top left; pushed in when down
/// A rocker switch from the front: its black bezel, the rocker in it pressed in at I (on) or O (off), the pressed half
/// sunk and the other standing out; lit (glow 0..1), it glows from inside and round the bezel
void rocker(QPainter& p, const QRectF& r, bool on, double glow, const QColor& body, bool held) {
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 120));   // its shadow on what it's in
    p.drawRoundedRect(r.translated(1.5, 2), 3, 3);
    if (glow > 0) {
        for (int i = 4; i >= 1; --i) {
            QColor halo = body;
            halo.setAlphaF(.07 * glow * (5 - i));
            p.setBrush(halo);
            p.drawRoundedRect(r.adjusted(-i * 1.6, -i * 1.6, i * 1.6, i * 1.6), 4 + i, 4 + i);
        }
    }
    p.setBrush(QColor(8, 8, 8));   // the bezel
    p.drawRoundedRect(r, 3, 3);
    const QRectF in = r.adjusted(3, 3, -3, -3);
    QColor c = glow > 0 ? body.lighter(100 + static_cast<int>(40 * glow)) : body.darker(150);
    if (held) {
        c = c.darker(120);
    }
    const double mid = in.center().y();
    const auto half = [&](const QRectF& h, bool sunk, bool upper) {
        QLinearGradient g(h.topLeft(), h.bottomLeft());
        if (sunk) {   // in: shadowed where it goes under the other half
            g.setColorAt(0, c.darker(upper ? 165 : 112));
            g.setColorAt(1, c.darker(upper ? 112 : 165));
        } else {      // out: catching the light at its top
            g.setColorAt(0, c.lighter(upper ? 130 : 112));
            g.setColorAt(1, c.darker(upper ? 104 : 128));
        }
        p.setBrush(g);
        p.drawRect(h);
    };
    half(QRectF(in.x(), in.y(), in.width(), mid - in.y()), on, true);
    half(QRectF(in.x(), mid, in.width(), in.bottom() - mid), !on, false);
    p.setPen(QColor(255, 255, 255, glow > 0 ? 225 : 110));
    p.setFont(pxFont(8, QFont::Bold));
    p.drawText(QRectF(in.x(), in.y(), in.width(), mid - in.y()), Qt::AlignCenter, QStringLiteral("I"));
    p.drawText(QRectF(in.x(), mid, in.width(), in.bottom() - mid), Qt::AlignCenter, QStringLiteral("O"));
    p.restore();
}

void key(QPainter& p, const QRectF& r, const QString& label, bool down) {
    p.save();
    const QRectF face = down ? r.translated(1, 1) : r;
    if (!down) {   // its shadow on the panel
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 110));
        p.drawRoundedRect(r.translated(3, 4), 4, 4);
    }
    QLinearGradient g(face.topLeft(), face.bottomLeft());
    g.setColorAt(0, down ? QColor(0x40, 0x43, 0x47) : QColor(0x55, 0x57, 0x5c));
    g.setColorAt(.55, down ? QColor(0x48, 0x4b, 0x4f) : QColor(0x41, 0x44, 0x48));
    g.setColorAt(1, down ? QColor(0x4a, 0x4d, 0x51) : QColor(0x3c, 0x3e, 0x43));
    p.setPen(QPen(QColor(5, 5, 5), 3));
    p.setBrush(g);
    p.drawRoundedRect(face.adjusted(1.5, 1.5, -1.5, -1.5), 3, 3);
    if (!down) {
        p.setPen(QPen(QColor(255, 255, 255, 50), 1));
        p.drawLine(face.topLeft() + QPointF(4, 3.5), face.topRight() + QPointF(-4, 3.5));
    }
    p.setPen(QColor(0xf1, 0xf1, 0xf1));
    p.setFont(pxFont(11, QFont::Bold, 1.4));
    p.drawText(face.adjusted(10, 0, -24, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
    p.restore();
}

/// ZyDeck's 2x2 process lamp: one dot going round while busy, all green done, all red failed, dark off
void deviceIcon(QPainter& p, const QRectF& r, bool tablet) {
    p.save();
    p.setPen(QPen(QColor(0x9a, 0x9a, 0x9a), 1.6));
    p.setBrush(Qt::NoBrush);
    const QRectF body = tablet ? QRectF(r.x() + 1, r.y() + 3, r.width() - 2, r.height() - 6)
                               : QRectF(r.x() + r.width() * .28, r.y() + 1, r.width() * .44, r.height() - 2);
    p.drawRoundedRect(body, 2.2, 2.2);
    p.drawLine(QPointF(body.center().x() - 1.5, body.bottom() - 2.6), QPointF(body.center().x() + 1.5, body.bottom() - 2.6));
    p.restore();
}
} // namespace

/// The ZyDeck app's status bar icon (packaging/android/res/drawable/ic_stat_zydeck.xml: the open-reel deck, on a
/// 24 x 24 grid), with a dot at the corner while a device is linked (or something's wrong). On the Mac a template
/// image (black, the menu bar tints it); elsewhere white with a dark rim, to show on light and dark taskbars, the dot
/// in the state's colour.
QIcon Window::trayIcon(bool dot, const QColor& c) const {
    QPainterPath deck;
    deck.setFillRule(Qt::OddEvenFill);
    for (const double x : {6.5, 17.5}) {   // the reels: a disc, its hub and three holes
        deck.addEllipse(QPointF(x, 10.5), 5, 5);
        deck.addEllipse(QPointF(x, 10.5), 1.2, 1.2);
        deck.addEllipse(QPointF(x, 7.6), 1.05, 1.05);
        deck.addEllipse(QPointF(x + 2.511, 11.95), 1.05, 1.05);
        deck.addEllipse(QPointF(x - 2.511, 11.95), 1.05, 1.05);
    }
    QPainterPath base;   // the tape path under them, and the head
    base.moveTo(3.2, 14.4);
    for (const QPointF& pt : {QPointF(9.2, 19.2), QPointF(14.8, 19.2), QPointF(20.8, 14.4), QPointF(21.6, 15.4),
                 QPointF(15.3, 20.4), QPointF(8.7, 20.4), QPointF(2.4, 15.4)}) {
        base.lineTo(pt);
    }
    base.closeSubpath();
    base.addRect(QRectF(10.2, 18.6, 3.6, 3.2));
#ifdef Q_OS_MACOS
    const bool mask = true;
#else
    const bool mask = false;
#endif
    QIcon icon;
    for (const int px : {16, 22, 32, 44, 64}) {
        QPixmap pm(px, px);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.scale(px / 24.0, px / 24.0);
        const QColor ink = mask ? QColor(Qt::black) : QColor(Qt::white);
        if (!mask) {
            p.setPen(QPen(QColor(0, 0, 0, 160), 1.2));
        } else {
            p.setPen(Qt::NoPen);
        }
        p.setBrush(ink);
        p.drawPath(deck);
        p.drawPath(base);
        if (dot) {
            p.setPen(mask ? Qt::NoPen : QPen(QColor(0, 0, 0, 160), 1));
            p.setBrush(mask ? ink : c);
            p.drawEllipse(QPointF(21, 3), 2.6, 2.6);
        }
        p.end();
        icon.addPixmap(pm);
    }
    icon.setIsMask(mask);
    return icon;
}


Window::Window(Core* core, Setup* setup, Discovery* discovery)
        : m_core(core),
          m_setup(setup),
          m_discovery(discovery),
          m_w(kMinW) {
    setWindowTitle(QString::fromLatin1(kAppName));
    setWindowFlags(Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::Window);   // (its own bar and shadow)
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(m_w + 2 * kPad, windowHeight(false));
    setMouseTracking(true);
    m_clock.start();

    m_tagAnim.setStartValue(0.0);
    connect(&m_tagAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) { tagPose(v.toDouble()); });
    connect(&m_tagAnim, &QVariantAnimation::finished, this, [this] {
        if (!m_tagWanted) {
            setFixedSize(m_w + 2 * kPad, windowHeight(false));   // (all in: the window ends under the unit again)
        }
    });

    // The menu: from the menu bar's icon (the tray on Windows and Linux), and from the bar's menu key (BarMenu), for
    // when the icon isn't there to click (no tray, or a full menu bar hides it, behind the camera notch say)
    m_menu = new QMenu(this);
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        m_tray = new QSystemTrayIcon(this);
        m_tray->setContextMenu(m_menu);
        connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
#ifndef Q_OS_MACOS   // (the Mac opens the menu on a click)
            if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) {
                show();
                raise();
                activateWindow();
            }
#else
            Q_UNUSED(r);
#endif
        });
        m_tray->show();
    }

    auto* second = new QTimer(this);
    connect(second, &QTimer::timeout, this, &Window::refresh);
    second->start(1000);
    auto* frame = new QTimer(this);   // the lamps that move: MIDI activity, the MIXXX rocker breathing, a slot waiting to pair
    connect(frame, &QTimer::timeout, this, [this] {
        if (isVisible()) {
            update();
        }
    });
    frame->start(80);
    connect(m_core, &Core::changed, this, &Window::refresh);
    connect(m_setup, &Setup::changed, this, &Window::setupChanged);
    connect(m_discovery, &Discovery::changed, this, &Window::refresh);
    refresh();
}

// ---- Mixxx's side: the readout's second line, the MIXXX rocker, and asking (each thing once a run) only when it needs the
// user: whether to set Mixxx up at all, to quit it so its settings can change, to restart it when it doesn't answer
namespace {
QString setupLine(Setup::State s, int tracks) {
    switch (s) {
    case Setup::State::Linked:
        return tracks >= 0 ? QStringLiteral("MIXXX LINKED  %1 TRK").arg(tracks) : QStringLiteral("MIXXX LINKED");
    case Setup::State::StartMixxx: return QStringLiteral("MIXXX SET UP: START IT");
    case Setup::State::QuitMixxx: return QStringLiteral("MIXXX QUIT IT TO SET UP");
    case Setup::State::RestartMixxx: return QStringLiteral("MIXXX RESTART IT");
    case Setup::State::NeedConsent: return QStringLiteral("MIXXX NOT SET UP: MENU");
    case Setup::State::NoMixxx: return QStringLiteral("MIXXX NOT FOUND");
    case Setup::State::NoPort: return QStringLiteral("MIXXX NO PORT");
    case Setup::State::Failed: return QStringLiteral("MIXXX SETUP FAILED");
    case Setup::State::Checking: break;
    }
    return QStringLiteral("MIXXX CHECKING");
}

QString setupLamp(Setup::State s) {   // Mixxx's side for its rocker: "ok" linked, "busy" waiting, "err" failed
    switch (s) {
    case Setup::State::Linked: return QStringLiteral("ok");
    case Setup::State::Failed:
    case Setup::State::NoPort: return QStringLiteral("err");
    case Setup::State::NoMixxx: return QString();
    default: return QStringLiteral("busy");
    }
}
} // namespace

void Window::say(const QString& title, const QString& text, bool warning) {
    if (!isVisible() && m_tray) {   // closed to the menu bar: a notification there
        m_tray->showMessage(title, text, warning ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information, 15000);
        return;
    }
    auto* box = new QMessageBox(warning ? QMessageBox::Warning : QMessageBox::Information, title, text, QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->show();
}

void Window::setupChanged() {
    const Setup::State s = m_setup->state();
    const int was = m_lastSetup;
    m_lastSetup = static_cast<int>(s);
    refresh();
    const auto once = [this](Setup::State k) {
        if (m_asked.contains(static_cast<int>(k))) {
            return false;
        }
        m_asked.insert(static_cast<int>(k));
        return true;
    };
    switch (s) {
    case Setup::State::NeedConsent:
        if (once(s) && isVisible()) {
            auto* box = new QMessageBox(QMessageBox::Question, tr("Set up Mixxx?"),
                    tr("ZyDeck Connect can set Mixxx up for ZyDeck: it puts the ZyDeck mapping in Mixxx's controllers "
                       "folder and turns it on for the ZyDeck Connect port, so a phone or tablet works as a controller "
                       "straight away.\n\nMixxx's settings are backed up first (mixxx.cfg.before-zydeck).") +
                            (m_setup->mixxxRunning() ? tr("\n\nMixxx is open: it'll ask you to quit it once, as Mixxx "
                                                          "keeps its settings to itself while it runs.")
                                                     : QString()),
                    QMessageBox::NoButton, this);
            QPushButton* yes = box->addButton(tr("Set Up"), QMessageBox::AcceptRole);
            box->addButton(tr("Not Now"), QMessageBox::RejectRole);
            box->setDefaultButton(yes);
            box->setAttribute(Qt::WA_DeleteOnClose);
            connect(box, &QMessageBox::buttonClicked, this, [this, yes](QAbstractButton* b) { m_setup->consent(b == yes); });
            box->show();
        }
        break;
    case Setup::State::QuitMixxx:
        if (once(s)) {
            say(tr("Quit Mixxx once"), tr("Mixxx is open, and ZyDeck Connect needs to turn the controller on in its "
                                          "settings, which Mixxx keeps to itself while it runs. Quit Mixxx: ZyDeck "
                                          "Connect does the rest. Then start Mixxx again."));
        }
        break;
    case Setup::State::StartMixxx:
        if (was == static_cast<int>(Setup::State::QuitMixxx)) {
            say(tr("Mixxx is set up"), tr("The ZyDeck controller is on in Mixxx. Start Mixxx."));
        }
        break;
    case Setup::State::RestartMixxx:
        if (once(s)) {
            say(tr("Restart Mixxx"), tr("Mixxx doesn't answer through the ZyDeck Connect port: it was probably started "
                                        "before ZyDeck Connect, and Mixxx only looks for MIDI ports when it starts. "
                                        "Restart Mixxx.\n\nWith Open at Login on (the menu bar menu), ZyDeck "
                                        "Connect starts with the computer, so it's always there first."));
        }
        break;
    case Setup::State::Failed:
        say(tr("Couldn't set up Mixxx"), m_setup->why(), true);
        break;
    case Setup::State::NoMixxx:
        if (once(s)) {
            say(tr("Start Mixxx once"), tr("Mixxx hasn't run on this computer yet (its library and settings aren't "
                                           "there). Start Mixxx once and quit it: ZyDeck Connect sets it up after."));
        }
        break;
    default:
        break;
    }
}

QRectF Window::bay(int slot) const {
    return {kBayX, kBayY + slot * (kBayH + kBayGap), m_w - kEar - kBayRight - kBayX, kBayH};
}

QRectF Window::slotKey(int slot) const {   // 3:2, at the top of the bay; its word goes under it
    const QRectF b = bay(slot);
    return {b.right() - kCluster, b.y() + 6, 30, 20};
}

QRectF Window::tagRect() const {   // (straight: the slant turns it about its top inner corner)
    return {double(kTagX), kH - kTagH + m_drop, double(kTagW), double(kTagH)};
}

void Window::tagPose(double ms) {
    if (!m_tagWanted) {   // back in: a plain slide, straightening as it goes
        const double u = std::min(ms / kInMs, 1.0);
        m_drop = m_fromDrop * (1 - easeInOut(u));
        m_angle = m_fromAngle * (1 - easeOut(u));
    } else if (ms <= kCatchMs) {   // pulled out; near the end its inner corner catches and it slants
        const double u = ms / kCatchMs;
        m_drop = m_fromDrop + (kTravel - kSnag - m_fromDrop) * easeOut(u);
        m_angle = m_fromAngle + (kSlant - m_fromAngle) * smooth(.55, 1, u);
    } else if (ms <= kHoldMs) {   // caught: held there, slanted
        m_drop = kTravel - kSnag;
        m_angle = kSlant;
    } else if (ms <= kBackMs) {   // pushed back a little (the catch eases)
        const double u = (ms - kHoldMs) / (kBackMs - kHoldMs);
        m_drop = kTravel - kSnag - kPushBack * easeInOut(u);
        m_angle = kSlant * (1 - .35 * easeInOut(u));
    } else {   // pulled again: all the way, and straight
        const double u = std::min((ms - kBackMs) / (kOutMs - kBackMs), 1.0);
        m_drop = kTravel - kSnag - kPushBack + (kSnag + kPushBack) * easeOut(u);
        m_angle = kSlant * .65 * (1 - easeOut(u));
    }
    update();
}

void Window::showPairingCode() {
    m_tagManual = 1;
    setTag(true);
}

void Window::setTag(bool out) {
    if (out == m_tagWanted && (m_tagAnim.state() == QAbstractAnimation::Running || m_drop == (out ? kTravel : 0))) {
        return;
    }
    m_tagWanted = out;
    m_tagAnim.stop();
    m_fromDrop = m_drop;
    m_fromAngle = m_angle;
    if (out) {
        setFixedSize(m_w + 2 * kPad, windowHeight(true));   // (room for it, slanted too)
    }
    m_tagAnim.setEndValue(out ? kOutMs : kInMs);
    m_tagAnim.setDuration(static_cast<int>(out ? kOutMs : kInMs));
    m_tagAnim.start();
}

/// As wide as the longest device name needs: 30 px clear of its slot's PAIR and lamps (names past kMaxW are cut short)
void Window::layoutWidth() {
    const QFontMetricsF name(pxFont(13, QFont::DemiBold));
    double widest = 0;
    for (auto it = m_slotOf.cbegin(); it != m_slotOf.cend(); ++it) {
        widest = std::max(widest, name.horizontalAdvance(it.key()));
    }
    const int w = std::clamp(static_cast<int>(std::ceil(kBayX + kNameX + widest + kNameGap + kCluster + kBayRight + kEar)), kMinW, kMaxW);
    if (w != m_w) {
        m_w = w;
        setFixedSize(m_w + 2 * kPad, height());
        m_menuFor.clear();
    }
}

void Window::refresh() {
    const QList<Core::Device> devices = m_core->devices();
    // slots: a device keeps its slot while it's there; a new one goes to the slot whose PAIR asked for it, or the
    // first free one
    QSet<QString> here;
    for (const Core::Device& d : devices) {
        here.insert(d.name);
    }
    for (auto it = m_slotOf.begin(); it != m_slotOf.end();) {
        it = here.contains(it.key()) ? std::next(it) : m_slotOf.erase(it);
    }
    for (const Core::Device& d : devices) {
        if (m_slotOf.contains(d.name)) {
            continue;
        }
        const QList<int> taken = m_slotOf.values();
        int slot = m_pairSlot >= 0 && !taken.contains(m_pairSlot) ? m_pairSlot : -1;
        for (int i = 0; slot < 0 && i < 4; ++i) {
            if (!taken.contains(i)) {
                slot = i;
            }
        }
        if (slot < 0) {
            break;   // (more than four: the menu lists them all)
        }
        m_slotOf.insert(d.name, slot);
        if (slot == m_pairSlot) {
            m_pairSlot = -1;
        }
    }
    // ZyDecks found on the network and not linked: the free slots, in the order they turned up. A linked one is the
    // same device when it's at the same address (the search finds it by its marketing name, "OnePlus 13"; linked, it
    // says its model, "CPH2653")
    m_foundIn.clear();
    {
        QSet<QString> linkedAt;
        for (const Core::Device& d : devices) {
            if (!d.address.isEmpty()) {
                linkedAt.insert(d.address);
            }
        }
        const QList<int> taken = m_slotOf.values();
        int slot = 0;
        for (const Discovery::Found& f : m_discovery->found()) {
            if (m_slotOf.contains(f.name) || linkedAt.contains(f.address)) {
                continue;   // (linked already)
            }
            while (slot < 4 && (taken.contains(slot) || slot == m_pairSlot)) {
                ++slot;
            }
            if (slot >= 4) {
                break;
            }
            m_foundIn.insert(slot++, {f.name, f.address, QString::number(f.asked)});
        }
    }
    layoutWidth();
    // the tag: out while nothing's linked, in once something is (PAIR or the tag overrides until that changes)
    if (devices.size() != m_lastDeviceCount) {
        if (m_lastDeviceCount >= 0) {
            m_tagManual = -1;
            m_pairSlot = -1;
        }
        m_lastDeviceCount = static_cast<int>(devices.size());
    }
    const bool problem = !m_core->midi()->problem().isEmpty();
    setTag(m_tagManual >= 0 ? m_tagManual == 1 : devices.isEmpty() && !problem);

    if (!m_tracksAt.isValid() || m_tracksAt.elapsed() > 5000) {   // (on a worker: it reads the whole library)
        m_tracksAt.start();
        QPointer<Window> self(this);
        Library* library = m_core->library();
        QThreadPool::globalInstance()->start([self, library] {
            const int n = mixxxDir().isEmpty() ? -1 : library->trackCount();
            QMetaObject::invokeMethod(qApp, [self, n] {
                if (self) {
                    self->m_tracks = n;
                }
            });
        });
    }
    const QStringList hosts = lanAddresses();
    m_vfd = {problem ? QStringLiteral("MIDI  NO PORT: LOOPMIDI?") : QStringLiteral("MIDI  ZYDECK CONNECT  OK"),
             setupLine(m_setup->state(), m_tracks),
             hosts.isEmpty() ? QStringLiteral("NO NETWORK") : QStringLiteral("%1:%2").arg(hosts.first()).arg(kTcpPort)};
    const QString url = m_core->pairUrl();
    if (url != m_qrFor) {
        m_qrFor = url;
        m_qr = qrPixmap(url);
    }
    setToolTip(problem ? m_core->midi()->problem() : QString());
    updateTray();
    update();
}

void Window::updateTray() {
    const QList<Core::Device> devices = m_core->devices();
    if (m_tray) {
        const bool problem = !m_core->midi()->problem().isEmpty();
        m_tray->setIcon(trayIcon(problem || !devices.isEmpty(), problem ? kRed : kGreen));
        m_tray->setToolTip(devices.isEmpty() ? tr("ZyDeck Connect: no device yet")
                                             : tr("ZyDeck Connect: %n device(s)", nullptr, static_cast<int>(devices.size())));
    }
    QStringList lines;
    for (const Core::Device& d : devices) {
        lines << d.name + " · " + d.how;
    }
    QStringList foundNames;
    for (const QStringList& f : std::as_const(m_foundIn)) {
        foundNames << f.value(0);
    }
    const QString menuFor = lines.join('\n') + "|" + foundNames.join('\n') + (openAtLogin() ? "+" : "-");
    if (menuFor == m_menuFor) {   // (not rebuilt under an open menu)
        return;
    }
    m_menuFor = menuFor;
    m_menu->clear();
    m_menu->addAction(devices.isEmpty() ? tr("ZyDeck Connect · waiting for a device") : tr("ZyDeck Connect · ready"))->setEnabled(false);
    for (const QString& l : std::as_const(lines)) {
        m_menu->addAction("   " + l)->setEnabled(false);
    }
    for (auto it = m_foundIn.cbegin(); it != m_foundIn.cend(); ++it) {   // found on the network: a click asks it to link
        const QString address = it.value().value(1);
        m_menu->addAction(tr("   Link %1 (found on Wi-Fi)…").arg(it.value().value(0)), this,
                [this, address] { m_discovery->offer(address, m_core->offer()); refresh(); });
    }
    m_menu->addSeparator();
    const auto showIt = [this] {
        show();
        raise();
        activateWindow();
    };
    m_menu->addAction(tr("Show Pairing Code"), this, [this, showIt] {
        showPairingCode();
        showIt();
    });
    m_menu->addAction(tr("Open ZyDeck Connect"), this, showIt);
    m_menu->addAction(tr("Set Up Mixxx Again"), this, [this] { setUpMixxx(); });
    m_menu->addAction(tr("Discord Bot…"), this, [this] { discordBot(); });
    m_menu->addAction(tr("Session Profiler…"), this, [] {   // (profiler.h: its page, served here to this computer)
        QDesktopServices::openUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/profile").arg(kTcpPort)));
    });
    QAction* login = m_menu->addAction(tr("Open at Login"));
    login->setCheckable(true);
    login->setChecked(openAtLogin());
    connect(login, &QAction::toggled, this, [this](bool on) {
        setOpenAtLogin(on);
        refresh();
    });
    m_menu->addSeparator();
    m_menu->addAction(tr("Quit ZyDeck Connect"), qApp, &QApplication::quit);
}

/// Discord Bot...: Mixxx ZyDeck's Live Broadcasting to the ZyDeck Discord bot. The bot's address (as the PC's stem
/// server's, port 8780, at first) and its stream key, typed here; handed to Mixxx ZyDeck once, in a file only this user
/// can read, which it takes within a second (zydeck/discordbroadcast.h in the fork), deletes, and keeps the key in
/// the keychain. Not taken (Mixxx not running, or plain Mixxx): the file goes, so the key isn't left there
void Window::discordBot() {
    QSettings settings;
    QString address = settings.value(QStringLiteral("discord/host")).toString();
    if (address.isEmpty()) {   // the PC that makes the stems runs the bot too, as a rule
        const QString stems = settings.value(QStringLiteral("stems/server")).toString().section(QLatin1Char('/'), -1).section(QLatin1Char(':'), 0, 0);
        address = stems.isEmpty() ? QString() : stems + QStringLiteral(":8780");
    }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("ZyDeck Discord Bot"));
    auto* form = new QFormLayout(&dialog);
    auto* about = new QLabel(tr("Mixxx ZyDeck streams your mix to the bot with Live Broadcasting (its \"ZyDeck Discord\" "
                                "connection). The stream key is kept by Mixxx in this computer's keychain, and nowhere else."));
    about->setWordWrap(true);
    form->addRow(about);
    auto* host = new QLineEdit(address);
    host->setPlaceholderText(QStringLiteral("192.168.0.100:8780"));
    form->addRow(tr("The bot's address"), host);
    auto* key = new QLineEdit;
    key->setEchoMode(QLineEdit::Password);
    key->setPlaceholderText(tr("STREAM_KEY, as in the bot's .env (empty: keep the one Mixxx has)"));
    form->addRow(tr("Stream key"), key);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    dialog.resize(520, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    QString hostName = host->text().trimmed().remove(QRegularExpression(QStringLiteral("^[a-z]+://"))).section(QLatin1Char('/'), 0, 0);
    int port = 8780;
    if (hostName.contains(QLatin1Char(':'))) {
        port = hostName.section(QLatin1Char(':'), -1).toInt();
        hostName = hostName.section(QLatin1Char(':'), 0, -2);
    }
    if (hostName.isEmpty() || port <= 0 || port > 65535) {
        QMessageBox::warning(this, tr("ZyDeck Discord Bot"), tr("That isn't an address (host, or host:port)."));
        return;
    }
    settings.setValue(QStringLiteral("discord/host"), QStringLiteral("%1:%2").arg(hostName).arg(port));
    const QString dir = mixxxDir();
    const QString handover = dir + QStringLiteral("/zydeck-broadcast.json"), done = dir + QStringLiteral("/zydeck-broadcast-done.json");
    QFile::remove(done);
    {
        QFile f(handover);
        if (dir.isEmpty() || !f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::warning(this, tr("ZyDeck Discord Bot"), tr("Couldn't hand it to Mixxx ZyDeck (its folder: %1).").arg(dir));
            return;
        }
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);   // (before the key goes in)
        f.write(QJsonDocument(QJsonObject{{"host", hostName}, {"port", port}, {"key", key->text()}}).toJson(QJsonDocument::Compact));
    }
    key->clear();
    // its answer (without the key), or nothing in 8 s
    auto tries = std::make_shared<int>(0);
    auto* poll = new QTimer(this);
    poll->setInterval(250);
    connect(poll, &QTimer::timeout, this, [this, poll, tries, handover, done] {
        QFile f(done);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject answer = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            f.remove();
            poll->deleteLater();
            if (answer.value(QStringLiteral("ok")).toBool()) {
                QMessageBox::information(this, tr("ZyDeck Discord Bot"),
                        tr("Mixxx ZyDeck has its \"ZyDeck Discord\" connection to %1:%2%3. Switch Live Broadcasting on in Mixxx to stream to the bot.")
                                .arg(answer.value(QStringLiteral("host")).toString())
                                .arg(answer.value(QStringLiteral("port")).toInt())
                                .arg(answer.value(QStringLiteral("key")).toBool() ? tr(", with the new key (in the keychain)") : QString()));
            } else {
                QMessageBox::warning(this, tr("ZyDeck Discord Bot"), answer.value(QStringLiteral("error")).toString(tr("Mixxx ZyDeck couldn't set it up.")));
            }
            return;
        }
        if (++*tries < 32) {
            return;
        }
        QFile::remove(handover);   // (not taken: the key doesn't stay in a file)
        poll->deleteLater();
        QMessageBox::warning(this, tr("ZyDeck Discord Bot"),
                tr("Mixxx ZyDeck didn't take it: is it running? (Plain Mixxx can't: this needs Mixxx ZyDeck.) Try again with it open."));
    });
    poll->start();
}

/// Set Mixxx up (again): the mapping, the controller on, and look whether it answers (asks first if it wasn't allowed)
void Window::setUpMixxx() {
    if (m_setup->state() == Setup::State::NeedConsent && !m_setup->consented()) {
        m_asked.remove(static_cast<int>(Setup::State::NeedConsent));
        setupChanged();   // (asks again)
        return;
    }
    m_asked.clear();
    m_mapLamp = QStringLiteral("busy");
    update();
    QTimer::singleShot(600, this, [this] {
        m_mapLamp.clear();
        m_setup->redo();
        refresh();
    });
}

void Window::press(const Hit& h) {
    switch (h.key) {
    case Key::Map: {   // the MIXXX switch: off (Mixxx isn't running) opens Mixxx; on asks, then quits it
        const auto lookSoon = [this] {
            m_mapLamp = QStringLiteral("busy");   // (its glow breathing till Mixxx is seen to start or stop)
            update();
            for (const int ms : {2000, 5000, 9000}) {
                QTimer::singleShot(ms, this, [this] { m_setup->lookNow(); });
            }
            QTimer::singleShot(10000, this, [this] { m_mapLamp.clear(); refresh(); });
        };
        if (!m_setup->mixxxRunning()) {
            if (startMixxx()) {
                lookSoon();
            } else {
                say(tr("Mixxx isn't here"), tr("ZyDeck Connect couldn't find Mixxx on this computer. Install it from "
                                               "mixxx.org, then turn MIXXX on again."), true);
            }
            break;
        }
        QMessageBox ask(QMessageBox::Question, tr("Turn Mixxx off?"),
                tr("This switch quits Mixxx. If a deck is playing, Mixxx asks before it stops."), QMessageBox::NoButton, this);
        QPushButton* quit = ask.addButton(tr("Quit Mixxx"), QMessageBox::DestructiveRole);
        ask.addButton(tr("Keep It On"), QMessageBox::RejectRole);
        ask.exec();
        if (ask.clickedButton() == quit) {
            quitMixxx();
            lookSoon();
        }
        break;
    }
    case Key::Slot:   // a found ZyDeck's PAIR: ask it to link (it asks its user); an empty slot's: the tag out for it
        if (m_foundIn.contains(h.slot)) {
            m_discovery->offer(m_foundIn.value(h.slot).value(1), m_core->offer());
            refresh();
        } else if (m_slotOf.key(h.slot).isEmpty()) {
            const bool cancel = m_pairSlot == h.slot && m_tagWanted;
            m_pairSlot = cancel ? -1 : h.slot;
            m_tagManual = cancel ? 0 : 1;
            setTag(!cancel);
        }
        break;
    case Key::Tag:   // pushed back in
        m_pairSlot = -1;
        m_tagManual = 0;
        setTag(false);
        break;
    case Key::Slit:   // the tag's slot: pulls it out, or pushes it back
        m_pairSlot = -1;
        m_tagManual = m_tagWanted ? 0 : 1;
        setTag(!m_tagWanted);
        break;
    case Key::BarClose:
        close();
        break;
    case Key::BarMin:
        showMinimized();
        break;
    case Key::BarMenu: {   // the menu bar icon's menu, under the key
        updateTray();
        const QRectF r = barButton(Key::BarMenu);
        m_menu->popup(mapToGlobal(QPointF(r.left(), r.bottom() + 2).toPoint()));
        break;
    }
    case Key::Bar:   // (dragged: the system moves the window)
        break;
    case Key::Scan:   // look for devices now, and the tag out for one to scan (a click on the tag puts it back)
        m_core->midi()->rescan();
        m_discovery->scanNow();
        m_pairSlot = -1;
        m_tagManual = 1;
        setTag(true);
        refresh();
        break;
    case Key::None:
        break;
    }
}

// The bar's buttons: on the Mac the three lamps on the left (close, minimize; the third, zoom, does nothing for a unit
// of fixed size), elsewhere minimize and close on the right
QRectF Window::barButton(Key k) const {
#ifdef Q_OS_MACOS
    if (k == Key::BarMenu) {   // (the lamps are on the left: the menu key on the right)
        return {double(kPad + m_w - 13 - 24), kPad + kBar / 2.0 - 11, 24, 22};
    }
    const double x = kPad + 13 + (k == Key::BarMin ? 20 : 0);
    return {x - 3, kPad + kBar / 2.0 - 9, 18, 18};
#else
    const double w = 46;
    return {kPad + m_w - w * (k == Key::BarClose ? 1 : k == Key::BarMin ? 2 : 3), double(kPad), w, double(kBar)};
#endif
}

Window::Hit Window::hitAt(const QPointF& at) const {
    if (QRectF(kPad, kPad, m_w, kBar).contains(at)) {   // the bar
        if (barButton(Key::BarClose).contains(at)) {
            return {Key::BarClose, -1};
        }
        if (barButton(Key::BarMin).contains(at)) {
            return {Key::BarMin, -1};
        }
        if (barButton(Key::BarMenu).contains(at)) {
            return {Key::BarMenu, -1};
        }
        return {Key::Bar, -1};
    }
    const QPointF p = at - kUnit;   // (the unit's own coordinates)
    if (QRectF(kTagX - 6, kH - 14, kTagW + 12, 16).contains(p)) {   // the tag's slot (generous: it's thin)
        return {Key::Slit, -1};
    }
    if (kMixxxRocker.adjusted(-3, -3, 3, 3).contains(p)) {
        return {Key::Map, -1};
    }
    if (QRectF(kScan).adjusted(-2, -2, 2, 14).contains(p)) {
        return {Key::Scan, -1};
    }
    for (int i = 0; i < 4; ++i) {
        if (slotKey(i).adjusted(-3, -3, 3, 12).contains(p)) {   // (its word under it too)
            return {Key::Slot, i};
        }
    }
    if (m_drop > kTravel / 2 && p.y() > kH && tagRect().adjusted(-30, 0, 30, 0).contains(p)) {
        return {Key::Tag, -1};
    }
    return {};
}

void Window::mousePressEvent(QMouseEvent* e) {
    m_down = hitAt(e->position());
    if ((m_down.key == Key::None || m_down.key == Key::Bar) && e->button() == Qt::LeftButton && windowHandle()) {
        m_down = {};
        windowHandle()->startSystemMove();   // the bar (or the panel): drag the window
    }
    update();
}

void Window::mouseReleaseEvent(QMouseEvent* e) {
    const Hit h = m_down;
    m_down = {};
    if (h.key != Key::None && hitAt(e->position()) == h) {
        press(h);
    }
    update();
}

void Window::leaveEvent(QEvent*) {
    if (m_hover.key != Key::None) {
        m_hover = {};
        update();
    }
}

void Window::mouseMoveEvent(QMouseEvent* e) {
    const Hit h = hitAt(e->position());
    if (!(h == m_hover)) {   // (the bar's buttons light up under the pointer)
        m_hover = h;
        update();
    }
    const bool live = h.key != Key::None && h.key != Key::Bar && !(h.key == Key::Slot && !m_slotOf.key(h.slot).isEmpty() && !m_foundIn.contains(h.slot));
    setCursor(live ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void Window::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape || e->matches(QKeySequence::Close)) {
        close();
        return;
    }
    QWidget::keyPressEvent(e);
}

void Window::closeEvent(QCloseEvent* e) {
    if (m_tray && m_tray->isVisible()) {
        hide();   // still running: the menu bar icon brings it back, and Quit stops it
        e->ignore();
        return;
    }
    QWidget::closeEvent(e);
    qApp->quit();
}

void Window::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int W = m_w;

    // ---- the drop shadow of the bar and the unit on what's behind (lit from above: soft, a little lower) ----
    p.translate(kPad, kPad);
    p.setPen(Qt::NoPen);
    for (int i = 12; i >= 1; --i) {
        p.setBrush(QColor(0, 0, 0, 8 + (12 - i) * 3));
        p.drawRoundedRect(QRectF(0, 0, W, kBar + kH).translated(0, 6).adjusted(-i, -i, i, i), 8 + i, 8 + i);
    }

    // ---- the bar, in the system's manner ----
    {
        QPainterPath bar;
        bar.addRoundedRect(QRectF(0, 0, W, kBar + 10), 9, 9);   // (rounded on top; its foot is under the unit)
        QLinearGradient fill(QPointF(0, 0), QPointF(0, kBar));
        fill.setColorAt(0, QColor(0x33, 0x34, 0x38));
        fill.setColorAt(1, QColor(0x26, 0x27, 0x2a));
        p.setBrush(fill);
        p.drawPath(bar);
        p.setPen(QPen(QColor(255, 255, 255, 30), 1));
        p.drawLine(QPointF(9, .5), QPointF(W - 9, .5));
        p.setPen(QColor(0xc8, 0xc8, 0xcb));
        p.setFont(pxFont(13, QFont::DemiBold));
        const QPointF o(kPad, kPad);   // (barButton is in the window's coordinates)
#ifdef Q_OS_MACOS
        p.drawText(QRectF(0, 0, W, kBar), Qt::AlignCenter, QString::fromLatin1(kAppName));
        const bool over = m_hover.key == Key::BarClose || m_hover.key == Key::BarMin;
        const QColor lamps[3] = {QColor(0xff, 0x5f, 0x57), QColor(0xfe, 0xbc, 0x2e), QColor(0x4a, 0x4b, 0x4e)};
        for (int i = 0; i < 3; ++i) {
            const QPointF c(13 + 6 + i * 20, kBar / 2.0);
            p.setPen(QPen(QColor(0, 0, 0, 60), .8));
            p.setBrush(lamps[i]);
            p.drawEllipse(c, 6, 6);
            if (over && i < 2) {   // (their marks show with the pointer over them, as the system's do)
                p.setPen(QPen(QColor(0x4d, 0x1a, 0x10, 200), 1.3));
                if (i == 0) {
                    p.drawLine(c + QPointF(-2.5, -2.5), c + QPointF(2.5, 2.5));
                    p.drawLine(c + QPointF(2.5, -2.5), c + QPointF(-2.5, 2.5));
                } else {
                    p.drawLine(c + QPointF(-3, 0), c + QPointF(3, 0));
                }
            }
        }
#else
        p.drawText(QRectF(14, 0, W, kBar), Qt::AlignVCenter | Qt::AlignLeft, QString::fromLatin1(kAppName));
        for (const Key k : {Key::BarMin, Key::BarClose}) {
            const QRectF r = barButton(k).translated(-o);
            const bool over = m_hover.key == k;
            if (over) {
                QPainterPath b;
                b.addRect(r);
                p.setPen(Qt::NoPen);
                p.fillPath(k == Key::BarClose ? b.intersected(bar) : b, k == Key::BarClose ? QColor(0xc4, 0x2b, 0x1c) : QColor(255, 255, 255, 25));
            }
            p.setPen(QPen(over && k == Key::BarClose ? Qt::white : QColor(0xc8, 0xc8, 0xcb), 1.1));
            const QPointF c = r.center();
            if (k == Key::BarClose) {
                p.drawLine(c + QPointF(-5, -5), c + QPointF(5, 5));
                p.drawLine(c + QPointF(5, -5), c + QPointF(-5, 5));
            } else {
                p.drawLine(c + QPointF(-5, 0), c + QPointF(5, 0));
            }
        }
#endif
        {   // the menu key: three lines, lit with the pointer over it
            const QRectF r = barButton(Key::BarMenu).translated(-o);
            if (m_hover.key == Key::BarMenu) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(255, 255, 255, 25));
                p.drawRoundedRect(r, 4, 4);
            }
            p.setPen(QPen(QColor(0xc8, 0xc8, 0xcb), 1.4, Qt::SolidLine, Qt::RoundCap));
            const QPointF c = r.center();
            for (const double dy : {-4.5, 0.0, 4.5}) {
                p.drawLine(c + QPointF(-6, dy), c + QPointF(6, dy));
            }
        }
        Q_UNUSED(o);
    }
    p.translate(0, kBar);   // the unit's own coordinates from here

    // ---- the tag, behind the unit: out of the slit in its bottom edge. Black, with a white sticker (the code, what
    // to do with it, the address) as a real one would have; slanted about its top inner corner while it snags ----
    if (m_drop > 0.5) {
        p.save();
        p.setClipRect(QRectF(0, kH - 4, W, height()));
        const QRectF tag = tagRect();
        p.translate(tag.topLeft());
        p.rotate(m_angle);
        p.translate(-tag.topLeft());
        // its shadow on whatever's behind the window: soft, a little below and to the right (lit from the top left)
        p.setPen(Qt::NoPen);
        for (int i = 9; i >= 0; --i) {
            p.setBrush(QColor(0, 0, 0, 7 + (9 - i) * 2));
            p.drawRoundedRect(tag.translated(4, 8).adjusted(-i, -i, i, i), 7 + i, 7 + i);
        }
        QPainterPath body;
        body.addRoundedRect(tag, 7, 7);
        QLinearGradient black(tag.topLeft(), tag.topRight());
        black.setColorAt(0, QColor(0x16, 0x17, 0x1a));
        black.setColorAt(.5, QColor(0x0e, 0x0f, 0x11));
        black.setColorAt(1, QColor(0x14, 0x15, 0x18));
        p.setPen(QPen(QColor(0x2e, 0x30, 0x34), 1));
        p.setBrush(black);
        p.drawPath(body);
        const QRectF sticker(tag.x() + 14, tag.y() + 14, kTagW - 28, kTagH - 14 - 34);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xf6, 0xf6, 0xf3));
        p.drawRect(sticker);   // (a printed sticker: square corners)
        const QRectF code(sticker.x() + 9, sticker.y() + 9, sticker.width() - 18, sticker.width() - 18);
        if (!m_qr.isNull()) {
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawPixmap(code, m_qr, m_qr.rect());
        }
        p.setPen(QColor(0x1a, 0x1b, 0x1e));
        p.setFont(pxFont(10, QFont::Bold, 1.2));
        p.drawText(QRectF(sticker.x(), code.bottom() + 5, sticker.width(), 14), Qt::AlignHCenter, tr("SCAN IN ZYDECK"));
        p.setFont(pxFont(10));
        p.drawText(QRectF(sticker.x(), code.bottom() + 19, sticker.width(), 14), Qt::AlignHCenter, tr("Settings › Pair by QR"));
        p.setPen(QColor(0x5a, 0x5c, 0x60));
        p.setFont(pxFont(9, QFont::Normal, 0, true));
        p.drawText(QRectF(sticker.x(), code.bottom() + 33, sticker.width(), 13), Qt::AlignHCenter, m_vfd.value(2));
        // the finger pull: a recess in the black under the sticker
        const QRectF pull(tag.center().x() - 22, tag.bottom() - 17, 44, 8);
        p.setBrush(QColor(5, 5, 6));
        p.drawRoundedRect(pull, 4, 4);
        p.setPen(QPen(QColor(255, 255, 255, 30), 1));
        p.drawLine(pull.bottomLeft() + QPointF(4, .5), pull.bottomRight() + QPointF(-4, .5));
        p.restore();
    }

    // ---- the unit ----
    for (const double x : {0.0, double(W - kEar)}) {   // ears with their slots
        QLinearGradient ear(QPointF(x, 0), QPointF(x + kEar, 0));
        ear.setColorAt(0, x == 0 ? QColor(0x2c, 0x2e, 0x31) : QColor(0x1d, 0x1f, 0x22));
        ear.setColorAt(1, x == 0 ? QColor(0x1d, 0x1f, 0x22) : QColor(0x2c, 0x2e, 0x31));
        p.setPen(Qt::NoPen);
        p.setBrush(ear);
        p.drawRect(QRectF(x, 0, kEar, kH));
        for (const double sy : {30.0, double(kH - 48)}) {
            p.setBrush(QColor(6, 6, 6));
            p.drawRoundedRect(QRectF(x + kEar / 2.0 - 6, sy, 12, 18), 6, 6);
            p.setPen(QPen(QColor(255, 255, 255, 20), 1));
            p.drawLine(QPointF(x + kEar / 2.0 - 4, sy + 18.5), QPointF(x + kEar / 2.0 + 4, sy + 18.5));
            p.setPen(Qt::NoPen);
        }
    }
    const QRectF face(kEar, 0, W - 2 * kEar, kH);   // brushed, lit from the top
    p.setBrush(QColor(0x1b, 0x1c, 0x1f));
    p.drawRect(face);
    p.setPen(QPen(QColor(255, 255, 255, 6), 1));
    for (double x = face.left() + 1; x < face.right(); x += 3) {
        p.drawLine(QPointF(x, 1), QPointF(x, kH - 1));
    }
    p.setPen(QPen(QColor(255, 255, 255, 22), 1));
    p.drawLine(QPointF(0, 0.5), QPointF(W, 0.5));
    p.setPen(QPen(QColor(0, 0, 0), 1));
    p.drawLine(QPointF(0, kH - 0.5), QPointF(W, kH - 0.5));
    p.setPen(Qt::NoPen);   // the slit the tag comes out of
    p.setBrush(QColor(4, 4, 4));
    p.drawRoundedRect(QRectF(kTagX - 4, kH - 7, kTagW + 8, 5), 2, 2);

    // ---- the edge: machined, catching the light (top and left bright, bottom and right dark; the ears' fold) ----
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, 70), 1.5));
    p.drawLine(QPointF(1, 1), QPointF(W - 1, 1));
    p.drawLine(QPointF(1, 1), QPointF(1, kH - 1));
    p.setPen(QPen(QColor(255, 255, 255, 28), 1));
    p.drawLine(QPointF(3, 3.5), QPointF(W - 3, 3.5));
    p.setPen(QPen(QColor(0, 0, 0, 200), 1.5));
    p.drawLine(QPointF(1, kH - 1), QPointF(W - 1, kH - 1));
    p.drawLine(QPointF(W - 1, 1), QPointF(W - 1, kH - 1));
    for (const double x : {double(kEar), double(W - kEar)}) {
        p.setPen(QPen(QColor(0, 0, 0, 160), 1));
        p.drawLine(QPointF(x - .5, 2), QPointF(x - .5, kH - 2));
        p.setPen(QPen(QColor(255, 255, 255, 40), 1));
        p.drawLine(QPointF(x + .5, 2), QPointF(x + .5, kH - 2));
    }

    // brand and power
    p.setPen(QColor(0xf2, 0xf2, 0xf2));
    p.setFont(pxFont(17, QFont::Bold, 3.5));
    p.drawText(QPointF(54, 34), QStringLiteral("ZYDECK"));
    p.setPen(QColor(0x9a, 0x9a, 0x9a));
    p.setFont(pxFont(9, QFont::Medium, 3));
    p.drawText(QPointF(55, 47), QStringLiteral("CONNECT"));
    lamp(p, QRectF(262, 22, 8, 8), kGreen, true);
    p.setPen(QColor(0x77, 0x77, 0x77));
    p.setFont(pxFont(7, QFont::Bold, 1.2));
    p.drawText(QRectF(236, 33, 60, 10), Qt::AlignLeft, QStringLiteral("POWER"));

    // the readout
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(5, 8, 7));
    p.drawRoundedRect(kVfd, 3, 3);
    p.setPen(QPen(QColor(0, 0, 0), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(kVfd.adjusted(.5, .5, -.5, -.5), 3, 3);
    p.setFont(pxFont(11, QFont::DemiBold, 0.6, true));
    for (int i = 0; i < m_vfd.size(); ++i) {
        const QPointF at(kVfd.x() + 10, kVfd.y() + 21 + i * 19);
        QColor glow = kVfdInk;
        glow.setAlpha(55);
        p.setPen(glow);   // the phosphor's glow: the line a touch to each side
        p.drawText(at + QPointF(.7, 0), m_vfd[i]);
        p.drawText(at - QPointF(.7, 0), m_vfd[i]);
        p.setPen(i == 2 ? QColor(0x2b, 0x9f, 0x8a) : kVfdInk);
        p.drawText(at, m_vfd[i]);
    }

    // MIXXX: the plate standing out of the panel (its shadow at 45°), Mixxx's red rocker in it and a cable plugged in
    // beside it, out through the plate's foot. On and lit while Mixxx answers, on with its glow breathing while it
    // waits (for Mixxx, or for the user), off when it failed (MIXXX in red) or Mixxx isn't there. A press: set it up
    // again (as MAP did)
    {
        const QRectF plate = kMixxx;
        // on while Mixxx runs: lit when it answers, its glow breathing while it doesn't yet (or Mixxx is starting or
        // quitting); off and dark when it isn't running
        const QString state = m_mapLamp.isEmpty() ? setupLamp(m_setup->state()) : m_mapLamp;
        const bool on = m_setup->mixxxRunning();
        const double breathe = .35 + .3 * (1 + std::sin(m_clock.elapsed() / 260.0));
        const double glow = !m_mapLamp.isEmpty() ? breathe   // (just pressed: Mixxx starting or quitting)
                : !on ? 0
                : state == QLatin1String("ok") ? 1
                                               : breathe;
        p.setPen(Qt::NoPen);
        for (int i = 7; i >= 1; --i) {   // the shadow, down and to the right
            p.setBrush(QColor(0, 0, 0, 20));
            p.drawRoundedRect(plate.translated(i * .8, i * .8).adjusted(-i * .35, -i * .35, i * .35, i * .35), 5 + i * .3, 5 + i * .3);
        }
        QLinearGradient face(plate.topLeft(), plate.bottomLeft());
        face.setColorAt(0, QColor(0x3e, 0x40, 0x45));
        face.setColorAt(1, QColor(0x2d, 0x2f, 0x33));
        p.setBrush(face);
        p.drawRoundedRect(plate, 5, 5);
        QLinearGradient rim(plate.topLeft(), plate.bottomRight());   // its edge: lit top left, dark bottom right
        rim.setColorAt(0, QColor(255, 255, 255, 70));
        rim.setColorAt(.5, QColor(255, 255, 255, 10));
        rim.setColorAt(1, QColor(0, 0, 0, 140));
        p.setPen(QPen(QBrush(rim), 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(plate.adjusted(.6, .6, -.6, -.6), 5, 5);
        p.setPen(state == QLatin1String("err") ? kRed : QColor(0xc4, 0xc4, 0xc6));
        p.setFont(pxFont(8, QFont::Bold, 2.6));
        p.drawText(QPointF(plate.x() + 10, plate.y() + 14), QStringLiteral("MIXXX"));
        rocker(p, kMixxxRocker, on, glow, QColor(0xe8, 0x2a, 0x1f), m_down.key == Key::Map);

        // the cable: a socket with its plug in, the cable hanging from it and out through a grommet in the plate's foot
        p.save();
        p.setClipRect(plate.adjusted(1, 1, -1, 4));
        const QRectF plug(plate.x() + 52, plate.y() + 19, 38, 24);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x14, 0x15, 0x17));   // the socket's surround
        p.drawRoundedRect(plug.adjusted(-3, -3, 3, 3), 3, 3);
        QPainterPath body;   // the plug, its top corners cut (an IEC plug)
        body.moveTo(plug.left(), plug.top() + 6);
        body.lineTo(plug.left() + 6, plug.top());
        body.lineTo(plug.right() - 6, plug.top());
        body.lineTo(plug.right(), plug.top() + 6);
        body.lineTo(plug.right(), plug.bottom());
        body.lineTo(plug.left(), plug.bottom());
        body.closeSubpath();
        QLinearGradient shine(plug.topLeft(), plug.bottomLeft());
        shine.setColorAt(0, QColor(0x2e, 0x30, 0x33));
        shine.setColorAt(1, QColor(0x0d, 0x0d, 0x0e));
        p.setBrush(shine);
        p.drawPath(body);
        const double cx = plug.center().x();
        QPainterPath relief;   // the strain relief, narrowing to the cable
        relief.moveTo(cx - 8, plug.bottom());
        relief.lineTo(cx + 8, plug.bottom());
        relief.lineTo(cx + 4.5, plug.bottom() + 9);
        relief.lineTo(cx - 4.5, plug.bottom() + 9);
        relief.closeSubpath();
        p.setBrush(QColor(0x10, 0x10, 0x11));
        p.drawPath(relief);
        QPainterPath cable;
        cable.moveTo(cx, plug.bottom() + 8);
        cable.cubicTo(QPointF(cx, plug.bottom() + 14), QPointF(cx + 7, plate.bottom() - 6), QPointF(cx + 8, plate.bottom() + 3));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0x0b, 0x0b, 0x0c), 6, Qt::SolidLine, Qt::RoundCap));
        p.drawPath(cable);
        p.setPen(QPen(QColor(255, 255, 255, 32), 1.2, Qt::SolidLine, Qt::RoundCap));
        p.drawPath(cable.translated(-1.6, 0));
        p.restore();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x0a, 0x0a, 0x0b));   // the grommet
        p.drawRoundedRect(QRectF(cx + 2, plate.bottom() - 3, 12, 4), 2, 2);
    }

    // the unit's own POWER: a server's push button with its symbol lit (for show: it's on, and nothing else)
    {
        const QRectF k = kPowerKey;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 120));
        p.drawRoundedRect(k.translated(1.2, 1.6), 4, 4);
        QLinearGradient face(k.topLeft(), k.bottomLeft());
        face.setColorAt(0, QColor(0x2c, 0x2d, 0x30));
        face.setColorAt(1, QColor(0x17, 0x18, 0x1a));
        p.setBrush(face);
        p.drawRoundedRect(k, 4, 4);
        p.setPen(QPen(QColor(255, 255, 255, 30), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(k.adjusted(.5, .5, -.5, -.5), 4, 4);
        const QPointF c = k.center();
        const double r = k.width() * .27;
        const auto symbol = [&p, c, r](const QColor& ink, double width) {   // ⏻: a ring open at the top, a bar through it
            p.setPen(QPen(ink, width, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(QRectF(c.x() - r, c.y() - r + 1, 2 * r, 2 * r), 120 * 16, 300 * 16);
            p.drawLine(QPointF(c.x(), c.y() - r - 1.5), QPointF(c.x(), c.y() + 1));
        };
        for (const double w : {5.0, 3.6}) {   // its glow
            QColor halo = kGreen;
            halo.setAlpha(w > 4 ? 45 : 90);
            symbol(halo, w);
        }
        symbol(QColor(0xc8, 0xff, 0xdf), 1.6);
        p.setPen(QColor(0x80, 0x80, 0x82));
        p.setFont(pxFont(7, QFont::Bold, 1.2));
        p.drawText(QRectF(k.center().x() - 20, k.bottom() + 4, 40, 10), Qt::AlignHCenter, QStringLiteral("POWER"));
    }

    // SCAN
    {   // SCAN: a round push button (it doesn't latch), its word under it
        const bool down = m_down.key == Key::Scan;
        const QPointF c = kScan.center() + (down ? QPointF(1, 1) : QPointF());
        const double r = kScan.width() / 2;
        p.setPen(Qt::NoPen);
        if (!down) {   // its shadow on the panel
            p.setBrush(QColor(0, 0, 0, 110));
            p.drawEllipse(kScan.center() + QPointF(3, 4), r, r);
        }
        p.setBrush(QColor(5, 5, 5));   // the black ring
        p.drawEllipse(c, r, r);
        QRadialGradient face(c - QPointF(r * .35, r * .4), r * 1.3);
        face.setColorAt(0, down ? QColor(0x4a, 0x4d, 0x51) : QColor(0x60, 0x63, 0x68));
        face.setColorAt(1, down ? QColor(0x3c, 0x3e, 0x43) : QColor(0x36, 0x38, 0x3d));
        p.setBrush(face);
        p.drawEllipse(c, r - 3, r - 3);
        if (!down) {
            p.setPen(QPen(QColor(255, 255, 255, 45), 1));
            p.setBrush(Qt::NoBrush);
            p.drawArc(QRectF(c.x() - r + 5, c.y() - r + 5, 2 * r - 10, 2 * r - 10), 70 * 16, 80 * 16);
        }
        p.setPen(m_tagWanted ? kLamp : QColor(0x88, 0x88, 0x88));
        p.setFont(pxFont(8, QFont::Bold, 1.2));
        p.drawText(QRectF(kScan.x() - 12, kScan.bottom() + 3, kScan.width() + 24, 12), Qt::AlignHCenter, QStringLiteral("SCAN"));
    }

    // the four slots, stacked: each a bay with the device, then 30 px clear, then PAIR and the IN / OUT lamps
    QHash<int, Core::Device> inSlot;
    for (const Core::Device& d : m_core->devices()) {
        if (m_slotOf.contains(d.name)) {
            inSlot.insert(m_slotOf.value(d.name), d);
        }
    }
    static const QRegularExpression tablet(QStringLiteral("tab|pad|SM-[TPX]|tablet"), QRegularExpression::CaseInsensitiveOption);
    for (int i = 0; i < 4; ++i) {
        const QRectF b = bay(i);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x0d, 0x0e, 0x10));
        p.drawRoundedRect(b, 5, 5);
        p.setPen(QPen(QColor(0, 0, 0, 230), 2));   // recessed: dark along the top, a glint along the bottom
        p.drawLine(b.topLeft() + QPointF(4, 1), b.topRight() + QPointF(-4, 1));
        p.setPen(QPen(QColor(255, 255, 255, 16), 1));
        p.drawLine(b.bottomLeft() + QPointF(4, .5), b.bottomRight() + QPointF(-4, .5));
        p.setPen(QColor(0x7a, 0x7a, 0x7a));
        p.setFont(pxFont(11, QFont::DemiBold, 0, true));
        p.drawText(QRectF(b.x() + 12, b.y(), 14, b.height()), Qt::AlignVCenter, QString::number(i + 1));

        const bool linked = inSlot.contains(i);
        const QRectF k = slotKey(i);
        const double row = k.center().y();   // the cluster's lamps sit level with the key, their words with its word
        const QRectF text(b.x() + kNameX, b.y() + 6, k.left() - kNameGap - (b.x() + kNameX), 18);
        if (linked) {
            const Core::Device& d = inSlot[i];
            deviceIcon(p, QRectF(b.x() + 32, b.center().y() - 10, 20, 20), tablet.match(d.name).hasMatch());
            p.setPen(QColor(0xec, 0xec, 0xec));
            p.setFont(pxFont(13, QFont::DemiBold));
            p.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(d.name, Qt::ElideRight, int(text.width())));
            p.setPen(QColor(0x8f, 0x8f, 0x8f));
            p.setFont(pxFont(11));
            p.drawText(text.translated(0, 17), Qt::AlignLeft | Qt::AlignVCenter, d.how);
        } else if (m_foundIn.contains(i)) {   // found on the network, not linked: dimmed, PAIR asks it
            const QStringList f = m_foundIn.value(i);
            const bool asked = now - f.value(2).toLongLong() < 60000;
            p.setOpacity(.55);
            deviceIcon(p, QRectF(b.x() + 32, b.center().y() - 10, 20, 20), tablet.match(f.value(0)).hasMatch());
            p.setOpacity(1);
            p.setPen(QColor(0x9a, 0x9a, 0x9a));
            p.setFont(pxFont(13, QFont::DemiBold));
            p.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(f.value(0), Qt::ElideRight, int(text.width())));
            p.setPen(asked ? kLamp : QColor(0x6f, 0x6f, 0x6f));
            p.setFont(pxFont(11));
            p.drawText(text.translated(0, 17), Qt::AlignLeft | Qt::AlignVCenter,
                    asked ? tr("asked: say yes on it") : tr("found on Wi-Fi · PAIR links it"));
        } else {
            p.setPen(QColor(0x45, 0x46, 0x4a));
            p.setFont(pxFont(11, QFont::Normal, 1));
            p.drawText(QRectF(b.x() + kNameX, b.y(), text.width(), b.height()), Qt::AlignVCenter,
                    m_pairSlot == i && m_tagWanted ? tr("scan the tag…") : tr("empty"));
        }
        // PAIR: a small key (3:2) with a strip lamp: green while a device is in, blinking amber waiting for one
        const bool down = m_down.key == Key::Slot && m_down.slot == i && !linked;
        key(p, k, QString(), down);
        // (a found one's: steady amber, blinking once asked; waiting for the tag: blinking)
        const bool found = m_foundIn.contains(i), asked = found && now - m_foundIn.value(i).value(2).toLongLong() < 60000;
        const bool waiting = (m_pairSlot == i && m_tagWanted) || asked;
        const QRectF strip(k.center().x() - 6 + down, k.bottom() - 6.5 + down, 12, 3);
        lamp(p, strip, linked ? kGreen : kLamp, linked || (found && !asked) || (waiting && m_clock.elapsed() / 400 % 2 == 0), false);
        p.setPen(QColor(0x8a, 0x8a, 0x8a));
        p.setFont(pxFont(7, QFont::Bold, 1));
        p.drawText(QRectF(k.x() - 8, k.bottom() + 3, k.width() + 16, 10), Qt::AlignHCenter, QStringLiteral("PAIR"));
        // MIDI IN (from it) and OUT (to it): lit for a moment with each message
        const QRectF in(b.right() - 52, row - 4, 8, 8), out(b.right() - 28, row - 4, 8, 8);
        lamp(p, in, kLamp, linked && now - inSlot[i].lastIn < 160);
        lamp(p, out, kGreen, linked && now - inSlot[i].lastOut < 160);
        p.setPen(QColor(0x66, 0x66, 0x66));
        p.drawText(QRectF(in.center().x() - 14, k.bottom() + 3, 28, 10), Qt::AlignHCenter, QStringLiteral("IN"));
        p.drawText(QRectF(out.center().x() - 14, k.bottom() + 3, 28, 10), Qt::AlignHCenter, QStringLiteral("OUT"));
    }
}

} // namespace zc
