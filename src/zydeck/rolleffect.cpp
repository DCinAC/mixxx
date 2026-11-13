#include "zydeck/rolleffect.h"

#include <algorithm>
#include <cmath>

namespace {
// without a beat grid
constexpr double kFreeBeatSeconds = 0.5;
// the shortest roll, and the longest
constexpr double kMinBeats = 1.0 / 16;
constexpr double kMaxBeats = 4;
} // namespace

// static
QString RollEffect::getId() {
    return "org.zydeck.effects.roll";
}

// static
QString BackwardRollEffect::getId() {
    return "org.zydeck.effects.backroll";
}

// static
EffectManifestPointer RollEffect::getManifest() {
    return manifest(false);
}

// static
EffectManifestPointer BackwardRollEffect::getManifest() {
    return manifest(true);
}

// static
EffectManifestPointer RollEffect::manifest(bool backward) {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(backward ? BackwardRollEffect::getId() : RollEffect::getId());
    pManifest->setName(backward ? QObject::tr("Backward Roll") : QObject::tr("Roll"));
    pManifest->setShortName(backward ? QObject::tr("Back Roll") : QObject::tr("Roll"));
    pManifest->setAuthor("ZyDeck");
    pManifest->setVersion("1.0");
    pManifest->setDescription(backward
                    ? QObject::tr("Records the next few beats as it's switched on and repeats them backwards, the "
                                  "track still playing underneath: the mix sets how much of each is heard")
                    : QObject::tr("Records the next few beats as it's switched on and repeats them, the track still "
                                  "playing underneath: the mix sets how much of each is heard"));
    pManifest->setEffectRampsFromDry(true);
    pManifest->setMetaknobDefault(0.0);

    EffectManifestParameterPointer beats = pManifest->addParameter();
    beats->setId("beats");
    beats->setName(QObject::tr("Beats"));
    beats->setShortName(QObject::tr("Beats"));
    beats->setDescription(QObject::tr("How long the repeated part is: 1/16 to 4 beats"));
    beats->setValueScaler(EffectManifestParameter::ValueScaler::Logarithmic);
    beats->setUnitsHint(EffectManifestParameter::UnitsHint::Beats);   // (ZyDeck's beat keys set it)
    beats->setRange(kMinBeats, 1, kMaxBeats);

    EffectManifestParameterPointer fade = pManifest->addParameter();
    fade->setId("fade");
    fade->setName(QObject::tr("Fade"));
    fade->setShortName(QObject::tr("Fade"));
    fade->setDescription(QObject::tr("How much quieter each repeat gets"));
    fade->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    fade->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    fade->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    fade->setRange(0, 0, 1);

    EffectManifestParameterPointer quantize = pManifest->addParameter();
    quantize->setId("quantize");
    quantize->setName(QObject::tr("Quantize"));
    quantize->setShortName(QObject::tr("Quantize"));
    quantize->setDescription(QObject::tr(
            "Start on the next beat (a roll shorter than a beat: the next part of one as long), and round "
            "the length to 1/16 beat or a triplet (1/12, 1/6, 1/3)"));
    quantize->setValueScaler(EffectManifestParameter::ValueScaler::Toggle);
    quantize->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    quantize->setRange(0, 1, 1);

    return pManifest;
}

void RollEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pBeatsParameter = parameters.value("beats");
    m_pFadeParameter = parameters.value("fade");
    m_pQuantizeParameter = parameters.value("quantize");
}

