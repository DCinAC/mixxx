#pragma once

#include <vector>

#include "effects/backends/effectmanifest.h"
#include "effects/backends/effectprocessor.h"
#include "engine/effects/engineeffectparameter.h"
#include "util/class.h"
#include "util/sample.h"
#include "util/samplebuffer.h"
#include "util/types.h"

// ZyDeck's PAD FX effects (besides Roll and Backward Roll, rolleffect.h), the kinds of effect a DJ mixer's pads play,
// written for ZyDeck: a beat echo whose repeats can rise or fall in pitch, be filtered or crushed, tap more than once
// or ring out alone; a filter swept to the beat; one knob from a beat gate to a band sweep; a robot voice; a reverb
// that rises or falls while it's held; a noise riser. Each locked to the deck's beat grid where it has a length.
namespace zydeckfx {

// A state-variable filter (Zavalishin's, trapezoidal): low, band or high pass, one channel
struct Svf {
    double ic1 = 0, ic2 = 0;
    void reset() {
        ic1 = ic2 = 0;
    }
    // mode: 0 low, 1 band, 2 high; g = tan(pi fc / fs), k = 1 / Q
    double process(double x, double g, double k, int mode) {
        const double a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const double v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        return mode == 0 ? v2 : mode == 1 ? v1 : x - k * v1 - v2;
    }
};

// A pitch shifter of two read heads sweeping a short delay, crossfaded: plenty for echoes and reverb tails; latency()
// frames late on average
class PitchShifter {
  public:
    void setup(double sampleRate) {
        m_window = std::max(256, static_cast<int>(0.05 * sampleRate));
        m_buffer.assign(static_cast<size_t>(m_window) * 2 + 4, 0.f);
        m_write = 0;
        m_phase = 0;
    }
    int latency() const {
        return m_window / 2;
    }
    float process(float x, double ratio);

  private:
    std::vector<float> m_buffer;
    int m_window = 2048;
    int m_write = 0;
    double m_phase = 0;
};

// A reverb (Schroeder-Moorer, as Freeverb): 8 combs and 4 all-passes a channel, the right one spread
class Reverb {
  public:
    void setup(double sampleRate);
    void process(float inL, float inR, double size, double damp, float* pOutL, float* pOutR);

  private:
    struct Comb {
        std::vector<float> buf;
        size_t at = 0;
        float store = 0;
    };
    struct AllPass {
        std::vector<float> buf;
        size_t at = 0;
    };
    Comb m_combs[2][8];
    AllPass m_allPasses[2][4];
};

// Where the deck is in its beats, frame by frame (the deck's grid; 120 BPM running free without one)
struct BeatClock {
    double framesPerBeat = 24000;
    double endFraction = 0;     // (of the buffer's end)
    unsigned int freeFrame = 0;
    void update(const GroupFeatureState& gf, double sampleRate, SINT frames);
    double fractionAt(SINT f, SINT frames) const {   // frame f of this buffer: where in its beat (0-1)
        const double x = endFraction - static_cast<double>(frames - f) / framesPerBeat;
        return x - std::floor(x);
    }
};

} // namespace zydeckfx

// ---- Beat Echo -----------------------------------------------------------------------------------------------------
class BeatEchoState : public EffectState {
  public:
    static constexpr int kMaxSeconds = 4;
    BeatEchoState(const mixxx::EngineParameters& engineParameters);
    mixxx::SampleBuffer delay;   // (interleaved, both channels)
    SINT write = 0;
    zydeckfx::PitchShifter shift[2];
    zydeckfx::Svf filter[2];
    float crushHold[2] = {0, 0};
    int crushCount = 0;
    double frames = 0;           // since it was switched on
    double delayFrames = 0;      // the delay as it's moving to a new length (no jump)
    double dryGain = 1;
    zydeckfx::BeatClock clock;
};

class BeatEchoEffect : public EffectProcessorImpl<BeatEchoState> {
  public:
    BeatEchoEffect() = default;
    static QString getId();
    static EffectManifestPointer getManifest();
    void loadEngineEffectParameters(const QMap<QString, EngineEffectParameterPointer>& parameters) override;
    void processChannel(BeatEchoState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }
    EngineEffectParameterPointer m_pTime, m_pFeedback, m_pPitch, m_pGlide, m_pTaps, m_pTone, m_pBand, m_pCrush, m_pRelease;
    DISALLOW_COPY_AND_ASSIGN(BeatEchoEffect);
};

