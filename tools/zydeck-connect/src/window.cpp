#include "window.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>

#include "core.h"
#include "library.h"
#include "qrcodegen.hpp"
#include "system.h"

namespace zc {

namespace {
QPixmap qrPixmap(const QString& text, int modulePx) {
    const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.toUtf8().constData(), qrcodegen::QrCode::Ecc::LOW);
    const int n = qr.getSize(), quiet = 3, side = (n + 2 * quiet) * modulePx;
    QPixmap pm(side, side);
    pm.fill(Qt::white);
    QPainter p(&pm);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::black);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            if (qr.getModule(x, y)) {
                p.drawRect((x + quiet) * modulePx, (y + quiet) * modulePx, modulePx, modulePx);
            }
        }
    }
    return pm;
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
    setMinimumWidth(460);
    auto* root = new QVBoxLayout(this);

    auto* midiBox = new QGroupBox(tr("MIDI"));
    m_port = new QLabel;
    m_port->setWordWrap(true);
    (new QVBoxLayout(midiBox))->addWidget(m_port);

    auto* mixxxBox = new QGroupBox(tr("Mixxx"));
    auto* mixxxLay = new QVBoxLayout(mixxxBox);
    m_mixxx = new QLabel;
    m_mixxx->setWordWrap(true);
    m_mixxx->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_install = new QPushButton(tr("Install the ZyDeck mapping in Mixxx"));
    mixxxLay->addWidget(m_mixxx);
    mixxxLay->addWidget(m_install, 0, Qt::AlignLeft);

    auto* devBox = new QGroupBox(tr("Devices"));
    auto* devLay = new QVBoxLayout(devBox);
    m_devices = new QListWidget;
    m_devices->setMaximumHeight(90);
    m_devices->setWordWrap(true);
    m_devices->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    devLay->addWidget(m_devices);

    auto* codeBox = new QGroupBox(tr("Link without the cable"));
    auto* codeLay = new QHBoxLayout(codeBox);
    m_code = new QLabel;
    m_code->setFixedSize(180, 180);
    m_code->setAlignment(Qt::AlignCenter);
    m_code->hide();
    auto* codeSide = new QVBoxLayout;
    m_codeNote = new QLabel(tr("In ZyDeck: Settings › Connections › Pair by QR, then scan this. It links over Wi-Fi to "
                               "this computer, no cable needed."));
    m_codeNote->setWordWrap(true);
    m_showCode = new QPushButton(tr("Show pairing code"));
    m_showCode->setCheckable(true);
    codeSide->addWidget(m_codeNote);
    codeSide->addWidget(m_showCode, 0, Qt::AlignLeft);
    codeSide->addStretch();
    codeLay->addWidget(m_code);
    codeLay->addLayout(codeSide, 1);

    auto* bottom = new QHBoxLayout;
    m_login = new QCheckBox(tr("Open at login (so Mixxx always finds the port)"));
    m_login->setChecked(openAtLogin());
    auto* quit = new QPushButton(tr("Quit"));
    bottom->addWidget(m_login, 1);
    bottom->addWidget(quit);
    m_here = new QLabel;
    m_here->setTextInteractionFlags(Qt::TextSelectableByMouse);

    root->addWidget(midiBox);
    root->addWidget(mixxxBox);
    root->addWidget(devBox);
    root->addWidget(codeBox);
    root->addWidget(m_here);
    root->addLayout(bottom);

    connect(m_install, &QPushButton::clicked, this, [this] {
        try {
            m_core->status = tr("Installed in %1. Restart Mixxx if it's open.").arg(installMapping());
        } catch (const QString& why) {
            m_core->status = tr("Couldn't install the mapping: %1").arg(why);
        }
        refresh();
    });
    connect(m_showCode, &QPushButton::toggled, this, &Window::showCode);
    connect(m_login, &QCheckBox::toggled, this, [](bool on) { setOpenAtLogin(on); });
    connect(quit, &QPushButton::clicked, qApp, &QApplication::quit);

    // In the menu bar (the tray on Windows and Linux): it keeps running with the window closed
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        m_tray = new QSystemTrayIcon(this);
        m_menu = new QMenu(this);
        m_tray->setContextMenu(m_menu);
        connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
            if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) {
#ifndef Q_OS_MACOS   // (the Mac opens the menu on a click)
                show();
                raise();
                activateWindow();
#endif
            }
        });
        m_tray->show();
    }

    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &Window::refresh);
    timer->start(1000);
    connect(m_core, &Core::changed, this, &Window::refresh);
    refresh();
}

void Window::showPairingCode() {
    m_showCode->setChecked(true);
}

