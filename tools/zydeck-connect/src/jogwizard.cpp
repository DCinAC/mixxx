#include "jogwizard.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStyleFactory>
#include <QTimer>
#include <QVBoxLayout>
#include <QWizard>
#include <QWizardPage>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <memory>
#include <vector>

#include "RtMidi.h"
#include "library.h"   // (mixxxDir)
#include "midi.h"      // (kPortName)
#include "midiin.h"

namespace zc {

namespace {

// ---- what's measured ------------------------------------------------------------------------------------------

// How a jog sends its steps: as a signed 7-bit number (1 = one step one way, 127 = one the other: most), around 64
// (65 / 63), or as where it is (0-127, wrapping round: the step is the difference)
enum class Enc { Twos, Offset, Absolute };

struct Session {
    QString port;
    bool found = false;
    quint8 st = 0, cc = 0;   // its steps: this control change
    Enc enc = Enc::Twos;
    bool hasTouch = false;   // a touch-sensitive top: this note on as it's touched, off as it's let go
    quint8 touchSt = 0, touchNote = 0;
    int lastAbs = -1;
    QList<int> turns;        // steps in each full turn
    int maxStep = 0;         // the most steps in one message (turned fast)

    int step(quint8 v) {
        switch (enc) {
        case Enc::Twos:
            return v < 64 ? v : v - 128;
        case Enc::Offset:
            return v - 64;
        case Enc::Absolute:
            break;
        }
        if (lastAbs < 0) {
            lastAbs = v;
            return 0;
        }
        const int d = ((v - lastAbs + 64) & 127) - 64;
        lastAbs = v;
        return d;
    }
    bool isSteps(quint8 status, quint8 d1) const { return found && status == st && d1 == cc; }
    bool isTouch(quint8 status, quint8 d1) const {
        return hasTouch && d1 == touchNote && ((status & 0x0F) == (touchSt & 0x0F)) && ((status & 0xF0) == 0x90 || (status & 0xF0) == 0x80);
    }
    int measured() const {   // the middle one of the turns
        if (turns.isEmpty()) {
            return 0;
        }
        QList<int> t = turns;
        std::sort(t.begin(), t.end());
        return t[t.size() / 2];
    }
    // What the wheel really has: a hand stops a little short or long, so a round number of steps close by (within
    // 2 %) is the likelier answer
    int stepsPerTurn() const {
        const int m = measured();
        static const int kRound[] = {128, 200, 250, 256, 300, 360, 400, 480, 500, 512, 600, 720, 800, 900, 1000, 1024,
                1200, 1440, 2000, 2048, 2400, 3600, 4096};
        int best = m;
        double bestOff = 0.02;
        for (int r : kRound) {
            const double off = std::abs(m - r) / double(r);
            if (off <= bestOff) {
                best = r;
                bestOff = off;
            }
        }
        return best;
    }
};

// The controller's MIDI input, read only (midiin.h)
using Input = MidiReader;

// ---- the look ----------------------------------------------------------------------------------------------------

const QColor kInk(0x10, 0x10, 0x10);
const QColor kBlueDeep(0x0a, 0x24, 0x6a), kBlue(0x31, 0x5d, 0xc6), kBlueLight(0xa6, 0xc0, 0xf2);
const QColor kGreen(0x2f, 0xb3, 0x3a), kRed(0xd2, 0x2b, 0x1e);

// The side picture of the first and last pages: a turntable in a blue sky
QPixmap watermark() {
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap px(QSize(164, 440) * dpr);
    px.setDevicePixelRatio(dpr);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient sky(0, 0, 0, 440);
    sky.setColorAt(0, kBlueDeep);
    sky.setColorAt(0.55, kBlue);
    sky.setColorAt(1, kBlueLight);
    p.fillRect(QRect(0, 0, 164, 440), sky);
    p.setPen(QPen(QColor(255, 255, 255, 40), 1));   // soft rings, as light on glass
    for (int r = 40; r < 260; r += 22) {
        p.drawEllipse(QPointF(150, 70), r, r);
    }
    // the deck, seen from above, slightly from the side
    p.save();
    p.translate(82, 300);
    p.scale(1, 0.62);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 70));
    p.drawEllipse(QPointF(4, 10), 66, 66);
    QRadialGradient metal(QPointF(-20, -24), 90);
    metal.setColorAt(0, QColor(0xf4, 0xf4, 0xf4));
    metal.setColorAt(1, QColor(0x8a, 0x92, 0x9e));
    p.setBrush(metal);
    p.drawEllipse(QPointF(0, 0), 64, 64);
    p.setBrush(QColor(0x18, 0x18, 0x1a));
    p.drawEllipse(QPointF(0, 0), 56, 56);
    p.setPen(QPen(QColor(255, 255, 255, 22), 1));
    p.setBrush(Qt::NoBrush);
    for (int r = 24; r < 55; r += 4) {
        p.drawEllipse(QPointF(0, 0), r, r);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xf0, 0x9a, 0x2a));
    p.drawEllipse(QPointF(0, 0), 20, 20);
    p.setPen(QPen(Qt::white, 3, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(0, -4), QPointF(0, -52));   // the mark
    p.restore();
    p.setPen(Qt::white);
    QFont f = qApp->font();
    f.setBold(true);
    f.setPixelSize(15);
    p.setFont(f);
    p.drawText(QRect(14, 18, 140, 60), Qt::AlignLeft | Qt::TextWordWrap, QObject::tr("Jog Wheel\nSetup"));
    return px;
}

