#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

LyricMomentsProcessor::LyricMomentsProcessor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
}

void LyricMomentsProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    currentSampleRate = juce::jmax(8000.0, sampleRate);
    timeline.prepare(currentSampleRate);

    juce::dsp::ProcessSpec spec { currentSampleRate, (juce::uint32)juce::jmax(1, samplesPerBlock), 2 };
    reverb.prepare(spec);
    delayL.prepare(spec);
    delayR.prepare(spec);
    doublerL.prepare(spec);
    doublerR.prepare(spec);

    mainPitchShifter.prepare(currentSampleRate);
    harmonyShifter1.prepare(currentSampleRate);
    harmonyShifter2.prepare(currentSampleRate);

    distLpL = 0.0f;
    distLpR = 0.0f;
    doublerLfoPhase = 0.0;

    // 15ms crossfade ramp so FX throws enter/exit smoothly without clicks
    doublerSendGain.reset(currentSampleRate, 0.015);
    delaySendGain.reset(currentSampleRate, 0.015);
    reverbSendGain.reset(currentSampleRate, 0.015);
    distSendGain.reset(currentSampleRate, 0.015);
    harmonySendGain.reset(currentSampleRate, 0.015);
    pitchSendGain.reset(currentSampleRate, 0.015);
    smoothedDelaySamples.reset(currentSampleRate, 0.025);
    smoothedDelaySamples.setCurrentAndTargetValue((latchedDelay.delayTimeMs / 1000.0f) * (float)currentSampleRate);

    reverbParams.roomSize = juce::jlimit(0.1f, 0.99f, latchedReverb.reverbDecaySec / 6.0f);
    reverbParams.damping = 0.35f;
    reverbParams.width = latchedReverb.reverbWidth;
    reverbParams.wetLevel = 1.0f;
    reverbParams.dryLevel = 0.0f;
    reverb.setParameters(reverbParams);

    rebuildPreverbCache();
}

void LyricMomentsProcessor::releaseResources() {}

std::vector<LyricWord> LyricMomentsProcessor::getWordsCopy() const {
    const juce::SpinLock::ScopedLockType guard(wordsLock);
    return words;
}

void LyricMomentsProcessor::setWords(std::vector<LyricWord> newWords) {
    {
        const juce::SpinLock::ScopedLockType guard(wordsLock);
        words = std::move(newWords);
    }
    rebuildPreverbCache();
}

int LyricMomentsProcessor::getWordCount() const {
    const juce::SpinLock::ScopedLockType guard(wordsLock);
    return (int)words.size();
}

void LyricMomentsProcessor::updateWordFxByIndex(int index, const WordEffectSettings& fx) {
    {
        const juce::SpinLock::ScopedLockType guard(wordsLock);
        if (index < 0 || index >= (int)words.size())
            return;
        words[(size_t)index].fx = fx;
    }
    rebuildPreverbCache();
}

void LyricMomentsProcessor::setWordSelection(int index, bool selected) {
    const juce::SpinLock::ScopedLockType guard(wordsLock);
    if (index >= 0 && index < (int)words.size())
        words[(size_t)index].isSelected = selected;
}

void LyricMomentsProcessor::selectWordRange(int fromIndex, int toIndex) {
    const juce::SpinLock::ScopedLockType guard(wordsLock);
    if (words.empty()) return;
    const int lo = juce::jlimit(0, (int)words.size() - 1, juce::jmin(fromIndex, toIndex));
    const int hi = juce::jlimit(0, (int)words.size() - 1, juce::jmax(fromIndex, toIndex));
    for (int i = lo; i <= hi; ++i)
        words[(size_t)i].isSelected = true;
}

void LyricMomentsProcessor::clearAllSelections() {
    const juce::SpinLock::ScopedLockType guard(wordsLock);
    for (auto& w : words)
        w.isSelected = false;
}

void LyricMomentsProcessor::updateSearchHighlights(const juce::String& query) {
    const auto q = query.trim().toLowerCase();
    const juce::SpinLock::ScopedLockType guard(wordsLock);
    for (auto& w : words)
        w.isSearchMatch = q.isNotEmpty() && w.word.toLowerCase().contains(q);
}

