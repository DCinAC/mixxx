#include "zydek/zydekstems.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtEndian>

#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "moc_zydekstems.cpp"
#include "qml/qmllibraryproxy.h"
#include "sources/audiosource.h"
#include "sources/soundsourceproxy.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"
#include "track/trackref.h"
#include "util/samplebuffer.h"

#ifdef ZYDEK_LIVESTEMS
#include <fftw3.h>
#include <onnxruntime_cxx_api.h>

#include <array>
#include <cmath>
#include <complex>
#include <memory>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}
#endif

namespace zydek {

namespace {

const QString kFilesJson = QStringLiteral("zydek-livestems.json");

/// Music/ZyDeck Stems on the shared storage when it can write there.
QString stemsFolder() {
    const QString shared = QStringLiteral("/storage/emulated/0/Music");
    const QString music = QFileInfo(shared).isWritable() ? shared : QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    const QString dir = music + QStringLiteral("/ZyDeck Stems");
    QDir().mkpath(dir);
    return dir;
}

QString safeName(QString s) {
    static const QRegularExpression bad(QStringLiteral(R"([\\/:*?"<>|\x00-\x1f])"));
    s.replace(bad, QStringLiteral(" "));
    return s.simplified().left(150);
}

/// NI Stems' manifest (moov/udta/stem): the four stems' names and colours, in the file's order.
QByteArray stemManifest() {
    const QJsonObject off{{"enabled", false}};
    QJsonObject compressor = off, limiter = off;
    compressor.insert(QStringLiteral("input_gain"), 0.5);
    compressor.insert(QStringLiteral("output_gain"), 0.5);
    compressor.insert(QStringLiteral("threshold"), 0);
    compressor.insert(QStringLiteral("dry_wet"), 50);
    compressor.insert(QStringLiteral("attack"), 0.003);
    compressor.insert(QStringLiteral("release"), 0.3);
    compressor.insert(QStringLiteral("ratio"), 16);
    compressor.insert(QStringLiteral("hp_cutoff"), 300);
    limiter.insert(QStringLiteral("threshold"), 0);
    limiter.insert(QStringLiteral("ceiling"), -0.35);
    limiter.insert(QStringLiteral("release"), 0.05);
    const QJsonArray stems{QJsonObject{{"name", "Drums"}, {"color", "#009E73"}},
            QJsonObject{{"name", "Bass"}, {"color", "#D55E00"}},
            QJsonObject{{"name", "Other"}, {"color", "#CC79A7"}},
            QJsonObject{{"name", "Vocals"}, {"color", "#56B4E9"}}};
    return QJsonDocument(QJsonObject{{"version", 1},
                                 {"mastering_dsp", QJsonObject{{"compressor", compressor}, {"limiter", limiter}}},
                                 {"stems", stems}})
            .toJson(QJsonDocument::Compact);
}

/// Puts the stem manifest into the file's moov/udta box. The muxer writes moov last, so it can grow there
/// without moving any sample data.
bool addStemBox(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite)) {
        return false;
    }
    const qint64 fileSize = f.size();
    qint64 pos = 0, moovPos = -1, moovSize = 0;
    while (pos + 8 <= fileSize) {
        f.seek(pos);
        const QByteArray head = f.read(16);
        qint64 size = qFromBigEndian<quint32>(head.constData());
        const QByteArray type = head.mid(4, 4);
        if (size == 1) {
            size = static_cast<qint64>(qFromBigEndian<quint64>(head.constData() + 8));
        } else if (size == 0) {
            size = fileSize - pos;
        }
        if (size < 8) {
            return false;
        }
        if (type == "moov") {
            moovPos = pos;
            moovSize = size;
        }
        pos += size;
    }
    if (moovPos < 0 || moovPos + moovSize != fileSize) {
        return false;   // moov isn't last: leave the file as it is (Mixxx plays it, without stem names)
    }
    f.seek(moovPos);
    QByteArray moov = f.read(moovSize);
    const QByteArray manifest = stemManifest();
    QByteArray stemBox(8, '\0');
    qToBigEndian<quint32>(static_cast<quint32>(8 + manifest.size()), stemBox.data());
    stemBox.replace(4, 4, "stem");
    stemBox += manifest;
    // find moov/udta
    qint64 p = 8, udtaPos = -1;
    while (p + 8 <= moov.size()) {
        const qint64 size = qFromBigEndian<quint32>(moov.constData() + p);
        if (size < 8) {
            break;
        }
        if (moov.mid(p + 4, 4) == "udta") {
            udtaPos = p;
            break;
        }
        p += size;
    }
    if (udtaPos >= 0) {
        const quint32 udtaSize = qFromBigEndian<quint32>(moov.constData() + udtaPos);
        moov.insert(udtaPos + udtaSize, stemBox);
        qToBigEndian<quint32>(udtaSize + static_cast<quint32>(stemBox.size()), moov.data() + udtaPos);
    } else {
        QByteArray udta(8, '\0');
        qToBigEndian<quint32>(static_cast<quint32>(8 + stemBox.size()), udta.data());
        udta.replace(4, 4, "udta");
        moov += udta + stemBox;
    }
    qToBigEndian<quint32>(static_cast<quint32>(moov.size()), moov.data());
    f.seek(moovPos);
    return f.write(moov) == moov.size() && f.resize(moovPos + moov.size());
}

