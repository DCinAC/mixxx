#pragma once

#include <QJsonObject>
#include <QString>

#include "preferences/usersettings.h"
#include "zydek/zydecklibraryquery.h"

namespace zydek {

class Library;

/// The phone page's Auto DJ: Mixxx's own (its processor, queue and settings, as its Auto DJ view uses them), so what
/// it does is what Mixxx does: decks 1 and 2, a crossfade by the crossfader, the queue played top down, and when it
/// runs low, random tracks from the crates marked as its source (or the whole library when none is).
///
/// Mixxx's objects are only touched on its main thread; the queue and crates are read from the library's database.
class AutoDj {
  public:
    AutoDj(UserSettingsPointer pConfig, Library* pLibrary);

    /// {on, state, queue: {id, count}, transition: {seconds, mode}, random: {on, min}, requeue, crates: [{id, name,
    /// count, source}]}. state: off | waiting (nothing playing yet) | playing | fading (1 → 2 or 2 → 1).
    QJsonObject status();
    /// on: Mixxx's Auto DJ on or off. It says why it won't: an empty queue, both decks playing, deck 3 or 4 playing.
    QJsonObject setOn(bool on);
    /// fade (now, to the next track) | skip (the next track) | skiptrack (the playing one, gracefully: to 16 beats
    /// before its transition, which then goes as usual) | random (one random track to the queue) | shuffle
    QJsonObject act(const QString& action);
    /// seconds: the crossfade's length (negative: a gap); mode: intro-outro | outro-start | full-track |
    /// skip-silence | skip-silence-full (Mixxx's Auto DJ "Transition" choices, in that order). Either may be empty.
    QJsonObject setTransition(const QString& seconds, const QString& mode);
    /// random: refill the queue from the source crates when it's shorter than min (Preferences › Auto DJ);
    /// requeue: a played track goes back to the end of the queue. Each may be empty (unchanged).
    /// beatmatch: Auto DJ's transitions synced when the tempos are near (zydekautodj.cpp's Beatmatch)
    QJsonObject setOptions(const QString& random, const QString& min, const QString& requeue, const QString& beatmatch = QString());
    /// crate is (or isn't) one of the crates random tracks come from.
    QJsonObject setSource(int crateId, bool source);
    /// where: bottom | top (next, after the one loaded) | replace (the queue becomes this track)
    QJsonObject add(int trackId, const QString& where);
    QJsonObject remove(int trackId);
    QJsonObject clear();

    /// Autoplay: the whole library through Auto DJ, at random, within limits (genres in and out, a BPM range), none
    /// twice in a session; the queue is kept two tracks ahead. pool: {genres, skip, bpmMin, bpmMax}; on: start Auto DJ
    /// too (and take over Mixxx's own top-up from crates), off: stop topping up (Auto DJ plays on to the queue's end).
    QJsonObject setAutoplay(bool on, const QJsonObject& pool);
    /// Every few seconds (the hub's timer): tops the queue up while autoplay is on and Auto DJ plays
    void autoplayTick();
    /// What played while Auto DJ or player mode ran, newest first: picked or cut in (the last 500, kept between
    /// sessions: zydeck-player-history.json)
    QList<int> autoplayHistory();
    /// The phrase mixing (zydekautodj.cpp's MixPlanner: by the tracks' sections, zydekstructurecache.h's files), on
    /// Mixxx's main thread from now on; setMix: its choices, {entry, exit, bars, style} (each given: changed)
    void startMixing();
    QJsonObject setMix(const QJsonObject& change);
    /// Player mode's buttons (the notification's, the lock screen's, a headset's): toggle | play | pause | next |
    /// stop. Pausing turns Auto DJ off (a deck paused under it confuses it) and playing turns it on again; play with
    /// player mode off starts it.
    QJsonObject player(const QString& what);

  private:
    void topUp();
    void readPool();
    QJsonObject autoplayJson() const;

    UserSettingsPointer m_pConfig;
    Library* m_pLibrary;
    bool m_autoplay = false;
    zydeck::library::Pool m_pool;
    bool m_poolRead = false;
    int m_pausedDeck = 0;   // the deck player mode paused (0: none)
    int m_queueMisses = 0;  // top-ups whose tracks didn't stay in the queue, running
    QString m_autoplayError;
};

} // namespace zydek
