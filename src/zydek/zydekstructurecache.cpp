#include "zydek/zydekstructurecache.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QThread>
#include <cmath>
#include <vector>

#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "qml/qmllibraryproxy.h"
#include "sources/audiosource.h"
#include "sources/soundsourceproxy.h"
#include "track/beats.h"
#include "track/track.h"
#include "util/samplebuffer.h"
#include "zydek/zydeckstructure.h"
#include "zydek/zydeckstructurejson.h"
#include "zydek/zydekmain.h"

namespace zydek {

namespace {

constexpr double kRate = 22050;   // about this (the file's rate over a whole number)

QJsonObject error(const QString& message) {
    return {{"ok", false}, {"error", message}};
}

struct Grid {
    std::vector<double> beats;   // seconds, from the track's start
    int bar = 0;                 // beat i starts a bar when (i + bar) % 4 == 0 (Mixxx: from its first marker)
    QString signature;           // what the result was worked out from: when it changes, it's out of date
};

// The track's beat grid, as the hub's /beats/<id> gives it
Grid gridOf(const TrackPointer& pTrack) {
    Grid grid;
    const mixxx::BeatsPointer pBeats = pTrack->getBeats();
    const double rate = pTrack->getSampleRate().toDouble();
    if (!pBeats || rate <= 0) {
        return grid;
    }
    const double endFrame = pTrack->getDuration() * rate;
    auto it = pBeats->iteratorFrom(mixxx::audio::kStartFramePos);
    const int firstIndex = static_cast<int>(it - pBeats->cfirstmarker());
    for (int n = 0; n < 20000 && it->value() < endFrame; ++n, ++it) {
        grid.beats.push_back(it->value() / rate);
    }
    grid.bar = ((firstIndex % 4) + 4) % 4;
    grid.signature = zydeck::structure::signature(grid.beats, grid.bar);
    return grid;
}

// The whole file, one channel, at about kRate
bool decode(const TrackPointer& pTrack, std::vector<float>* pMono, double* pRate, const std::atomic<bool>& cancel) {
    mixxx::AudioSource::OpenParams params;
    params.setChannelCount(mixxx::audio::ChannelCount::stereo());   // (a stems file: its mix)
    const mixxx::AudioSourcePointer pSource = SoundSourceProxy(pTrack).openAudioSource(params);
    if (!pSource || pSource->getSignalInfo().getChannelCount() != mixxx::audio::ChannelCount::stereo()) {
        return false;
    }
    const double rate = pSource->getSignalInfo().getSampleRate().toDouble();
    const int step = std::max(1, static_cast<int>(std::lround(rate / kRate)));   // frames averaged into one sample
    *pRate = rate / step;
    const mixxx::IndexRange frames = pSource->frameIndexRange();
    pMono->clear();
    pMono->reserve(static_cast<size_t>(frames.length() / step + 1));
    constexpr SINT kChunk = 65536;
    mixxx::SampleBuffer buffer(kChunk * 2);
    double sum = 0;
    int summed = 0;
    for (SINT at = frames.start(); at < frames.end();) {
        if (cancel.load()) {
            return false;
        }
        const mixxx::ReadableSampleFrames read = pSource->readSampleFrames(mixxx::WritableSampleFrames(
                mixxx::IndexRange::between(at, std::min(at + kChunk, frames.end())), mixxx::SampleBuffer::WritableSlice(buffer)));
        const SINT n = read.readableLength() / 2;   // (readableLength: samples, two per frame)
        if (n <= 0) {
            break;
        }
        const CSAMPLE* p = read.readableData();
        for (SINT i = 0; i < n; ++i) {
            sum += 0.5 * (p[2 * i] + p[2 * i + 1]);
            if (++summed == step) {
                pMono->push_back(static_cast<float>(sum / step));
                sum = 0;
                summed = 0;
            }
        }
        at = read.frameIndexRange().end();
    }
    return !pMono->empty();
}

} // namespace

StructureCache::StructureCache(UserSettingsPointer pConfig)
        : m_pConfig(pConfig) {
    m_pool.setMaxThreadCount(1);   // one track at a time: it shares the device with the decks
}

StructureCache::~StructureCache() {
    m_cancel = true;
    m_pool.waitForDone();
}

QString StructureCache::path(int trackId) const {
    return QDir(m_pConfig->getSettingsPath()).filePath(QStringLiteral("zydeck-structure/%1.json").arg(trackId));
}

QJsonObject StructureCache::get(int trackId, bool fresh) {
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    if (trackId <= 0 || !pLibrary) {
        return error(QStringLiteral("No such track"));
    }
    TrackPointer pTrack;
    TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
    runOnMain(pLibrary, [&] { pTrack = pCollection->getTrackById(TrackId(QVariant(trackId))); });
    if (!pTrack) {
        return error(QStringLiteral("No such track"));
    }
    Grid grid = gridOf(pTrack);
    if (grid.beats.size() < 64) {
        return error(QStringLiteral("No beat grid yet: analyze the track first"));
    }
    QMutexLocker lock(&m_mutex);
    if (m_running.contains(trackId)) {
        return {{"ok", true}, {"pending", true}};
    }
    if (!fresh) {
        QFile f(path(trackId));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject kept = QJsonDocument::fromJson(f.readAll()).object();
            if (kept.value(QStringLiteral("sig")).toString() == grid.signature) {
                return kept;
            }
        }
        const QJsonObject failed = m_failed.value(trackId);
        if (failed.value(QStringLiteral("sig")).toString() == grid.signature) {
            return error(failed.value(QStringLiteral("error")).toString());
        }
    }
    m_failed.remove(trackId);
    m_running.insert(trackId);
    const QString file = path(trackId);
    m_pool.start([this, pTrack, grid = std::move(grid), trackId, file] {
        QThread::currentThread()->setPriority(QThread::LowestPriority);
        std::vector<float> mono;
        double rate = 0;
        zydeck::structure::Result r;
        if (!decode(pTrack, &mono, &rate, m_cancel)) {
            r.error = m_cancel.load() ? "Cancelled" : "Couldn't read the track";
        } else {
            r = zydeck::structure::analyse(mono, rate, grid.beats, grid.bar, &m_cancel);
        }
        if (r.ok) {
            QDir().mkpath(QFileInfo(file).absolutePath());
            QSaveFile f(file);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(QJsonDocument(zydeck::structure::toJson(r, grid.beats, grid.bar)).toJson(QJsonDocument::Compact));
                f.commit();
            }
        }
        QMutexLocker lock(&m_mutex);
        m_running.remove(trackId);
        if (!r.ok) {
            m_failed.insert(trackId, {{"sig", grid.signature}, {"error", QString::fromStdString(r.error)}});
            qWarning() << "Zydek: no structure for track" << trackId << QString::fromStdString(r.error);
        }
    });
    return {{"ok", true}, {"pending", true}};
}

} // namespace zydek
