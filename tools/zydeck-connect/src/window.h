#pragma once
// ZyDeck Connect's window (for now the barebones one: the system's own controls; the look you pick goes on top) and
// its icon in the menu bar / tray, which keeps it running with the window closed.

#include <QWidget>

class QCheckBox;
class QLabel;
class QListWidget;
class QMenu;
class QPushButton;
class QSystemTrayIcon;

namespace zc {

class Core;

class Window : public QWidget {
    Q_OBJECT
  public:
    explicit Window(Core* core);
    void showPairingCode();

  protected:
    void closeEvent(QCloseEvent* e) override;

  private:
    void refresh();
    void showCode(bool on);
    QIcon lampIcon(const QColor& c) const;   // ZyDeck's 2x2 lamp, lit in c

    Core* m_core;
    QLabel *m_port, *m_mixxx, *m_here, *m_code, *m_codeNote;
    QListWidget* m_devices;
    QPushButton *m_install, *m_showCode;
    QCheckBox* m_login;
    QSystemTrayIcon* m_tray = nullptr;
    QMenu* m_menu = nullptr;
    QString m_codeFor;   // the pairing code drawn (redrawn when the addresses change)
    QString m_menuFor;   // what the menu shows (rebuilt only when that changes: not under an open menu)
};

} // namespace zc
