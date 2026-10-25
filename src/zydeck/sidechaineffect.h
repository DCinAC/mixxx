#pragma once

#include "effects/backends/effectmanifest.h"
#include "effects/backends/effectprocessor.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/class.h"
#include "util/types.h"

// Zydek: a sidechain pump, as a kick ducks a pad: the sound drops on each beat (or every 1/2 or 1/4 beat) and
// swells back before the next, locked to the deck's beat grid (the beat's position at the buffer's end, so it can't
// drift); 120 BPM free-running where there's no grid. Depth: how far it drops; Release: how much of each pump it
// takes to come back (a smooth curve: it stays down a moment, swells, then eases in); Rate: 1/4, 1/2 or 1 beat.
class SidechainState : public EffectState {
  public:
    SidechainState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters),
              gain(1),
              freeFrame(0) {
    }
    ~SidechainState() override = default;

    double gain;            // what the last frame got (the next moves towards its target, never in a jump)
    unsigned int freeFrame; // without a beat grid: frames into the free-running beat
};

class SidechainEffect : public EffectProcessorImpl<SidechainState> {
  public:
    SidechainEffect() = default;
    ~SidechainEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            SidechainState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }

    EngineEffectParameterPointer m_pDepthParameter;
    EngineEffectParameterPointer m_pReleaseParameter;
    EngineEffectParameterPointer m_pRateParameter;

    DISALLOW_COPY_AND_ASSIGN(SidechainEffect);
};
