#pragma once
// ZyDeck Connect's downloads and stems: the computer does them, for a phone or tablet's Web tab in controller mode
// and for Mixxx ZyDeck's track menu, over Connect's HTTP port (core.h; the pairing key, or from this computer itself):
//   GET /web/search?q=                 a YouTube search, a pasted link or a Bilibili BV code: a job whose result is a
//                                      list of {id, title, channel, duration, views, thumbnail, url, site}
//   GET /web/download?url=&title=      the audio into the downloads folder: a job, its result
//                                      {path, artist, title, duration, codec, kbps, playable}
//   GET /web/stems?path=&preset=&server=   a track's stems from the PC's stem server (stemserver/server.py), into the
//                                      downloads folder as "<name>.stem.mp4": a job, its result {path, artist, title,
//                                      seconds, original}
//   GET /web/job?id=                   {state: running|done|error, progress 0-1, message, result | error}
//   GET /web/cancel?id=
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
    QJsonObject handle(const QString& what, const QUrlQuery& query, int* status);
    /// Where downloads go ("" while Mixxx has no music folder).
    QString folder() const;
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
    void stemsPoll(Job* job);
    void stemsFetch(Job* job);
    QNetworkReply* stemRequest(const QString& method, const QString& path, QIODevice* body = nullptr);
    void prune();

    Library* m_library;
    QNetworkAccessManager m_net;
    QHash<QString, Job*> m_jobs;
};

} // namespace zc
