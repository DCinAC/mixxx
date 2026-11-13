#include "effects/backends/builtin/zydeckfx.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFreeBeatSeconds = 0.5;   // (no beat grid: 120 BPM)

EffectManifestPointer manifest(const QString& id, const QString& name, const QString& shortName, const QString& description,
        double metaDefault) {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(id);
    pManifest->setName(name);
    pManifest->setShortName(shortName);
    pManifest->setAuthor("ZyDeck");
    pManifest->setVersion("1.0");
    pManifest->setDescription(description);
    pManifest->setEffectRampsFromDry(true);
    pManifest->setMetaknobDefault(metaDefault);
    return pManifest;
}
EffectManifestParameterPointer knob(const EffectManifestPointer& pManifest, const char* id, const QString& name,
        const QString& description, double min, double def, double max, bool linked = false,
        EffectManifestParameter::ValueScaler scaler = EffectManifestParameter::ValueScaler::Linear,
        EffectManifestParameter::UnitsHint hint = EffectManifestParameter::UnitsHint::Unknown) {
    EffectManifestParameterPointer p = pManifest->addParameter();
    p->setId(QString::fromLatin1(id));
    p->setName(name);
    p->setShortName(name);
    p->setDescription(description);
    p->setValueScaler(scaler);
    p->setUnitsHint(hint);
    if (linked) {
        p->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    }
    p->setRange(min, def, max);
    return p;
}
EffectManifestParameterPointer beatsKnob(const EffectManifestPointer& pManifest, const char* id, const QString& name,
        const QString& description, double min, double def, double max) {
    return knob(pManifest, id, name, description, min, def, max, false,
            EffectManifestParameter::ValueScaler::Logarithmic, EffectManifestParameter::UnitsHint::Beats);
}
EffectManifestParameterPointer toggle(const EffectManifestPointer& pManifest, const char* id, const QString& name,
        const QString& description, double def) {
    EffectManifestParameterPointer p = pManifest->addParameter();
    p->setId(QString::fromLatin1(id));
    p->setName(name);
    p->setShortName(name);
    p->setDescription(description);
    p->setValueScaler(EffectManifestParameter::ValueScaler::Toggle);
    p->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    p->setRange(0, def, 1);
    return p;
}
// beats on a grid that has both straight (1/16) and triplet (1/12) lengths
double onGrid(double beats) {
    return std::max(1.0 / 48, std::round(beats * 48) / 48);
}
double gOf(double fc, double sampleRate) {
    return std::tan(kPi * std::clamp(fc, 10.0, sampleRate * 0.45) / sampleRate);
}

} // namespace

