#pragma once
// ZyDeck Connect's window: a rack unit. The face: ZYDECK CONNECT and its power lamp, a VFD readout (the MIDI port,
// Mixxx's library and the mapping, this computer's address), the four device slots stacked (each a bay with the
// device, how it's linked and its MIDI IN / OUT lamps), and MAP (install the mapping), PAIR and the LOGIN switch.
// Under it, a tag slides out of the bottom edge with the pairing code printed on it: by itself while no device is
// linked, back in once one is; PAIR pulls it out or pushes it in. Drawn by hand (QPainter), in a frameless window
// with nothing around the unit; it moves by dragging the panel. The menu bar / tray icon keeps it running closed.

#include <QElapsedTimer>
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
    enum class Key { None, Map, Pair, Login, Close };
    Key keyAt(const QPointF& p) const;
    void press(Key k);
    void refresh();            // once a second, and when a device comes or goes
    void setTag(bool out);     // slide it
    void updateTray();
    QIcon lampIcon(const QColor& c) const;

    Core* m_core;
    QVariantAnimation m_tagAnim;
    double m_tag = 0;          // 0 in, 1 out
    bool m_tagWanted = false;  // where it's going
    int m_tagManual = -1;      // PAIR's say (-1: none, then it follows the devices)
    int m_lastDeviceCount = -1;
    Key m_down = Key::None;
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
