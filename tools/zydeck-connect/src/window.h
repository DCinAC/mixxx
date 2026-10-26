#pragma once
// ZyDeck Connect's window: a rack unit. The face: ZYDECK CONNECT and its power lamp, a VFD readout (the MIDI port,
// Mixxx's side, this computer's address), the MIXXX plate (Mixxx's red rocker with a cable plugged in beside it, standing
// out of the panel: on while Mixxx runs, off opens it, on asks then quits it; only the rocker takes a press) and SCAN
// (look for devices now and release the tag), the unit's own POWER push button in the corner, lit (for show: so the red
// switch reads as Mixxx's),
// and the four device slots stacked as bays (the ZyDecks found on the network, not linked yet, wait dimmed in the
// free ones: their PAIR asks them to link): the device and how it's linked, then its PAIR key (3:2, the word under
// it) and its MIDI IN / OUT lamps. The rocker: on while Mixxx runs, lit when it answers (setup.h), its glow breathing
// while it doesn't yet or Mixxx is starting or quitting; MIXXX in red if setting it up failed. The unit is as wide as the longest device name needs (30 px clear of that
// cluster). Open at login is in the menu bar menu.
// Under it, a black tag comes out of the bottom edge with the pairing code on a white sticker: by itself while no
// device is linked, back in once one is. Coming out it snags: its inner corner catches and it slants, a moment's
// pause, it's pushed back a little, then pulled again and hangs straight. A click on its slot, SCAN, or PAIR on an
// empty slot (the next device lands there) pulls it out; the slot again, PAIR again, or a click on the tag pushes it
// back. Drawn by hand (QPainter) in a see-through window: nothing round the unit but a title bar on top, in the
// system's manner (it moves the window; close, minimize), and their drop shadow on what's behind. The edge is machined
// (a bevel catching the light). The menu bar / tray icon keeps it running with the window closed.

#include <QElapsedTimer>
#include <QHash>
#include <QPixmap>
#include <QSet>
#include <QVariantAnimation>
#include <QWidget>

class QMenu;
class QSystemTrayIcon;

namespace zc {

class Core;
class Discovery;
class Setup;

class Window : public QWidget {
    Q_OBJECT
  public:
    Window(Core* core, Setup* setup, Discovery* discovery);
    void showPairingCode();   // the tag out (and kept out)

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void leaveEvent(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

  private:
    enum class Key { None, Map, Scan, Tag, Slot, Slit, Bar, BarClose, BarMin, BarMenu };
    struct Hit {
        Key key = Key::None;
        int slot = -1;
        bool operator==(const Hit& o) const { return key == o.key && slot == o.slot; }
    };
    Hit hitAt(const QPointF& p) const;
    QRectF barButton(Key k) const;   // the bar's close and minimize (window coordinates)
    Hit m_hover;
    void press(const Hit& h);
    void refresh();            // once a second, and when a device comes or goes
    void setTag(bool out);     // slide it
    void layoutWidth();        // as wide as the device names need
    void updateTray();
    QIcon trayIcon(bool dot, const QColor& c) const;   // the ZyDeck app's status bar icon
    QRectF bay(int slot) const;
    QRectF slotKey(int slot) const;
    QRectF tagRect() const;

    void setupChanged();       // Mixxx's side moved on: the readout, the MIXXX rocker, and asking when it needs the user
    void setUpMixxx();         // the menu's Set Up Mixxx Again
    void say(const QString& title, const QString& text, bool warning = false);

    Core* m_core;
    Setup* m_setup;
    Discovery* m_discovery;
    QHash<int, QStringList> m_foundIn;   // a free slot -> a ZyDeck found on the network: {name, address, asked ms}
    QSet<int> m_asked;         // what's been asked this run (each once)
    int m_lastSetup = -1;
    int m_w;                   // the unit's width
    QVariantAnimation m_tagAnim;   // its time (ms): tagPose() turns that into where it is
    void tagPose(double ms);
    double m_drop = 0, m_angle = 0;          // the tag now: how far out (px), its slant (degrees)
    double m_fromDrop = 0, m_fromAngle = 0;  // where the current move started
    bool m_tagWanted = false;  // where it's going
    int m_tagManual = -1;      // PAIR's (or the tag's) say: -1 none (it follows the devices), 0 in, 1 out
    int m_pairSlot = -1;       // the slot whose PAIR pulled the tag out: the next device lands there
    QHash<QString, int> m_slotOf;   // device -> its slot
    int m_lastDeviceCount = -1;
    Hit m_down;
    QString m_mapLamp;         // the MIXXX rocker for a moment after a press: "", "busy", "ok", "err"
    QPixmap m_qr;
    QString m_qrFor;
    QStringList m_vfd;         // the readout's three lines
    int m_tracks = -1;
    QElapsedTimer m_tracksAt;
    QSystemTrayIcon* m_tray = nullptr;
    QMenu* m_menu = nullptr;
    QString m_menuFor;
    QElapsedTimer m_clock;     // for the lamps that blink
};

} // namespace zc
