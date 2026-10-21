#pragma once
// ZyDeck Connect's window: a rack unit. The face: ZYDECK CONNECT and its power lamp, a VFD readout (the MIDI port,
// Mixxx's library and the mapping, this computer's address), MAP (install the mapping) and the LOGIN switch, and the
// four device slots stacked as bays: the device and how it's linked, then its PAIR key (3:2, the word under it) and
// its MIDI IN / OUT lamps. The unit is as wide as the longest device name needs (30 px clear of that cluster).
// Under it, a tag slides out of the bottom edge with the pairing code printed on it: by itself while no device is
// linked, back in once one is. PAIR on an empty slot pulls it out for that slot (the next device lands there), and
// again (or a click on the tag) pushes it back. Drawn by hand (QPainter) in a frameless window; it moves by dragging
// the panel. The menu bar / tray icon keeps it running with the window closed.

#include <QElapsedTimer>
#include <QHash>
#include <QPixmap>
#include <QVariantAnimation>
#include <QWidget>

class QMenu;
class QSystemTrayIcon;

namespace zc {

class Core;

class Window : public QWidget {
    Q_OBJECT
  public:
    explicit Window(Core* core);
    void showPairingCode();   // the tag out (and kept out)

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

  private:
    enum class Key { None, Map, Login, Close, Tag, Slot };
    struct Hit {
        Key key = Key::None;
        int slot = -1;
        bool operator==(const Hit& o) const { return key == o.key && slot == o.slot; }
    };
    Hit hitAt(const QPointF& p) const;
    void press(const Hit& h);
    void refresh();            // once a second, and when a device comes or goes
    void setTag(bool out);     // slide it
    void layoutWidth();        // as wide as the device names need
    void updateTray();
    QIcon lampIcon(const QColor& c) const;
    QRectF bay(int slot) const;
    QRectF slotKey(int slot) const;
    QRectF tagRect() const;

    Core* m_core;
    int m_w;                   // the unit's width
    QVariantAnimation m_tagAnim;
    double m_tag = 0;          // 0 in, 1 out
    bool m_tagWanted = false;  // where it's going
    int m_tagManual = -1;      // PAIR's (or the tag's) say: -1 none (it follows the devices), 0 in, 1 out
    int m_pairSlot = -1;       // the slot whose PAIR pulled the tag out: the next device lands there
    QHash<QString, int> m_slotOf;   // device -> its slot
    int m_lastDeviceCount = -1;
    Hit m_down;
    QString m_mapLamp;         // the MAP key's 2x2 lamp: "", "busy", "ok", "err"
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
