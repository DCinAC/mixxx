#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
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
    /// Controller mode (Android): bytes from the computer over the USB MIDI cable, and the cable's state
    /// (0 unplugged, 1 plugged in but not set to MIDI, 2 ready), from ZydekMidi.java.
    void usbMidiReceived(const QByteArray& data);
    void usbMidiState(int state);
    /// The output's measured latency (ZydekAudio.java), for drawing the waveforms in time with the sound.
    void outputLatencyMeasured(int ms, const QString& route);

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
    // Sampler pad modes (ZyDeck's own, shared by every page): "oneshot" plays to the end (a tap restarts it),
    // "hold" plays while held, "loop" starts and stops on alternate taps. Kept in zydek-samplers.json.
    QJsonObject samplerModes();
    QJsonObject setSamplerMode(int slot, const QString& mode);
    // Sampler kits: named sets of what's on the 32 pads (file, mode, following the tempo), in zydek-kits.json.
    QJsonObject kits() const;
    QJsonObject saveKit(const QString& name);
    QJsonObject loadKit(const QString& name);
    QJsonObject deleteKit(const QString& name);
    // audio outputs (the phone page's settings)
    QJsonObject audioStatus() const;
    QJsonObject setAudio(const HttpServer::Query& query);
    QJsonObject setAudioBuffer(int index);
    // Output latency compensation: measured (or Mixxx's buffer until then) + the user's fine-tune offset
    int m_measuredLatencyMs = -1;
    QString m_latencyRoute;
    int m_latencyOffsetMs = 0;
    double latencySeconds() const;
    void emitLatency();
    void saveAudioPrefs() const;
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
    void updateName(bool sampler, int index, int lengthMs);
    void applyName(bool sampler, int index, const QJsonValue& name);
    void handleMixxx(const QByteArray& msg);   // from the mapping, here or on the computer
    QJsonObject snapshot() const;
    QJsonObject deckJson(int i) const;

    // settings shared by the controller page and the debug page
    void sendCtl(QTcpSocket* pClient, const QJsonObject& message);
    void broadcastPresence();
    void saveSettings() const;

    UserSettingsPointer m_pConfig;
    HttpServer m_server;
    Library m_library;   // the phone page's library API (/api/...)
    QStringList m_samplerModes;   // per sampler, 1..64 at 0..63 (empty until read from the file)
    // "Last session": the pads as they are, saved as a kit a moment after they change and loaded again when
    // ZyDeck starts (Mixxx on Android doesn't keep its samplers between runs)
    QTimer m_lastSessionTimer;
    bool m_lastSessionRestored = false;
    void restoreLastSession(int tries);
    ZydekController* m_pController = nullptr;

    // Controller mode: the pages control Mixxx on a computer instead of the one here. The same MIDI/SysEx
    // goes over the USB MIDI cable, or over Wi-Fi to ZyDeck Link on the computer (which hands it to Mixxx
    // through its "ZyDeck" MIDI port). ZyDeck Link answers a hello over the cable with its address and a
    // key (F0 7D 60/61), and looks up track names (F0 7D 62/63); then the pages offer the wireless link.
    struct MidiParser {   // a MIDI byte stream (running status, SysEx split over packets) into messages
        QByteArray message;
        int status = 0;
        bool sysex = false;
        template<typename F>
        void feed(const QByteArray& data, F&& onMessage);
    };
    bool m_controllerMode = false;
    int m_usbState = 0;
    bool m_remoteLive = false;   // the mapping on the computer answered
    qint64 m_remoteRxMs = 0;     // when it last did
    qint64 m_helloMs = -100000;  // when the hello last went over the cable
    MidiParser m_usbParser;
    MidiParser m_linkParser;
    QTcpSocket* m_pLink = nullptr;   // to ZyDeck Link over Wi-Fi
    QByteArray m_linkBuffer;
    bool m_linkUp = false;
    int m_linkHost = 0;              // which of the computer's addresses is being tried
    QJsonObject m_linkTarget;        // the computer being linked to (name, hosts, port, key)
    QJsonObject m_linkOffer;         // what ZyDeck Link said over the cable
    bool m_offerDeclined = false;
    QJsonObject m_linkSaved;         // the computer linked last time, tried again over Wi-Fi
    QTimer m_remoteTimer;
    void setControllerMode(bool on);
    QJsonObject remoteStatus() const;
    void emitRemote();
    bool live() const;
    bool canSend() const;
    void sendMidi(const QByteArray& data);
    void setControl(const QString& group, const QString& key, double value);
    void remoteMessage(const QByteArray& msg, bool viaUsb);
    void sendSysexText(int type, const QByteArray& text);
    void linkConnect(const QJsonObject& computer);
    void linkDrop(bool retry);
    void linkLine(const QByteArray& line);
    void remoteTick();
    void resendSubscriptions();
    void resetMixxxState();
    void loadRemote();
    void saveRemote() const;
    QString deviceName() const;

    // Controller mode's "Standard MIDI" layout, for Serato, rekordbox and other DJ apps (MIDI Learn): plain
    // notes and CCs instead of the ZyDeck mapping's SysEx; their LED notes come back as the pages' state.
    // The chart is in tools/zydeck-link/MIDI.md.
    bool m_standardMidi = false;
    int m_stdKeyShift[kNumDecks] = {};
    bool standardMidi() const { return m_controllerMode && m_standardMidi; }
    void standardFromPage(const QJsonObject& m);
    void standardPad(int channel, int note, bool down);
    void standardFromDj(const QByteArray& msg);
    void stdSend(int status, int data1, int data2);

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
