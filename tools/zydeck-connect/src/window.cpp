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
#include <cmath>

#include "core.h"
#include "library.h"
#include "qrcodegen.hpp"
#include "system.h"

namespace zc {

namespace {
// The unit, in logical pixels: the ears, the face, the tag under it
constexpr int kW = 700, kH = 236, kEar = 30;
constexpr int kTagW = 196, kTagH = 254, kTagX = 66;   // under the left column
const QRectF kVfd(54, 58, 220, 74);
const QRectF kMap(54, 150, 80, 42), kPair(142, 150, 80, 42), kLogin(232, 148, 42, 46);
const QRectF kClose(kW - kEar + 7, 7, 16, 16);
constexpr double kBayX = 298, kBayW = kW - kEar - 22 - kBayX, kBayH = 46, kBayGap = 8, kBayY = 16;

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
        : m_core(core) {
    setWindowTitle(QString::fromLatin1(kAppName));
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(kW, kH);
    setMouseTracking(true);
    m_clock.start();

    m_tagAnim.setDuration(520);
    connect(&m_tagAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_tag = v.toDouble();
        setFixedSize(kW, kH + static_cast<int>(std::ceil(m_tag * (kTagH - 6))));
        update();
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
    auto* frame = new QTimer(this);   // the lamps that move: MIDI activity, a busy MAP
    connect(frame, &QTimer::timeout, this, [this] {
        if (isVisible()) {
            update();
        }
    });
    frame->start(80);
    connect(m_core, &Core::changed, this, &Window::refresh);
    refresh();
}

void Window::showPairingCode() {
    m_tagManual = 1;
    setTag(true);
}

void Window::setTag(bool out) {
    if (out == m_tagWanted && (m_tagAnim.state() == QAbstractAnimation::Running || m_tag == (out ? 1 : 0))) {
        return;
    }
    m_tagWanted = out;
    m_tagAnim.stop();
    m_tagAnim.setStartValue(m_tag);
    m_tagAnim.setEndValue(out ? 1.0 : 0.0);
    m_tagAnim.setEasingCurve(out ? QEasingCurve::OutCubic : QEasingCurve::InOutCubic);
    m_tagAnim.start();
}

void Window::refresh() {
    const QList<Core::Device> devices = m_core->devices();
    // the tag: out while nothing's linked, in once something is (PAIR overrides until that changes)
    if (devices.size() != m_lastDeviceCount) {
        if (m_lastDeviceCount >= 0) {
            m_tagManual = -1;
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
    m_menu->addAction(tr("Install Mapping in Mixxx"), this, [this] { press(Key::Map); });
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

void Window::press(Key k) {
    switch (k) {
    case Key::Map:
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
    case Key::Pair:
        m_tagManual = m_tagWanted ? 0 : 1;
        setTag(!m_tagWanted);
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

Window::Key Window::keyAt(const QPointF& p) const {
    if (kMap.contains(p)) {
        return Key::Map;
    }
    if (kPair.contains(p)) {
        return Key::Pair;
    }
    if (kLogin.contains(p)) {
        return Key::Login;
    }
    if (kClose.adjusted(-4, -4, 4, 4).contains(p)) {
        return Key::Close;
    }
    return Key::None;
}

void Window::mousePressEvent(QMouseEvent* e) {
    m_down = keyAt(e->position());
    if (m_down == Key::None && e->button() == Qt::LeftButton && windowHandle()) {
        windowHandle()->startSystemMove();   // the panel: drag the unit
    }
    update();
}

void Window::mouseReleaseEvent(QMouseEvent* e) {
    const Key k = m_down;
    m_down = Key::None;
    if (k != Key::None && keyAt(e->position()) == k) {
        press(k);
    }
    update();
}

void Window::mouseMoveEvent(QMouseEvent* e) {
    setCursor(keyAt(e->position()) == Key::None ? Qt::ArrowCursor : Qt::PointingHandCursor);
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

    // ---- the tag, behind the unit: out of the slit in its bottom edge ----
    if (m_tag > 0.001) {
        p.save();
        p.setClipRect(QRectF(0, kH - 4, kW, height()));
        const double y = kH - kTagH + m_tag * (kTagH - 6);
        const QRectF tag(kTagX, y, kTagW, kTagH);
        QPainterPath body;
        body.addRoundedRect(tag, 7, 7);
        QLinearGradient metal(tag.topLeft(), tag.topRight());
        metal.setColorAt(0, QColor(0xc9, 0xcb, 0xce));
        metal.setColorAt(.5, QColor(0xe2, 0xe3, 0xe5));
        metal.setColorAt(1, QColor(0xb8, 0xba, 0xbe));
        p.setPen(QPen(QColor(0x6e, 0x70, 0x74), 1));
        p.setBrush(metal);
        p.drawPath(body);
        // the printed label: the code, what to do with it, this computer's address
        const QRectF label(tag.x() + 14, tag.y() + 16, kTagW - 28, kTagW - 28);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawRoundedRect(label, 3, 3);
        if (!m_qr.isNull()) {
            const QRectF code = label.adjusted(8, 8, -8, -8);
            p.setRenderHint(QPainter::SmoothPixmapTransform, false);
            p.drawPixmap(code, m_qr, m_qr.rect());
        }
        p.setPen(QColor(0x22, 0x23, 0x26));
        p.setFont(pxFont(10, QFont::Bold, 1.2));
        p.drawText(QRectF(tag.x(), label.bottom() + 6, kTagW, 14), Qt::AlignHCenter, tr("SCAN IN ZYDECK"));
        p.setFont(pxFont(10));
        p.drawText(QRectF(tag.x(), label.bottom() + 20, kTagW, 14), Qt::AlignHCenter, tr("Settings › Pair by QR"));
        p.setPen(QColor(0x55, 0x57, 0x5b));
        p.setFont(pxFont(9, QFont::Normal, 0, true));
        p.drawText(QRectF(tag.x(), label.bottom() + 34, kTagW, 13), Qt::AlignHCenter, m_vfd.value(2));
        // the finger pull
        p.setPen(QPen(QColor(0x8a, 0x8c, 0x90), 1));
        p.setBrush(QColor(0x9c, 0x9e, 0xa2));
        p.drawRoundedRect(QRectF(tag.center().x() - 22, tag.bottom() - 15, 44, 7), 3.5, 3.5);
        p.restore();
    }

    // ---- the unit ----
    // ears with their slots
    for (const double x : {0.0, double(kW - kEar)}) {
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
    // the face: brushed, lit from the top
    const QRectF face(kEar, 0, kW - 2 * kEar, kH);
    p.setBrush(QColor(0x1b, 0x1c, 0x1f));
    p.drawRect(face);
    p.setPen(QPen(QColor(255, 255, 255, 6), 1));
    for (double x = face.left() + 1; x < face.right(); x += 3) {
        p.drawLine(QPointF(x, 1), QPointF(x, kH - 1));
    }
    p.setPen(QPen(QColor(255, 255, 255, 22), 1));
    p.drawLine(QPointF(0, 0.5), QPointF(kW, 0.5));
    p.setPen(QPen(QColor(0, 0, 0), 1));
    p.drawLine(QPointF(0, kH - 0.5), QPointF(kW, kH - 0.5));
    // the slit the tag comes out of
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(4, 4, 4));
    p.drawRoundedRect(QRectF(kTagX - 4, kH - 7, kTagW + 8, 5), 2, 2);
    // close: an engraved x on the right ear
    p.setPen(QPen(m_down == Key::Close ? QColor(0xdd, 0xdd, 0xdd) : QColor(0x77, 0x79, 0x7d), 1.6));
    p.drawLine(kClose.topLeft() + QPointF(4, 4), kClose.bottomRight() - QPointF(4, 4));
    p.drawLine(kClose.topRight() + QPointF(-4, 4), kClose.bottomLeft() + QPointF(4, -4));

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

    // MAP, PAIR, LOGIN
    key(p, kMap, QStringLiteral("MAP"), m_down == Key::Map);
    processLamp(p, kMap.topLeft() + QPointF(kMap.width() - 25 + (m_down == Key::Map), 13 + (m_down == Key::Map)), m_mapLamp,
            m_clock.elapsed());
    key(p, kPair, QStringLiteral("PAIR"), m_down == Key::Pair);
    lamp(p, QRectF(kPair.right() - 22 + (m_down == Key::Pair), kPair.y() + 14 + (m_down == Key::Pair), 9, 14), kLamp,
            m_tagWanted, false);
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
        const QRectF lever = on ? QRectF(c.x() - 3.5, c.y() - 17, 7, 18) : QRectF(c.x() - 3.5, c.y() - 1, 7, 18);
        p.drawRoundedRect(lever, 3.5, 3.5);
        p.setPen(on ? kLamp : QColor(0x88, 0x88, 0x88));
        p.setFont(pxFont(8, QFont::Bold, 1.2));
        p.drawText(QRectF(kLogin.x() - 10, kLogin.bottom() + 1, kLogin.width() + 20, 12), Qt::AlignHCenter, QStringLiteral("LOGIN"));
    }

    // the four slots, stacked: each a bay with the device in it
    const QList<Core::Device> devices = m_core->devices();
    for (int i = 0; i < 4; ++i) {
        const QRectF bay(kBayX, kBayY + i * (kBayH + kBayGap), kBayW, kBayH);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x0d, 0x0e, 0x10));
        p.drawRoundedRect(bay, 5, 5);
        p.setPen(QPen(QColor(0, 0, 0, 230), 2));   // recessed: dark along the top, a glint along the bottom
        p.drawLine(bay.topLeft() + QPointF(4, 1), bay.topRight() + QPointF(-4, 1));
        p.setPen(QPen(QColor(255, 255, 255, 16), 1));
        p.drawLine(bay.bottomLeft() + QPointF(4, .5), bay.bottomRight() + QPointF(-4, .5));
        p.setPen(QColor(0x7a, 0x7a, 0x7a));
        p.setFont(pxFont(11, QFont::DemiBold, 0, true));
        p.drawText(QRectF(bay.x() + 12, bay.y(), 14, bay.height()), Qt::AlignVCenter, QString::number(i + 1));
        if (i >= devices.size()) {
            p.setPen(QColor(0x45, 0x46, 0x4a));
            p.setFont(pxFont(11, QFont::Normal, 1));
            p.drawText(QRectF(bay.x() + 64, bay.y(), 200, bay.height()), Qt::AlignVCenter, tr("empty"));
            lamp(p, QRectF(bay.right() - 48, bay.center().y() - 4, 8, 8), kLamp, false);
            lamp(p, QRectF(bay.right() - 24, bay.center().y() - 4, 8, 8), kGreen, false);
            continue;
        }
        const Core::Device& d = devices[i];
        static const QRegularExpression tablet(QStringLiteral("tab|pad|SM-[TPX]|tablet"), QRegularExpression::CaseInsensitiveOption);
        deviceIcon(p, QRectF(bay.x() + 32, bay.center().y() - 10, 20, 20), tablet.match(d.name).hasMatch());
        p.setPen(QColor(0xec, 0xec, 0xec));
        p.setFont(pxFont(13, QFont::DemiBold));
        const QRectF text(bay.x() + 64, bay.y() + 6, bay.width() - 64 - 70, 18);
        p.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(d.name, Qt::ElideRight, int(text.width())));
        p.setPen(QColor(0x8f, 0x8f, 0x8f));
        p.setFont(pxFont(11));
        p.drawText(text.translated(0, 17), Qt::AlignLeft | Qt::AlignVCenter, d.how);
        // MIDI IN (from it) and OUT (to it): lit for a moment with each message
        lamp(p, QRectF(bay.right() - 48, bay.center().y() - 6, 8, 8), kLamp, now - d.lastIn < 160);
        lamp(p, QRectF(bay.right() - 24, bay.center().y() - 6, 8, 8), kGreen, now - d.lastOut < 160);
        p.setPen(QColor(0x66, 0x66, 0x66));
        p.setFont(pxFont(7, QFont::Bold, .8));
        p.drawText(QRectF(bay.right() - 58, bay.center().y() + 5, 28, 10), Qt::AlignHCenter, QStringLiteral("IN"));
        p.drawText(QRectF(bay.right() - 34, bay.center().y() + 5, 28, 10), Qt::AlignHCenter, QStringLiteral("OUT"));
    }
}

} // namespace zc
