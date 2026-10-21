#include "window.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWindow>
#include <QSet>
#include <algorithm>
#include <cmath>

#include "core.h"
#include "library.h"
#include "qrcodegen.hpp"
#include "system.h"

namespace zc {

namespace {
// The unit, in logical pixels: the ears, the face, the tag under it. Its width follows the device names (layoutWidth).
constexpr int kMinW = 700, kMaxW = 1000, kH = 236, kEar = 30;
constexpr int kTagW = 196, kTagH = 254, kTagX = 66;   // under the left column
// how it comes out (ms): pulled until its inner corner catches and it slants (to 300), held there (to 500), pushed back
// a little (to 700), pulled again, straight at 1000; back in, a plain slide
constexpr double kTravel = kTagH - 6, kCatchMs = 300, kHoldMs = 500, kBackMs = 700, kOutMs = 1000, kInMs = 400;
constexpr double kSnag = 14, kPushBack = 12, kSlant = -7.5;   // px short of out when it catches; px back; degrees
double easeOut(double u) { return 1 - std::pow(1 - u, 3); }
double easeInOut(double u) { return u < .5 ? 4 * u * u * u : 1 - std::pow(-2 * u + 2, 3) / 2; }
double smooth(double a, double b, double u) { const double t = std::clamp((u - a) / (b - a), 0.0, 1.0); return t * t * (3 - 2 * t); }
const QRectF kVfd(54, 58, 220, 74);
const QRectF kMap(54, 150, 120, 42), kLogin(212, 148, 42, 46);
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
void processLamp(QPainter& p, const QPointF& at, const QString& state, qint64 ms) {
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x38, 0x3a, 0x3e));
    p.drawRect(QRectF(at, QSizeF(16, 16)));
    static const int order[4] = {0, 1, 3, 2};   // clockwise
    const int lit = static_cast<int>(ms / 300 % 4);
    for (int i = 0; i < 4; ++i) {
        const QRectF cell(at.x() + 2 + (i % 2) * 6.5, at.y() + 2 + (i / 2) * 6.5, 5.5, 5.5);
        const bool on = state == QLatin1String("ok") || state == QLatin1String("err") ||
                (state == QLatin1String("busy") && order[lit] == i);
        lamp(p, cell, state == QLatin1String("err") ? kRed : state == QLatin1String("ok") ? kGreen : kLamp, on, false);
    }
    p.restore();
}

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

QIcon Window::lampIcon(const QColor& c) const {
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x38, 0x3a, 0x3e));
    p.drawRoundedRect(QRectF(2, 2, 28, 28), 4, 4);
    p.setBrush(c);
    for (int i = 0; i < 4; ++i) {
        p.drawRect(QRectF(5 + (i % 2) * 12, 5 + (i / 2) * 12, 10, 10));
    }
    return QIcon(pm);
}