namespace zydeckfx {

float PitchShifter::process(float x, double ratio) {
    const int size = static_cast<int>(m_buffer.size());
    m_buffer[static_cast<size_t>(m_write)] = x;
    m_phase += (1.0 - ratio) / m_window;
    m_phase -= std::floor(m_phase);
    float out = 0;
    for (int h = 0; h < 2; ++h) {
        double ph = m_phase + 0.5 * h;
        ph -= std::floor(ph);
        const double d = 1 + ph * m_window;
        double pos = m_write - d;
        while (pos < 0) {
            pos += size;
        }
        const int i0 = static_cast<int>(pos), i1 = (i0 + 1) % size;
        const double frac = pos - i0;
        const double s = m_buffer[static_cast<size_t>(i0)] * (1 - frac) + m_buffer[static_cast<size_t>(i1)] * frac;
        const double w = std::sin(kPi * ph);
        out += static_cast<float>(s * w * w);   // (the two weights add to 1: sin² and cos²)
    }
    m_write = (m_write + 1) % size;
    return out;
}

void Reverb::setup(double sampleRate) {
    static const int combs[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const int allPasses[4] = {556, 441, 341, 225};
    const double k = sampleRate / 44100.0;
    for (int c = 0; c < 2; ++c) {
        const int spread = c ? 23 : 0;
        for (int i = 0; i < 8; ++i) {
            m_combs[c][i].buf.assign(static_cast<size_t>((combs[i] + spread) * k) + 1, 0.f);
        }
        for (int i = 0; i < 4; ++i) {
            m_allPasses[c][i].buf.assign(static_cast<size_t>((allPasses[i] + spread) * k) + 1, 0.f);
        }
    }
}

void Reverb::process(float inL, float inR, double size, double damp, float* pOutL, float* pOutR) {
    const float input = (inL + inR) * 0.015f;
    const float feedback = static_cast<float>(0.7 + 0.28 * size), d = static_cast<float>(damp);
    for (int c = 0; c < 2; ++c) {
        float out = 0;
        for (Comb& comb : m_combs[c]) {
            const float y = comb.buf[comb.at];
            comb.store = y * (1 - d) + comb.store * d;
            comb.buf[comb.at] = input + comb.store * feedback;
            comb.at = (comb.at + 1) % comb.buf.size();
            out += y;
        }
        for (AllPass& ap : m_allPasses[c]) {
            const float b = ap.buf[ap.at];
            const float y = -out + b;
            ap.buf[ap.at] = out + b * 0.5f;
            ap.at = (ap.at + 1) % ap.buf.size();
            out = y;
        }
        *(c ? pOutR : pOutL) = out;
    }
}

void BeatClock::update(const GroupFeatureState& gf, double sampleRate, SINT frames) {
    if (gf.beat_length.has_value() && gf.beat_fraction_buffer_end.has_value() && gf.beat_length->seconds > 0) {
        framesPerBeat = gf.beat_length->seconds * sampleRate;
        endFraction = *gf.beat_fraction_buffer_end;
    } else {
        framesPerBeat = kFreeBeatSeconds * sampleRate;
        freeFrame = static_cast<unsigned int>(std::fmod(freeFrame + frames, framesPerBeat));
        endFraction = freeFrame / framesPerBeat;
    }
}

} // namespace zydeckfx

// ---- Beat Echo -----------------------------------------------------------------------------------------------------
// Repeats every so many beats, each repeat quieter (Feedback); with Pitch each repeat rises or falls by so many
// semitones, with Glide it does more the longer it's held (8 beats to reach it); Taps adds repeats at 3/4 and 1/2 of
// the time; Tone filters the repeats (left a low pass, right a high pass; Band: a band round it); Crush crushes
// them. Release: it takes the track in for one repeat's time, then lets the track go and only its echoes ring out.

BeatEchoState::BeatEchoState(const mixxx::EngineParameters& engineParameters)
        : EffectState(engineParameters),
          delay(kMaxSeconds * engineParameters.sampleRate() * engineParameters.channelCount()) {
    delay.clear();
    for (zydeckfx::PitchShifter& s : shift) {
        s.setup(engineParameters.sampleRate());
    }
}

QString BeatEchoEffect::getId() {
    return "org.zydeck.effects.beatecho";
}

EffectManifestPointer BeatEchoEffect::getManifest() {
    EffectManifestPointer m = manifest(getId(), QObject::tr("Beat Echo"), QObject::tr("B.Echo"),
            QObject::tr("Repeats on the beat; the repeats can rise or fall in pitch, be filtered, crushed, tap more "
                        "than once, or ring out alone (Release)"),
            0.5);
    beatsKnob(m, "time", QObject::tr("Time"), QObject::tr("Beats between repeats"), 1.0 / 16, 0.5, 4);
    knob(m, "feedback", QObject::tr("Feedback"), QObject::tr("How long the repeats go on"), 0, 0.5, 0.98, true);
    knob(m, "pitch", QObject::tr("Pitch"), QObject::tr("Semitones up or down at each repeat"), -12, 0, 12);
    knob(m, "glide", QObject::tr("Glide"), QObject::tr("Semitones the repeats go to while it's held (in 8 beats)"), -24, 0, 24);
    knob(m, "taps", QObject::tr("Taps"), QObject::tr("Repeats at the time, and at 3/4 and 1/2 of it"), 1, 1, 3);
    knob(m, "tone", QObject::tr("Tone"), QObject::tr("The repeats filtered: left a low pass, right a high pass"), -1, 0, 1);
    toggle(m, "band", QObject::tr("Band"), QObject::tr("A band pass round Tone instead"), 0);
    knob(m, "crush", QObject::tr("Crush"), QObject::tr("The repeats crushed (fewer bits, a lower rate)"), 0, 0, 1);
    toggle(m, "release", QObject::tr("Release"), QObject::tr("One repeat's time of the track, then only its echoes"), 0);
    return m;
}

void BeatEchoEffect::loadEngineEffectParameters(const QMap<QString, EngineEffectParameterPointer>& p) {
    m_pTime = p.value("time");
    m_pFeedback = p.value("feedback");
    m_pPitch = p.value("pitch");
    m_pGlide = p.value("glide");
    m_pTaps = p.value("taps");
    m_pTone = p.value("tone");
    m_pBand = p.value("band");
    m_pCrush = p.value("crush");
    m_pRelease = p.value("release");
}

void BeatEchoEffect::processChannel(BeatEchoState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    BeatEchoState& s = *pState;
    const int ch = engineParameters.channelCount();
    const SINT frames = engineParameters.samplesPerBuffer() / ch;
    const double rate = engineParameters.sampleRate();
    const SINT capacity = s.delay.size() / ch;
    s.clock.update(groupFeatures, rate, frames);

    const double pitch = m_pPitch->value(), glide = m_pGlide->value();
    const bool shifting = std::abs(pitch) > 0.01 || std::abs(glide) > 0.01;
    const int latency = shifting ? s.shift[0].latency() : 0;
    const double wanted = std::clamp(onGrid(m_pTime->value()) * s.clock.framesPerBeat, 64.0, capacity - 8.0);
    if (enableState == EffectEnableState::Enabling) {
        s.delay.clear();
        s.write = 0;
        s.frames = 0;
        s.delayFrames = std::max(32.0, wanted - latency);
        s.dryGain = 1;
        for (int c = 0; c < 2; ++c) {
            s.filter[c].reset();
            s.crushHold[c] = 0;
        }
    }
    const double feedback = std::clamp(m_pFeedback->value(), 0.0, 0.98);
    const int taps = std::clamp(static_cast<int>(std::lround(m_pTaps->value())), 1, 3);
    const double tone = std::clamp(m_pTone->value(), -1.0, 1.0);
    const bool band = m_pBand->toBool();
    const bool filtering = band || std::abs(tone) > 0.02;
    int mode = 0;
    double fc = 1000, k = 1.4;
    if (band) {
        mode = 1;
        fc = 300 * std::pow(4000.0 / 300, (tone + 1) / 2);
        k = 0.7;
    } else if (tone < 0) {
        fc = 18000 * std::pow(250.0 / 18000, -tone);
    } else {
        mode = 2;
        fc = 30 * std::pow(5000.0 / 30, tone);
    }
    const double g = gOf(fc, rate);
    const double crush = std::clamp(m_pCrush->value(), 0.0, 1.0);
    const double levels = std::exp2(16 - 12 * crush);
    const int hold = 1 + static_cast<int>(crush * 8);
    const bool release = m_pRelease->toBool();
    const double glideFrames = 8 * s.clock.framesPerBeat;
    const double tapAt[3] = {1, 0.75, 0.5}, tapGain[3] = {1, 0.7, 0.5};

    for (SINT f = 0; f < frames; ++f) {
        const double ramp = std::min(1.0, s.frames / glideFrames);
        const double ratio = std::exp2((pitch + glide * ramp) / 12);
        s.delayFrames += (std::max(32.0, wanted - latency) - s.delayFrames) * 0.0005;   // (a new time: gliding to it)
        // Release: the track in for one repeat's time, then out (a few ms' fade), its echoes going on
        const double send = release && s.frames >= wanted ? 0 : 1;
        if (release && s.frames >= wanted) {
            s.dryGain = std::max(0.0, s.dryGain - 1.0 / (0.005 * rate));
        }
        const bool crushNow = crush > 0.01 && (s.crushCount++ % hold) == 0;
        for (int c = 0; c < ch && c < 2; ++c) {
            double wet = 0, first = 0;
            for (int t = 0; t < taps; ++t) {
                double pos = s.write - s.delayFrames * tapAt[t];
                while (pos < 0) {
                    pos += capacity;
                }
                const SINT i0 = static_cast<SINT>(pos), i1 = (i0 + 1) % capacity;
                const double frac = pos - i0;
                const double y = s.delay[i0 * ch + c] * (1 - frac) + s.delay[i1 * ch + c] * frac;
                wet += y * tapGain[t];
                if (t == 0) {
                    first = y;
                }
            }
            double fb = first * feedback;
            if (shifting) {
                fb = s.shift[c].process(static_cast<float>(fb), ratio);
            }
            if (filtering) {
                fb = s.filter[c].process(fb, g, k, mode);
            }
            if (crush > 0.01) {
                if (crushNow) {
                    s.crushHold[c] = static_cast<float>(std::round(fb * levels) / levels);
                }
                fb = s.crushHold[c];
            }
            const double in = pInput[f * ch + c];
            const double into = std::clamp(in * send + fb, -2.0, 2.0);   // (never running away)
            s.delay[s.write * ch + c] = static_cast<CSAMPLE>(into);
            pOutput[f * ch + c] = static_cast<CSAMPLE>(in * s.dryGain + wet);
        }
        s.write = (s.write + 1) % capacity;
        s.frames += 1;
    }
}

// ---- Filter LFO ----------------------------------------------------------------------------------------------------
// A filter swept up and down by the beat (once every Rate beats), Depth how far; low, band or high pass

QString FilterLfoEffect::getId() {
    return "org.zydeck.effects.filterlfo";
}

EffectManifestPointer FilterLfoEffect::getManifest() {
    EffectManifestPointer m = manifest(getId(), QObject::tr("Filter LFO"), QObject::tr("Filt LFO"),
            QObject::tr("A filter swept up and down to the beat"), 0.6);
    beatsKnob(m, "rate", QObject::tr("Rate"), QObject::tr("Beats a sweep takes"), 0.25, 2, 32);
    knob(m, "depth", QObject::tr("Depth"), QObject::tr("How far it sweeps"), 0, 0.6, 1, true);
    knob(m, "resonance", QObject::tr("Resonance"), QObject::tr("How much it rings at its edge"), 0, 0.4, 1);
    knob(m, "mode", QObject::tr("Mode"), QObject::tr("Low pass, band pass or high pass"), 0, 0, 2);
    return m;
}

void FilterLfoEffect::loadEngineEffectParameters(const QMap<QString, EngineEffectParameterPointer>& p) {
    m_pRate = p.value("rate");
    m_pDepth = p.value("depth");
    m_pResonance = p.value("resonance");
    m_pMode = p.value("mode");
}

void FilterLfoEffect::processChannel(FilterLfoState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    FilterLfoState& s = *pState;
    const int ch = engineParameters.channelCount();
    const SINT frames = engineParameters.samplesPerBuffer() / ch;
    const double rate = engineParameters.sampleRate();
    s.clock.update(groupFeatures, rate, frames);
    if (enableState == EffectEnableState::Enabling) {
        s.filter[0].reset();
        s.filter[1].reset();
        s.beat = 0;
        s.lastFraction = -1;
    }
    const double beats = std::max(0.25, onGrid(m_pRate->value()));
    const double depth = std::clamp(m_pDepth->value(), 0.0, 1.0);
    const double k = 1.0 / (0.7 + 7 * std::clamp(m_pResonance->value(), 0.0, 1.0));
    const int mode = std::clamp(static_cast<int>(std::lround(m_pMode->value())), 0, 2);
    const int svfMode = mode == 0 ? 0 : mode == 1 ? 1 : 2;
    for (SINT f = 0; f < frames; ++f) {
        const double fr = s.clock.fractionAt(f, frames);
        if (s.lastFraction >= 0 && fr < s.lastFraction - 0.5) {
            s.beat = (s.beat + 1) % 1024;
        }
        s.lastFraction = fr;
        double phase = (s.beat % std::max(1, static_cast<int>(beats)) + fr) / beats;
        phase -= std::floor(phase);
        const double lfo = 0.5 - 0.5 * std::cos(2 * kPi * phase);   // (0 at the sweep's start, 1 halfway)
        double fc;
        if (mode == 0) {
            fc = 16000 * std::pow(200.0 / 16000, depth * lfo);
        } else if (mode == 2) {
            fc = 30 * std::pow(6000.0 / 30, depth * lfo);
        } else {
            fc = 400 * std::pow(5000.0 / 400, lfo * depth + (1 - depth) * 0.5);
        }
        const double g = gOf(fc, rate);
        for (int c = 0; c < ch && c < 2; ++c) {
            pOutput[f * ch + c] = static_cast<CSAMPLE>(s.filter[c].process(pInput[f * ch + c], g, k, svfMode));
        }
    }
}

// ---- Sweep ---------------------------------------------------------------------------------------------------------
// One knob, nothing at its middle: left a gate on the quarter beats closing as it's turned, right a band of the track
// sweeping up and narrowing

QString SweepEffect::getId() {
    return "org.zydeck.effects.sweep";
}

EffectManifestPointer SweepEffect::getManifest() {
    EffectManifestPointer m = manifest(getId(), QObject::tr("Sweep"), QObject::tr("Sweep"),
            QObject::tr("One knob: left a beat gate closing, right a band sweeping up; nothing at the middle"), 0.5);
    knob(m, "amount", QObject::tr("Amount"), QObject::tr("Left: the gate; right: the band"), -1, 0, 1, true);
    return m;
}

void SweepEffect::loadEngineEffectParameters(const QMap<QString, EngineEffectParameterPointer>& p) {
    m_pAmount = p.value("amount");
}

void SweepEffect::processChannel(SweepState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    SweepState& s = *pState;
    const int ch = engineParameters.channelCount();
    const SINT frames = engineParameters.samplesPerBuffer() / ch;
    const double rate = engineParameters.sampleRate();
    s.clock.update(groupFeatures, rate, frames);
    if (enableState == EffectEnableState::Enabling) {
        s.filter[0].reset();
        s.filter[1].reset();
        s.gain = 1;
    }
    const double a = std::clamp(m_pAmount->value(), -1.0, 1.0);
    const double step = 1.0 / (0.002 * rate);
    const double g = gOf(300 * std::pow(6000.0 / 300, std::max(0.0, a)), rate);
    const double k = 1.0 / (0.7 + 4 * std::max(0.0, a));
    for (SINT f = 0; f < frames; ++f) {
        double quarter = s.clock.fractionAt(f, frames) * 4;
        quarter -= std::floor(quarter);
        const double open = a < 0 ? (quarter < 1 - 0.85 * -a ? 1.0 : 0.0) : 1.0;
        s.gain += std::clamp(open - s.gain, -step, step);
        for (int c = 0; c < ch && c < 2; ++c) {
            const double in = pInput[f * ch + c];
            double out = in * s.gain;
            if (a > 0) {
                const double bp = s.filter[c].process(in, g, k, 1) * 1.6;
                out = in + (bp - in) * std::min(1.0, a * 2);
            }
            pOutput[f * ch + c] = static_cast<CSAMPLE>(out);
        }
    }
}

// ---- Robot ---------------------------------------------------------------------------------------------------------
// The track ring-modulated at a pitch and rung through a comb tuned to it: a robot's voice

RobotState::RobotState(const mixxx::EngineParameters& engineParameters)
        : EffectState(engineParameters) {
    for (std::vector<float>& c : comb) {
        c.assign(static_cast<size_t>(engineParameters.sampleRate() / 30) + 2, 0.f);
    }
}

QString RobotEffect::getId() {
    return "org.zydeck.effects.robot";
}

EffectManifestPointer RobotEffect::getManifest() {
    EffectManifestPointer m = manifest(getId(), QObject::tr("Robot"), QObject::tr("Robot"),
            QObject::tr("A robot's voice: the track ring-modulated and rung at a pitch"), 0.4);
    knob(m, "pitch", QObject::tr("Pitch"), QObject::tr("The robot's pitch"), 0, 0.4, 1, true);
    knob(m, "metal", QObject::tr("Metal"), QObject::tr("How much it rings"), 0, 0.6, 1);
    return m;
}

void RobotEffect::loadEngineEffectParameters(const QMap<QString, EngineEffectParameterPointer>& p) {
    m_pPitch = p.value("pitch");
    m_pMetal = p.value("metal");
}

void RobotEffect::processChannel(RobotState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState&) {
    RobotState& s = *pState;
    const int ch = engineParameters.channelCount();
    const SINT frames = engineParameters.samplesPerBuffer() / ch;
    const double rate = engineParameters.sampleRate();
    if (enableState == EffectEnableState::Enabling) {
        std::fill(s.comb[0].begin(), s.comb[0].end(), 0.f);
        std::fill(s.comb[1].begin(), s.comb[1].end(), 0.f);
        s.phase = 0;
    }
    const double hz = 40 * std::pow(600.0 / 40, std::clamp(m_pPitch->value(), 0.0, 1.0));
    const double fb = 0.92 * std::clamp(m_pMetal->value(), 0.0, 1.0);
    const size_t size = s.comb[0].size();
    const size_t period = std::clamp<size_t>(static_cast<size_t>(rate / hz), 2, size - 1);
    for (SINT f = 0; f < frames; ++f) {
        const double carrier = std::sin(2 * kPi * s.phase);
        s.phase += hz / rate;
        s.phase -= std::floor(s.phase);
        const size_t back = (s.at + size - period) % size;
        for (int c = 0; c < ch && c < 2; ++c) {
            const double ring = pInput[f * ch + c] * carrier * 1.4;
            const double y = ring + s.comb[c][back] * fb;
            s.comb[c][s.at] = static_cast<float>(std::clamp(y, -2.0, 2.0));
            pOutput[f * ch + c] = static_cast<CSAMPLE>(y * (1 - fb * 0.5));
        }
        s.at = (s.at + 1) % size;
    }
}

// ---- Reverb Rise ---------------------------------------------------------------------------------------------------
// A reverb that builds while it's held: in Time beats its tail rises (or falls: Pitch below 0) by Pitch semitones and
// the track under it fades by Duck

ReverbRiseState::ReverbRiseState(const mixxx::EngineParameters& engineParameters)
        : EffectState(engineParameters) {
    reverb.setup(engineParameters.sampleRate());
    for (zydeckfx::PitchShifter& s : shift) {
        s.setup(engineParameters.sampleRate());
    }
}

QString ReverbRiseEffect::getId() {
    return "org.zydeck.effects.reverbrise";
}

EffectManifestPointer ReverbRiseEffect::getManifest() {
    EffectManifestPointer m = manifest(getId(), QObject::tr("Reverb Rise"), QObject::tr("Rev Rise"),
            QObject::tr("A reverb that builds while it's held, its tail rising (or falling) and the track fading under it"), 0.8);
    beatsKnob(m, "time", QObject::tr("Time"), QObject::tr("Beats it takes to build"), 1, 8, 32);
    knob(m, "pitch", QObject::tr("Pitch"), QObject::tr("Semitones its tail goes to: up, or down"), -12, 12, 12);
    knob(m, "size", QObject::tr("Size"), QObject::tr("How big the room"), 0, 0.8, 1, true);
    knob(m, "duck", QObject::tr("Duck"), QObject::tr("How much the track fades under it"), 0, 0.5, 1);
    return m;
}

void ReverbRiseEffect::loadEngineEffectParameters(const QMap<QString, EngineEffectParameterPointer>& p) {
    m_pTime = p.value("time");
    m_pPitch = p.value("pitch");
    m_pSize = p.value("size");
    m_pDuck = p.value("duck");
}

void ReverbRiseEffect::processChannel(ReverbRiseState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    ReverbRiseState& s = *pState;
    const int ch = engineParameters.channelCount();
    const SINT frames = engineParameters.samplesPerBuffer() / ch;
    const double rate = engineParameters.sampleRate();
    zydeckfx::BeatClock clock;
    clock.update(groupFeatures, rate, frames);
    if (enableState == EffectEnableState::Enabling) {
        s.frames = 0;
    }
    const double length = std::max(1.0, m_pTime->value()) * clock.framesPerBeat;
    const double pitch = m_pPitch->value(), size = std::clamp(m_pSize->value(), 0.0, 1.0), duck = std::clamp(m_pDuck->value(), 0.0, 1.0);
    for (SINT f = 0; f < frames; ++f) {
        const double ramp = std::min(1.0, s.frames / length);
        const double ratio = std::exp2(pitch * ramp / 12);
        const float inL = pInput[f * ch], inR = ch > 1 ? pInput[f * ch + 1] : inL;
        float wetL = 0, wetR = 0;
        s.reverb.process(inL, inR, size, 0.3, &wetL, &wetR);
        wetL = s.shift[0].process(wetL, ratio);
        wetR = s.shift[1].process(wetR, ratio);
        const double wetGain = (0.4 + 0.6 * ramp) * 3, dry = 1 - duck * ramp;
        pOutput[f * ch] = static_cast<CSAMPLE>(inL * dry + wetL * wetGain);
        if (ch > 1) {
            pOutput[f * ch + 1] = static_cast<CSAMPLE>(inR * dry + wetR * wetGain);
        }
        s.frames += 1;
    }
}

// ---- Noise Riser ---------------------------------------------------------------------------------------------------
// Noise swept over Time beats while it's held: up through a high pass (a riser), or with Fall down through a low pass

QString NoiseRiserEffect::getId() {
    return "org.zydeck.effects.noiseriser";
}

EffectManifestPointer NoiseRiserEffect::getManifest() {
    EffectManifestPointer m = manifest(getId(), QObject::tr("Noise Riser"), QObject::tr("N.Riser"),
            QObject::tr("Noise swept while it's held: rising (or falling) over so many beats"), 0.5);
    beatsKnob(m, "time", QObject::tr("Time"), QObject::tr("Beats the sweep takes"), 1, 8, 32);
    knob(m, "level", QObject::tr("Level"), QObject::tr("How loud the noise"), 0, 0.5, 1, true);
    toggle(m, "fall", QObject::tr("Fall"), QObject::tr("Sweeping down instead"), 0);
    return m;
}

void NoiseRiserEffect::loadEngineEffectParameters(const QMap<QString, EngineEffectParameterPointer>& p) {
    m_pTime = p.value("time");
    m_pLevel = p.value("level");
    m_pFall = p.value("fall");
}

void NoiseRiserEffect::processChannel(NoiseRiserState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    NoiseRiserState& s = *pState;
    const int ch = engineParameters.channelCount();
    const SINT frames = engineParameters.samplesPerBuffer() / ch;
    const double rate = engineParameters.sampleRate();
    zydeckfx::BeatClock clock;
    clock.update(groupFeatures, rate, frames);
    if (enableState == EffectEnableState::Enabling) {
        s.frames = 0;
        s.filter[0].reset();
        s.filter[1].reset();
    }
    const double length = std::max(1.0, m_pTime->value()) * clock.framesPerBeat;
    const double level = std::clamp(m_pLevel->value(), 0.0, 1.0) * 0.35;
    const bool fall = m_pFall->toBool();
    for (SINT f = 0; f < frames; ++f) {
        const double ramp = std::min(1.0, s.frames / length);
        const double fc = fall ? 14000 * std::pow(150.0 / 14000, ramp) : 150 * std::pow(9000.0 / 150, ramp);
        const double g = gOf(fc, rate), k = 1.0 / 2.5;
        const double amp = level * (fall ? 1 - 0.8 * ramp : 0.2 + 0.8 * ramp);
        for (int c = 0; c < ch && c < 2; ++c) {
            s.seed = s.seed * 1664525u + 1013904223u;
            const double noise = (static_cast<double>(s.seed >> 8) / 8388608.0) - 1;
            const double shaped = s.filter[c].process(noise, g, k, fall ? 0 : 2);
            pOutput[f * ch + c] = static_cast<CSAMPLE>(pInput[f * ch + c] + shaped * amp);
        }
        s.frames += 1;
    }
}
