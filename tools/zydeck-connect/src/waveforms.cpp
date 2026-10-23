#include "waveforms.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QtEndian>
#include <algorithm>
#include <cmath>

#ifdef ZC_DECODE
#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#endif

#include "library.h"

namespace zc {

namespace {
constexpr int kRate = 44100;            // decoded at this rate: the filters below are for it
constexpr int kPointsPerSecond = 441;   // as Mixxx's waveforms
constexpr int kStride = kRate / kPointsPerSecond;   // frames a point
constexpr qint64 kCacheBytes = 2LL << 30;
constexpr int kAheadCount = 600;        // tracks made ahead
constexpr int kAheadPauseMs = 1000;     // between them
const QString kCacheVersion = QStringLiteral("v2");   // (v1: MP3s without Mixxx's timing)

/// Mixxx's MP3 decoder (libmad, sources/soundsourcemp3.cpp) doesn't trim a file's start as FFmpeg does: it drops the
/// first frame (LAME's info frame; or audio, in a file without one) and keeps the rest. Measured against Mixxx's own
/// waveforms, to the sample: with LAME's tag, Mixxx plays the track (encoder delay + 529) samples later than FFmpeg's
/// output; with no info frame, 529 earlier. In the file's samples (+ silence before, - dropped), and its sample rate.
int mp3Shift(const QString& file, int* rate) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) {
        return 0;
    }
    QByteArray b = f.read(10);
    qint64 at = 0;
    if (b.size() == 10 && b.startsWith("ID3")) {   // (its size is "syncsafe": 7 bits a byte)
        const auto u = [&b](int i) { return static_cast<quint8>(b[i]); };
        at = 10 + ((u(6) << 21) | (u(7) << 14) | (u(8) << 7) | u(9)) + ((u(5) & 0x10) ? 10 : 0);
    }
    f.seek(at);
    b = f.read(64 * 1024);
    int i = 0;
    while (i + 4 < b.size() && !(static_cast<quint8>(b[i]) == 0xFF && (static_cast<quint8>(b[i + 1]) & 0xE0) == 0xE0)) {
        ++i;
    }
    if (i + 4 >= b.size()) {
        return 0;
    }
    const int version = (static_cast<quint8>(b[i + 1]) >> 3) & 3;   // 3: MPEG 1, 2: MPEG 2, 0: MPEG 2.5
    const int index = (static_cast<quint8>(b[i + 2]) >> 2) & 3;
    static const int kRates[3] = {44100, 48000, 32000};
    if (index == 3 || version == 1) {
        return 0;
    }
    *rate = kRates[index] / (version == 3 ? 1 : version == 2 ? 2 : 4);
    const QByteArray frame = b.mid(i, 2000);
    const bool info = frame.left(200).contains("Xing") || frame.left(200).contains("Info");
    if (!info) {
        return -529;
    }
    const int lame = frame.indexOf("LAME");
    if (lame < 0 || lame + 24 > frame.size()) {
        return 0;   // (an info frame without LAME's delay: both start after it)
    }
    const auto u = [&frame, lame](int k) { return static_cast<quint8>(frame[lame + k]); };
    return ((u(21) << 4) | (u(22) >> 4)) + 529;
}

struct Biquad {
    double b0, b1, b2, a1, a2;
};
// Mixxx's EngineFilterBessel4Low / Band / High at 600 and 4000 Hz, as second-order sections for 44.1 kHz (scipy's
// bessel(4, ..., norm="mag"); the waveforms they give match Mixxx's analysis files)
const Biquad kLow[] = {
        {1.4401468432355305e-05, 2.880293686471061e-05, 1.4401468432355305e-05, -1.7775513973325179, 0.79090229871298079},
        {1, 2, 1, -1.8265188696166015, 0.84377789118760005},
};
const Biquad kMid[] = {
        {0.0065947242259965814, 0.013189448451993163, 0.0065947242259965814, -1.0364006507681316, 0.28672667482142167},
        {1, 2, 1, -1.017891315681732, 0.43213024493122881},
        {1, -2, 1, -1.8614774698571386, 0.86690809139959524},
        {1, -2, 1, -1.9300570886193547, 0.9336436989173339},
};
const Biquad kHigh[] = {
        {0.55323391341841732, -1.1064678268368346, 0.55323391341841732, -1.3358493258429707, 0.4528343984036185},
        {1, -2, 1, -1.5341105773747192, 0.64005454494876135},
};

