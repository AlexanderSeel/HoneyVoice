#pragma once
#include <JuceHeader.h>
#include "Source/WordModels.h"
#include "Source/AudioTimeline.h"
#include "Source/PitchShifter.h"
#include "Source/VocalTranscriber.h"
#include <atomic>
#include <vector>

struct PreverbSlice {
    int64_t startSample = 0;
    int64_t endSample = 0;
    float amount = 0.55f;
    juce::AudioBuffer<float> stereoAudio;
};

class LyricMomentsProcessor : public juce::AudioProcessor {
public:
    LyricMomentsProcessor();
    ~LyricMomentsProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "HoneyVoice"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // Thread-safe lyric & FX word accessors
    std::vector<LyricWord> getWordsCopy() const;
    void setWords(std::vector<LyricWord> newWords);
    int getWordCount() const;
    void updateWordFxByIndex(int index, const WordEffectSettings& fx);
    void setWordSelection(int index, bool selected);
    void selectWordRange(int fromIndex, int toIndex);
    void clearAllSelections();
    void updateSearchHighlights(const juce::String& query);

    // Pre-renders reverse-reverb preverb slices from AudioTimeline
    void rebuildPreverbCache();

    // Audio timeline & standalone transport controls
    AudioTimeline& getTimeline() noexcept { return timeline; }
    const AudioTimeline& getTimeline() const noexcept { return timeline; }

    void setMuteDry(bool shouldMute) noexcept { muteDry.store(shouldMute, std::memory_order_relaxed); }
    bool getMuteDry() const noexcept { return muteDry.load(std::memory_order_relaxed); }

    void setStandalonePlaying(bool shouldPlay);
    bool isStandalonePlaying() const noexcept { return standalonePlaying.load(std::memory_order_relaxed); }
    void setStandaloneLoop(bool shouldLoop) noexcept { standaloneLoop.store(shouldLoop, std::memory_order_relaxed); }
    bool isStandaloneLoop() const noexcept { return standaloneLoop.load(std::memory_order_relaxed); }
    void seekStandaloneSample(int64_t samplePos) noexcept { standalonePlayheadSample.store(juce::jmax<int64_t>(0, samplePos), std::memory_order_relaxed); }

    int64_t getCurrentPlayheadSample() const noexcept { return lastPlayheadSample.load(std::memory_order_relaxed); }
    bool isTransportPlaying() const noexcept { return lastIsPlaying.load(std::memory_order_relaxed); }
    int getActiveWordIndex() const noexcept { return activeWordIndex.load(std::memory_order_relaxed); }
    double getCurrentSampleRate() const noexcept { return currentSampleRate; }

    // Async transcription status shared with editor
    void setLastTranscriptionStatus(const juce::String& status);
    juce::String getLastTranscriptionStatus() const;

private:
    AudioTimeline timeline;

    mutable juce::SpinLock wordsLock;
    std::vector<LyricWord> words;

    mutable juce::SpinLock preverbLock;
    std::vector<PreverbSlice> preverbSlices;

    mutable juce::SpinLock statusLock;
    juce::String lastTranscriptionStatus { "Play DAW audio or load a vocal file, then click Transcribe." };

    std::atomic<bool> muteDry { false };
    std::atomic<bool> standalonePlaying { false };
    std::atomic<bool> standaloneLoop { true };
    std::atomic<int64_t> standalonePlayheadSample { 0 };
    std::atomic<int64_t> lastPlayheadSample { 0 };
    std::atomic<bool> lastIsPlaying { false };
    std::atomic<int> activeWordIndex { -1 };

    double currentSampleRate = 44100.0;

    // DSP Engines
    juce::dsp::Reverb reverb;
    juce::dsp::Reverb::Parameters reverbParams;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayL { 192000 };
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayR { 192000 };
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> doublerL { 48000 };
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> doublerR { 48000 };

    StereoPitchShifter mainPitchShifter;
    StereoPitchShifter harmonyShifter1;
    StereoPitchShifter harmonyShifter2;

    // Distortion 1-pole tone filter states
    float distLpL = 0.0f;
    float distLpR = 0.0f;

    // Doubler modulation LFO phase
    double doublerLfoPhase = 0.0;

    // Latched tail settings so delay/reverb tails decay naturally after word ends
    WordEffectSettings latchedDelay;
    WordEffectSettings latchedReverb;
    WordEffectSettings latchedDistortion;
    WordEffectSettings latchedHarmony;
    WordEffectSettings latchedPitch;

    // Smoothers
    juce::SmoothedValue<float> doublerSendGain { 0.0f };
    juce::SmoothedValue<float> delaySendGain   { 0.0f };
    juce::SmoothedValue<float> reverbSendGain  { 0.0f };
    juce::SmoothedValue<float> distSendGain    { 0.0f };
    juce::SmoothedValue<float> harmonySendGain { 0.0f };
    juce::SmoothedValue<float> pitchSendGain   { 0.0f };
    juce::SmoothedValue<float> smoothedDelaySamples { 15214.5f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LyricMomentsProcessor)
};