void LyricMomentsProcessor::setStandalonePlaying(bool shouldPlay) {
    if (shouldPlay && timeline.hasAudio()) {
        const int64_t pos = standalonePlayheadSample.load(std::memory_order_relaxed);
        const int64_t vStart = timeline.getValidStartSample();
        const int64_t vEnd = timeline.getValidEndSample();
        if (pos < vStart || pos >= vEnd)
            standalonePlayheadSample.store(vStart, std::memory_order_relaxed);
    }
    standalonePlaying.store(shouldPlay, std::memory_order_relaxed);
}

void LyricMomentsProcessor::setLastTranscriptionStatus(const juce::String& status) {
    const juce::SpinLock::ScopedLockType guard(statusLock);
    lastTranscriptionStatus = status;
}

juce::String LyricMomentsProcessor::getLastTranscriptionStatus() const {
    const juce::SpinLock::ScopedLockType guard(statusLock);
    return lastTranscriptionStatus;
}

void LyricMomentsProcessor::rebuildPreverbCache() {
    const auto snapshot = getWordsCopy();
    if (!timeline.hasAudio()) {
        const juce::SpinLock::ScopedLockType guard(preverbLock);
        preverbSlices.clear();
        return;
    }

    std::vector<PreverbSlice> rendered;
    const double sr = currentSampleRate;

    for (const auto& w : snapshot) {
        if (!w.fx.preverbActive || w.endSample <= w.startSample)
            continue;

        const int64_t wordLen = juce::jmin<int64_t>(w.endSample - w.startSample, (int64_t)(sr * 4.0));
        const int64_t preLen = juce::jlimit<int64_t>(64, (int64_t)(sr * 2.5),
                                                     (int64_t)std::llround((w.fx.preverbLengthMs / 1000.0) * sr));
        if (wordLen <= 0 || preLen <= 0)
            continue;

        auto wordMono = timeline.copyMonoRange(w.startSample, wordLen);
        if (wordMono.empty())
            continue;

        // Build reversed buffer: [reversed word] followed by [preLen silence for reverb tail]
        const int totalLen = (int)(wordLen + preLen);
        juce::AudioBuffer<float> revBuf(2, totalLen);
        revBuf.clear();
        for (int64_t i = 0; i < wordLen; ++i) {
            const float s = wordMono[(size_t)(wordLen - 1 - i)];
            revBuf.setSample(0, (int)i, s);
            revBuf.setSample(1, (int)i, s);
        }

        juce::dsp::Reverb preRev;
        preRev.prepare({ sr, (juce::uint32)totalLen, 2 });
        juce::dsp::Reverb::Parameters params;
        params.roomSize = juce::jlimit(0.2f, 0.98f, w.fx.preverbDecaySec / 5.0f);
        params.damping = 0.25f;
        params.width = 0.85f;
        params.wetLevel = 1.0f;
        params.dryLevel = 0.0f;
        preRev.setParameters(params);

        juce::dsp::AudioBlock<float> block(revBuf);
        preRev.process(juce::dsp::ProcessContextReplacing<float>(block));

        // Reverse the tail portion back so it swells UP to w.startSample
        PreverbSlice slice;
        slice.startSample = w.startSample - preLen;
        slice.endSample = w.startSample;
        slice.amount = w.fx.preverbAmount;
        slice.stereoAudio.setSize(2, (int)preLen);

        const int fadeInLen = juce::jmin((int)preLen / 3, (int)(sr * 0.04));
        const int fadeOutLen = juce::jmin((int)preLen / 6, (int)(sr * 0.012));

        for (int ch = 0; ch < 2; ++ch) {
            for (int i = 0; i < (int)preLen; ++i) {
                float val = revBuf.getSample(ch, totalLen - 1 - i);
                if (i < fadeInLen)
                    val *= (float)i / (float)juce::jmax(1, fadeInLen);
                if (i >= (int)preLen - fadeOutLen)
                    val *= (float)((int)preLen - 1 - i) / (float)juce::jmax(1, fadeOutLen);
                slice.stereoAudio.setSample(ch, i, val);
            }
        }
        rendered.push_back(std::move(slice));
    }

    const juce::SpinLock::ScopedLockType guard(preverbLock);
    preverbSlices = std::move(rendered);
}

void LyricMomentsProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    // 1. Determine DAW vs Standalone transport position
    int64_t currentSamplePos = 0;
    bool dawPlaying = false;
    bool hasDawSamplePos = false;
    if (auto* ph = getPlayHead()) {
        if (auto pos = ph->getPosition()) {
            dawPlaying = pos->getIsPlaying();
            if (pos->getTimeInSamples().hasValue()) {
                currentSamplePos = *pos->getTimeInSamples();
                hasDawSamplePos = true;
            }
        }
    }

    const bool previewPlaying = !dawPlaying && standalonePlaying.load(std::memory_order_relaxed);
    bool isPlaying = dawPlaying || previewPlaying;

    if (dawPlaying && hasDawSamplePos) {
        // Check if input has non-trivial signal and capture DAW audio for waveform & transcription
        float peak = 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
            const float* ptr = buffer.getReadPointer(ch);
            for (int i = 0; i < numSamples; ++i)
                peak = juce::jmax(peak, std::abs(ptr[i]));
        }
        timeline.captureBlock(buffer, currentSamplePos, peak > 1e-4f);
    } else if (previewPlaying) {
        currentSamplePos = standalonePlayheadSample.load(std::memory_order_relaxed);
        const bool loop = standaloneLoop.load(std::memory_order_relaxed);
        const int64_t vStart = timeline.getValidStartSample();
        const int64_t vEnd = timeline.getValidEndSample();
        buffer.clear();
        if (vEnd > vStart && timeline.readPlaybackBlock(currentSamplePos, buffer, loop)) {
            int64_t nextPos = currentSamplePos + numSamples;
            if (nextPos >= vEnd) {
                if (loop) {
                    nextPos = vStart + ((nextPos - vStart) % (vEnd - vStart));
                } else {
                    nextPos = vStart;
                    standalonePlaying.store(false, std::memory_order_relaxed);
                }
            }
            standalonePlayheadSample.store(nextPos, std::memory_order_relaxed);
        } else {
            standalonePlaying.store(false, std::memory_order_relaxed);
            isPlaying = false;
        }
    }

    lastPlayheadSample.store(currentSamplePos, std::memory_order_relaxed);
    lastIsPlaying.store(isPlaying, std::memory_order_relaxed);

    // 2. Match current playhead against transcribed words (lock-free try-lock)
    WordEffectSettings activeSettings;
    int matchedWordIdx = -1;
    if (isPlaying) {
        const juce::SpinLock::ScopedTryLockType guard(wordsLock);
        if (guard.isLocked()) {
            const int64_t blockMidSample = currentSamplePos + numSamples / 2;
            for (size_t idx = 0; idx < words.size(); ++idx) {
                const auto& w = words[idx];
                if (blockMidSample >= w.startSample && blockMidSample <= w.endSample) {
                    activeSettings = w.fx;
                    matchedWordIdx = (int)idx;
                    break;
                }
            }
        }
    }
    activeWordIndex.store(matchedWordIdx, std::memory_order_relaxed);

    // Latch tail-bearing effect parameters so tails decay smoothly after a word ends
    if (activeSettings.delayActive)      latchedDelay = activeSettings;
    if (activeSettings.reverbActive)     latchedReverb = activeSettings;
    if (activeSettings.distortionActive) latchedDistortion = activeSettings;
    if (activeSettings.harmonyActive)    latchedHarmony = activeSettings;
    if (activeSettings.pitchActive)      latchedPitch = activeSettings;

    // 3. Update target smoothers
    doublerSendGain.setTargetValue(activeSettings.doublerActive ? activeSettings.doublerAmount : 0.0f);
    delaySendGain.setTargetValue(activeSettings.delayActive ? activeSettings.delayAmount : 0.0f);
    reverbSendGain.setTargetValue(activeSettings.reverbActive ? activeSettings.reverbAmount : 0.0f);
    distSendGain.setTargetValue(activeSettings.distortionActive ? activeSettings.distortionAmount : 0.0f);
    harmonySendGain.setTargetValue(activeSettings.harmonyActive ? activeSettings.harmonyAmount : 0.0f);
    pitchSendGain.setTargetValue(activeSettings.pitchActive ? activeSettings.pitchAmount : 0.0f);

    const float targetDelaySamples = juce::jlimit(1.0f, 190000.0f,
                                                  (latchedDelay.delayTimeMs / 1000.0f) * (float)currentSampleRate);
    smoothedDelaySamples.setTargetValue(targetDelaySamples);

    const float targetRoom = juce::jlimit(0.1f, 0.99f, latchedReverb.reverbDecaySec / 6.0f);
    const float targetWidth = juce::jlimit(0.0f, 1.0f, latchedReverb.reverbWidth);
    if (std::abs(reverbParams.roomSize - targetRoom) > 0.002f || std::abs(reverbParams.width - targetWidth) > 0.002f) {
        reverbParams.roomSize = targetRoom;
        reverbParams.width = targetWidth;
        reverb.setParameters(reverbParams);
    }

    juce::AudioBuffer<float> doublerSendBuf(2, numSamples);
    juce::AudioBuffer<float> delaySendBuf(2, numSamples);
    juce::AudioBuffer<float> reverbSendBuf(2, numSamples);
    juce::AudioBuffer<float> harmonySendBuf(2, numSamples);
    juce::AudioBuffer<float> pitchedDryBuf(2, numSamples);

    doublerSendBuf.clear();
    delaySendBuf.clear();
    reverbSendBuf.clear();
    harmonySendBuf.clear();

    const float* inL = buffer.getReadPointer(0);
    const float* inR = buffer.getNumChannels() > 1 ? buffer.getReadPointer(1) : inL;

    const double lfoInc = 1.35 / currentSampleRate; // 1.35 Hz doubler detune LFO
    const float baseDoublerSamplesL = (activeSettings.doublerDelayMs / 1000.0f) * (float)currentSampleRate;
    const float baseDoublerSamplesR = ((activeSettings.doublerDelayMs + 7.5f) / 1000.0f) * (float)currentSampleRate;
    const float detuneDepthSamples = (activeSettings.doublerDetuneCents / 100.0f) * 0.0022f * (float)currentSampleRate;

    const float distDriveGain = 1.0f + latchedDistortion.drive * 18.0f;
    const float distNorm = 1.0f / std::tanh(distDriveGain);
    const float toneCoeff = juce::jlimit(0.05f, 0.98f, 0.08f + latchedDistortion.distortionTone * 0.90f);

    const float harmonyPan = juce::jlimit(0.0f, 1.0f, latchedHarmony.harmonyWidth);
    const float gainV1L = 0.5f + 0.45f * harmonyPan;
    const float gainV1R = 0.5f - 0.45f * harmonyPan;
    const float gainV2L = 0.5f - 0.45f * harmonyPan;
    const float gainV2R = 0.5f + 0.45f * harmonyPan;

    for (int i = 0; i < numSamples; ++i) {
        const float dblGain  = doublerSendGain.getNextValue();
        const float dGain    = delaySendGain.getNextValue();
        const float rGain    = reverbSendGain.getNextValue();
        const float distGain = distSendGain.getNextValue();
        const float harmGain = harmonySendGain.getNextValue();
        const float pGain    = pitchSendGain.getNextValue();
        const float dSamples = smoothedDelaySamples.getNextValue();

        delayL.setDelay(dSamples);
        delayR.setDelay(dSamples);

        float dryL = inL[i];
        float dryR = inR[i];

        // Distortion (soft asymmetric waveshaper + 1-pole tone + level compensation)
        if (distGain > 0.0005f) {
            const float shapedL = std::tanh(dryL * distDriveGain + 0.05f * distDriveGain * dryL * dryL) * distNorm;
            const float shapedR = std::tanh(dryR * distDriveGain + 0.05f * distDriveGain * dryR * dryR) * distNorm;
            distLpL += toneCoeff * (shapedL - distLpL);
            distLpR += toneCoeff * (shapedR - distLpR);
            dryL = juce::jmap(distGain, dryL, distLpL);
            dryR = juce::jmap(distGain, dryR, distLpR);
        }

        // Pitch Shift (replaces/crossfades dry when active)
        if (pGain > 0.0005f && latchedPitch.semitoneShift != 0) {
            float shL = dryL, shR = dryR;
            mainPitchShifter.processSample(dryL, dryR, latchedPitch.semitoneShift, shL, shR);
            dryL = juce::jmap(pGain, dryL, shL);
            dryR = juce::jmap(pGain, dryR, shR);
        } else {
            mainPitchShifter.pushOnly(dryL, dryR);
        }

        pitchedDryBuf.setSample(0, i, dryL);
        pitchedDryBuf.setSample(1, i, dryR);

        // Harmony Voices (1 or 2 pitch-shifted voices mixed on top)
        if (harmGain > 0.0005f) {
            float h1L = 0.0f, h1R = 0.0f;
            harmonyShifter1.processSample(dryL, dryR, latchedHarmony.harmonyInterval1, h1L, h1R);

            float mixL = h1L * gainV1L;
            float mixR = h1R * gainV1R;

            if (latchedHarmony.harmonyInterval2 != 0) {
                float h2L = 0.0f, h2R = 0.0f;
                harmonyShifter2.processSample(dryL, dryR, latchedHarmony.harmonyInterval2, h2L, h2R);
                mixL = 0.65f * (mixL + h2L * gainV2L);
                mixR = 0.65f * (mixR + h2R * gainV2R);
            } else {
                harmonyShifter2.pushOnly(dryL, dryR);
            }

            harmonySendBuf.setSample(0, i, mixL * harmGain);
            harmonySendBuf.setSample(1, i, mixR * harmGain);
        } else {
            harmonyShifter1.pushOnly(dryL, dryR);
            harmonyShifter2.pushOnly(dryL, dryR);
        }

        // Stereo Detuned Doubler (quadrature LFO modulated delay)
        const float lfoL = (float)std::sin(2.0 * juce::MathConstants<double>::pi * doublerLfoPhase);
        const float lfoR = (float)std::cos(2.0 * juce::MathConstants<double>::pi * doublerLfoPhase);
        doublerLfoPhase += lfoInc;
        if (doublerLfoPhase >= 1.0) doublerLfoPhase -= 1.0;

        doublerL.setDelay(juce::jlimit(4.0f, 47000.0f, baseDoublerSamplesL + lfoL * detuneDepthSamples));
        doublerR.setDelay(juce::jlimit(4.0f, 47000.0f, baseDoublerSamplesR + lfoR * detuneDepthSamples));

        doublerL.pushSample(0, dryL);
        doublerR.pushSample(1, dryR);
        doublerSendBuf.setSample(0, i, doublerL.popSample(0) * dblGain);
        doublerSendBuf.setSample(1, i, doublerR.popSample(1) * dblGain);

        // Stereo / Ping-Pong Delay Send (gated by word send gain, tail rings out via latched feedback)
        const float sendInL = dryL * dGain;
        const float sendInR = dryR * dGain;
        const float delayedL = delayL.popSample(0);
        const float delayedR = delayR.popSample(1);
        const float fb = juce::jlimit(0.0f, 0.95f, latchedDelay.delayFeedback);

        if (latchedDelay.pingPong) {
            delayL.pushSample(0, sendInL + delayedR * fb);
            delayR.pushSample(1, sendInR + delayedL * fb);
        } else {
            delayL.pushSample(0, sendInL + delayedL * fb);
            delayR.pushSample(1, sendInR + delayedR * fb);
        }

        delaySendBuf.setSample(0, i, delayedL);
        delaySendBuf.setSample(1, i, delayedR);

        reverbSendBuf.setSample(0, i, dryL * rGain);
        reverbSendBuf.setSample(1, i, dryR * rGain);
    }

    // 4. Process Reverb Tail
    juce::dsp::AudioBlock<float> reverbBlock(reverbSendBuf);
    reverb.process(juce::dsp::ProcessContextReplacing<float>(reverbBlock));

    // 5. Output Mix (Dry + Doubler + Delay + Reverb + Harmony + Preverb)
    if (muteDry.load(std::memory_order_relaxed)) {
        buffer.clear();
    } else {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.copyFrom(ch, 0, pitchedDryBuf, juce::jmin(ch, 1), 0, numSamples);
    }

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        const int srcCh = juce::jmin(ch, 1);
        buffer.addFrom(ch, 0, doublerSendBuf, srcCh, 0, numSamples);
        buffer.addFrom(ch, 0, delaySendBuf, srcCh, 0, numSamples);
        buffer.addFrom(ch, 0, reverbSendBuf, srcCh, 0, numSamples);
        buffer.addFrom(ch, 0, harmonySendBuf, srcCh, 0, numSamples);
    }

    // Add pre-rendered reverse-reverb Preverb swells when approaching target words
    if (isPlaying) {
        const juce::SpinLock::ScopedTryLockType pGuard(preverbLock);
        if (pGuard.isLocked()) {
            const int64_t blockEnd = currentSamplePos + numSamples;
            for (const auto& slice : preverbSlices) {
                if (slice.endSample <= currentSamplePos || slice.startSample >= blockEnd)
                    continue;
                const int64_t ovStart = juce::jmax(currentSamplePos, slice.startSample);
                const int64_t ovEnd = juce::jmin(blockEnd, slice.endSample);
                const int count = (int)(ovEnd - ovStart);
                if (count <= 0) continue;

                const int dstStart = (int)(ovStart - currentSamplePos);
                const int srcStart = (int)(ovStart - slice.startSample);
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
                    const int srcCh = juce::jmin(ch, slice.stereoAudio.getNumChannels() - 1);
                    buffer.addFrom(ch, dstStart, slice.stereoAudio, srcCh, srcStart, count, slice.amount);
                }
            }
        }
    }
}

