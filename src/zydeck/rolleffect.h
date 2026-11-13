#pragma once

#include "effects/backends/effectmanifest.h"
#include "effects/backends/effectprocessor.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/class.h"
#include "util/sample.h"
#include "util/samplebuffer.h"
#include "util/types.h"

// ZyDeck: a beat roll as an effect, so it mixes with the track: switched on, it records the next 1/16 to 4 beats
// of what the deck plays (from the next beat, or the next 1/4 of one for a shorter roll, on the deck's grid) and
// then repeats them until it's switched off. The track keeps playing underneath: the effect unit's MIX sets how
// much of each is heard (half and half at the middle), unlike Mixxx's own roll, which takes the deck's sound over.
// Fade (the meta knob): each repeat a little quieter. While it records, it lets the track through as it is.
class RollState : public EffectState {
  public:
    // 4 seconds: 4 beats down to 60 BPM (a longer roll is cut to what fits)
    static constexpr int kMaxSeconds = 4;
    // the seam: the loop's last few ms fade into what came just before it started, so it joins without a click
    static constexpr double kSeamSeconds = 0.004;

    RollState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters) {
        audioParametersChanged(engineParameters);
        clear();
    }
    ~RollState() override = default;

    void audioParametersChanged(const mixxx::EngineParameters& engineParameters) {
        loop = mixxx::SampleBuffer(kMaxSeconds * engineParameters.sampleRate() * engineParameters.channelCount());
        seamFrames = std::max<SINT>(1, static_cast<SINT>(kSeamSeconds * engineParameters.sampleRate()));
        before = mixxx::SampleBuffer(seamFrames * engineParameters.channelCount());
    }

    void clear() {
        before.clear();
        waiting = true;
        recorded = 0;
        played = 0;
        beforeAt = 0;
        beforeFilled = 0;
        length = 0;
        beats = 0;
        lastFraction = -1;
        gain = 1;
    }

    mixxx::SampleBuffer loop;     // what it recorded (frames, interleaved)
    mixxx::SampleBuffer before;   // the frames just before the loop started (a ring until then)
    SINT seamFrames = 1;
    bool waiting = true;          // for the beat (or the part of one) it starts on
    SINT recorded = 0;            // frames recorded
    SINT played = 0;              // frames into the loop, repeating
    SINT beforeAt = 0;            // where the ring writes next
    SINT beforeFilled = 0;        // and how much of it has been (none: switched on without waiting, no seam)
    SINT length = 0;              // the loop's frames (its beats at the tempo it started at)
    double beats = 0;             // the length it was asked for (a new one while it repeats: shorter, or again)
    double lastFraction = -1;     // where in the beat the last frame was
    double gain = 1;              // the repeats' level (Fade)
};

class RollEffect : public EffectProcessorImpl<RollState> {
  public:
    RollEffect() = default;
    ~RollEffect() override = default;

    static QString getId();
    static EffectManifestPointer getManifest();

    void loadEngineEffectParameters(
            const QMap<QString, EngineEffectParameterPointer>& parameters) override;

    void processChannel(
            RollState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }

    EngineEffectParameterPointer m_pBeatsParameter;
    EngineEffectParameterPointer m_pFadeParameter;
    EngineEffectParameterPointer m_pQuantizeParameter;

    DISALLOW_COPY_AND_ASSIGN(RollEffect);
};
