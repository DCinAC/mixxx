#include "effects/backends/builtin/sidechaineffect.h"

#include <algorithm>
#include <cmath>

namespace {
// the gain moves by its whole range in no less than this: a duck without a click
constexpr double kMinSwingSeconds = 0.0015;
// without a beat grid
constexpr double kFreeBeatSeconds = 0.5;
} // namespace

// static
QString SidechainEffect::getId() {
    return "org.zydeck.effects.sidechain";
}

// static
EffectManifestPointer SidechainEffect::getManifest() {
    EffectManifestPointer pManifest(new EffectManifest());
    pManifest->setId(getId());
    pManifest->setName(QObject::tr("Sidechain"));
    pManifest->setShortName(QObject::tr("Sidechain"));
    pManifest->setAuthor("ZyDeck");
    pManifest->setVersion("1.0");
    pManifest->setDescription(QObject::tr(
            "Pumps the volume with the beat, as a kick ducks a pad: it drops on each beat and swells back "
            "before the next"));

    EffectManifestParameterPointer depth = pManifest->addParameter();
    depth->setId("depth");
    depth->setName(QObject::tr("Depth"));
    depth->setShortName(QObject::tr("Depth"));
    depth->setDescription(QObject::tr("How far the volume drops on the beat"));
    depth->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    depth->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    depth->setDefaultLinkType(EffectManifestParameter::LinkType::Linked);
    depth->setRange(0, 0.8, 1);

    EffectManifestParameterPointer rate = pManifest->addParameter();
    rate->setId("rate");
    rate->setName(QObject::tr("Rate"));
    rate->setShortName(QObject::tr("Rate"));
    rate->setDescription(QObject::tr("How often it pumps: every 1/4, 1/2, 1, 2 or 4 beats"));
    rate->setValueScaler(EffectManifestParameter::ValueScaler::Logarithmic);
    rate->setUnitsHint(EffectManifestParameter::UnitsHint::Beats);   // (ZyDeck's beat keys set it)
    rate->setRange(1.0 / 4, 1, 4);

    EffectManifestParameterPointer release = pManifest->addParameter();
    release->setId("release");
    release->setName(QObject::tr("Release"));
    release->setShortName(QObject::tr("Release"));
    release->setDescription(QObject::tr("How much of each pump the volume takes to come back"));
    release->setValueScaler(EffectManifestParameter::ValueScaler::Linear);
    release->setUnitsHint(EffectManifestParameter::UnitsHint::Unknown);
    release->setRange(0.1, 0.7, 1);

    return pManifest;
}

void SidechainEffect::loadEngineEffectParameters(
        const QMap<QString, EngineEffectParameterPointer>& parameters) {
    m_pDepthParameter = parameters.value("depth");
    m_pRateParameter = parameters.value("rate");
    m_pReleaseParameter = parameters.value("release");
}

void SidechainEffect::processChannel(
        SidechainState* pState,
        const CSAMPLE* pInput,
        CSAMPLE* pOutput,
        const mixxx::EngineParameters& engineParameters,
        const EffectEnableState enableState,
        const GroupFeatureState& groupFeatures) {
    const double depth = std::clamp(m_pDepthParameter->value(), 0.0, 1.0);
    const double release = std::clamp(m_pReleaseParameter->value(), 0.1, 1.0);
    // one pump's length in beats: 1/4, 1/2, 1, 2 or 4
    const double beats = std::clamp(std::exp2(std::round(std::log2(std::max(m_pRateParameter->value(), 0.25)))), 0.25, 4.0);
    const int channels = engineParameters.channelCount();
    const SINT frames = engineParameters.samplesPerBuffer() / channels;
    const double sampleRate = engineParameters.sampleRate();
    if (enableState == EffectEnableState::Enabling) {
        pState->gain = 1;
        pState->beat = -1;           // (the next beat is the first pump's)
        pState->lastFraction = -1;
    }

    // where in the beat the buffer ends: the deck's grid, or 120 BPM running free
    double framesPerBeat, endFraction;
    const GroupFeatureState& gf = groupFeatures;
    if (gf.beat_length.has_value() && gf.beat_fraction_buffer_end.has_value() && gf.beat_length->seconds > 0) {
        framesPerBeat = gf.beat_length->seconds * sampleRate;
        endFraction = *gf.beat_fraction_buffer_end;
    } else {
        framesPerBeat = kFreeBeatSeconds * sampleRate;
        pState->freeFrame = static_cast<unsigned int>(std::fmod(pState->freeFrame + frames, framesPerBeat));
        endFraction = pState->freeFrame / framesPerBeat;
    }

    const double maxStep = 1.0 / (kMinSwingSeconds * sampleRate);
    double gain = pState->gain;
    for (SINT f = 0; f < frames; ++f) {
        double fraction = endFraction - static_cast<double>(frames - f) / framesPerBeat;
        fraction -= std::floor(fraction);
        if (pState->lastFraction >= 0 && fraction < pState->lastFraction - 0.5) {
            pState->beat = (pState->beat + 1) % 4;   // (a new beat: counted round a bar, enough for 4-beat pumps)
        }
        pState->lastFraction = fraction;
        double phase;
        if (beats > 1) {   // over several beats: which of them, and where in it (before the first: up, waiting)
            const int n = static_cast<int>(beats);
            phase = pState->beat < 0 ? 0.999 : ((pState->beat % n) + fraction) / beats;
        } else {
            phase = fraction / beats;
            phase -= std::floor(phase);
        }
        // down at the pump's start, then back up over `release` of it: a moment low, a swell, an easing in
        const double x = std::min(1.0, phase / release);
        const double target = 1.0 - depth * (1.0 - x * x * (3.0 - 2.0 * x));
        gain += std::clamp(target - gain, -maxStep, maxStep);
        for (int c = 0; c < channels; ++c) {
            pOutput[f * channels + c] = static_cast<CSAMPLE>(gain) * pInput[f * channels + c];
        }
    }
    pState->gain = gain;
}
