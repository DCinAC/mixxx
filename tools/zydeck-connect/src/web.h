#pragma once
// ZyDeck Connect's downloads and stems: the computer does them, for a phone or tablet's Web tab in controller mode
// and for Mixxx ZyDeck's track menu, over Connect's HTTP port (core.h; the pairing key, or from this computer itself):
//   GET /web/search?q=                 a YouTube search, a pasted link or a Bilibili BV code: a job whose result is a
//                                      list of {id, title, channel, duration, views, thumbnail, url, site}
//   GET /web/download?url=&title=      the audio into the downloads folder: a job, its result
//                                      {path, artist, title, duration, codec, kbps, playable}
//   GET /web/stems?path=&preset=&server=   a track's stems from the PC's stem server (stemserver/server.py), into the
//                                      downloads folder as "<name>.stem.mp4", with a hidden note naming the original
//                                      (Mixxx ZyDeck gives the stems its cues and beat grid): a job, its result
//                                      {path, artist, title, seconds, original}
//   GET /web/job?id=                   {state: running|done|error, progress 0-1, message, result | error}
//   GET /web/cancel?id=
//   GET /web/fx                        Mixxx's effects as a unit's slots number them: {effects: [name, ...], beats:
//                                      [{knob, min, max} or null, ...] (the knob in beats, for the BEAT keys)} (Mixxx
//                                      ZyDeck's live list, else effects.xml, which Mixxx writes as it quits)
// The phone's library page in controller mode, on this computer's Mixxx (the same answers as its hub's /api/...):
//   GET /web/library/views | tracks?q=&view=&sort=&limit=&offset=&genre= ({tracks: [...]}) | genres | track?id=
//   GET /web/load?id=&target=deck1..4|preview   (or job=: a stems job's file) a job: Mixxx ZyDeck loads it there
//                                      (a preview plays in the headphones); GET /web/preview/stop
//   GET /web/autodj                    Mixxx's Auto DJ: {ready, on, state, transition, random, requeue, queue, crates,
//                                      done: [{id, ok, error}]}; and what changes it, each answering {asked: id} (its
//                                      outcome comes in done): /web/autodj/on?on=, act?do=fade|skip|random|shuffle,
//                                      transition?seconds=&mode=, options?random=&min=&requeue=, source?crate=&on=,
//                                      add?id=&where=bottom|top|replace, remove?id=, clear (Mixxx ZyDeck carries them
//                                      out: its zydeck/autodjrequest.h)
// The same shapes as the phone's own downloader (res/zydek/python/zydek_web.py), so a page needs only the address.
// The downloads folder is "ZyDeck Web" in Mixxx's first music folder (settings: web/folder), which Mixxx ZyDeck
// watches: a file is written into its hidden ".incoming" folder and moved in when it's finished, so Mixxx only ever
// sees whole files and takes each in as it lands. yt-dlp does the downloading (bundled with Connect, else the
// computer's own); ffmpeg, if the computer has it, tags the file (title, artist, cover, link).

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkReply;
class QProcess;
class QUrlQuery;

namespace zc {

class Library;

class Web : public QObject {
    Q_OBJECT
  public:
    explicit Web(Library* library, QObject* parent = nullptr);
    ~Web() override;

    /// /web/<what>: the JSON answer; status gets the HTTP status. On this object's thread.
    QJsonObject handle(const QString& what, const QUrlQuery& query, int* status, bool keyed);   // keyed: with the pairing key
    /// /web/library/... and /web/autodj (the reading ones): safe on any thread (a worker's: they read the database)
    static QJsonObject library(Library* library, const QString& what, const QUrlQuery& query, int* status);
    /// Where downloads go ("" while Mixxx has no music folder).
    QString folder() const;
    /// {effects: [names]}: Mixxx's effects in its order (a slot's loaded_effect is a place in it)
    static QJsonObject effects();
    static QString ytDlp();    // "" if there's none
    static QString ffmpeg();   // "" if there's none

  private:
    struct Job {
        QString id, kind, title;
        QString state = QStringLiteral("running");
        double progress = 0;
        QString message;
        QJsonValue result;
        QString error;
        qint64 endedMs = 0;
        bool cancelled = false;
        QPointer<QProcess> process;
        QPointer<QNetworkReply> reply;
        QString remoteId, server, original, outName;   // stems
        QString deck;                                    // a download to load there ("[Channel1]"; "": none)
    };
    Job* start(const QString& kind, const QString& title);
    void finish(Job* job, const QJsonValue& result);
    void fail(Job* job, const QString& error);
    QProcess* run(Job* job, const QStringList& args);
    void search(Job* job, const QString& query);
    void searchWith(Job* job, const QString& target);
    void download(Job* job, const QString& url);
    void downloaded(Job* job, const QJsonObject& info);
    void stems(Job* job, const QString& path, const QString& preset);
    void loadToDeck(Job* job, QJsonObject result);   // (finishes the job when Mixxx ZyDeck has, or hasn't)
    QJsonObject askAutoDj(QJsonObject request);      // (handed to Mixxx ZyDeck: {asked: id})
    void stemsPoll(Job* job);
    void stemsFetch(Job* job);
    static QString placeStem(const QString& local, const QString& dir, const QString& name, const QString& original, QString* error);
    void stemPlaced(Job* job, const QString& local, const QString& dir, const QString& placed, const QString& error);
    QNetworkReply* stemRequest(const QString& method, const QString& path, QIODevice* body = nullptr);
    void prune();

    Library* m_library;
    QNetworkAccessManager m_net;
    QHash<QString, Job*> m_jobs;
};

} // namespace zc