// The small picture at the right of the white band: a jog wheel
QPixmap logo() {
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap px(QSize(49, 49) * dpr);
    px.setDevicePixelRatio(dpr);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    QRadialGradient metal(QPointF(18, 16), 34);
    metal.setColorAt(0, QColor(0xfa, 0xfa, 0xfa));
    metal.setColorAt(1, QColor(0x7c, 0x84, 0x90));
    p.setPen(QPen(QColor(0x50, 0x56, 0x60), 1));
    p.setBrush(metal);
    p.drawEllipse(QPointF(24.5, 24.5), 21, 21);
    p.setBrush(QColor(0x22, 0x22, 0x26));
    p.drawEllipse(QPointF(24.5, 24.5), 15, 15);
    p.setPen(QPen(QColor(0xf0, 0x9a, 0x2a), 3, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(QRectF(13, 13, 23, 23), 40 * 16, 110 * 16);   // turning
    p.setPen(QPen(Qt::white, 2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(24.5, 24.5), QPointF(24.5, 11));
    return px;
}

// The hand: a pointer of the old pixel kind (an open hand), filled blue. Drawn from its cells: the outline is every
// cell beside the hand
void paintHand(QPainter& p, QPointF at, qreal cell) {
    static const char* const kRows[] = {
            "      XX        ",
            "      XX XX     ",
            "   XX XX XX     ",
            "   XX XX XX XX  ",
            "   XX XX XX XX  ",
            "   XX XX XX XX  ",
            "   XX XX XX XX  ",
            "   XX XX XX XX  ",
            "   XX XX XX XX  ",
            "XX XXXXXXXXXXX  ",
            "XXXXXXXXXXXXXX  ",
            " XXXXXXXXXXXXX  ",
            " XXXXXXXXXXXXX  ",
            "  XXXXXXXXXXXX  ",
            "  XXXXXXXXXXXX  ",
            "   XXXXXXXXXXX  ",
            "   XXXXXXXXXXX  ",
            "    XXXXXXXXXX  ",
            "     XXXXXXXX   ",
    };
    const int rows = int(std::size(kRows)), cols = 16;
    const auto in = [&](int x, int y) { return y >= 0 && y < rows && x >= 0 && x < cols && kRows[y][x] == 'X'; };
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    for (int y = -1; y <= rows; ++y) {
        for (int x = -1; x <= cols; ++x) {
            const QRectF r(at.x() + (x + 1) * cell, at.y() + (y + 1) * cell, cell, cell);
            if (in(x, y)) {
                const double t = y / double(rows - 1);   // light at the fingertips, deep at the wrist
                p.fillRect(r, QColor::fromRgbF(0.80 - 0.62 * t, 0.88 - 0.50 * t, 1.0 - 0.15 * t));
            } else if (in(x - 1, y) || in(x + 1, y) || in(x, y - 1) || in(x, y + 1)) {
                p.fillRect(r, QColor(0x0b, 0x1f, 0x4f));
            }
        }
    }
    p.restore();
}

// The jog wheel as it's being turned: the red arrow is where the mark starts and ends (12 o'clock); the drawn mark
// turns with the steps once one turn's steps are known
class Platter : public QWidget {
  public:
    // The hand showing what to do (two frames, up and down: turning it): on its edge, or on its top
    enum class Hand { None, Edge, Top };
    explicit Platter(QWidget* parent = nullptr)
            : QWidget(parent) {
        m_tick.setInterval(450);
        connect(&m_tick, &QTimer::timeout, this, [this] {
            m_frame ^= 1;
            update();
        });
    }
    void setHand(Hand h) {
        if (h == m_hand) {
            return;
        }
        m_hand = h;
        h == Hand::None ? m_tick.stop() : m_tick.start();
        update();
    }
    Hand hand() const { return m_hand; }
    double angle = 0;        // degrees, clockwise
    bool showMark = false;   // known how far a step is
    bool active = false;     // being touched / turned
    QString big, small;
    QSize sizeHint() const override { return {240, 236}; }
    QSize minimumSizeHint() const override { return {200, 200}; }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal side = std::min(width(), height() - 44);
        const QPointF c(width() / 2.0, 14 + side / 2.0);
        const qreal R = side / 2.0 - 12;
        // the red arrow at 12 o'clock
        QPainterPath arrow;
        arrow.moveTo(c.x() - 8, c.y() - R - 13);
        arrow.lineTo(c.x() + 8, c.y() - R - 13);
        arrow.lineTo(c.x(), c.y() - R - 1);
        arrow.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(kRed);
        p.drawPath(arrow);
        // the ring (with a strobe's dots) and the record
        QRadialGradient metal(c - QPointF(R * 0.35, R * 0.4), R * 1.5);
        metal.setColorAt(0, QColor(0xfb, 0xfb, 0xfb));
        metal.setColorAt(1, QColor(0x86, 0x8e, 0x9a));
        p.setPen(QPen(QColor(0x5a, 0x60, 0x6a), 1));
        p.setBrush(metal);
        p.drawEllipse(c, R, R);
        p.save();
        p.translate(c);
        p.rotate(showMark ? angle : 0);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x5a, 0x60, 0x6a));
        for (int i = 0; i < 72; ++i) {
            p.save();
            p.rotate(i * 5);
            p.drawEllipse(QPointF(0, -R + 4.5), 1.6, 1.6);
            p.restore();
        }
        p.setBrush(QColor(0x17, 0x17, 0x19));
        p.drawEllipse(QPointF(0, 0), R - 10, R - 10);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, 18), 1));
        for (qreal r = R * 0.42; r < R - 12; r += 3.5) {
            p.drawEllipse(QPointF(0, 0), r, r);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(active ? QColor(0xf5, 0xa6, 0x32) : QColor(0xe0, 0x8c, 0x22));
        p.drawEllipse(QPointF(0, 0), R * 0.36, R * 0.36);
        if (showMark) {
            p.setPen(QPen(Qt::white, 4, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(0, -R * 0.12), QPointF(0, -(R - 13)));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xd8, 0xd8, 0xd8));
        p.drawEllipse(QPointF(0, 0), 3.5, 3.5);
        p.restore();
        // the count
        p.setPen(kInk);
        QFont f = font();
        f.setBold(true);
        f.setPixelSize(20);
        p.setFont(f);
        p.drawText(QRectF(0, c.y() + R + 6, width(), 24), Qt::AlignHCenter | Qt::AlignTop, big);
        f.setBold(false);
        f.setPixelSize(11);
        p.setFont(f);
        p.setPen(QColor(0x55, 0x55, 0x55));
        p.drawText(QRectF(0, c.y() + R + 28, width(), 16), Qt::AlignHCenter | Qt::AlignTop, small);
        if (m_hand != Hand::None) {
            const qreal cell = std::max(2.0, std::round(R / 36)), w = 18 * cell, h = 21 * cell, dy = (m_frame ? 1 : -1) * cell * 3;
            const QPointF at = m_hand == Hand::Edge ? QPointF(c.x() + R - w * 0.4, c.y() - h * 0.45 + dy)
                                                    : QPointF(c.x() + R * 0.18, c.y() - R * 0.62 + dy);
            paintHand(p, at, cell);
        }
    }

  private:
    Hand m_hand = Hand::None;
    int m_frame = 0;
    QTimer m_tick;
};

// A row of boxes that fill green, as the old progress bars did: done of n
class Blocks : public QWidget {
  public:
    using QWidget::QWidget;
    int n = 3, done = 0;
    QStringList labels;
    QSize sizeHint() const override { return {240, 40}; }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const int gap = 6, w = (width() - gap * (n - 1)) / n;
        for (int i = 0; i < n; ++i) {
            const QRect r(i * (w + gap), 0, w, 20);
            p.setPen(QColor(0x7f, 0x9d, 0xb9));
            p.setBrush(Qt::white);
            p.drawRect(r.adjusted(0, 0, -1, -1));
            if (i < done) {   // segments, as the green bar was drawn
                for (int x = r.left() + 3; x + 8 <= r.right() - 2; x += 10) {
                    QLinearGradient g(0, r.top(), 0, r.bottom());
                    g.setColorAt(0, kGreen.lighter(150));
                    g.setColorAt(0.5, kGreen);
                    g.setColorAt(1, kGreen.lighter(130));
                    p.fillRect(QRect(x, r.top() + 3, 8, r.height() - 6), g);
                }
            }
            p.setPen(kInk);
            p.drawText(QRect(r.left(), 22, r.width(), 16), Qt::AlignHCenter, labels.value(i));
        }
    }
};