juce::AudioProcessorEditor* LyricMomentsProcessor::createEditor() {
    return new LyricMomentsEditor(*this);
}

void LyricMomentsProcessor::getStateInformation(juce::MemoryBlock& destData) {
    juce::ValueTree state("HoneyVoiceState");
    state.setProperty("version", 2, nullptr);
    state.setProperty("muteDry", muteDry.load(std::memory_order_relaxed), nullptr);
    state.setProperty("standaloneLoop", standaloneLoop.load(std::memory_order_relaxed), nullptr);

    const auto snapshot = getWordsCopy();
    juce::ValueTree wordsTree("Words");
    for (const auto& w : snapshot) {
        juce::ValueTree wt("Word");
        wt.setProperty("text", w.word, nullptr);
        wt.setProperty("startSample", (juce::int64)w.startSample, nullptr);
        wt.setProperty("endSample", (juce::int64)w.endSample, nullptr);
        wt.setProperty("selected", w.isSelected, nullptr);

        wt.setProperty("doublerActive", w.fx.doublerActive, nullptr);
        wt.setProperty("doublerAmount", w.fx.doublerAmount, nullptr);
        wt.setProperty("doublerDelayMs", w.fx.doublerDelayMs, nullptr);
        wt.setProperty("doublerDetuneCents", w.fx.doublerDetuneCents, nullptr);

        wt.setProperty("delayActive", w.fx.delayActive, nullptr);
        wt.setProperty("delayAmount", w.fx.delayAmount, nullptr);
        wt.setProperty("delayTimeMs", w.fx.delayTimeMs, nullptr);
        wt.setProperty("delayFeedback", w.fx.delayFeedback, nullptr);
        wt.setProperty("pingPong", w.fx.pingPong, nullptr);

        wt.setProperty("reverbActive", w.fx.reverbActive, nullptr);
        wt.setProperty("reverbAmount", w.fx.reverbAmount, nullptr);
        wt.setProperty("reverbDecaySec", w.fx.reverbDecaySec, nullptr);
        wt.setProperty("reverbWidth", w.fx.reverbWidth, nullptr);

        wt.setProperty("preverbActive", w.fx.preverbActive, nullptr);
        wt.setProperty("preverbAmount", w.fx.preverbAmount, nullptr);
        wt.setProperty("preverbLengthMs", w.fx.preverbLengthMs, nullptr);
        wt.setProperty("preverbDecaySec", w.fx.preverbDecaySec, nullptr);

        wt.setProperty("distortionActive", w.fx.distortionActive, nullptr);
        wt.setProperty("distortionAmount", w.fx.distortionAmount, nullptr);
        wt.setProperty("drive", w.fx.drive, nullptr);
        wt.setProperty("distortionTone", w.fx.distortionTone, nullptr);

        wt.setProperty("harmonyActive", w.fx.harmonyActive, nullptr);
        wt.setProperty("harmonyAmount", w.fx.harmonyAmount, nullptr);
        wt.setProperty("harmonyInterval1", w.fx.harmonyInterval1, nullptr);
        wt.setProperty("harmonyInterval2", w.fx.harmonyInterval2, nullptr);
        wt.setProperty("harmonyWidth", w.fx.harmonyWidth, nullptr);

        wt.setProperty("pitchActive", w.fx.pitchActive, nullptr);
        wt.setProperty("pitchAmount", w.fx.pitchAmount, nullptr);
        wt.setProperty("semitoneShift", w.fx.semitoneShift, nullptr);

        wordsTree.addChild(wt, -1, nullptr);
    }
    state.addChild(wordsTree, -1, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void LyricMomentsProcessor::setStateInformation(const void* data, int sizeInBytes) {
    juce::ValueTree state;
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        state = juce::ValueTree::fromXml(*xml);
    else
        state = juce::ValueTree::readFromData(data, (size_t)sizeInBytes);

    if (!state.isValid() || !state.hasType("HoneyVoiceState"))
        return;

    muteDry.store((bool)state.getProperty("muteDry", false), std::memory_order_relaxed);
    standaloneLoop.store((bool)state.getProperty("standaloneLoop", true), std::memory_order_relaxed);

    auto wordsTree = state.getChildWithName("Words");
    if (wordsTree.isValid()) {
        std::vector<LyricWord> loaded;
        loaded.reserve((size_t)wordsTree.getNumChildren());
        for (int i = 0; i < wordsTree.getNumChildren(); ++i) {
            const auto wt = wordsTree.getChild(i);
            LyricWord w;
            w.word = wt.getProperty("text").toString();
            w.startSample = (int64_t)(juce::int64)wt.getProperty("startSample", 0);
            w.endSample = (int64_t)(juce::int64)wt.getProperty("endSample", 0);
            w.isSelected = (bool)wt.getProperty("selected", false);

            w.fx.doublerActive      = (bool)wt.getProperty("doublerActive", false);
            w.fx.doublerAmount      = (float)wt.getProperty("doublerAmount", 0.70f);
            w.fx.doublerDelayMs     = (float)wt.getProperty("doublerDelayMs", 10.0f);
            w.fx.doublerDetuneCents = (float)wt.getProperty("doublerDetuneCents", 16.0f);

            w.fx.delayActive        = (bool)wt.getProperty("delayActive", false);
            w.fx.delayAmount        = (float)wt.getProperty("delayAmount", 0.55f);
            w.fx.delayTimeMs        = (float)wt.getProperty("delayTimeMs", 345.0f);
            w.fx.delayFeedback      = (float)wt.getProperty("delayFeedback", 0.60f);
            w.fx.pingPong           = (bool)wt.getProperty("pingPong", true);

            w.fx.reverbActive       = (bool)wt.getProperty("reverbActive", false);
            w.fx.reverbAmount       = (float)wt.getProperty("reverbAmount", 0.40f);
            w.fx.reverbDecaySec     = (float)wt.getProperty("reverbDecaySec", 3.8f);
            w.fx.reverbWidth        = (float)wt.getProperty("reverbWidth", 0.60f);

            w.fx.preverbActive      = (bool)wt.getProperty("preverbActive", false);
            w.fx.preverbAmount      = (float)wt.getProperty("preverbAmount", 0.55f);
            w.fx.preverbLengthMs    = (float)wt.getProperty("preverbLengthMs", 450.0f);
            w.fx.preverbDecaySec    = (float)wt.getProperty("preverbDecaySec", 2.2f);

            w.fx.distortionActive   = (bool)wt.getProperty("distortionActive", false);
            w.fx.distortionAmount   = (float)wt.getProperty("distortionAmount", 0.75f);
            w.fx.drive              = (float)wt.getProperty("drive", 0.50f);
            w.fx.distortionTone     = (float)wt.getProperty("distortionTone", 0.65f);

            w.fx.harmonyActive      = (bool)wt.getProperty("harmonyActive", false);
            w.fx.harmonyAmount      = (float)wt.getProperty("harmonyAmount", 0.55f);
            w.fx.harmonyInterval1   = (int)wt.getProperty("harmonyInterval1", 4);
            w.fx.harmonyInterval2   = (int)wt.getProperty("harmonyInterval2", 7);
            w.fx.harmonyWidth       = (float)wt.getProperty("harmonyWidth", 0.75f);

            w.fx.pitchActive        = (bool)wt.getProperty("pitchActive", false);
            w.fx.pitchAmount        = (float)wt.getProperty("pitchAmount", 1.0f);
            w.fx.semitoneShift      = (int)wt.getProperty("semitoneShift", -12);

            loaded.push_back(std::move(w));
        }
        setWords(std::move(loaded));
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new LyricMomentsProcessor();
}