void Window::showCode(bool on) {
    m_code->setVisible(on);
    m_showCode->setText(on ? tr("Hide pairing code") : tr("Show pairing code"));
    m_codeFor.clear();
    refresh();
    adjustSize();   // (room for it, or none)
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

void Window::refresh() {
    const QString problem = m_core->midi()->problem();
    m_port->setText(!problem.isEmpty() ? problem
            : tr("MIDI port for Mixxx: %1. In Mixxx: Preferences › Controllers › %1 › load ZyDeck, tick Enabled. "
                 "Mixxx only finds MIDI ports when it starts: start ZyDeck Connect first.").arg(QString::fromLatin1(kPortName)));
    const QString dir = mixxxDir();
    QString mixxx = m_core->status.isEmpty() ? QString() : m_core->status + '\n';
    mixxx += dir.isEmpty() ? tr("Mixxx's library not found: no track names or waveforms")
                           : tr("Mixxx's library: %1 (%2 tracks)").arg(dir).arg(m_core->library()->trackCount());
    mixxx += '\n' + (mappingInstalled() ? tr("ZyDeck mapping: installed") : tr("ZyDeck mapping: not installed yet"));
    m_mixxx->setText(mixxx);
    m_install->setText(mappingInstalled() ? tr("Install the ZyDeck mapping again") : tr("Install the ZyDeck mapping in Mixxx"));

    const QList<Core::Device> devices = m_core->devices();
    QStringList lines;
    for (const Core::Device& d : devices) {
        lines << QStringLiteral("%1 (%2)").arg(d.name, d.how);
    }
    QStringList shown;
    for (int i = 0; i < m_devices->count(); ++i) {
        shown << m_devices->item(i)->text();
    }
    if (lines.isEmpty()) {
        lines << tr("None yet. On the phone or tablet: ZyDeck › Settings › Controller mode, then plug in the USB cable "
                    "(choose MIDI in Android's notification), or scan the pairing code.");
    }
    if (shown != lines) {
        m_devices->clear();
        m_devices->addItems(lines);
        for (int i = 0; i < m_devices->count(); ++i) {
            m_devices->item(i)->setToolTip(m_devices->item(i)->text());
        }
    }
    const QStringList hosts = lanAddresses();
    m_here->setText(tr("This computer: %1 · %2 · port %3")
                            .arg(m_core->name(), hosts.isEmpty() ? tr("no network") : hosts.join(QLatin1String(", ")))
                            .arg(kTcpPort));
    if (m_code->isVisible()) {
        const QString url = m_core->pairUrl();
        if (url != m_codeFor) {
            m_codeFor = url;
            m_code->setPixmap(qrPixmap(url, 4).scaled(m_code->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
        }
    }

    if (m_tray) {   // the lamp: green linked, amber waiting for a device, red without a port
        const QColor lamp = !problem.isEmpty() ? QColor(0xff, 0x4a, 0x3d) : devices.isEmpty() ? QColor(0xff, 0xc2, 0x3d) : QColor(0x3d, 0xdc, 0x84);
        m_tray->setIcon(lampIcon(lamp));
        m_tray->setToolTip(devices.isEmpty() ? tr("ZyDeck Connect: no device yet")
                                             : tr("ZyDeck Connect: %n device(s)", nullptr, static_cast<int>(devices.size())));
        const QString menuFor = lines.join('\n') + (m_login->isChecked() ? "+" : "-");
        if (menuFor == m_menuFor) {
            return;
        }
        m_menuFor = menuFor;
        m_menu->clear();
        m_menu->addAction(devices.isEmpty() ? tr("ZyDeck Connect · waiting for a device") : tr("ZyDeck Connect · ready"))->setEnabled(false);
        for (const Core::Device& d : devices) {
            m_menu->addAction(QStringLiteral("   %1 · %2").arg(d.name, d.how))->setEnabled(false);
        }
        m_menu->addSeparator();
        m_menu->addAction(tr("Show Pairing Code…"), this, [this] {
            m_showCode->setChecked(true);
            show();
            raise();
            activateWindow();
        });
        m_menu->addAction(tr("Open ZyDeck Connect…"), this, [this] {
            show();
            raise();
            activateWindow();
        });
        m_menu->addAction(tr("Install Mapping in Mixxx"), m_install, &QPushButton::click);
        QAction* login = m_menu->addAction(tr("Open at Login"));
        login->setCheckable(true);
        login->setChecked(m_login->isChecked());
        connect(login, &QAction::toggled, m_login, &QCheckBox::setChecked);
        m_menu->addSeparator();
        m_menu->addAction(tr("Quit ZyDeck Connect"), qApp, &QApplication::quit);
    }
}

} // namespace zc