#ifdef ZYDEK_LIVESTEMS

constexpr int kRate = 44100;   // what the model learned
constexpr int kFft = 4096;
constexpr int kHop = 1024;
constexpr int kBins = kFft / 2 + 1;
constexpr int kChunk = 512;   // frames per model run (11.9 s): the exported models have this fixed
constexpr int kMargin = 32;   // frames at each side of a chunk with too little context, done by the neighbour
constexpr int kStems = 4;
constexpr float kOlaNorm = 1.5f;   // sum of the squared Hann windows at 75 % overlap
// the file's order (NI Stems): drums, bass, other, vocals
const char* const kModelNames[kStems] = {"drums", "bass", "other", "vocals"};

/// Sample-rate conversion, interleaved stereo float in and out.
class Resampler {
  public:
    Resampler(int from, int to)
            : m_same(from == to) {
        if (m_same) {
            return;
        }
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        swr_alloc_set_opts2(&m_swr, &stereo, AV_SAMPLE_FMT_FLT, to, &stereo, AV_SAMPLE_FMT_FLT, from, 0, nullptr);
        swr_init(m_swr);
    }
    ~Resampler() {
        swr_free(&m_swr);
    }
    /// frames == 0 and in == nullptr: flush what's left
    void convert(const float* in, int frames, std::vector<float>* pOut) {
        pOut->clear();
        if (m_same) {
            if (in) {
                pOut->assign(in, in + 2 * frames);
            }
            return;
        }
        const int cap = swr_get_out_samples(m_swr, frames) + 64;
        pOut->resize(2 * static_cast<size_t>(cap));
        uint8_t* outPlanes[1] = {reinterpret_cast<uint8_t*>(pOut->data())};
        const uint8_t* inPlanes[1] = {reinterpret_cast<const uint8_t*>(in)};
        const int got = swr_convert(m_swr, outPlanes, cap, in ? inPlanes : nullptr, frames);
        pOut->resize(2 * static_cast<size_t>(std::max(0, got)));
    }

  private:
    bool m_same;
    SwrContext* m_swr = nullptr;
};

/// The NI Stems file: track 0 the mix, then the stems, each stereo AAC in one MP4.
class StemWriter {
  public:
    ~StemWriter() {
        for (Track& t : m_tracks) {
            av_audio_fifo_free(t.fifo);
            avcodec_free_context(&t.ctx);
        }
        if (m_fmt) {
            if (m_fmt->pb) {
                avio_closep(&m_fmt->pb);
            }
            avformat_free_context(m_fmt);
        }
    }

