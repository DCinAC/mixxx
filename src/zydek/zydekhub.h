#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <memory>

#include "preferences/usersettings.h"
#include "track/beats.h"
#include "zydek/zydekhttpserver.h"
#include "zydek/zydeklibrary.h"

class QTcpSocket;
class VisualPlayPosition;
class ZydekController;

namespace zydek {
class LiveStems;
class DiscordRelay;

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
    ~Hub() override;

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
    // (pLoaded: whether the track is on a deck at all: empty and loaded means it's still being analysed)
    QByteArray waveform(int trackId, bool stems = false, bool* pLoaded = nullptr) const;
    QByteArray beats(int trackId, bool* pLoaded = nullptr) const;
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
    // No output open (the one chosen last time not found under the same name, a Bluetooth device gone):
    // Mixxx's engine then stops altogether (nothing plays, records or streams). After 4 s of that, the main
    // goes to "Android: follow the system".
    int m_audioDownSeconds = 0;
    int m_audioTick = 0;
    void checkAudioOutput();
    // One-tap audio optimisation: plays a heavy load on all four decks with the main output silent (two
    // stems tracks when there are some, key lock, +4 %), tries buffers from the smallest until one has no
    // dropouts, keeps that one, puts the decks and the volume back and measures the output again.
    struct Optimize {
        bool running = false;
        int serial = 0;
        int step = 0;               // index into kOptimizeBuffers being tried
        double mainGain = 1;
        int xrunsAt = 0, xrunsMid = 0;   // the output's underruns as the step's measuring starts, and halfway
        QJsonArray results;         // [{buffer, glitches, late, queueMs, bursts}]
        QString state;              // what it's doing, for the pages
        int chosen = 0;
        int chosenBursts = 0;       // the queue it settled at with that block
        QString error;
    };
    Optimize m_optimize;
    // Recording the set: Mixxx's recorder (the main output, effects and EQ included, after the main volume)
    // as FLAC in Music/ZyDeck Recordings, with a .cue tracklist beside it
    QTimer m_recordTimer;
    QElapsedTimer m_recordClock;
    int m_recordStatus = 0;
    bool m_recordPreFx = false;   // record before the effect units (EQ and filter in), not what's heard
    QJsonObject setRecording(bool on);
    QJsonObject recordStatus() const;
    void recordTick();
    // Slip: where the track runs on underneath a loop, a scratch or CENSOR (Mixxx's slip position), sent to
    // the pages while it differs from where the deck plays, for the ghost waveform Mixxx draws above
    // Mixxx's Echo and Reverb load with their meta knob at 0, and their Send follows it: silent until it's
    // turned up (the Beat FX layout has no meta knob). One loaded so in unit 1 or 2 gets a usable send.
    double m_loadedFx[2][3] = {{-1, -1, -1}, {-1, -1, -1}};
    void checkFxMeta();
    // Discord: the mix streamed (Mixxx's Live Broadcasting, Opus, the recording's signal) to the ZyDeck
    // Discord bot (tools/zydeck-discord), which plays it in a voice channel
    QString m_discordHost;   // the bot: host[:port]
    QString m_discordKey;    // its stream key
    DiscordRelay* m_pRelay = nullptr;   // Mixxx streams to it on this device; it sends on to the bot
    int m_discordStatus = -1;
    QJsonObject setDiscord(bool on);
    QJsonObject discordStatus() const;
    // While live: the bot's /status every 10 s (who's listening; kicked from the channel: a notice)
    QPointer<QTcpSocket> m_pBotCheck;
    QJsonObject m_bot;
    double m_botKickedAt = 0;
    int m_botTick = 0;
    void checkBot();
    void onBotStatus(const QJsonObject& status);
    QTimer m_slipTimer;
    QSharedPointer<VisualPlayPosition> m_visualPos[kNumDecks];
    QMetaObject::Connection m_waveformDone[kNumDecks];   // each deck's track: tells the pages when it's analysed
    bool m_slipShown[kNumDecks] = {};
    void slipTick();
    QJsonObject startOptimize();
    void optimizeStep(int serial);
    void optimizeMeasure(int serial);
    void finishOptimize(const QString& error);
    QJsonObject optimizeStatus() const;
    // FX panel: the effects Mixxx shows (by name, in its order) and what units 1-4 have loaded; beat lengths
    // go to whichever parameter of the effect is measured in beats (Echo's Time, Flanger's period...)
    QJsonObject fxStatus() const;
    // Stem FX (the STEM pads' second row, like Serato's): 0 echo out the vocals, 1 the instrumental, 2 brake
    // the instrumental, 3 echo out the drums; again: the stems back. Effect unit 3 (decks 1/3) or 4 (2/4).
    QJsonObject stemFx(int deck, int fx);
    void stemFxEnd(int deck, int unit);
    struct StemFxRun {
        int fx = -1;     // running, or its stems out
        int serial = 0;  // timers of an earlier run see a newer serial and stop
    };
    StemFxRun m_stemFx[kNumDecks];
    QJsonArray stemFxJson() const;
    QJsonObject setFxBeats(int unit, int slot, double beats);
    // Output latency compensation: measured (or Mixxx's buffer until then) + the user's fine-tune offset
    int m_measuredLatencyMs = -1;
    /// When Mixxx's own output stream is heard, from Android's timestamps of it (PortAudio's Oboe host, our patch:
    /// PaOboe_GetOutputPresentationDelay), ms; -1 unknown. Once known it stands for buffer + m_measuredLatencyMs
    /// (that one's a separate test sound).
    int m_streamDelayMs = -1;
    QTimer m_streamDelayTimer;
    void streamDelayTick();
    QString m_latencyRoute;
    /// The user's fine-tune (Calibrate, or the slider), per kind of output (routeKind()); 0 where never set
    QHash<QString, int> m_latencyOffsets;
    int latencyOffsetMs() const;   // the one for the output now
    /// The output's queue (how far ahead of the hardware Android's buffer is kept filled; PortAudio's Oboe host grows it
    /// a burst at each underrun), in bursts, per kind of output (routeKind()): where it settled, so the next stream on that
    /// kind starts there (Optimize sets it); 4 until known
    QHash<QString, int> m_queueBursts;
    static constexpr int kDefaultQueueBursts = 4;
    QString routeKind() const;     // "speaker", "USB", "wired", "Bluetooth", "output"; "" until ZydekAudio has looked
    void applyQueue();             // the learned size for the output now
    QJsonObject outputPath() const;   // PaOboe_GetOutputStatus: straight to the hardware or through Android's mixer, etc.
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
    // Live stems (zydekstems.h): a deck's track separated on the phone when it loads, then swapped for its
    // stems version (at once if the deck isn't playing; the STEM pads offer it if it is, the swap has a gap).
    struct DeckStems {
        int trackId = 0;
        QString state;   // "" | "making" | "ready" (waiting for the swap) | "on" | "failed"
        double progress = 0;
        QString error;
        bool onPc = false;   // making them on the PC's stem server (not this phone)
    };
    int m_stemServerTick = 0;
    // KEY LOCK, QUANTIZE, SLIP when a deck gets a new track (not a live-stems swap): 1 on, 0 off, -1 as they were
    int m_deckDefaults[3] = {-1, -1, -1};
    /// Lining the waveform up by ear (/api/calibrate, the /calibrate page): the test pattern plays on the preview
    /// deck, in the headphones
    struct Calibration {
        QString group;
    } m_calib;
    /// The beat grid editor: each deck's grid as it was when the editor opened (/api/grid/begin), put back if its
    /// edits are thrown away (/api/grid/revert)
    struct GridStart {
        QString location;          // the track's file (it must still be that one)
        mixxx::BeatsPointer beats;
        bool locked = false;
    };
    QHash<QString, GridStart> m_gridStart;
    /// Its "Analyze range" (Library::analyzeRange), on a worker: one at a time, polled (/api/grid/range?status=1)
    struct GridRange {
        bool running = false;
        int serial = 0;
        QJsonObject result;   // once done: {ok, bpm} or {ok: false, error}
        std::shared_ptr<std::atomic<bool>> cancel;
    } m_gridRange;
    QJsonObject debugPos(const QString& group);
    /// Where each playing deck (and the preview deck) is heard, ~60 times a second: Mixxx's own waveform position
    /// (its audio timing) and speed, stamped with the hub's clock. Pages draw from it (no polling, no smoothing).
    void visualTick();
    QSharedPointer<VisualPlayPosition> visualPos(const QString& group);
    QTimer m_visualTimer;
    QHash<QString, QSharedPointer<VisualPlayPosition>> m_visualPosOf;
    QHash<QString, int> m_visualQuiet;   // ticks a group has been still (it's sent a few times after it stops)
    double visualExtraSeconds() const;   // the delay Mixxx's own timing doesn't know about (see latencySeconds)
    // The decks as they play (zydek-decks.json: track, position, tempo, key shift, KEY LOCK, QUANTIZE), loaded back at
    // start, paused where they were, when the last run stopped while something played or crashed
    QString m_deckSessionKey;      // the tracks and what played, at the last write
    qint64 m_deckSessionAt = 0;
    bool m_deckSessionReady = false;   // not written before the restore had its chance
    QString m_notice;              // the latest notice, for pages opening just after (with its time)
    qint64 m_noticeAt = 0;
    void saveDeckSession();
    void restoreDeckSession(int tries);
    void notice(const QString& text);
    void applyDeckDefaults(int deck);
    QJsonArray deckDefaultsJson() const;
    QJsonObject calibrate(bool on);
    static QString calibrationFile();
    bool m_stemServer = false;   // a stem server is set (the Web tab's settings): stems are made there first
    bool stemServerSet() const;
    LiveStems* m_pLiveStems = nullptr;
    bool m_liveStemsAuto = true;
    DeckStems m_deckStems[kNumDecks];
    struct PendingSeek {   // a swap (to the stems, or back): where the deck was, for when the other file loads
        double position = 0;
        bool playing = false;
        QString location;   // the file that should land there
    };
    QHash<QString, PendingSeek> m_pendingSeek;   // by group
    void deckTrackLoaded(int deck, const TrackPointer& pTrack);
    // The Stems crate, like Serato's: every track in the crate called "Stems" gets its stems made in the
    // background (the PC's when there is one), whatever live stems is set to; taking it out deletes them.
    QTimer m_stemsCrateTimer;
    QSet<int> m_stemsCrate;
    QSet<int> m_stemsCrateAsked;   // asked for this run (a failing one isn't asked every 15 s)
    bool m_stemsCrateRead = false;
    void checkStemsCrate();
    QJsonObject stemsCrateStatus(int trackId);
    /// A file standing in for a track on the decks: our live stems, or the PC's adopted for it.
    bool isLiveStems(const QString& location) const;
    void liveStemsProgress(int trackId, double fraction);
    void liveStemsFinished(int trackId, const QString& path, const QString& error);
    QJsonObject swapToStems(int deck, bool keepPosition);
    /// Back to the original track, where it is (live stems turned off), when its stems aren't in use.
    QJsonObject swapToOriginal(int deck);
    void emitDeckStems(int deck);
    QJsonArray deckStemsJson() const;
    QJsonObject liveStemsStatus() const;

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
