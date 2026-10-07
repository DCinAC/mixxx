#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>

#include "preferences/usersettings.h"
#include "zydek/zydekhttpserver.h"
#include "zydek/zydeklibrary.h"

class QTcpSocket;
class ZydekController;

namespace zydek {

/// The link between the Zydek pages (tablet controller, settings/debug page) and Mixxx.
///
/// Serves the pages and two WebSockets on port 8766, the same as the desktop Tablet Pads server
/// (mixxx_yt/pads/server.py), so the pages work unchanged:
///   /ws   controller page <-> Mixxx: JSON {set, sub, scratch, tempo, sync} and "d|u <channel> <note>" pad
///         presses become the MIDI/SysEx the Zydek mapping understands; its MIDI/SysEx output becomes
///         JSON state for the page (sampler/deck/cue/roll/stem/FX state, subscribed control values)
///   /ctl  controller settings shared with the debug page, and telemetry relayed to it
/// Unlike the desktop bridge it knows exactly which track each deck and sampler holds, and can hand the
/// page a track's analysed waveform (/waveform/<track id>).
class Hub : public QObject {
    Q_OBJECT
  public:
    static constexpr quint16 kPort = 8766;
    static constexpr int kNumSamplers = 64;
    static constexpr int kNumDecks = 4;

    Hub(UserSettingsPointer pConfig, QObject* pParent);

    void start();
    /// The open controller (nullptr while it's closed: the pages then show their offline simulation).
    void setController(ZydekController* pController);
    /// MIDI or SysEx the mapping sent to the "device".
    void fromMixxx(const QByteArray& message);

  private:
    struct Deck {
        int v = 0;   // state flags (play, loaded...) from the mapping
        QJsonValue name;   // {t, a, id} or null
        int cues[8] = {};
        int rolls[8] = {};
        int stems[4] = {1, 1, 1, 1};
        int stemCount = 0;
        int pitch = 0;
    };

    HttpServer::Response handleHttp(const QString& path, const HttpServer::Query& query);
    HttpServer::Response handleApi(const QString& path, const HttpServer::Query& query);
    HttpServer::Response staticFile(const QString& name) const;
    QByteArray waveform(int trackId) const;
    QString lanUrl() const;

    void onWsOpened(QTcpSocket* pClient, const QString& path, const HttpServer::Query& query);
    void onWsText(QTcpSocket* pClient, const QString& path, const QByteArray& text);
    void onWsClosed(QTcpSocket* pClient, const QString& path);

    // page -> Mixxx
    void handlePageMessage(const QByteArray& text);
    void sysex(const QList<int>& parts);
    void press(int channel, int note, bool down);

    // Mixxx -> pages
    void emitJson(const QJsonObject& message);
    void setDeckField(int deck, int& field, int value, const char* kind);
    void updateName(bool sampler, int index);
    QJsonObject snapshot() const;
    QJsonObject deckJson(int i) const;

    // settings shared by the controller page and the debug page
    void sendCtl(QTcpSocket* pClient, const QJsonObject& message);
    void broadcastPresence();
    void saveSettings() const;

    UserSettingsPointer m_pConfig;
    HttpServer m_server;
    Library m_library;   // the phone page's library API (/api/...)
    ZydekController* m_pController = nullptr;

    int m_samplers[kNumSamplers] = {};
    QJsonValue m_samplerNames[kNumSamplers];
    Deck m_decks[kNumDecks];
    int m_fx[4][7] = {};
    QMap<QString, int> m_subscriptions;   // "group,key" -> kind; re-sent when the mapping restarts
    QJsonObject m_controlValues;          // last value Mixxx reported per subscribed control

    QJsonObject m_ctlSettings;
    QHash<QTcpSocket*, QString> m_ctlRoles;
};

} // namespace zydek