    bool open(const QString& path, int rate, const QString& title, const QString& artist, const QString& comment) {
        if (avformat_alloc_output_context2(&m_fmt, nullptr, "mp4", path.toUtf8().constData()) < 0) {
            return false;
        }
        const AVCodec* pAac = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!pAac) {
            return false;
        }
        for (Track& t : m_tracks) {
            t.st = avformat_new_stream(m_fmt, nullptr);
            t.ctx = avcodec_alloc_context3(pAac);
            t.ctx->sample_rate = rate;
            t.ctx->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
            t.ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
            t.ctx->bit_rate = 192000;
            t.ctx->time_base = AVRational{1, rate};
            if (m_fmt->oformat->flags & AVFMT_GLOBALHEADER) {
                t.ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }
            if (avcodec_open2(t.ctx, pAac, nullptr) < 0 || avcodec_parameters_from_context(t.st->codecpar, t.ctx) < 0) {
                return false;
            }
            t.st->time_base = AVRational{1, rate};
            t.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, 8192);
        }
        m_tracks[0].st->disposition = AV_DISPOSITION_DEFAULT;
        av_dict_set(&m_fmt->metadata, "title", title.toUtf8().constData(), 0);
        av_dict_set(&m_fmt->metadata, "artist", artist.toUtf8().constData(), 0);
        av_dict_set(&m_fmt->metadata, "comment", comment.toUtf8().constData(), 0);
        if (avio_open(&m_fmt->pb, path.toUtf8().constData(), AVIO_FLAG_WRITE) < 0) {
            return false;
        }
        return avformat_write_header(m_fmt, nullptr) >= 0;
    }

    /// Interleaved stereo for track i (0 = the mix).
    bool push(int i, const float* data, int frames) {
        if (frames <= 0) {
            return true;
        }
        Track& t = m_tracks[i];
        m_left.resize(frames);
        m_right.resize(frames);
        for (int n = 0; n < frames; ++n) {
            m_left[n] = data[2 * n];
            m_right[n] = data[2 * n + 1];
        }
        void* planes[2] = {m_left.data(), m_right.data()};
        if (av_audio_fifo_write(t.fifo, planes, frames) < frames) {
            return false;
        }
        return drain(i, false);
    }

    bool finish() {
        for (int i = 0; i < static_cast<int>(m_tracks.size()); ++i) {
            if (!drain(i, true)) {
                return false;
            }
        }
        return av_write_trailer(m_fmt) >= 0;
    }

  private:
    struct Track {
        AVStream* st = nullptr;
        AVCodecContext* ctx = nullptr;
        AVAudioFifo* fifo = nullptr;
        int64_t pts = 0;
    };

    bool drain(int i, bool last) {
        Track& t = m_tracks[i];
        const int size = t.ctx->frame_size > 0 ? t.ctx->frame_size : 1024;
        while (av_audio_fifo_size(t.fifo) >= size || (last && av_audio_fifo_size(t.fifo) > 0)) {
            const int n = std::min(size, av_audio_fifo_size(t.fifo));
            AVFrame* pFrame = av_frame_alloc();
            pFrame->nb_samples = n;
            pFrame->format = AV_SAMPLE_FMT_FLTP;
            pFrame->sample_rate = t.ctx->sample_rate;
            av_channel_layout_copy(&pFrame->ch_layout, &t.ctx->ch_layout);
            av_frame_get_buffer(pFrame, 0);
            av_audio_fifo_read(t.fifo, reinterpret_cast<void**>(pFrame->data), n);
            pFrame->pts = t.pts;
            t.pts += n;
            const bool ok = avcodec_send_frame(t.ctx, pFrame) >= 0 && writePackets(i);
            av_frame_free(&pFrame);
            if (!ok) {
                return false;
            }
        }
        if (last) {
            avcodec_send_frame(t.ctx, nullptr);
            return writePackets(i);
        }
        return true;
    }

    bool writePackets(int i) {
        Track& t = m_tracks[i];
        AVPacket* pPacket = av_packet_alloc();
        bool ok = true;
        while (avcodec_receive_packet(t.ctx, pPacket) >= 0) {
            av_packet_rescale_ts(pPacket, t.ctx->time_base, t.st->time_base);
            pPacket->stream_index = t.st->index;
            if (av_interleaved_write_frame(m_fmt, pPacket) < 0) {
                ok = false;
                break;
            }
        }
        av_packet_free(&pPacket);
        return ok;
    }

    AVFormatContext* m_fmt = nullptr;
    std::array<Track, 1 + kStems> m_tracks;
    std::vector<float> m_left, m_right;
};