template <int N>
struct Cascade {
    explicit Cascade(const Biquad (&s)[N])
            : sections(s) {
    }
    const Biquad* sections;
    double z[2][N][2] = {};   // per channel and section (transposed direct form II)
    float run(int ch, float x) {
        double y = x;
        for (int i = 0; i < N; ++i) {
            const Biquad& q = sections[i];
            double* w = z[ch][i];
            const double out = q.b0 * y + w[0];
            w[0] = q.b1 * y - q.a1 * out + w[1];
            w[1] = q.b2 * y - q.a2 * out;
            y = out;
        }
        return static_cast<float>(y);
    }
};
} // namespace

/// One track as it decodes: the loudest |sample| of each signal over each stride, as Mixxx's AnalyzerWaveform
struct Waveforms::Analysis {
    Cascade<2> low{kLow};
    Cascade<4> mid{kMid};
    Cascade<2> high{kHigh};
    float peak[4] = {};   // all, low, mid, high (either channel)
    int frames = 0;       // into this stride
    int drop = 0;         // frames still to leave out (an MP3's start, as Mixxx has it)
    bool bad = false;     // it didn't come at 44.1 kHz
    QByteArray points;    // all/low/mid/high, a byte each

    void frame(float left, float right) {
        if (drop > 0) {
            --drop;
            return;
        }
        for (int ch = 0; ch < 2; ++ch) {
            const float s = ch ? right : left;
            peak[0] = std::max(peak[0], std::fabs(s));
            peak[1] = std::max(peak[1], std::fabs(low.run(ch, s)));
            peak[2] = std::max(peak[2], std::fabs(mid.run(ch, s)));
            peak[3] = std::max(peak[3], std::fabs(high.run(ch, s)));
        }
        if (++frames == kStride) {
            for (float& p : peak) {
                points.append(static_cast<char>(static_cast<unsigned char>(std::min(255.0f, p * 255 + 0.5f))));
                p = 0;
            }
            frames = 0;
        }
    }
    QByteArray packed() const {
        if (bad) {
            return {};
        }
        QByteArray out("MXWF", 4);
        char head[8];
        qToLittleEndian(static_cast<float>(kPointsPerSecond), head);
        qToLittleEndian(static_cast<quint32>(points.size() / 4), head + 4);
        out.append(head, 8);
        out.append(points);
        return out;
    }
};

Waveforms::Waveforms(Library* library, QObject* parent)
        : QObject(parent),
          m_library(library),
          m_cacheDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/waveforms")) {
#ifdef ZC_DECODE
    QDir().mkpath(m_cacheDir);
    for (const QString& old : QDir(m_cacheDir).entryList({QStringLiteral("*.mxwf")}, QDir::Files)) {   // an older make
        if (!old.endsWith(QStringLiteral("-%1.mxwf").arg(kCacheVersion))) {
            QFile::remove(m_cacheDir + '/' + old);
        }
    }
    m_context = new QObject;
    m_context->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_context, &QObject::deleteLater);
    m_thread.setObjectName(QStringLiteral("waveforms"));
    m_thread.start(QThread::LowPriority);
    QMetaObject::invokeMethod(m_context, [this] {
        m_aheadTimer = new QTimer(m_context);
        m_aheadTimer->setSingleShot(true);
        m_aheadTimer->setInterval(kAheadPauseMs);
        connect(m_aheadTimer, &QTimer::timeout, m_context, [this] { next(); });
        m_stuckTimer = new QTimer(m_context);
        m_stuckTimer->setSingleShot(true);
        m_stuckTimer->setInterval(120000);
        connect(m_stuckTimer, &QTimer::timeout, m_context, [this] { finish({}); });
        m_planTimer = new QTimer(m_context);
        m_planTimer->setInterval(30 * 60 * 1000);   // (tracks added or played since)
        connect(m_planTimer, &QTimer::timeout, m_context, [this] { planAhead(); });
        m_planTimer->start();
        QTimer::singleShot(15000, m_context, [this] { planAhead(); });   // (once Connect has settled)
    });
