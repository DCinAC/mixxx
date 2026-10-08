#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <QTimer>

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
/// Latency log: the controller page pings through /ws (and through the mapping, when it runs) and sends a
/// summary every second over /ctl; the hub adds its own numbers and appends it to zydek-latency.log in the
/// settings folder (GET /api/latency-log?lines=N).
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
    QByteArray beats(int trackId) const;
    // audio outputs (the phone page's settings)
    QJsonObject audioStatus() const;
    QJsonObject setAudio(const HttpServer::Query& query);
    QString lanUrl() const;
    /// The phone's addresses a tablet can reach: [{label: "Wi-Fi" | "Hotspot", url}], Wi-Fi first.
    QJsonArray lanUrls() const;

    void onWsOpened(QTcpSocket* pClient, const QString& path, const HttpServer::Query& query);
    void onWsText(QTcpSocket* pClient, const QString& path, const QByteArray& text);
    void onWsClosed(QTcpSocket* pClient, const QString& path);

    // latency log
    void ping(QTcpSocket* pClient, const QJsonArray& ping);
    void pong(int seq, bool viaMapping);
    void logLatency(const QJsonObject& summary);
    HttpServer::Response latencyLog(const HttpServer::Query& query) const;

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

    struct Ping {
        QTcpSocket* pClient;
        double clientMs;
        qint64 hubMs;
    };
    QElapsedTimer m_clock;
    QHash<int, Ping> m_pings;   // sent through the mapping, waiting for its echo
    // How late this (the controller) thread runs a 20 ms timer, and HTTP requests that took long: both
    // delay the tablet's input, which is handled on the same thread.
    QTimer m_lagTimer;
    qint64 m_lagLastMs = 0;
    qint64 m_lagMaxMs = 0;
    QJsonArray m_slowRequests;
};

} // namespace zydek