/// The four models, loaded once (from the APK's assets) and kept.
class Models {
  public:
    static Models* get(QString* pError) {
        static std::unique_ptr<Models> s_models;
        if (!s_models) {
            auto pModels = std::make_unique<Models>();
            if (!pModels->load(pError)) {
                return nullptr;
            }
            s_models = std::move(pModels);
        }
        return s_models.get();
    }

    bool load(QString* pError) {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(4);   // faster than 6-8 on both a 2024 phone and a 2020 tablet
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        for (int s = 0; s < kStems; ++s) {
            QFile f(QStringLiteral("assets:/zydek-stems/umxhq-%1.int8.onnx").arg(QLatin1String(kModelNames[s])));
            if (!f.open(QIODevice::ReadOnly)) {
                *pError = QStringLiteral("This build has no stem models");
                return false;
            }
            const QByteArray bytes = f.readAll();
            try {
                m_sessions[s] = std::make_unique<Ort::Session>(m_env, bytes.constData(), bytes.size(), options);
            } catch (const Ort::Exception& e) {
                *pError = QStringLiteral("Couldn't load the stem model: %1").arg(QString::fromUtf8(e.what()));
                return false;
            }
        }
        return true;
    }

    /// mag: (2, bins, kChunk) -> est: the stems' magnitudes, same layout each
    bool run(std::vector<float>& mag, std::array<std::vector<float>, kStems>* pEst) {
        const int64_t shape[4] = {1, 2, kBins, kChunk};
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input = Ort::Value::CreateTensor<float>(memory, mag.data(), mag.size(), shape, 4);
        const char* inName = "mag";
        const char* outName = "est";
        try {
            for (int s = 0; s < kStems; ++s) {
                auto out = m_sessions[s]->Run(Ort::RunOptions{nullptr}, &inName, &input, 1, &outName, 1);
                const float* p = out[0].GetTensorData<float>();
                (*pEst)[s].assign(p, p + mag.size());
            }
        } catch (const Ort::Exception& e) {
            qWarning() << "Zydek live stems:" << e.what();
            return false;
        }
        return true;
    }

  private:
    Ort::Env m_env{ORT_LOGGING_LEVEL_WARNING, "zydek-stems"};
    std::array<std::unique_ptr<Ort::Session>, kStems> m_sessions;
};

/// Short-time Fourier analysis and resynthesis, frame by frame (FFTW plans aren't thread-safe to make: the
/// separation thread owns these).
class Fft {
  public:
    Fft() {
        m_time = fftwf_alloc_real(kFft);
        m_freq = fftwf_alloc_complex(kBins);
        m_forward = fftwf_plan_dft_r2c_1d(kFft, m_time, m_freq, FFTW_ESTIMATE);
        m_inverse = fftwf_plan_dft_c2r_1d(kFft, m_freq, m_time, FFTW_ESTIMATE);
        m_window.resize(kFft);
        for (int n = 0; n < kFft; ++n) {   // periodic Hann, as in training
            m_window[n] = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * n / kFft);
        }
    }
    ~Fft() {
        fftwf_destroy_plan(m_forward);
        fftwf_destroy_plan(m_inverse);
        fftwf_free(m_time);
        fftwf_free(m_freq);
    }
    /// One channel of interleaved stereo (stride 2) -> spectrum
    void forward(const float* stereo, int channel, std::complex<float>* out) {
        for (int n = 0; n < kFft; ++n) {
            m_time[n] = stereo[2 * n + channel] * m_window[n];
        }
        fftwf_execute(m_forward);
        std::memcpy(out, m_freq, sizeof(fftwf_complex) * kBins);
    }
    /// spectrum -> windowed frame, added to out (interleaved stereo, stride 2)
    void inverseAdd(const std::complex<float>* spec, float* stereo, int channel) {
        std::memcpy(m_freq, spec, sizeof(fftwf_complex) * kBins);
        fftwf_execute(m_inverse);
        const float scale = 1.0f / (kFft * kOlaNorm);
        for (int n = 0; n < kFft; ++n) {
            stereo[2 * n + channel] += m_time[n] * m_window[n] * scale;
        }
    }

  private:
    float* m_time;
    fftwf_complex* m_freq;
    fftwf_plan m_forward, m_inverse;
    std::vector<float> m_window;
};