#endif
}

Waveforms::~Waveforms() {
    m_thread.quit();
    m_thread.wait(3000);
}

QString Waveforms::cacheFileFor(int trackId, const QString& file) const {
    const QFileInfo info(file);
    if (file.isEmpty() || !info.isFile()) {
        return {};
    }
    // (another size or time: the file changed, so its waveform is made again)
    return QStringLiteral("%1/%2-%3-%4-%5.mxwf")
            .arg(m_cacheDir)
            .arg(trackId)
            .arg(info.size())
            .arg(info.lastModified().toMSecsSinceEpoch())
            .arg(kCacheVersion);
}

void Waveforms::get(int trackId, Done done) {
#ifdef ZC_DECODE
    const QString file = m_library->location(trackId);
    const QString cached = cacheFileFor(trackId, file);
    if (cached.isEmpty()) {
        done({});
        return;
    }
    QFile f(cached);
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray data = qUncompress(f.readAll());
        if (data.startsWith("MXWF")) {
            done(data);
            return;
        }
    }
    Job job;
    job.id = trackId;
    job.file = file;
    job.cacheFile = cached;
    job.done.append(std::move(done));
    QMetaObject::invokeMethod(m_context, [this, job] { enqueue(job); });
#else
    Q_UNUSED(trackId);
    done({});
#endif
}

#ifdef ZC_DECODE

/// A track asked for goes ahead of those made ahead (after the others asked for); one already waiting joins it
void Waveforms::enqueue(Job job) {
    if (m_current && m_current->cacheFile == job.cacheFile) {
        m_current->done += job.done;
        return;
    }
    for (int i = 0; i < m_queue.size(); ++i) {
        if (m_queue[i].cacheFile == job.cacheFile) {
            job.done = m_queue.takeAt(i).done + job.done;
            break;
        }
    }
    int at = m_queue.size();
    if (!job.done.isEmpty()) {
        at = 0;
        while (at < m_queue.size() && !m_queue[at].done.isEmpty()) {
            ++at;
        }
    }
    m_queue.insert(at, job);
    next();
}

void Waveforms::next() {
    if (m_current || m_queue.isEmpty()) {
        return;
    }
    const bool asked = !m_queue.first().done.isEmpty();
    if (!asked && m_aheadTimer->isActive()) {
        return;   // (a pause between tracks made ahead: it calls again)
    }
    m_current = std::make_unique<Job>(m_queue.takeFirst());
    m_analysis = std::make_unique<Analysis>();
    if (m_current->file.endsWith(QLatin1String(".mp3"), Qt::CaseInsensitive)) {   // on Mixxx's timeline
        int rate = kRate;
        const int shift = static_cast<int>(std::lround(mp3Shift(m_current->file, &rate) * static_cast<double>(kRate) / rate));
        for (int k = 0; k < shift; ++k) {
            m_analysis->frame(0, 0);
        }
        m_analysis->drop = std::max(0, -shift);
    }
    m_thread.setPriority(asked ? QThread::NormalPriority : QThread::LowPriority);
    // a decoder per track: nothing from the last one's can be taken for this one's
    m_decoder = new QAudioDecoder(m_context);
    QAudioFormat format;
    format.setSampleFormat(QAudioFormat::Float);
    format.setChannelCount(2);
    format.setSampleRate(kRate);
    m_decoder->setAudioFormat(format);
    QAudioDecoder* decoder = m_decoder;
    connect(decoder, &QAudioDecoder::bufferReady, m_context, [this, decoder] {
        if (decoder == m_decoder) {
            decoded();
        }
    });
    connect(decoder, &QAudioDecoder::finished, m_context, [this, decoder] {
        if (decoder == m_decoder) {
            decoded();
            finish(m_analysis ? m_analysis->packed() : QByteArray());
        }
    });
    connect(decoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), m_context, [this, decoder](QAudioDecoder::Error) {
        if (decoder == m_decoder) {
            qWarning() << "waveform: couldn't decode" << m_current->file << "-" << decoder->errorString();
            finish({});
        }
    });
    m_decoder->setSource(QUrl::fromLocalFile(m_current->file));
    m_decoder->start();
    m_stuckTimer->start();
}

