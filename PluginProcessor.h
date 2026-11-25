#pragma once
#include <JuceHeader.h>

struct WordEffectSettings {
    // Doubler
    bool doublerActive = false;
    float doublerAmount = 0.7f;
    float doublerDelayMs = 10.0f;
    float doublerDetuneCents = 16.0f;

    // Delay
    bool delayActive = false;
    float delayAmount = 0.55f;
    float delayTimeMs = 345.0f;
    float delayFeedback = 0.60f;
    bool pingPong = true;

    // Reverb
    bool reverbActive = false;
    float reverbAmount = 0.40f;
    float reverbDecaySec = 3.8f;
    float reverbWidth = 0.60f;

    // Distortion
    bool distortionActive = false;
    float drive = 0.0f;

    // Pitch / Harmony
    bool pitchActive = false;
    int semitoneShift = 0;
};

struct LyricWord {
    juce::String word;
    int64_t startSample = 0;
    int64_t endSample = 0;
    bool isSelected = false;
    WordEffectSettings fx;
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

    // Transcribed lyrics storage
    std::vector<LyricWord> words;
    bool muteDry = false;

private:
    double currentSampleRate = 44100.0;

    // DSP Engines
    juce::dsp::Reverb reverb;
    juce::dsp::Reverb::Parameters reverbParams;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayL { 192000 };
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayR { 192000 };
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> doublerL { 48000 };
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> doublerR { 48000 };
    juce::dsp::WaveShaper<float> distortion;

    // Smoothers to avoid audio clicks when words trigger FX
    juce::SmoothedValue<float> doublerSendGain { 0.0f };
    juce::SmoothedValue<float> delaySendGain   { 0.0f };
    juce::SmoothedValue<float> reverbSendGain  { 0.0f };
    juce::SmoothedValue<float> distSendGain    { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LyricMomentsProcessor)
};
