#include "zydeck/stems.h"

#include <QAction>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QProgressDialog>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>
#include <utility>

#include "library/dao/playlistdao.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "track/cue.h"
#include "track/track.h"

namespace zydeck {

namespace {

// ZyDeck Connect on this computer (it answers this computer without its pairing key)
const QString kConnect = QStringLiteral("http://127.0.0.1:8771/web/");

QNetworkAccessManager* net() {
    static QNetworkAccessManager* pNet = new QNetworkAccessManager;
    return pNet;
}

// GET kConnect + what; done gets the answer (an object; {"error": ...} when it went wrong)
void ask(const QString& what, const QUrlQuery& query, std::function<void(const QJsonObject&)> done) {
    QUrl url(kConnect + what);
    url.setQuery(query);
    QNetworkRequest r(url);
    r.setTransferTimeout(15000);
    QNetworkReply* reply = net()->get(r);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done] {
        reply->deleteLater();
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() == QNetworkReply::ConnectionRefusedError || reply->error() == QNetworkReply::HostNotFoundError) {
            done({{"error", QObject::tr("ZyDeck Connect isn't running on this computer")}});
        } else if (o.isEmpty()) {
            done({{"error", reply->errorString()}});
        } else {
            done(o);
        }
    });
}

// One track's stems: asked for, then followed in a small window until they're in (or it went wrong)
void makeStems(const TrackPointer& pTrack) {
    const QString title = pTrack->getTitle().isEmpty() ? QFileInfo(pTrack->getLocation()).completeBaseName() : pTrack->getTitle();
    // (closed, it's gone: an answer that comes after finds nothing to update)
    QPointer<QProgressDialog> pDialog = new QProgressDialog(QObject::tr("Stems: %1").arg(title), QObject::tr("Cancel"), 0, 100);
    pDialog->setWindowTitle(QObject::tr("Making stems"));
    pDialog->setAttribute(Qt::WA_DeleteOnClose);
    pDialog->setMinimumDuration(0);
    pDialog->setAutoClose(false);
    pDialog->setAutoReset(false);
    pDialog->setValue(0);
    pDialog->show();
    const auto failed = [pDialog, title](const QString& error) {
        if (pDialog) {
            pDialog->close();
        }
        QMessageBox::warning(nullptr, QObject::tr("Making stems"), QObject::tr("Stems for %1: %2").arg(title, error));
    };
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("path"), pTrack->getLocation());
    ask(QStringLiteral("stems"), q, [pDialog, title, failed](const QJsonObject& started) {
        if (!pDialog) {
            return;
        }
        const QString job = started.value(QStringLiteral("job")).toString();
        if (job.isEmpty()) {
            failed(started.value(QStringLiteral("error")).toString(QObject::tr("ZyDeck Connect said no")));
            return;
        }
        QObject::connect(pDialog, &QProgressDialog::canceled, pDialog, [job] {
            QUrlQuery c;
            c.addQueryItem(QStringLiteral("id"), job);
            ask(QStringLiteral("cancel"), c, [](const QJsonObject&) {});
        });
        auto* pPoll = new QTimer(pDialog);
        pPoll->setInterval(1500);
        QObject::connect(pPoll, &QTimer::timeout, pDialog, [pDialog, job, title, failed, pPoll] {
            QUrlQuery j;
            j.addQueryItem(QStringLiteral("id"), job);
            ask(QStringLiteral("job"), j, [pDialog, title, failed, pPoll](const QJsonObject& s) {
                if (!pDialog) {
                    return;
                }
                const QString state = s.value(QStringLiteral("state")).toString();
                if (state == QLatin1String("done")) {
                    pPoll->stop();
                    pDialog->setValue(100);
                    pDialog->setLabelText(QObject::tr("Stems: %1\nDone: in the ZyDeck Web crate, with its cues and beat grid").arg(title));
                    QTimer::singleShot(4000, pDialog, &QWidget::close);
                } else if (state == QLatin1String("error") || s.contains(QStringLiteral("error"))) {
                    pPoll->stop();
                    const QString e = s.value(QStringLiteral("error")).toString();
                    if (e == QLatin1String("Cancelled")) {
                        pDialog->close();
                    } else {
                        failed(e);
                    }
                } else {
                    pDialog->setValue(static_cast<int>(100 * s.value(QStringLiteral("progress")).toDouble()));
                    pDialog->setLabelText(QObject::tr("Stems: %1\n%2").arg(title, s.value(QStringLiteral("message")).toString()));
                }
            });
        });
        pPoll->start();
    });
}

} // namespace

void addStemsAction(QMenu* pMenu, std::function<TrackPointerList()> tracks) {
    QAction* pAction = pMenu->addAction(QObject::tr("Make Stems (ZyDeck)"));
    pAction->setToolTip(QObject::tr("Separate the track into stems on the PC's stem server, through ZyDeck Connect"));
    QObject::connect(pAction, &QAction::triggered, pMenu, [tracks] {
        for (const TrackPointer& pTrack : tracks()) {
            if (pTrack && !pTrack->getLocation().endsWith(QLatin1String(".stem.mp4"), Qt::CaseInsensitive)) {
                makeStems(pTrack);
            }
        }
    });
}

void takeOver(TrackCollectionManager* pManager, const TrackPointer& pStem, const TrackPointer& pOriginal) {
    if (!pStem || !pOriginal || pStem == pOriginal) {
        return;
    }
    if (const mixxx::BeatsPointer pBeats = pOriginal->getBeats()) {
        pStem->trySetBeats(pBeats);
    }
    pStem->setKeys(pOriginal->getKeys());
    pStem->setColor(pOriginal->getColor());
    pStem->setRating(pOriginal->getRating());
    pStem->setMainCuePosition(pOriginal->getMainCuePosition());
    for (const CuePointer& pCue : pOriginal->getCuePoints()) {
        if (pCue->getType() == mixxx::CueType::MainCue) {
            continue;   // (set above)
        }
        const CuePointer pNew = pStem->createAndAddCue(pCue->getType(), pCue->getHotCue(), pCue->getPosition(),
                pCue->getEndPosition(), pCue->getColor());
        if (pNew) {
            pNew->setLabel(pCue->getLabel());
        }
    }
    // and the original says it has stems: "stem available" in its comment (before what was there, if anything)
    const QString kNote = QStringLiteral("stem available");
    const QString comment = pOriginal->getComment().trimmed();
    if (!comment.startsWith(kNote)) {
        pOriginal->setComment(comment.isEmpty() ? kNote : kNote + QStringLiteral(", ") + comment);
    }
    TrackCollection* const pCollection = pManager->internalCollection();
    const TrackId stemId = pStem->getId(), originalId = pOriginal->getId();
    for (const CrateId& crateId : pCollection->crates().collectCrateIdsOfTracks({originalId})) {
        pCollection->addCrateTracks(crateId, {stemId});
    }
    PlaylistDAO& playlists = pCollection->getPlaylistDAO();
    QSet<int> in;
    playlists.getPlaylistsTrackIsIn(originalId, &in);
    for (const int playlistId : std::as_const(in)) {
        if (playlists.getHiddenType(playlistId) == PlaylistDAO::PLHT_NOT_HIDDEN) {
            playlists.appendTrackToPlaylist(stemId, playlistId);
        }
    }
}

} // namespace zydeck