void Waveforms::decoded() {
    while (m_decoder->bufferAvailable()) {
        const QAudioBuffer b = m_decoder->read();
        const QAudioFormat f = b.format();
        Analysis& a = *m_analysis;
        if (f.sampleRate() != kRate) {
            a.bad = true;   // (the filters are for 44.1 kHz)
            continue;
        }
        const qsizetype frames = b.frameCount();
        const int channels = f.channelCount();
        if (f.sampleFormat() == QAudioFormat::Float && channels == 2) {
            const float* x = b.constData<float>();
            for (qsizetype i = 0; i < frames; ++i) {
                a.frame(x[2 * i], x[2 * i + 1]);
            }
        } else if (channels > 0) {   // (another layout: sample by sample)
            const char* x = b.constData<char>();
            const int bytes = f.bytesPerSample();
            for (qsizetype i = 0; i < frames; ++i) {
                const char* frame = x + i * bytes * channels;
                const float left = f.normalizedSampleValue(frame);
                a.frame(left, channels > 1 ? f.normalizedSampleValue(frame + bytes) : left);
            }
        }
    }
}

void Waveforms::finish(const QByteArray& packed) {
    if (!m_current) {
        return;
    }
    m_stuckTimer->stop();
    // out of the way first: stopping it can report "finished" again, from inside stop()
    QAudioDecoder* decoder = m_decoder;
    m_decoder = nullptr;
    const Job job = *m_current;
    m_current.reset();
    m_analysis.reset();
    if (decoder) {
        decoder->stop();
        decoder->deleteLater();
    }
    const bool ok = packed.size() > 12 + 4 * kPointsPerSecond;   // (a second at least)
    if (ok) {
        // an older one of this track's (its file has changed) goes
        const QStringList old = QDir(m_cacheDir).entryList({QStringLiteral("%1-*.mxwf").arg(job.id)}, QDir::Files);
        for (const QString& name : old) {
            QFile::remove(m_cacheDir + '/' + name);
        }
        QSaveFile f(job.cacheFile);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(qCompress(packed, 6));
            f.commit();
        }
        trimCache();
    }
    for (const Done& done : job.done) {
        done(ok ? packed : QByteArray());
    }
    if (job.done.isEmpty()) {
        m_aheadTimer->start();
    }
    QTimer::singleShot(0, m_context, [this] { next(); });
}

/// The tracks most likely to be loaded next, made ahead: played last, added last, played most
void Waveforms::planAhead() {
    for (const auto& [id, file] : m_library->likelyTracks(kAheadCount)) {
        const QString cached = cacheFileFor(id, file);
        if (!cached.isEmpty() && !QFileInfo::exists(cached)) {
            Job job;
            job.id = id;
            job.file = file;
            job.cacheFile = cached;
            enqueue(job);
        }
    }
}

/// Past 2 GB, the oldest go
void Waveforms::trimCache() {
    QFileInfoList files = QDir(m_cacheDir).entryInfoList({QStringLiteral("*.mxwf")}, QDir::Files, QDir::Time | QDir::Reversed);
    qint64 total = 0;
    for (const QFileInfo& f : files) {
        total += f.size();
    }
    for (const QFileInfo& f : files) {
        if (total <= kCacheBytes) {
            break;
        }
        total -= f.size();
        QFile::remove(f.filePath());
    }
}

#else
void Waveforms::enqueue(Job) {}
void Waveforms::next() {}
void Waveforms::decoded() {}
void Waveforms::finish(const QByteArray&) {}
void Waveforms::planAhead() {}
void Waveforms::trimCache() {}
#endif

} // namespace zc