// ---- Filter LFO ----------------------------------------------------------------------------------------------------
class FilterLfoState : public EffectState {
  public:
    FilterLfoState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters) {
    }
    zydeckfx::Svf filter[2];
    zydeckfx::BeatClock clock;
    int beat = 0;
    double lastFraction = -1;
    double cutoff = 1000;
};

class FilterLfoEffect : public EffectProcessorImpl<FilterLfoState> {
  public:
    FilterLfoEffect() = default;
    static QString getId();
    static EffectManifestPointer getManifest();
    void loadEngineEffectParameters(const QMap<QString, EngineEffectParameterPointer>& parameters) override;
    void processChannel(FilterLfoState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }
    EngineEffectParameterPointer m_pRate, m_pDepth, m_pResonance, m_pMode;
    DISALLOW_COPY_AND_ASSIGN(FilterLfoEffect);
};

// ---- Sweep ---------------------------------------------------------------------------------------------------------
class SweepState : public EffectState {
  public:
    SweepState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters) {
    }
    zydeckfx::Svf filter[2];
    zydeckfx::BeatClock clock;
    double gain = 1;
};

class SweepEffect : public EffectProcessorImpl<SweepState> {
  public:
    SweepEffect() = default;
    static QString getId();
    static EffectManifestPointer getManifest();
    void loadEngineEffectParameters(const QMap<QString, EngineEffectParameterPointer>& parameters) override;
    void processChannel(SweepState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }
    EngineEffectParameterPointer m_pAmount;
    DISALLOW_COPY_AND_ASSIGN(SweepEffect);
};

// ---- Robot ---------------------------------------------------------------------------------------------------------
class RobotState : public EffectState {
  public:
    RobotState(const mixxx::EngineParameters& engineParameters);
    std::vector<float> comb[2];
    size_t at = 0;
    double phase = 0;
};

class RobotEffect : public EffectProcessorImpl<RobotState> {
  public:
    RobotEffect() = default;
    static QString getId();
    static EffectManifestPointer getManifest();
    void loadEngineEffectParameters(const QMap<QString, EngineEffectParameterPointer>& parameters) override;
    void processChannel(RobotState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }
    EngineEffectParameterPointer m_pPitch, m_pMetal;
    DISALLOW_COPY_AND_ASSIGN(RobotEffect);
};

// ---- Reverb Rise (and fall) ----------------------------------------------------------------------------------------
class ReverbRiseState : public EffectState {
  public:
    ReverbRiseState(const mixxx::EngineParameters& engineParameters);
    zydeckfx::Reverb reverb;
    zydeckfx::PitchShifter shift[2];
    double frames = 0;
};

class ReverbRiseEffect : public EffectProcessorImpl<ReverbRiseState> {
  public:
    ReverbRiseEffect() = default;
    static QString getId();
    static EffectManifestPointer getManifest();
    void loadEngineEffectParameters(const QMap<QString, EngineEffectParameterPointer>& parameters) override;
    void processChannel(ReverbRiseState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }
    EngineEffectParameterPointer m_pTime, m_pPitch, m_pSize, m_pDuck;
    DISALLOW_COPY_AND_ASSIGN(ReverbRiseEffect);
};

// ---- Noise Riser ---------------------------------------------------------------------------------------------------
class NoiseRiserState : public EffectState {
  public:
    NoiseRiserState(const mixxx::EngineParameters& engineParameters)
            : EffectState(engineParameters) {
    }
    zydeckfx::Svf filter[2];
    double frames = 0;
    unsigned int seed = 22222;
};

class NoiseRiserEffect : public EffectProcessorImpl<NoiseRiserState> {
  public:
    NoiseRiserEffect() = default;
    static QString getId();
    static EffectManifestPointer getManifest();
    void loadEngineEffectParameters(const QMap<QString, EngineEffectParameterPointer>& parameters) override;
    void processChannel(NoiseRiserState* pState,
            const CSAMPLE* pInput,
            CSAMPLE* pOutput,
            const mixxx::EngineParameters& engineParameters,
            const EffectEnableState enableState,
            const GroupFeatureState& groupFeatures) override;

  private:
    QString debugString() const {
        return getId();
    }
    EngineEffectParameterPointer m_pTime, m_pLevel, m_pFall;
    DISALLOW_COPY_AND_ASSIGN(NoiseRiserEffect);
};