void RollEffect::processChannel(
        RollState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    RollState& s = *pState;
    const int channels = engineParameters.channelCount();
    const SINT frames = engineParameters.samplesPerBuffer() / channels;
    const double sampleRate = engineParameters.sampleRate();
    const bool quantize = m_pQuantizeParameter->toBool();
    const GroupFeatureState& gf = groupFeatures;
    const bool grid = gf.beat_length.has_value() && gf.beat_fraction_buffer_end.has_value() && gf.beat_length->seconds > 0;

    double beats = std::clamp(m_pBeatsParameter->value(), kMinBeats, kMaxBeats);
    if (quantize) {
        beats = std::max(kMinBeats, std::round(beats * 48) / 48);   // (straight 1/16s and triplets: 1/12, 1/6, 1/3)
    }
    const double framesPerBeat = (grid ? gf.beat_length->seconds : kFreeBeatSeconds) * sampleRate;
    const SINT capacity = static_cast<SINT>(s.loop.size()) / channels;
    const SINT wanted = std::clamp(static_cast<SINT>(std::lround(beats * framesPerBeat)), 2 * s.seamFrames, capacity);
    // it starts on the next beat, or the next part of one as long as the roll's largest power of 2 (a 3/4 roll: 1/2)
    const double step = std::min(1.0, std::exp2(std::floor(std::log2(beats))));

    if (enableState == EffectEnableState::Enabling) {
        s.clear();
        s.waiting = quantize && grid;
        s.length = wanted;
        s.beats = beats;
    } else if (beats != s.beats) {   // a new length while it plays: a shorter one is in what it has; a longer, again
        s.beats = beats;
        if (!s.waiting && wanted <= s.recorded) {
            s.length = wanted;
            s.played %= wanted;
        } else if (s.recorded >= s.length) {
            s.clear();
            s.beats = beats;
            s.waiting = quantize && grid;
            s.length = wanted;
        } else {
            s.length = wanted;   // (still recording: it records that far)
        }
    }

    const double fade = std::clamp(m_pFadeParameter->value(), 0.0, 1.0);
    const double perFrame = std::pow(1.0 - 0.8 * fade, 1.0 / static_cast<double>(s.length));
    const SINT ringFrames = s.seamFrames;
    for (SINT f = 0; f < frames; ++f) {
        const CSAMPLE* in = pInput + f * channels;
        CSAMPLE* out = pOutput + f * channels;
        if (s.waiting) {
            const double fraction = grid ? *gf.beat_fraction_buffer_end - static_cast<double>(frames - f) / framesPerBeat : 0;
            const double fr = fraction - std::floor(fraction);
            const bool crossed = s.lastFraction >= 0 &&
                    (std::floor(fr / step) != std::floor(s.lastFraction / step) || fr < s.lastFraction - 0.5);
            s.lastFraction = fr;
            if (!crossed) {
                for (int c = 0; c < channels; ++c) {   // (the ring: what comes just before it starts)
                    s.before[s.beforeAt * channels + c] = in[c];
                    out[c] = in[c];
                }
                s.beforeAt = (s.beforeAt + 1) % ringFrames;
                s.beforeFilled = std::min(s.beforeFilled + 1, ringFrames);
                continue;
            }
            s.waiting = false;
        }
        SINT p;
        bool repeat;
        if (s.recorded < s.length) {   // recording: the track as it is
            p = s.recorded++;
            repeat = false;
            for (int c = 0; c < channels; ++c) {
                s.loop[p * channels + c] = in[c];
            }
        } else {
            if (s.played == 0) {
                ++s.passes;
            }
            p = s.played;
            s.played = (s.played + 1) % s.length;
            repeat = true;
            s.gain *= perFrame;
        }
        if (m_backward && repeat) {   // backwards: from its end to its start, a few ms' fade where it jumps back
            const SINT q = s.length - 1 - p;
            const SINT edge = std::min(s.seamFrames, s.length / 2);
            double env = 1;
            if (p >= s.length - edge) {
                env = static_cast<double>(s.length - p) / (edge + 1);   // (to its start: out)
            } else if (p < edge && s.passes > 1) {
                env = static_cast<double>(p + 1) / (edge + 1);          // (from its end again: in; the first follows on)
            }
            const CSAMPLE_GAIN g = static_cast<CSAMPLE_GAIN>(s.gain * env);
            for (int c = 0; c < channels; ++c) {
                out[c] = g * s.loop[q * channels + c];
            }
            continue;
        }
        if (m_backward) {   // (recording, before the first backwards one: the track as it is, no seam)
            for (int c = 0; c < channels; ++c) {
                out[c] = in[c];
            }
            continue;
        }
        const SINT seam = std::min(s.beforeFilled, s.length / 2);
        const CSAMPLE_GAIN g = repeat ? static_cast<CSAMPLE_GAIN>(s.gain) : 1;
        if (seam > 0 && p >= s.length - seam) {   // its end fades into what led to its start: the jump back to it doesn't click
            const SINT k = p - (s.length - seam);
            const double t = static_cast<double>(k + 1) / (seam + 1);
            const SINT b = (s.beforeAt + ringFrames - seam + k) % ringFrames;   // (the ring's oldest is at beforeAt)
            for (int c = 0; c < channels; ++c) {
                out[c] = g * static_cast<CSAMPLE>((1 - t) * s.loop[p * channels + c] + t * s.before[b * channels + c]);
            }
        } else {
            for (int c = 0; c < channels; ++c) {
                out[c] = g * s.loop[p * channels + c];
            }
        }
    }
}