#endif // ZYDEK_LIVESTEMS

} // namespace

LiveStems::LiveStems(UserSettingsPointer pConfig, QObject* pParent)
        : QObject(pParent),
          m_pConfig(std::move(pConfig)) {
    QFile f(QDir(m_pConfig->getSettingsPath()).filePath(kFilesJson));
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject j = QJsonDocument::fromJson(f.readAll()).object();
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (QFileInfo::exists(it.value().toString())) {
                m_files.insert(it.key().toInt(), it.value().toString());
            }
        }
    }
    m_thread = std::thread([this] { run(); });
}

LiveStems::~LiveStems() {
    {
        QMutexLocker lock(&m_mutex);
        m_stop = true;
        m_queue.clear();
    }
    m_wake.wakeAll();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool LiveStems::available() {
#ifdef ZYDEK_LIVESTEMS
    return QFile::exists(QStringLiteral("assets:/zydek-stems/umxhq-vocals.int8.onnx"));
#else
    return false;
#endif
}

void LiveStems::request(const TrackPointer& pTrack) {
    if (!pTrack) {
        return;
    }
    const int id = pTrack->getId().toVariant().toInt();
    {
        QMutexLocker lock(&m_mutex);
        if (id == m_busyId) {
            return;
        }
        for (const TrackPointer& p : m_queue) {
            if (p->getId() == pTrack->getId()) {
                return;
            }
        }
        m_queue.push_back(pTrack);
    }
    m_wake.wakeAll();
}

QString LiveStems::stemFileFor(int trackId) const {
    QMutexLocker lock(&m_mutex);
    const QString path = m_files.value(trackId);
    return QFileInfo::exists(path) ? path : QString();
}

QJsonObject LiveStems::status() const {
    QMutexLocker lock(&m_mutex);
    return {{"busy", m_busyId}, {"progress", m_progress}, {"queued", static_cast<int>(m_queue.size())}};
}

void LiveStems::remember(int trackId, const QString& path) {
    QJsonObject j;
    {
        QMutexLocker lock(&m_mutex);
        m_files.insert(trackId, path);
        for (auto it = m_files.cbegin(); it != m_files.cend(); ++it) {
            j.insert(QString::number(it.key()), it.value());
        }
    }
    QSaveFile f(QDir(m_pConfig->getSettingsPath()).filePath(kFilesJson));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(j).toJson());
        f.commit();
    }
}

void LiveStems::run() {
    while (true) {
        TrackPointer pTrack;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_stop && m_queue.empty()) {
                m_wake.wait(&m_mutex);
            }
            if (m_stop) {
                return;
            }
            pTrack = m_queue.front();
            m_queue.pop_front();
            m_busyId = pTrack->getId().toVariant().toInt();
            m_progress = 0;
        }
        const int id = m_busyId;
        QString error;
        const QString path = separate(pTrack, &error);
        if (!path.isEmpty()) {
            remember(id, path);
        }
        {
            QMutexLocker lock(&m_mutex);
            m_busyId = 0;
        }
        emit finished(id, path, error);
    }
}

