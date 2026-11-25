#include "PluginProcessor.h"
#include "PluginEditor.h"

LyricMomentsProcessor::LyricMomentsProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    distortion.functionToUse = [](float x) { return std::tanh(x * 4.0f); };
}

void LyricMomentsProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    currentSampleRate = sampleRate;

    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32)samplesPerBlock, 2 };
    reverb.prepare(spec);
    delayL.prepare(spec);
    delayR.prepare(spec);
    doublerL.prepare(spec);
    doublerR.prepare(spec);
    distortion.prepare(spec);

    // 15ms crossfade ramp so FX throws enter/exit smoothly without pops
    doublerSendGain.reset(sampleRate, 0.015);
    delaySendGain.reset(sampleRate, 0.015);
    reverbSendGain.reset(sampleRate, 0.015);
    distSendGain.reset(sampleRate, 0.015);
}

void LyricMomentsProcessor::releaseResources() {}

void LyricMomentsProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();

    // Get current DAW playhead position in samples
    int64_t currentSamplePos = 0;
    bool isPlaying = false;
    if (auto* playHead = getPlayHead()) {
        if (auto pos = playHead->getPosition()) {
            if (pos->getTimeInSamples().hasValue())
                currentSamplePos = *pos->getTimeInSamples();
            isPlaying = pos->getIsPlaying();
        }
    }

    // Determine if playhead is currently inside an active word
    WordEffectSettings activeSettings;
    if (isPlaying) {
        for (const auto& w : words) {
            if (currentSamplePos >= w.startSample && currentSamplePos <= w.endSample) {
                activeSettings = w.fx;
                break;
            }
        }
    }

    // Set target send gains
    doublerSendGain.setTargetValue(activeSettings.doublerActive ? activeSettings.doublerAmount : 0.0f);
    delaySendGain.setTargetValue(activeSettings.delayActive ? activeSettings.delayAmount : 0.0f);
    reverbSendGain.setTargetValue(activeSettings.reverbActive ? activeSettings.reverbAmount : 0.0f);
    distSendGain.setTargetValue(activeSettings.distortionActive ? 1.0f : 0.0f);

    // Update Reverb parameters
    reverbParams.roomSize = juce::jlimit(0.1f, 0.99f, activeSettings.reverbDecaySec / 5.0f);
    reverbParams.width = activeSettings.reverbWidth;
    reverbParams.wetLevel = 1.0f;
    reverbParams.dryLevel = 0.0f;
    reverb.setParameters(reverbParams);

    juce::AudioBuffer<float> doublerSendBuf(2, numSamples);
    juce::AudioBuffer<float> delaySendBuf(2, numSamples);
    juce::AudioBuffer<float> reverbSendBuf(2, numSamples);
    doublerSendBuf.clear();
    delaySendBuf.clear();
    reverbSendBuf.clear();

    auto* inL = buffer.getReadPointer(0);
    auto* inR = buffer.getNumChannels() > 1 ? buffer.getReadPointer(1) : inL;

    float delaySamples = (activeSettings.delayTimeMs / 1000.0f) * (float)currentSampleRate;
    delayL.setDelay(delaySamples);
    delayR.setDelay(delaySamples);

    float doublerSamplesL = (activeSettings.doublerDelayMs / 1000.0f) * (float)currentSampleRate;
    float doublerSamplesR = ((activeSettings.doublerDelayMs + 7.5f) / 1000.0f) * (float)currentSampleRate;
    doublerL.setDelay(doublerSamplesL);
    doublerR.setDelay(doublerSamplesR);

    for (int i = 0; i < numSamples; ++i) {
        float dblGain = doublerSendGain.getNextValue();
        float dGain = delaySendGain.getNextValue();
        float rGain = reverbSendGain.getNextValue();
        float distGain = distSendGain.getNextValue();

        float dryL = inL[i];
        float dryR = inR[i];

        if (distGain > 0.001f) {
            dryL = juce::jmap(distGain, dryL, distortion.processSample(dryL));
            dryR = juce::jmap(distGain, dryR, distortion.processSample(dryR));
        }

        // Stereo Doubler
        doublerL.pushSample(0, dryL);
        doublerR.pushSample(1, dryR);
        doublerSendBuf.setSample(0, i, doublerL.popSample(0) * dblGain);
        doublerSendBuf.setSample(1, i, doublerR.popSample(1) * dblGain);

        // Feed input into delay send ONLY during highlighted words
        float sendInL = dryL * dGain;
        float sendInR = dryR * dGain;

        float delayedL = delayL.popSample(0);
        float delayedR = delayR.popSample(1);

        if (activeSettings.pingPong) {
            delayL.pushSample(0, sendInL + delayedR * activeSettings.delayFeedback);
            delayR.pushSample(1, sendInR + delayedL * activeSettings.delayFeedback);
        } else {
            delayL.pushSample(0, sendInL + delayedL * activeSettings.delayFeedback);
            delayR.pushSample(1, sendInR + delayedR * activeSettings.delayFeedback);
        }

        delaySendBuf.setSample(0, i, delayedL);
        delaySendBuf.setSample(1, i, delayedR);

        reverbSendBuf.setSample(0, i, dryL * rGain);
        reverbSendBuf.setSample(1, i, dryR * rGain);
    }

    // Process Reverb tail
    juce::dsp::AudioBlock<float> reverbBlock(reverbSendBuf);
    reverb.process(juce::dsp::ProcessContextReplacing<float>(reverbBlock));

    // Mix Dry + FX Throws back to output
    if (muteDry) {
        buffer.clear();
    }

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        buffer.addFrom(ch, 0, doublerSendBuf, ch, 0, numSamples);
        buffer.addFrom(ch, 0, delaySendBuf, ch, 0, numSamples);
        buffer.addFrom(ch, 0, reverbSendBuf, ch, 0, numSamples);
    }
}

juce::AudioProcessorEditor* LyricMomentsProcessor::createEditor() {
    return new LyricMomentsEditor(*this);
}

void LyricMomentsProcessor::getStateInformation(juce::MemoryBlock& destData) {
    juce::ValueTree state("HoneyVoiceState");
    state.setProperty("muteDry", muteDry, nullptr);
    juce::MemoryOutputStream stream(destData, true);
    state.writeToStream(stream);
}

void LyricMomentsProcessor::setStateInformation(const void* data, int sizeInBytes) {
    auto state = juce::ValueTree::readFromData(data, (size_t)sizeInBytes);
    if (state.isValid()) {
        muteDry = state.getProperty("muteDry", false);
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new LyricMomentsProcessor();
}