Window::Window(Core* core)
        : m_core(core),
          m_w(kMinW) {
    setWindowTitle(QString::fromLatin1(kAppName));
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(m_w, kH);
    setMouseTracking(true);
    m_clock.start();

    m_tagAnim.setStartValue(0.0);
    connect(&m_tagAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) { tagPose(v.toDouble()); });
    connect(&m_tagAnim, &QVariantAnimation::finished, this, [this] {
        if (!m_tagWanted) {
            setFixedSize(m_w, kH);   // (all in: the window ends at the unit again)
        }
    });

    if (QSystemTrayIcon::isSystemTrayAvailable()) {   // the menu bar (the tray on Windows and Linux)
        m_tray = new QSystemTrayIcon(this);
        m_menu = new QMenu(this);
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
    auto* frame = new QTimer(this);   // the lamps that move: MIDI activity, a busy MAP, a slot waiting to pair
    connect(frame, &QTimer::timeout, this, [this] {
        if (isVisible()) {
            update();
        }
    });
    frame->start(80);
    connect(m_core, &Core::changed, this, &Window::refresh);
    refresh();
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
        setFixedSize(m_w, kH + kTagH + 30);   // (room for it, slanted too; the rest of the window is see-through)
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
        setFixedSize(m_w, height());
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

    if (!m_tracksAt.isValid() || m_tracksAt.elapsed() > 5000) {
        m_tracks = mixxxDir().isEmpty() ? -1 : m_core->library()->trackCount();
        m_tracksAt.start();
    }
    const QStringList hosts = lanAddresses();
    m_vfd = {problem ? QStringLiteral("MIDI  NO PORT: LOOPMIDI?") : QStringLiteral("MIDI  ZYDECK CONNECT  OK"),
             m_tracks < 0 ? QStringLiteral("MIXXX NOT FOUND")
                          : QStringLiteral("MIXXX %1 TRK  MAP %2").arg(m_tracks).arg(mappingInstalled() ? "OK" : "--"),
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
    if (!m_tray) {
        return;
    }
    const QList<Core::Device> devices = m_core->devices();
    const bool problem = !m_core->midi()->problem().isEmpty();
    m_tray->setIcon(lampIcon(problem ? kRed : devices.isEmpty() ? kLamp : kGreen));
    m_tray->setToolTip(devices.isEmpty() ? tr("ZyDeck Connect: no device yet")
                                         : tr("ZyDeck Connect: %n device(s)", nullptr, static_cast<int>(devices.size())));
    QStringList lines;
    for (const Core::Device& d : devices) {
        lines << d.name + " · " + d.how;
    }
    const QString menuFor = lines.join('\n') + (openAtLogin() ? "+" : "-");
    if (menuFor == m_menuFor) {   // (not rebuilt under an open menu)
        return;
    }
    m_menuFor = menuFor;
    m_menu->clear();
    m_menu->addAction(devices.isEmpty() ? tr("ZyDeck Connect · waiting for a device") : tr("ZyDeck Connect · ready"))->setEnabled(false);
    for (const QString& l : std::as_const(lines)) {
        m_menu->addAction("   " + l)->setEnabled(false);
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
    m_menu->addAction(tr("Install Mapping in Mixxx"), this, [this] { press({Key::Map, -1}); });
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

void Window::press(const Hit& h) {
    switch (h.key) {
    case Key::Map:   // install the ZyDeck mapping in Mixxx
        m_mapLamp = QStringLiteral("busy");
        update();
        QTimer::singleShot(600, this, [this] {   // (long enough to see it work)
            try {
                m_core->status = tr("Installed in %1. Restart Mixxx if it's open.").arg(installMapping());
                m_mapLamp = QStringLiteral("ok");
            } catch (const QString& why) {
                m_core->status = tr("Couldn't install the mapping: %1").arg(why);
                m_mapLamp = QStringLiteral("err");
            }
            setToolTip(m_core->status);
            refresh();
            QTimer::singleShot(4000, this, [this] {
                m_mapLamp.clear();
                update();
            });
        });
        break;
    case Key::Slot:   // an empty slot's PAIR: the tag out for it; again, back in
        if (m_slotOf.key(h.slot).isEmpty()) {
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
    case Key::Login:
        setOpenAtLogin(!openAtLogin());
        m_menuFor.clear();
        refresh();
        break;
    case Key::Close:
        close();
        break;
    case Key::None:
        break;
    }
}

Window::Hit Window::hitAt(const QPointF& p) const {
    if (kMap.contains(p)) {
        return {Key::Map, -1};
    }
    if (kLogin.contains(p)) {
        return {Key::Login, -1};
    }
    if (QRectF(m_w - kEar + 3, 3, 24, 24).contains(p)) {
        return {Key::Close, -1};
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
    if (m_down.key == Key::None && e->button() == Qt::LeftButton && windowHandle()) {
        windowHandle()->startSystemMove();   // the panel: drag the unit
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

void Window::mouseMoveEvent(QMouseEvent* e) {
    const Hit h = hitAt(e->position());
    const bool live = h.key != Key::None && !(h.key == Key::Slot && !m_slotOf.key(h.slot).isEmpty());
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

    // ---- the tag, behind the unit: out of the slit in its bottom edge. Black, with a white sticker (the code, what
    // to do with it, the address) as a real one would have; slanted about its top inner corner while it snags ----
    if (m_drop > 0.5) {
        p.save();
        p.setClipRect(QRectF(0, kH - 4, W, height()));
        const QRectF tag = tagRect();
        p.translate(tag.topLeft());
        p.rotate(m_angle);
        p.translate(-tag.topLeft());
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
    const QRectF closeX(W - kEar + 7, 7, 16, 16);   // close: an engraved x on the right ear
    p.setPen(QPen(m_down.key == Key::Close ? QColor(0xdd, 0xdd, 0xdd) : QColor(0x77, 0x79, 0x7d), 1.6));
    p.drawLine(closeX.topLeft() + QPointF(4, 4), closeX.bottomRight() - QPointF(4, 4));
    p.drawLine(closeX.topRight() + QPointF(-4, 4), closeX.bottomLeft() + QPointF(4, -4));

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

    // MAP and LOGIN
    const bool mapDown = m_down.key == Key::Map;
    key(p, kMap, QStringLiteral("MAP"), mapDown);
    processLamp(p, kMap.topLeft() + QPointF(kMap.width() - 25 + mapDown, 13 + mapDown), m_mapLamp, m_clock.elapsed());
    {   // LOGIN: a bat switch, up is on
        const bool on = openAtLogin();
        const QPointF c(kLogin.center().x(), kLogin.y() + 18);
        QRadialGradient plate(c, 13);
        plate.setColorAt(0, QColor(0x8a, 0x8c, 0x90));
        plate.setColorAt(1, QColor(0x3a, 0x3c, 0x40));
        p.setPen(QPen(QColor(8, 8, 8), 1.5));
        p.setBrush(plate);
        p.drawEllipse(c, 11, 11);
        QLinearGradient bat(c - QPointF(4, 0), c + QPointF(4, 0));
        bat.setColorAt(0, QColor(0xf0, 0xf0, 0xf2));
        bat.setColorAt(1, QColor(0x9a, 0x9c, 0xa0));
        p.setBrush(bat);
        p.setPen(QPen(QColor(20, 20, 22), 1));
        p.drawRoundedRect(on ? QRectF(c.x() - 3.5, c.y() - 17, 7, 18) : QRectF(c.x() - 3.5, c.y() - 1, 7, 18), 3.5, 3.5);
        p.setPen(on ? kLamp : QColor(0x88, 0x88, 0x88));
        p.setFont(pxFont(8, QFont::Bold, 1.2));
        p.drawText(QRectF(kLogin.x() - 10, kLogin.bottom() + 1, kLogin.width() + 20, 12), Qt::AlignHCenter, QStringLiteral("LOGIN"));
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
        } else {
            p.setPen(QColor(0x45, 0x46, 0x4a));
            p.setFont(pxFont(11, QFont::Normal, 1));
            p.drawText(QRectF(b.x() + kNameX, b.y(), text.width(), b.height()), Qt::AlignVCenter,
                    m_pairSlot == i && m_tagWanted ? tr("scan the tag…") : tr("empty"));
        }
        // PAIR: a small key (3:2) with a strip lamp: green while a device is in, blinking amber waiting for one
        const bool down = m_down.key == Key::Slot && m_down.slot == i && !linked;
        key(p, k, QString(), down);
        const bool waiting = m_pairSlot == i && m_tagWanted;
        const QRectF strip(k.center().x() - 6 + down, k.bottom() - 6.5 + down, 12, 3);
        lamp(p, strip, linked ? kGreen : kLamp, linked || (waiting && m_clock.elapsed() / 400 % 2 == 0), false);
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