QString LiveStems::separate(const TrackPointer& pTrack, QString* pError) {
#ifndef ZYDEK_LIVESTEMS
    Q_UNUSED(pTrack);
    *pError = QStringLiteral("This build can't make stems on the phone");
    return {};
#else
    QElapsedTimer timer;
    timer.start();
    Models* pModels = Models::get(pError);
    if (!pModels) {
        return {};
    }
    mixxx::AudioSource::OpenParams params;
    params.setChannelCount(mixxx::audio::ChannelCount::stereo());
    const mixxx::AudioSourcePointer pSource = SoundSourceProxy(pTrack).openAudioSource(params);
    if (!pSource) {
        *pError = QStringLiteral("Couldn't read the track");
        return {};
    }
    const int rate = pSource->getSignalInfo().getSampleRate();
    const mixxx::IndexRange range = pSource->frameIndexRange();
    const qint64 totalFrames = range.length();
    const int id = pTrack->getId().toVariant().toInt();

    const QString title = pTrack->getTitle().isEmpty() ? QFileInfo(pTrack->getLocation()).completeBaseName() : pTrack->getTitle();
    const QString base = safeName((pTrack->getArtist().isEmpty() ? QString() : pTrack->getArtist() + QStringLiteral(" - ")) + title);
    const QString dir = stemsFolder();
    QString path = dir + QLatin1Char('/') + base + QStringLiteral(".stem.m4a");
    for (int n = 2; QFileInfo::exists(path); ++n) {
        path = dir + QLatin1Char('/') + base + QStringLiteral(" %1.stem.m4a").arg(n);
    }
    const QString partPath = path + QStringLiteral(".part");
    StemWriter writer;
    if (!writer.open(partPath, rate, title, pTrack->getArtist(),
                QStringLiteral("ZyDeck live stems (Open-Unmix UMX-HQ) of %1").arg(pTrack->getLocation()))) {
        QFile::remove(partPath);
        *pError = QStringLiteral("Couldn't write the stems file");
        return {};
    }

    Fft fft;
    Resampler toModel(rate, kRate);
    std::array<std::unique_ptr<Resampler>, kStems> fromModel;
    for (auto& p : fromModel) {
        p = std::make_unique<Resampler>(kRate, rate);
    }
    // The model's signal, padded at the front so the first samples get all four overlapping frames: padded
    // sample p is signal sample p - kPad. Frame f covers padded samples [f * kHop, f * kHop + kFft).
    constexpr qint64 kPad = kFft - kHop;
    std::vector<float> in(2 * kPad, 0.0f);   // interleaved stereo, padded samples from inBase
    qint64 inBase = 0, signalLength = 0;
    std::deque<std::array<std::vector<std::complex<float>>, 2>> frames;   // spectra from frame frameBase
    qint64 frameBase = 0, nextFrame = 0, start = 0;
    std::array<std::vector<float>, kStems> out;   // overlap-add, interleaved stereo, padded samples from outBase
    qint64 outBase = 0;
    std::vector<float> mag(2 * static_cast<size_t>(kBins) * kChunk);
    std::array<std::vector<float>, kStems> est;
    std::vector<std::complex<float>> spec(kBins);
    std::vector<float> scratch, resampled;
    const double expectedFrames = static_cast<double>(totalFrames) * kRate / rate / kHop + 4;
    bool ended = false;
    qint64 totalModelFrames = -1;

    const auto computeFrames = [&](qint64 available) {   // padded samples available up to here
        while (nextFrame * kHop + kFft <= available) {
            const float* p = in.data() + 2 * (nextFrame * kHop - inBase);
            std::array<std::vector<std::complex<float>>, 2> fr;
            for (int c = 0; c < 2; ++c) {
                fr[c].resize(kBins);
                fft.forward(p, c, fr[c].data());
            }
            frames.push_back(std::move(fr));
            ++nextFrame;
        }
        // samples no later frame needs
        const qint64 drop = nextFrame * kHop - inBase;
        if (drop > 0) {
            in.erase(in.begin(), in.begin() + 2 * drop);
            inBase += drop;
        }
    };
    const auto emitOutput = [&](qint64 until) {   // padded samples before `until` are final: out to the file
        const qint64 n = until - outBase;
        if (n <= 0) {
            return true;
        }
        const qint64 from = std::max<qint64>(outBase, kPad), to = std::min<qint64>(until, kPad + signalLength);
        for (int s = 0; s < kStems; ++s) {
            out[s].resize(std::max<size_t>(out[s].size(), 2 * static_cast<size_t>(n)), 0.0f);
            if (to > from) {
                fromModel[s]->convert(out[s].data() + 2 * (from - outBase), static_cast<int>(to - from), &resampled);
                if (!writer.push(1 + s, resampled.data(), static_cast<int>(resampled.size() / 2))) {
                    return false;
                }
            }
            out[s].erase(out[s].begin(), out[s].begin() + 2 * n);
        }
        outBase = until;
        return true;
    };
    // Runs the models on every chunk that has enough frames, keeping the middle of each (the edges lack the
    // BiLSTM's context: the next chunk redoes them).
    const auto separateChunks = [&]() {
        while (true) {
            const qint64 w = std::max<qint64>(0, start - kMargin);
            const bool last = ended && start + kChunk - 2 * kMargin >= totalModelFrames;
            if (ended ? start >= totalModelFrames : nextFrame < w + kChunk) {
                return true;
            }
            const qint64 keep0 = start - w, keep1 = last ? totalModelFrames - w : kChunk - kMargin;
            std::fill(mag.begin(), mag.end(), 0.0f);
            for (qint64 t = 0; t < kChunk && w + t < nextFrame; ++t) {
                const auto& fr = frames[w + t - frameBase];
                for (int c = 0; c < 2; ++c) {
                    float* m = mag.data() + static_cast<size_t>(c) * kBins * kChunk + t;
                    for (int b = 0; b < kBins; ++b) {
                        m[static_cast<size_t>(b) * kChunk] = std::abs(fr[c][b]);
                    }
                }
            }
            if (!pModels->run(mag, &est)) {
                *pError = QStringLiteral("The stem model failed");
                return false;
            }
            for (qint64 t = keep0; t < keep1; ++t) {
                const qint64 f = w + t;
                const auto& fr = frames[f - frameBase];
                for (int s = 0; s < kStems; ++s) {
                    std::vector<float>& acc = out[s];
                    const size_t need = 2 * static_cast<size_t>(f * kHop + kFft - outBase);
                    if (acc.size() < need) {
                        acc.resize(need, 0.0f);
                    }
                    for (int c = 0; c < 2; ++c) {
                        const size_t at = static_cast<size_t>(c) * kBins * kChunk + t;
                        for (int b = 0; b < kBins; ++b) {
                            const size_t i = at + static_cast<size_t>(b) * kChunk;
                            const float sum = est[0][i] + est[1][i] + est[2][i] + est[3][i] + 1e-8f;
                            spec[b] = fr[c][b] * (est[s][i] / sum);   // its share of the mixture
                        }
                        fft.inverseAdd(spec.data(), acc.data() + 2 * (f * kHop - outBase), c);
                    }
                }
            }
            start = w + keep1;
            if (!emitOutput(start * kHop)) {
                *pError = QStringLiteral("Couldn't write the stems file");
                return false;
            }
            while (frameBase < std::max<qint64>(0, start - kMargin) && !frames.empty()) {
                frames.pop_front();
                ++frameBase;
            }
            QMutexLocker lock(&m_mutex);
            m_progress = std::min(0.99, start / expectedFrames);
            const double fraction = m_progress;
            lock.unlock();
            emit progress(id, fraction);
            if (m_stop) {
                return false;
            }
        }
    };

    constexpr SINT kBlock = 65536;
    mixxx::SampleBuffer buffer(kBlock * 2);
    for (SINT pos = range.start(); pos < range.end();) {
        const SINT n = std::min<SINT>(kBlock, range.end() - pos);
        const mixxx::ReadableSampleFrames read = pSource->readSampleFrames(mixxx::WritableSampleFrames(
                mixxx::IndexRange::forward(pos, n), mixxx::SampleBuffer::WritableSlice(buffer)));
        const SINT got = read.readableLength() / 2;
        if (got <= 0) {
            break;
        }
        if (!writer.push(0, read.readableData(), static_cast<int>(got))) {   // the mix, as it is
            *pError = QStringLiteral("Couldn't write the stems file");
            QFile::remove(partPath);
            return {};
        }
        toModel.convert(read.readableData(), static_cast<int>(got), &scratch);
        in.insert(in.end(), scratch.begin(), scratch.end());
        signalLength += static_cast<qint64>(scratch.size() / 2);
        computeFrames(inBase + static_cast<qint64>(in.size() / 2));
        if (!separateChunks()) {
            QFile::remove(partPath);
            return {};
        }
        pos += got;
    }
    toModel.convert(nullptr, 0, &scratch);
    in.insert(in.end(), scratch.begin(), scratch.end());
    signalLength += static_cast<qint64>(scratch.size() / 2);
    in.insert(in.end(), 2 * static_cast<size_t>(kFft), 0.0f);   // padding at the end, so the last samples get all frames
    const qint64 paddedLength = kPad + signalLength + kFft;
    totalModelFrames = (paddedLength - kFft) / kHop + 1;
    computeFrames(inBase + static_cast<qint64>(in.size() / 2));
    ended = true;
    if (!separateChunks() || !emitOutput(kPad + signalLength)) {
        QFile::remove(partPath);
        return {};
    }
    for (int s = 0; s < kStems; ++s) {
        fromModel[s]->convert(nullptr, 0, &resampled);
        writer.push(1 + s, resampled.data(), static_cast<int>(resampled.size() / 2));
    }
    if (!writer.finish()) {
        QFile::remove(partPath);
        *pError = QStringLiteral("Couldn't write the stems file");
        return {};
    }
    addStemBox(partPath);
    QFile::remove(path);
    if (!QFile::rename(partPath, path)) {
        QFile::remove(partPath);
        *pError = QStringLiteral("Couldn't write the stems file");
        return {};
    }
    qInfo() << "Zydek live stems:" << path << "in" << timer.elapsed() << "ms";

    // Into the library as the same track: its beat grid, key, cues and loops (same sample rate, same frames)
    ::Library* pLibrary = mixxx::qml::QmlLibraryProxy::get();
    if (!pLibrary) {
        *pError = QStringLiteral("Mixxx isn't ready");
        return {};
    }
    TrackCollectionManager* pCollection = pLibrary->trackCollectionManager();
    bool added = false;
    QMetaObject::invokeMethod(
            pCollection,
            [pCollection, path, pTrack, &added] {
                const TrackPointer pStems = pCollection->getOrAddTrack(TrackRef::fromFilePath(path));
                if (!pStems) {
                    return;
                }
                pStems->setArtist(pTrack->getArtist());
                pStems->setTitle(pTrack->getTitle());
                pStems->setAlbum(pTrack->getAlbum());
                pStems->setKeys(pTrack->getKeys());
                if (const mixxx::BeatsPointer pBeats = pTrack->getBeats()) {
                    pStems->trySetBeats(pBeats);
                }
                pStems->setBpmLocked(pTrack->isBpmLocked());
                pStems->setMainCuePosition(pTrack->getMainCuePosition());
                for (const CuePointer& pCue : pTrack->getCuePoints()) {
                    if (pCue->getType() == mixxx::CueType::MainCue) {
                        continue;
                    }
                    const CuePointer pCopy = pStems->createAndAddCue(pCue->getType(),
                            pCue->getHotCue(),
                            pCue->getPosition(),
                            pCue->getEndPosition(),
                            pCue->getColor());
                    if (pCopy) {
                        pCopy->setLabel(pCue->getLabel());
                    }
                }
                added = pCollection->saveTrack(pStems) != TrackCollectionManager::SaveTrackResult::Failed;
            },
            Qt::BlockingQueuedConnection);
    if (!added) {
        *pError = QStringLiteral("Mixxx couldn't add the stems file");
        return {};
    }
    Q_UNUSED(id);
    return path;
#endif
}

} // namespace zydek