// A meter for the fast page: how many steps one message held, the most so far marked
class Meter : public QWidget {
  public:
    using QWidget::QWidget;
    int now = 0, peak = 0, top = 8;
    QSize sizeHint() const override { return {300, 40}; }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const QRect box(0, 0, width() - 1, 22);
        p.setPen(QColor(0x7f, 0x9d, 0xb9));
        p.setBrush(Qt::white);
        p.drawRect(box);
        const int w = (box.width() - 4) / top;
        for (int i = 0; i < top; ++i) {
            const QRect seg(box.left() + 3 + i * w, 3, w - 2, 17);
            if (i < now) {
                p.fillRect(seg, i < 1 ? kGreen : i < 3 ? QColor(0x9a, 0xc8, 0x2a) : QColor(0xe8, 0xa0, 0x1c));
            }
            if (i == peak - 1) {
                p.fillRect(QRect(seg.right() - 2, 1, 3, 21), kRed);
            }
            p.setPen(QColor(0x55, 0x55, 0x55));
            p.drawText(QRect(seg.left(), 24, seg.width(), 14), Qt::AlignHCenter, QString::number(i + 1));
        }
    }
};

// Its own colours, whatever the system's (dark mode included): the beige and white of that era, blue selection, and
// buttons drawn as they were (white to beige, a dark blue edge, round corners). Qt's Fusion style under it, which
// takes the colours (the Mac's own style draws its buttons its own way)
QStyle* fusion() {
    static QPointer<QStyle> s;
    if (!s) {
        s = QStyleFactory::create(QStringLiteral("Fusion"));
        s->setParent(qApp);
    }
    return s;
}

QPalette eraPalette() {
    QPalette p;
    p.setColor(QPalette::Window, QColor(0xec, 0xe9, 0xd8));
    p.setColor(QPalette::WindowText, kInk);
    p.setColor(QPalette::Base, Qt::white);
    p.setColor(QPalette::AlternateBase, QColor(0xf6, 0xf5, 0xee));
    p.setColor(QPalette::Text, kInk);
    p.setColor(QPalette::Button, QColor(0xf4, 0xf3, 0xee));
    p.setColor(QPalette::ButtonText, kInk);
    p.setColor(QPalette::Highlight, QColor(0x31, 0x6a, 0xc5));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Light, Qt::white);
    p.setColor(QPalette::Midlight, QColor(0xf1, 0xef, 0xe2));
    p.setColor(QPalette::Mid, QColor(0xac, 0xa8, 0x99));
    p.setColor(QPalette::Dark, QColor(0x71, 0x6f, 0x64));
    p.setColor(QPalette::Shadow, QColor(0x40, 0x40, 0x40));
    p.setColor(QPalette::PlaceholderText, QColor(0x80, 0x80, 0x80));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0xa1, 0xa1, 0x92));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0xa1, 0xa1, 0x92));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0xa1, 0xa1, 0x92));
    return p;
}

const char* const kEraSheet = R"(
QPushButton { color: #000; padding: 3px 12px; min-width: 64px; min-height: 16px; border: 1px solid #003c74; border-radius: 3px;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #ffffff, stop:0.8 #ecebe6, stop:1 #d6d0c5); }
QPushButton:hover { border: 1px solid #e59700; }
QPushButton:pressed { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #e5e4dd, stop:1 #f2f1ec); }
QPushButton:default { border: 2px solid #3c7fb1; }
QPushButton:disabled { color: #a1a192; border: 1px solid #c9c7ba; background: #f4f3ee; }
QComboBox { color: #000; background: #fff; border: 1px solid #7f9db9; padding: 2px 6px; min-height: 18px; }
QComboBox QAbstractItemView { background: #fff; color: #000; selection-background-color: #316ac5; selection-color: #fff; }
)";

// ---- writing it into Mixxx's mapping -----------------------------------------------------------------------------

// Mixxx's settings folder (Connect's own lookup, or, before that has run, the first that has a mixxx.cfg)
QString settingsDir() {
    const QString d = mixxxDir();
    if (!d.isEmpty()) {
        return d;
    }
    for (const QString& c : mixxxDirCandidates()) {
        if (QFileInfo::exists(c + QStringLiteral("/mixxx.cfg"))) {
            return c;
        }
    }
    return {};
}

QString normalized(QString s) {
    s.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9]+")), QStringLiteral("_"));
    return s.toLower();
}

/// The mapping Mixxx uses for this MIDI input (mixxx.cfg [ControllerPreset]: the device's name, spaces as "_")
QString presetFor(const QString& port) {
    QFile f(settingsDir() + QStringLiteral("/mixxx.cfg"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    bool in = false;
    const QString want = normalized(port);
    QString loose;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('['))) {
            in = line == QLatin1String("[ControllerPreset]");
            continue;
        }
        if (!in || line.isEmpty()) {
            continue;
        }
        const int sp = line.indexOf(QLatin1Char(' '));
        if (sp < 0) {
            continue;
        }
        const QString key = normalized(line.left(sp)), value = line.mid(sp + 1).trimmed();
        if (key == want) {
            return value;
        }
        if (want.startsWith(key) || key.startsWith(want)) {   // (a port named with ", 0" or "#2" after it)
            loose = value;
        }
    }
    return loose;
}

QStringList scriptDirs(const QString& xml) {
    return {QFileInfo(xml).absolutePath(), settingsDir() + QStringLiteral("/controllers"),
            QStringLiteral("/Applications/Mixxx ZyDeck.app/Contents/Resources/controllers"),
            QStringLiteral("/Applications/Mixxx.app/Contents/Resources/controllers")};
}

struct Applied {
    bool ok = false;
    QString text;
};

/// Writes "<mapping> (jog set up)" into Mixxx's own controllers folder: the mapping's script with this many steps a
/// turn (and, for a wheel that packs several steps into one message, each message's own count instead of one)
Applied applyToMapping(const Session& s) {
    const QString xml = presetFor(s.port);
    if (xml.isEmpty() || !QFileInfo::exists(xml)) {
        return {false, QObject::tr("Mixxx has no mapping set for %1 yet: pick one in Mixxx (Preferences › Controllers), then run this again. "
                                   "Or set its steps per turn to %2 by hand.").arg(s.port).arg(s.stepsPerTurn())};
    }
    QFile xf(xml);
    if (!xf.open(QIODevice::ReadOnly)) {
        return {false, QObject::tr("Can't read %1").arg(xml)};
    }
    QString x = QString::fromUtf8(xf.readAll());
    xf.close();
    // The controller's own script first (the one with a function prefix): Mixxx's shared ones (midi-components and
    // the like) mention scratchEnable too
    static const QRegularExpression fileRe(QStringLiteral(R"re(<file\b[^>]*>)re"));
    static const QRegularExpression nameAttr(QStringLiteral(R"re(filename="([^"]+\.js)")re")), prefixAttr(QStringLiteral(R"re(functionprefix="([^"]+)")re"));
    QStringList own, shared;
    for (auto it = fileRe.globalMatch(x); it.hasNext();) {
        const QString tag = it.next().captured(0);
        const QRegularExpressionMatch n = nameAttr.match(tag);
        if (n.hasMatch()) {
            (prefixAttr.match(tag).hasMatch() ? own : shared).append(n.captured(1));
        }
    }
    QString scriptName, scriptPath, js;
    for (const QString& name : own + shared) {
        for (const QString& d : scriptDirs(xml)) {
            QFile sf(d + QLatin1Char('/') + name);
            if (sf.open(QIODevice::ReadOnly)) {
                const QString text = QString::fromUtf8(sf.readAll());
                if (text.contains(QLatin1String("scratchEnable("))) {
                    scriptName = name;
                    scriptPath = sf.fileName();
                    js = text;
                }
                break;
            }
        }
        if (!js.isEmpty()) {
            break;
        }
    }
    if (js.isEmpty()) {
        return {false, QObject::tr("The mapping (%1) doesn't scratch with Mixxx's engine.scratchEnable: set its steps per turn to %2 by hand.")
                               .arg(QFileInfo(xml).fileName()).arg(s.stepsPerTurn())};
    }
    QStringList done, notDone;
    // engine.scratchEnable(deck, <steps a turn>, rpm, alpha, beta): a number, or a name set to one elsewhere
    static const QRegularExpression scratchRe(QStringLiteral(R"(engine\.scratchEnable\(\s*[^,]+?\s*,\s*([^,]+?)\s*,)"));
    const QRegularExpressionMatch m = scratchRe.match(js);
    bool stepsSet = false;
    if (m.hasMatch()) {
        const QString arg = m.captured(1);
        bool numeric = false;
        arg.toDouble(&numeric);
        if (numeric) {
            js.replace(m.capturedStart(1), m.capturedLength(1), QString::number(s.stepsPerTurn()));
            stepsSet = true;
        } else {
            const QString ident = arg.section(QLatin1Char('.'), -1);
            const QRegularExpression setRe(QStringLiteral(R"(\b%1\s*=\s*(\d+))").arg(QRegularExpression::escape(ident)));
            const QRegularExpressionMatch sm = setRe.match(js);
            if (sm.hasMatch()) {
                js.replace(sm.capturedStart(1), sm.capturedLength(1), QString::number(s.stepsPerTurn()));
                stepsSet = true;
            }
        }
    }
    if (stepsSet) {
        done << QObject::tr("one turn = %1 steps").arg(s.stepsPerTurn());
    } else {
        notDone << QObject::tr("its steps per turn (set it to %1 by hand)").arg(s.stepsPerTurn());
    }
    if (s.maxStep > 1 && s.enc == Enc::Twos) {   // "value < 0x40 ? 1 : -1": every message one step, whatever it held
        static const QRegularExpression flatRe(QStringLiteral(R"(\b(\w+)\s*<\s*(?:0x40|64)\s*\)?\s*\?\s*1\s*:\s*-1\b)"));
        const QRegularExpressionMatch fm = flatRe.match(js);
        if (fm.hasMatch()) {
            const QString v = fm.captured(1);
            js.replace(fm.capturedStart(0), fm.capturedLength(0), QStringLiteral("%1 < 0x40 ? %1 : %1 - 128").arg(v));
            done << QObject::tr("each message's own steps (up to %1 when fast)").arg(s.maxStep);
        } else if (!js.contains(QLatin1String("- 128")) && !js.contains(QLatin1String("-128"))) {
            notDone << QObject::tr("whether it counts all %1 steps a fast message holds (look where it reads the jog)").arg(s.maxStep);
        }
    }
    if (done.isEmpty()) {
        return {false, QObject::tr("Nothing could be changed in %1 by itself: %2.").arg(scriptName, notDone.join(QLatin1String("; ")))};
    }
    const QString dir = settingsDir() + QStringLiteral("/controllers");
    QDir().mkpath(dir);
    static const QString kTag = QStringLiteral(" (jog set up)");
    const QString newScript = scriptName.endsWith(QLatin1String("-jog.js")) ? scriptName : scriptName.chopped(3) + QStringLiteral("-jog.js");
    QString base = QFileInfo(xml).fileName();
    base.chop(base.endsWith(QLatin1String(".midi.xml")) ? 9 : 4);
    base.remove(kTag);
    const QString newXml = dir + QLatin1Char('/') + base + kTag + QStringLiteral(".midi.xml");
    x.replace(QStringLiteral("filename=\"%1\"").arg(scriptName), QStringLiteral("filename=\"%1\"").arg(newScript));
    static const QRegularExpression nameRe(QStringLiteral(R"(<name>([^<]*)</name>)"));
    const QRegularExpressionMatch nm = nameRe.match(x);
    QString shown = base + kTag;
    if (nm.hasMatch()) {
        shown = nm.captured(1).remove(kTag).trimmed() + kTag;
        x.replace(nm.capturedStart(1), nm.capturedLength(1), shown);
    }
    QFile so(dir + QLatin1Char('/') + newScript), xo(newXml);
    if (!so.open(QIODevice::WriteOnly | QIODevice::Truncate) || so.write(js.toUtf8()) < 0 ||
            !xo.open(QIODevice::WriteOnly | QIODevice::Truncate) || xo.write(x.toUtf8()) < 0) {
        return {false, QObject::tr("Couldn't write into %1").arg(dir)};
    }
    QString text = QObject::tr("Written: \"%1\" (%2).").arg(shown, done.join(QLatin1String(", ")));
    if (!notDone.isEmpty()) {
        text += QLatin1Char(' ') + QObject::tr("Still to check: %1.").arg(notDone.join(QLatin1String("; ")));
    }
    text += QStringLiteral("\n\n") + QObject::tr("In Mixxx: Preferences › Controllers › %1 › Load Mapping: \"%2\", then Apply.").arg(s.port, shown);
    return {true, text};
}

// ---- the wizard --------------------------------------------------------------------------------------------------

class Page : public QWizardPage {
  public:
    std::function<bool()> complete;
    std::function<void()> enter;
    std::function<bool()> leave;   // Next: may it go on?
    bool isComplete() const override { return complete ? complete() : true; }
    void initializePage() override {
        if (enter) {
            enter();
        }
    }
    bool validatePage() override { return leave ? leave() : true; }
    void changed() { emit completeChanged(); }
};

// An instruction: its number, then what to do (a little larger than the rest)
QLabel* step(int n, const QString& t) {
    auto* l = new QLabel(QStringLiteral("<table cellspacing=0 cellpadding=0><tr><td width=22 valign=top><b>%1.</b></td><td>%2</td></tr></table>").arg(n).arg(t));
    l->setWordWrap(true);
    l->setTextFormat(Qt::RichText);
    QFont f = l->font();
    f.setPixelSize(14);
    l->setFont(f);
    return l;
}

QLabel* text(const QString& t, QWidget* parent = nullptr) {
    auto* l = new QLabel(t, parent);
    l->setWordWrap(true);
    l->setTextFormat(Qt::RichText);
    return l;
}

class JogWizard : public QWizard {
  public:
    JogWizard(QWidget* parent, bool demo, const QString& shotDir);

  private:
    enum { kIntro, kFind, kTurns, kFast, kDone };
    void message(quint8 st, quint8 d1, quint8 d2);
    void shot(const QString& name);
    void runDemo();
    Session m_s;
    Input m_in;
    bool m_demo;
    QString m_shotDir;
    std::function<void(quint8, quint8, quint8)> m_page;   // the page in front's own reading
    QPushButton* m_apply = nullptr;
    bool m_written = false;   // the mapping written (Finish then closes without asking)
    bool turnsDone() const {   // three turns, within 4 % of each other
        if (m_s.turns.size() < 3) {
            return false;
        }
        const auto [lo, hi] = std::minmax_element(m_s.turns.begin(), m_s.turns.end());
        return (*hi - *lo) / double(m_s.measured()) <= 0.04;
    }
    static bool demoWrites() { return !qEnvironmentVariableIsEmpty("ZC_JOG_DEMO_WRITE"); }   // (a made-up HOME: its checks)
    QElapsedTimer m_clock;
};

JogWizard::JogWizard(QWidget* parent, bool demo, const QString& shotDir)
        : QWizard(parent),
          m_demo(demo),
          m_shotDir(shotDir) {
    setWindowTitle(tr("Jog Wheel Setup"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::NoBackButtonOnStartPage);
    setOption(QWizard::NoBackButtonOnLastPage);
    setButtonText(QWizard::BackButton, tr("< &Back"));
    setButtonText(QWizard::NextButton, tr("&Next >"));
    setButtonText(QWizard::FinishButton, tr("Finish"));
    setButtonText(QWizard::CancelButton, tr("Cancel"));
    setPixmap(QWizard::LogoPixmap, logo());   // (the side picture: only the first and last pages, below)
    setTitleFormat(Qt::RichText);
    setPalette(eraPalette());
    if (QFontDatabase::families().contains(QStringLiteral("Tahoma"))) {
        QFont f(QStringLiteral("Tahoma"));
        f.setPixelSize(12);
        setFont(f);
    }
    setMinimumSize(600, 470);
    m_clock.start();
    m_in.context = this;
    m_in.onMessage = [this](quint8 st, quint8 d1, quint8 d2) { message(st, d1, d2); };

    // 1. Welcome, and which controller
    auto* intro = new Page;
    intro->setTitle(QStringLiteral("<span style='font-size:17px; font-weight:bold'>%1</span>").arg(tr("Welcome to the Jog Wheel Setup Wizard")));
    intro->setPixmap(QWizard::WatermarkPixmap, watermark());
    auto* il = new QVBoxLayout(intro);
    il->addWidget(step(1, tr("Choose your controller.")));
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(tr("Controller:")));
    auto* ports = new QComboBox;
    ports->setMinimumWidth(220);
    row->addWidget(ports, 1);
    auto* again = new QPushButton(tr("Look Again"));
    row->addWidget(again);
    il->addLayout(row);
    auto* none = text(QString());
    il->addWidget(none);
    il->addSpacing(8);
    il->addWidget(step(2, tr("Find a <b>mark on its platter</b>: a logo, a sticker, the grip dots. A bit of tape will do.")));
    il->addSpacing(8);
    il->addWidget(step(3, tr("Click <b>Next</b>.")));
    il->addStretch();
    const auto fill = [this, ports, none, intro] {
        ports->clear();
        const QStringList names = m_demo ? QStringList{tr("Demo controller")} : Input::ports();
        ports->addItems(names);
        for (int i = 0; i < names.size(); ++i) {   // (a DJ controller first, before keyboards and the like)
            if (names[i].contains(QLatin1String("DJ"), Qt::CaseInsensitive) || names[i].contains(QLatin1String("Inpulse"), Qt::CaseInsensitive)) {
                ports->setCurrentIndex(i);
                break;
            }
        }
        none->setText(names.isEmpty() ? tr("<b>No MIDI controller found.</b> Plug it in, then click Look Again.") : QString());
        intro->changed();
    };
    connect(again, &QPushButton::clicked, this, fill);
    intro->complete = [ports] { return ports->count() > 0; };
    intro->leave = [this, ports] {
        m_s = Session{};
        m_s.port = ports->currentText();
        if (m_demo) {
            return true;
        }
        if (!m_in.open(m_s.port)) {
            QMessageBox::warning(this, windowTitle(), tr("Couldn't listen to %1.").arg(m_s.port));
            return false;
        }
        return true;
    };
    setPage(kIntro, intro);
    fill();

    // The hand comes off while the jog is touched or moved, and back after 10 s of nothing (to show again what to do)
    const auto handBack = [this](Platter* pl, std::function<Platter::Hand()> which) {
        auto* t = new QTimer(this);
        t->setSingleShot(true);
        t->setInterval(10000);
        connect(t, &QTimer::timeout, pl, [pl, which] { pl->setHand(which()); });
        return [pl, t] {   // (activity)
            pl->setHand(Platter::Hand::None);
            t->start();
        };
    };
    // One instruction at a time, above the platter
    const auto instruction = [] {
        auto* l = new QLabel;
        l->setWordWrap(true);
        l->setTextFormat(Qt::RichText);
        l->setAlignment(Qt::AlignHCenter);
        l->setMinimumHeight(44);
        QFont f = l->font();
        f.setPixelSize(15);
        l->setFont(f);
        return l;
    };

    // 2. Find the jog: what it sends as it's touched and turned
    auto* find = new Page;
    find->setTitle(tr("Find the Jog Wheel"));
    find->setSubTitle(tr("Step 1 of 3"));   // (the band across the top: what to do is in the page)
    auto* fl = new QVBoxLayout(find);
    auto* fSay = instruction();
    fl->addWidget(fSay);
    auto* fPlatter = new Platter;
    fl->addWidget(fPlatter, 1);
    struct Cc {
        int n = 0, twos = 0, offset = 0;
        QSet<int> values;
        quint8 touchSt = 0, touchNote = 0;
        bool touchCand = false;
    };
    auto stats = std::make_shared<QHash<int, Cc>>();
    auto lastOn = std::make_shared<std::tuple<quint8, quint8, qint64>>(0, 0, -100000);
    auto offSeen = std::make_shared<bool>(false);
    const auto fActive = handBack(fPlatter, [this] { return m_s.found ? Platter::Hand::None : Platter::Hand::Top; });
    find->enter = [this, find, fPlatter, fSay, stats, lastOn, offSeen, fActive] {
        stats->clear();
        *lastOn = {0, 0, -100000};
        *offSeen = false;
        m_s.found = false;
        m_s.hasTouch = false;
        fSay->setText(tr("<b>Touch the top</b> of the <b>left</b> jog wheel and turn it a little, back and forth."));
        fPlatter->big = tr("Waiting…");
        fPlatter->small.clear();
        fPlatter->setHand(Platter::Hand::Top);
        fPlatter->update();
        m_page = [this, find, fPlatter, fSay, stats, lastOn, offSeen, fActive](quint8 st, quint8 d1, quint8 v) {
            const qint64 now = m_clock.elapsed();
            if ((st & 0xF0) == 0x90 && v > 0) {
                *lastOn = {st, d1, now};
            }
            const bool off = (st & 0xF0) == 0x80 || ((st & 0xF0) == 0x90 && v == 0);
            if (off && m_s.found && st % 16 == m_s.touchSt % 16 && d1 == m_s.touchNote) {
                *offSeen = true;
            }
            if ((st & 0xF0) == 0xB0) {
                fActive();
                Cc& c = (*stats)[st << 8 | d1];
                if (c.n == 0 && now - std::get<2>(*lastOn) < 2000) {   // a note just before it began: its touch, perhaps
                    c.touchCand = true;
                    c.touchSt = std::get<0>(*lastOn);
                    c.touchNote = std::get<1>(*lastOn);
                }
                ++c.n;
                c.twos += (v >= 1 && v <= 15) || (v >= 113 && v <= 127);
                c.offset += v >= 49 && v <= 79 && v != 64;
                c.values.insert(v);
            }
            // the busiest control change, once it's clearly a wheel
            int best = -1, bestN = 0;
            for (auto it = stats->cbegin(); it != stats->cend(); ++it) {
                if (it.value().n > bestN) {
                    best = it.key();
                    bestN = it.value().n;
                }
            }
            if (best < 0 || bestN < 40) {
                fPlatter->big = bestN ? tr("Turning…") : tr("Waiting…");
                fPlatter->active = bestN > 0;
                fPlatter->update();
                return;
            }
            const Cc& c = stats->value(best);
            m_s.found = true;
            m_s.st = static_cast<quint8>(best >> 8);
            m_s.cc = static_cast<quint8>(best & 0xFF);
            m_s.enc = c.twos >= c.n * 0.8 ? Enc::Twos : c.offset >= c.n * 0.8 ? Enc::Offset : Enc::Absolute;
            m_s.hasTouch = c.touchCand && *offSeen;
            m_s.touchSt = c.touchSt;
            m_s.touchNote = c.touchNote;
            fPlatter->big = tr("Found it");
            fPlatter->active = true;
            fPlatter->update();
            fSay->setText(c.touchCand && !m_s.hasTouch ? tr("<b>Let go.</b>") : tr("<b>Done.</b> Click Next."));
            find->changed();
        };
    };
    find->complete = [this, stats] {
        if (!m_s.found) {
            return false;
        }
        const Cc c = stats->value(m_s.st << 8 | m_s.cc);
        return m_s.hasTouch || !c.touchCand;
    };
    setPage(kFind, find);

    // 3. Three full turns, mark to mark: first lined up by the edge (not counted), then once round by the top
    auto* turns = new Page;
    turns->setTitle(tr("Measure One Turn"));
    turns->setSubTitle(tr("Step 2 of 3"));
    auto* tl = new QVBoxLayout(turns);
    auto* tSay = instruction();
    tl->addWidget(tSay);
    auto* tPlatter = new Platter;
    tl->addWidget(tPlatter, 1);
    auto* tBlocks = new Blocks;
    tBlocks->labels = {tr("Turn 1"), tr("Turn 2"), tr("Turn 3")};
    tBlocks->setMaximumWidth(420);
    tl->addWidget(tBlocks, 0, Qt::AlignHCenter);
    struct Turn {
        bool on = false;
        int sum = 0;
        bool lining = true;   // lining the mark up (by the edge), not turning it round yet
    };
    auto cur = std::make_shared<Turn>();
    auto idle = new QTimer(this);   // no touch-sensitive top: a turn ends when it stops
    idle->setSingleShot(true);
    idle->setInterval(800);
    auto edgeStill = new QTimer(this);   // turned by the edge, then left: lined up
    edgeStill->setSingleShot(true);
    edgeStill->setInterval(1000);
    const auto tHand = [this, cur] {
        if (turnsDone()) {
            return Platter::Hand::None;
        }
        return cur->lining && m_s.hasTouch ? Platter::Hand::Edge : Platter::Hand::Top;
    };
    const auto tActive = handBack(tPlatter, tHand);
    auto paintTurns = [this, tPlatter, tBlocks, tSay, cur] {
        const int est = m_s.measured();
        tPlatter->showMark = est > 0;
        tPlatter->angle = est > 0 ? cur->sum * 360.0 / est : 0;
        tPlatter->active = cur->on;
        tPlatter->big = cur->on || cur->sum ? tr("%1 steps").arg(std::abs(cur->sum)) : QString();
        tPlatter->update();
        tBlocks->done = static_cast<int>(m_s.turns.size());
        tBlocks->labels = {};
        for (int i = 0; i < 3; ++i) {
            tBlocks->labels << (i < m_s.turns.size() ? tr("%1 steps").arg(m_s.turns[i]) : tr("Turn %1").arg(i + 1));
        }
        tBlocks->update();
        if (turnsDone()) {
            tSay->setText(tr("<b>Done.</b> Click Next."));
        } else if (!m_s.hasTouch) {
            tSay->setText(tr("Turn it <b>clockwise</b>, once round, from the <b style='color:#d22b1e'>red arrow</b> back to it. Then stop."));
        } else if (cur->lining) {
            tSay->setText(tr("Turn it by its <b>edge</b> until your mark is at the <b style='color:#d22b1e'>red arrow</b>. <b>Don't touch the top.</b>"));
        } else if (m_s.turns.size() >= 3) {
            tSay->setText(tr("These don't agree: <b>touch the top</b> and turn it once round again, arrow to arrow. Then let go."));
        } else {
            tSay->setText(tr("<b>Touch the top</b> and turn it <b>clockwise</b>, once round, back to the arrow. Then let go."));
        }
    };
    auto endTurn = [this, turns, cur, paintTurns, tPlatter, tHand] {
        if (!cur->on) {
            return;
        }
        cur->on = false;
        if (std::abs(cur->sum) >= 50) {   // (a brush of the hand isn't a turn)
            m_s.turns.append(std::abs(cur->sum));
            while (m_s.turns.size() > 3) {
                m_s.turns.removeFirst();
            }
        }
        cur->sum = 0;
        cur->lining = false;   // (the mark is at the arrow again: round once more from there)
        paintTurns();
        tPlatter->setHand(tHand());
        turns->changed();
    };
    connect(idle, &QTimer::timeout, this, endTurn);
    connect(edgeStill, &QTimer::timeout, this, [cur, paintTurns, tPlatter, tHand] {
        if (cur->lining && !cur->on) {
            cur->lining = false;
            paintTurns();
            tPlatter->setHand(tHand());
        }
    });
    turns->enter = [this, cur, idle, edgeStill, paintTurns, endTurn, tPlatter, tHand, tActive] {
        m_s.turns.clear();
        *cur = Turn{};
        paintTurns();
        tPlatter->setHand(tHand());
        m_page = [this, cur, idle, edgeStill, paintTurns, endTurn, tPlatter, tHand, tActive](quint8 st, quint8 d1, quint8 v) {
            if (m_s.isTouch(st, d1)) {
                if ((st & 0xF0) == 0x90 && v > 0) {
                    tActive();
                    *cur = Turn{true, 0, false};
                    paintTurns();
                } else {
                    endTurn();
                }
                return;
            }
            const bool steps = m_s.isSteps(st, d1);
            if (!steps && st == m_s.st && (st & 0xF0) == 0xB0) {   // its edge (another control on its channel)
                tActive();
                if (cur->lining) {
                    edgeStill->start();
                }
                return;
            }
            if (!steps) {
                return;
            }
            tActive();
            const int step = m_s.step(v);
            m_s.maxStep = std::max(m_s.maxStep, std::abs(step));
            if (!m_s.hasTouch) {   // no touch: a turn is from moving to a stop
                if (!cur->on) {
                    *cur = Turn{true, 0, false};
                }
                idle->start();
            } else if (!cur->on) {   // turned without the top touched: by its edge
                if (cur->lining) {
                    edgeStill->start();
                }
                return;
            }
            cur->sum += step;
            paintTurns();
        };
    };
    turns->complete = [this] { return turnsDone(); };
    setPage(kTurns, turns);

    // 4. Fast: how many steps one message holds
    auto* fast = new Page;
    fast->setTitle(tr("Fast Moves"));
    fast->setSubTitle(tr("Step 3 of 3"));
    auto* xl = new QVBoxLayout(fast);
    auto* xSay = instruction();
    xl->addWidget(xSay);
    auto* xPlatter = new Platter;
    xl->addWidget(xPlatter, 1);
    auto* meter = new Meter;
    meter->setMaximumWidth(420);
    xl->addWidget(meter, 0, Qt::AlignHCenter);
    auto fastMsgs = std::make_shared<int>(0);
    auto fastStart = std::make_shared<qint64>(0);
    const auto fastDone = [this, fastMsgs, fastStart] { return *fastMsgs >= 150 && m_clock.elapsed() - *fastStart >= 2000; };
    const auto xActive = handBack(xPlatter, [fastDone] { return fastDone() ? Platter::Hand::None : Platter::Hand::Top; });
    fast->enter = [this, fast, meter, xSay, xPlatter, fastMsgs, fastStart, fastDone, xActive] {
        *fastMsgs = 0;
        *fastStart = 0;
        meter->now = 0;
        meter->peak = 0;
        meter->update();
        xSay->setText(tr("<b>Touch the top</b> and scratch back and forth, <b>fast</b>, for a few seconds."));
        xPlatter->setHand(Platter::Hand::Top);
        m_page = [this, fast, meter, xSay, xPlatter, fastMsgs, fastStart, fastDone, xActive](quint8 st, quint8 d1, quint8 v) {
            if (!m_s.isSteps(st, d1)) {
                return;
            }
            xActive();
            const int step = m_s.step(v);
            if (*fastMsgs == 0) {
                *fastStart = m_clock.elapsed();
            }
            ++*fastMsgs;
            xPlatter->angle += step * 360.0 / std::max(1, m_s.stepsPerTurn());
            xPlatter->showMark = true;
            xPlatter->update();
            meter->now = std::min(std::abs(step), meter->top);
            meter->peak = std::max(meter->peak, meter->now);
            meter->update();
            m_s.maxStep = std::max(m_s.maxStep, std::abs(step));
            if (fastDone()) {
                xSay->setText(tr("<b>Done.</b> Click Next."));
            }
            fast->changed();
        };
    };
    fast->complete = fastDone;
    setPage(kFast, fast);

    // 5. What was found, and into Mixxx with it
    auto* doneP = new Page;
    doneP->setTitle(QStringLiteral("<span style='font-size:17px; font-weight:bold'>%1</span>").arg(tr("Completing the Jog Wheel Setup Wizard")));
    doneP->setPixmap(QWizard::WatermarkPixmap, watermark());
    auto* dl = new QVBoxLayout(doneP);
    auto* summary = text(QString());
    dl->addWidget(summary);
    dl->addSpacing(6);
    dl->addWidget(step(1, tr("Click <b>Write It into Mixxx's Mapping</b>.")));
    auto* apply = m_apply = new QPushButton(tr("Write It into Mixxx's Mapping"));
    auto* applyRow = new QHBoxLayout;
    applyRow->addWidget(apply);
    applyRow->addStretch();
    dl->addLayout(applyRow);
    auto* applied = text(QString());
    applied->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dl->addWidget(applied);
    dl->addStretch();
    auto* closeStep = step(2, tr("Click <b>Finish</b>."));
    dl->addWidget(closeStep);
    doneP->enter = [this, summary, applied, apply] {
        m_written = false;
        m_page = nullptr;
        m_in.close();
        const int spt = m_s.stepsPerTurn();
        QStringList turnsText;
        for (int t : std::as_const(m_s.turns)) {
            turnsText << QString::number(t);
        }
        summary->setText(QStringLiteral("<b>%1</b><br>").arg(m_s.port.toHtmlEscaped()) +
                tr("One turn: <b>%1 steps</b> (%2)").arg(spt).arg(turnsText.join(QStringLiteral(", "))) + QStringLiteral("<br>") +
                (m_s.maxStep > 1 ? tr("Fast: up to <b>%1 steps</b> a message").arg(m_s.maxStep) : tr("Fast: 1 step a message")));
        applied->setText(QString());
        apply->setEnabled(true);
    };
    // Written: true once it is (or there was nothing to write: the demo)
    const auto writeIt = [this, applied, apply] {
        if (m_demo && !demoWrites()) {
            applied->setText(tr("(Demo: nothing written.)"));
            m_written = true;
            return true;
        }
        const Applied a = applyToMapping(m_s);
        applied->setText((a.ok ? QString() : QStringLiteral("<b>%1</b> ").arg(tr("Not written:"))) + a.text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
        apply->setEnabled(!a.ok);
        m_written = a.ok;
        return a.ok;
    };
    connect(apply, &QPushButton::clicked, this, writeIt);
    // Finish without writing it: ask (Yes writes and closes; No closes; Cancel stays)
    doneP->leave = [this, writeIt] {
        if (m_written) {
            return true;
        }
        QMessageBox box(QMessageBox::Question, windowTitle(), tr("Save this into Mixxx's mapping before closing?"),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, this);
        box.setInformativeText(tr("One turn: %1 steps.").arg(m_s.stepsPerTurn()));
        box.setDefaultButton(QMessageBox::Yes);
        box.setStyle(fusion());
        box.setPalette(eraPalette());
        for (QWidget* w : box.findChildren<QWidget*>()) {
            w->setStyle(fusion());
            w->setPalette(eraPalette());
            if (qobject_cast<QPushButton*>(w)) {
                w->setStyleSheet(QString::fromLatin1(kEraSheet));
            }
        }
        switch (box.exec()) {
        case QMessageBox::Yes:
            return writeIt();   // (not written: stay, the reason on the page)
        case QMessageBox::No:
            return true;
        default:
            return false;
        }
    };
    setPage(kDone, doneP);

    // Fusion under every part of it (a style isn't handed down to children), the wizard's own buttons too
    // (and the colours set on each: a style sheet on the whole would put the system's colours back on the rest)
    setStyle(fusion());
    for (QWidget* w : findChildren<QWidget*>()) {
        w->setStyle(fusion());
        w->setPalette(eraPalette());
        if (qobject_cast<QPushButton*>(w) || qobject_cast<QComboBox*>(w)) {
            w->setStyleSheet(QString::fromLatin1(kEraSheet));
        }
    }
    if (m_demo) {
        QTimer::singleShot(300, this, [this] { runDemo(); });
    }
}

void JogWizard::message(quint8 st, quint8 d1, quint8 d2) {
    if (m_page) {
        m_page(st, d1, d2);
    }
}

void JogWizard::shot(const QString& name) {
    if (!m_shotDir.isEmpty()) {
        grab().save(QStringLiteral("%1/%2.png").arg(m_shotDir, name));
    }
}

// A controller that isn't there: touch 91 08, its steps on B1 0A (as the Inpulse 300 MK2), 512 to a turn
void JogWizard::runDemo() {
    auto t = std::make_shared<int>(0);
    const auto at = [this, t](int after, std::function<void()> f) {
        *t += after;
        QTimer::singleShot(*t, this, std::move(f));
    };
    const auto send = [this](quint8 st, quint8 d1, quint8 d2) { m_in.inject(st, d1, d2); };
    const auto turn = [at, send](int steps, int value, int msPer10) {
        for (int i = 0; i < steps; i += 10) {
            at(i ? msPer10 : 0, [send, n = std::min(10, steps - i), value] {
                for (int k = 0; k < n; ++k) {
                    send(0xB1, 0x0A, static_cast<quint8>(value));
                }
            });
        }
    };
    at(400, [this] { shot(QStringLiteral("1-welcome")); next(); });
    at(500, [send] { send(0x91, 0x08, 0x7F); });
    turn(60, 1, 15);
    at(100, [send] { send(0x91, 0x08, 0x00); });
    at(300, [this] { shot(QStringLiteral("2-found")); next(); });
    at(300, [this] { shot(QStringLiteral("3a-lining")); });
    for (int i = 0; i < 20; ++i) {   // lined up by the edge (B1 09)
        at(20, [send] { send(0xB1, 0x09, 0x01); });
    }
    at(1300, [this] { shot(QStringLiteral("3b-touch")); });
    for (int n : {509, 513, 507}) {
        at(400, [send] { send(0x91, 0x08, 0x7F); });
        turn(n / 2, 1, 12);
        if (n == 509) {
            at(50, [this] { shot(QStringLiteral("3-turning")); });
        }
        turn(n - n / 2, 1, 12);
        at(150, [send] { send(0x91, 0x08, 0x00); });
    }
    at(400, [this] { shot(QStringLiteral("4-measured")); next(); });
    for (int i = 0; i < 40; ++i) {
        at(60, [send, i] {
            for (int k = 0; k < 4; ++k) {
                const int v = 1 + (i + k) % 3;
                send(0xB1, 0x0A, static_cast<quint8>(i % 8 < 4 ? v : 128 - v));
            }
        });
    }
    at(400, [this] { shot(QStringLiteral("5-fast")); next(); });
    at(500, [this] {
        shot(QStringLiteral("6-done"));
        if (demoWrites()) {
            m_apply->click();
            shot(QStringLiteral("7-written"));
        } else if (!m_shotDir.isEmpty()) {   // Finish without writing: the question (drawn, then Cancel)
            QTimer::singleShot(600, this, [this] {
                if (QWidget* box = QApplication::activeModalWidget()) {
                    box->grab().save(QStringLiteral("%1/8-ask.png").arg(m_shotDir));
                    shot(QStringLiteral("8-behind"));
                    static_cast<QDialog*>(box)->reject();
                }
            });
            button(QWizard::FinishButton)->click();
        }
        if (!m_shotDir.isEmpty()) {
            QCoreApplication::quit();
        }
    });
}

QPointer<JogWizard> g_wizard;

} // namespace

void showJogWizard(QWidget* parent, bool demo, const QString& shotDir) {
    if (g_wizard) {
        g_wizard->raise();
        g_wizard->activateWindow();
        return;
    }
    g_wizard = new JogWizard(parent, demo, shotDir);
    g_wizard->show();
    g_wizard->raise();
    g_wizard->activateWindow();
}

} // namespace zc
