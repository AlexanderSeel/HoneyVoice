#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

namespace {

static juce::String formatTimeSeconds(double seconds) {
    seconds = juce::jmax(0.0, seconds);
    const int mins = (int)(seconds / 60.0);
    const double rem = seconds - (double)mins * 60.0;
    return juce::String::formatted("%02d:%04.1f", mins, rem);
}

static juce::String buildLyricsTextFromWords(const std::vector<LyricWord>& words) {
    juce::StringArray parts;
    for (const auto& w : words)
        parts.add(w.word);
    return parts.joinIntoString(" ");
}

} // namespace

// ============================================================================
// WaveformTimelineComponent
// ============================================================================

WaveformTimelineComponent::WaveformTimelineComponent(LyricMomentsProcessor& proc,
                                                     std::function<void(const juce::File&)> onFileDropped,
                                                     std::function<void()> onWordsModified)
    : processor(proc),
      fileDroppedCallback(std::move(onFileDropped)),
      wordsModifiedCallback(std::move(onWordsModified))
{
}

void WaveformTimelineComponent::getViewRange(int64_t& outStart, int64_t& outEnd) const {
    const auto& tl = processor.getTimeline();
    const double sr = processor.getCurrentSampleRate();
    if (tl.hasAudio()) {
        outStart = tl.getValidStartSample();
        outEnd = juce::jmax(outStart + (int64_t)sr, tl.getValidEndSample());
    } else {
        outStart = 0;
        outEnd = (int64_t)(sr * 10.0);
    }

    const auto words = processor.getWordsCopy();
    for (const auto& w : words) {
        if (!tl.hasAudio() && w.endSample > outEnd)
            outEnd = w.endSample;
    }
}

int64_t WaveformTimelineComponent::xToSample(float x) const {
    int64_t s0 = 0, s1 = 1;
    getViewRange(s0, s1);
    const float norm = juce::jlimit(0.0f, 1.0f, x / (float)juce::jmax(1, getWidth()));
    return s0 + (int64_t)std::llround((double)norm * (double)(s1 - s0));
}

float WaveformTimelineComponent::sampleToX(int64_t sample) const {
    int64_t s0 = 0, s1 = 1;
    getViewRange(s0, s1);
    const double norm = (double)(sample - s0) / (double)juce::jmax<int64_t>(1, s1 - s0);
    return (float)(norm * (double)getWidth());
}

void WaveformTimelineComponent::refreshCacheIfNeeded() {
    const auto& tl = processor.getTimeline();
    const uint64_t ver = tl.getVersion();
    const int w = getWidth();
    int64_t s0 = 0, s1 = 1;
    getViewRange(s0, s1);

    if (ver != cachedVersion || w != cachedWidth || s0 != cachedViewStart || s1 != cachedViewEnd) {
        cachedVersion = ver;
        cachedWidth = w;
        cachedViewStart = s0;
        cachedViewEnd = s1;
        tl.computePeaks(juce::jmax(1, w / 2), s0, s1, cachedPeaks);
    }
}

void WaveformTimelineComponent::paint(juce::Graphics& g) {
    refreshCacheIfNeeded();

    const auto bounds = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff14161b));
    g.fillRoundedRectangle(bounds, 6.0f);

    if (dragHover) {
        g.setColour(juce::Colour(0xfff59e0b).withAlpha(0.14f));
        g.fillRoundedRectangle(bounds, 6.0f);
        g.setColour(juce::Colour(0xfff59e0b));
        g.drawRoundedRectangle(bounds.reduced(1.0f), 6.0f, 1.5f);
    }

    const auto& tl = processor.getTimeline();
    if (!tl.hasAudio()) {
        g.setColour(juce::Colour(0xff9ca3af).withAlpha(0.75f));
        g.setFont(juce::Font(juce::FontOptions(13.0f)));
        g.drawText("No real audio captured yet — play vocal track in DAW or drop/load WAV, AIFF, FLAC, MP3 here",
                   getLocalBounds(), juce::Justification::centred, true);
    } else {
        // Draw real audio peaks
        const float midY = bounds.getCentreY();
        const float halfH = bounds.getHeight() * 0.44f;
        const int numBuckets = (int)cachedPeaks.size();
        const float barW = numBuckets > 0 ? bounds.getWidth() / (float)numBuckets : 2.0f;

        g.setColour(juce::Colour(0xff64748b).withAlpha(0.8f));
        for (int b = 0; b < numBuckets; ++b) {
            const auto& p = cachedPeaks[(size_t)b];
            const float yTop = midY - p.maxVal * halfH;
            const float yBot = midY - p.minVal * halfH;
            const float h = juce::jmax(1.5f, yBot - yTop);
            g.fillRect((float)b * barW, yTop, juce::jmax(1.2f, barW - 0.5f), h);
        }
    }

    // Draw word regions on top of waveform
    const auto words = processor.getWordsCopy();
    const int activeIdx = processor.getActiveWordIndex();
    for (size_t i = 0; i < words.size(); ++i) {
        const auto& w = words[i];
        if (w.endSample <= w.startSample) continue;
        const float x0 = sampleToX(w.startSample);
        const float x1 = sampleToX(w.endSample);
        const float rw = juce::jmax(3.0f, x1 - x0);
        if (x1 < 0.0f || x0 > (float)getWidth()) continue;

        juce::Colour regCol(0xff475569);
        if (w.fx.hasAnyEffect()) regCol = juce::Colour(0xff38bdf8);
        if (w.isSelected)        regCol = juce::Colour(0xff3b82f6);
        if ((int)i == activeIdx) regCol = juce::Colour(0xfff59e0b);

        const auto regRect = juce::Rectangle<float>(x0, 2.0f, rw, bounds.getHeight() - 4.0f);
        g.setColour(regCol.withAlpha(w.isSelected || (int)i == activeIdx ? 0.22f : 0.10f));
        g.fillRoundedRectangle(regRect, 3.0f);
        g.setColour(regCol.withAlpha(0.75f));
        g.drawRoundedRectangle(regRect, 3.0f, 1.0f);

        if (rw > 24.0f) {
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.setFont(juce::Font(juce::FontOptions(10.5f)));
            g.drawText(w.word, (int)x0 + 3, 3, (int)rw - 6, 14, juce::Justification::centredLeft, true);
        }
    }

    // Draw playhead cursor
    if (tl.hasAudio() || processor.isTransportPlaying()) {
        const int64_t playSample = processor.getCurrentPlayheadSample();
        const float px = sampleToX(playSample);
        if (px >= 0.0f && px <= (float)getWidth()) {
            g.setColour(juce::Colour(0xfff59e0b));
            g.drawLine(px, 0.0f, px, bounds.getHeight(), 2.0f);
        }
    }
}

void WaveformTimelineComponent::mouseMove(const juce::MouseEvent& e) {
    const auto words = processor.getWordsCopy();
    bool nearEdge = false;
    for (const auto& w : words) {
        if (std::abs(e.position.x - sampleToX(w.startSample)) < 5.0f
            || std::abs(e.position.x - sampleToX(w.endSample)) < 5.0f) {
            nearEdge = true;
            break;
        }
    }
    setMouseCursor(nearEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void WaveformTimelineComponent::mouseDown(const juce::MouseEvent& e) {
    const auto words = processor.getWordsCopy();
    draggingWordIndex = -1;
    for (size_t i = 0; i < words.size(); ++i) {
        const float x0 = sampleToX(words[i].startSample);
        const float x1 = sampleToX(words[i].endSample);
        if (std::abs(e.position.x - x0) < 5.0f) {
            draggingWordIndex = (int)i;
            draggingStartEdge = true;
            return;
        }
        if (std::abs(e.position.x - x1) < 5.0f) {
            draggingWordIndex = (int)i;
            draggingStartEdge = false;
            return;
        }
    }

    if (processor.getTimeline().hasAudio()) {
        processor.seekStandaloneSample(xToSample(e.position.x));
        repaint();
    }
}

void WaveformTimelineComponent::mouseDrag(const juce::MouseEvent& e) {
    if (draggingWordIndex < 0)
        return;

    auto words = processor.getWordsCopy();
    if (draggingWordIndex >= (int)words.size())
        return;

    const int64_t sample = xToSample(e.position.x);
    const int64_t minLen = (int64_t)(processor.getCurrentSampleRate() * 0.04);
    auto& w = words[(size_t)draggingWordIndex];
    if (draggingStartEdge)
        w.startSample = juce::jmin(sample, w.endSample - minLen);
    else
        w.endSample = juce::jmax(sample, w.startSample + minLen);

    processor.setWords(std::move(words));
    repaint();
}

void WaveformTimelineComponent::mouseUp(const juce::MouseEvent&) {
    if (draggingWordIndex >= 0) {
        draggingWordIndex = -1;
        if (wordsModifiedCallback) wordsModifiedCallback();
    }
}

bool WaveformTimelineComponent::isInterestedInFileDrag(const juce::StringArray& files) {
    for (const auto& f : files) {
        const auto ext = juce::File(f).getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".aiff" || ext == ".aif" || ext == ".flac" || ext == ".ogg" || ext == ".mp3")
            return true;
    }
    return false;
}

void WaveformTimelineComponent::filesDropped(const juce::StringArray& files, int, int) {
    dragHover = false;
    repaint();
    if (!files.isEmpty() && fileDroppedCallback)
        fileDroppedCallback(juce::File(files[0]));
}

// ============================================================================
// FxRackPanel
// ============================================================================

FxRackPanel::FxRackPanel() {
    auto setupKnob = [this](juce::Slider& s, double min, double max, double step, double val,
                            const juce::String& suffix, juce::Colour accent, const juce::String& caption) {
        s.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        s.setRange(min, max, step);
        s.setValue(val, juce::dontSendNotification);
        s.setTextValueSuffix(suffix);
        s.setColour(juce::Slider::rotarySliderFillColourId, accent);
        addAndMakeVisible(s);
        addKnobLabel(s, caption);
    };

    const juce::Colour cDoubler(0xff2dd4bf);
    setupKnob(doublerAmountSlider, 0.0, 100.0, 1.0, 70.0, "%",  cDoubler, "Amount");
    setupKnob(doublerDelaySlider,  1.0, 50.0,  0.5, 10.0, "ms", cDoubler, "Delay");
    setupKnob(doublerDetuneSlider, 0.0, 50.0,  0.5, 16.0, "c",  cDoubler, "Detune");

    const juce::Colour cDelay(0xfff59e0b);
    setupKnob(delayAmountSlider,   0.0,  100.0,  1.0, 55.0,  "%",  cDelay, "Amount");
    setupKnob(delayTimeSlider,     20.0, 1000.0, 1.0, 345.0, "ms", cDelay, "Time");
    setupKnob(delayFeedbackSlider, 0.0,  95.0,   1.0, 60.0,  "%",  cDelay, "Feedback");
    pingPongToggle.setToggleState(true, juce::dontSendNotification);
    addAndMakeVisible(pingPongToggle);

    const juce::Colour cReverb(0xff3b82f6);
    setupKnob(reverbAmountSlider, 0.0, 100.0, 1.0, 40.0, "%", cReverb, "Amount");
    setupKnob(reverbDecaySlider,  0.2, 8.0,   0.1, 3.8,  "s", cReverb, "Decay");
    setupKnob(reverbWidthSlider,  0.0, 100.0, 1.0, 60.0, "%", cReverb, "Width");

    const juce::Colour cPreverb(0xff8b5cf6);
    setupKnob(preverbAmountSlider, 0.0,  100.0,  1.0, 55.0,  "%",  cPreverb, "Amount");
    setupKnob(preverbLengthSlider, 80.0, 1500.0, 5.0, 450.0, "ms", cPreverb, "Length");
    setupKnob(preverbDecaySlider,  0.4,  5.0,    0.1, 2.2,   "s",  cPreverb, "Decay");

    const juce::Colour cDist(0xffef4444);
    setupKnob(distortionAmountSlider, 0.0, 100.0, 1.0, 75.0, "%", cDist, "Amount");
    setupKnob(distortionDriveSlider,  0.0, 100.0, 1.0, 50.0, "%", cDist, "Drive");
    setupKnob(distortionToneSlider,   0.0, 100.0, 1.0, 65.0, "%", cDist, "Tone");

    const juce::Colour cHarm(0xffec4899);
    setupKnob(harmonyAmountSlider, 0.0,   100.0, 1.0, 55.0, "%",  cHarm, "Amount");
    setupKnob(harmonyInt1Slider,   -12.0, 12.0,  1.0, 4.0,  "st", cHarm, "Voice 1");
    setupKnob(harmonyInt2Slider,   -12.0, 12.0,  1.0, 7.0,  "st", cHarm, "Voice 2");
    setupKnob(harmonyWidthSlider,  0.0,   100.0, 1.0, 75.0, "%",  cHarm, "Width");

    const juce::Colour cPitch(0xffa855f7);
    setupKnob(pitchAmountSlider, 0.0,   100.0, 1.0, 100.0, "%",  cPitch, "Amount");
    setupKnob(pitchShiftSlider,  -12.0, 12.0,  1.0, -12.0, "st", cPitch, "Shift");
}

void FxRackPanel::addKnobLabel(juce::Slider& s, const juce::String& name) {
    auto kl = std::make_unique<KnobWithLabel>();
    kl->slider = &s;
    kl->label.setText(name, juce::dontSendNotification);
    kl->label.setJustificationType(juce::Justification::centred);
    kl->label.setFont(juce::Font(juce::FontOptions(10.5f)));
    kl->label.setColour(juce::Label::textColourId, juce::Colour(0xff9ca3af));
    addAndMakeVisible(kl->label);
    knobLabels.push_back(std::move(kl));
}

void FxRackPanel::setActiveStates(const WordEffectSettings& fx) {
    currentStates = fx;
    repaint();
}

int FxRackPanel::getCardY(int cardIndex) const {
    return cardIndex * (kCardHeight + kCardGap);
}

void FxRackPanel::paint(juce::Graphics& g) {
    struct CardMeta { const char* title; juce::uint32 col; bool active; };
    const CardMeta cards[7] = {
        { "Doubler",    0xff2dd4bf, currentStates.doublerActive },
        { "Delay",      0xfff59e0b, currentStates.delayActive },
        { "Reverb",     0xff3b82f6, currentStates.reverbActive },
        { "Preverb",    0xff8b5cf6, currentStates.preverbActive },
        { "Distortion", 0xffef4444, currentStates.distortionActive },
        { "Harmony",    0xffec4899, currentStates.harmonyActive },
        { "Pitch",      0xffa855f7, currentStates.pitchActive }
    };

    const float w = (float)getWidth() - 4.0f;
    for (int i = 0; i < 7; ++i) {
        const auto r = juce::Rectangle<float>(0.0f, (float)getCardY(i), w, (float)kCardHeight);
        const auto col = juce::Colour(cards[i].col);
        g.setColour(juce::Colour(cards[i].active ? 0xff1f242e : 0xff181b21));
        g.fillRoundedRectangle(r, 8.0f);
        g.setColour(col.withAlpha(cards[i].active ? 0.85f : 0.30f));
        g.drawRoundedRectangle(r, 8.0f, cards[i].active ? 1.5f : 1.0f);
        g.setColour(col.withAlpha(cards[i].active ? 1.0f : 0.65f));
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawText(cards[i].title, r.reduced(12, 6).removeFromTop(20), juce::Justification::centredLeft);

        g.setColour(cards[i].active ? col : juce::Colour(0xff4b5563));
        g.fillEllipse(r.getRight() - 20.0f, r.getY() + 10.0f, 8.0f, 8.0f);
    }
}

void FxRackPanel::resized() {
    auto place3 = [&](int cardIdx, juce::Slider& a, juce::Slider& b, juce::Slider& c) {
        const int y = getCardY(cardIdx) + 28;
        a.setBounds(18,  y, 58, 58);
        b.setBounds(112, y, 58, 58);
        c.setBounds(206, y, 58, 58);
    };

    place3(0, doublerAmountSlider, doublerDelaySlider, doublerDetuneSlider);
    place3(1, delayAmountSlider, delayTimeSlider, delayFeedbackSlider);
    pingPongToggle.setBounds(110, getCardY(1) + 5, 110, 20);
    place3(2, reverbAmountSlider, reverbDecaySlider, reverbWidthSlider);
    place3(3, preverbAmountSlider, preverbLengthSlider, preverbDecaySlider);
    place3(4, distortionAmountSlider, distortionDriveSlider, distortionToneSlider);

    const int hy = getCardY(5) + 28;
    harmonyAmountSlider.setBounds(12,  hy, 56, 56);
    harmonyInt1Slider.setBounds  (84,  hy, 56, 56);
    harmonyInt2Slider.setBounds  (156, hy, 56, 56);
    harmonyWidthSlider.setBounds (228, hy, 56, 56);

    const int py = getCardY(6) + 28;
    pitchAmountSlider.setBounds(18,  py, 58, 58);
    pitchShiftSlider.setBounds (112, py, 58, 58);

    for (auto& kl : knobLabels) {
        if (kl->slider) {
            const auto sb = kl->slider->getBounds();
            kl->label.setBounds(sb.getX() - 8, sb.getBottom() - 2, sb.getWidth() + 16, 15);
        }
    }
}

// ============================================================================
// TranscribeOverlayPanel
// ============================================================================

TranscribeOverlayPanel::TranscribeOverlayPanel(
    std::function<void(VocalTranscriber::EngineMode, const juce::String&, const juce::File&)> onRunTranscribe,
    std::function<void(const juce::String&)> onAlignLyrics,
    std::function<void()> onBrowseModel,
    std::function<void()> onClose)
    : runCallback(std::move(onRunTranscribe)),
      alignCallback(std::move(onAlignLyrics)),
      browseModelCallback(std::move(onBrowseModel)),
      closeCallback(std::move(onClose))
{
    titleLabel.setText("Transcribe & Align Real Audio", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(16.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, juce::Colour(0xfff59e0b));
    addAndMakeVisible(titleLabel);

    engineLabel.setText("Engine:", juce::dontSendNotification);
    engineLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd4d4d8));
    addAndMakeVisible(engineLabel);

    engineBox.addItem("Auto (Whisper -> Windows Speech -> Segmenter)", 1);
    engineBox.addItem("Whisper Model Only (.bin)", 2);
    engineBox.addItem("Windows Speech Recognition (SAPI)", 3);
    engineBox.addItem("Acoustic Vocal Segmenter Only", 4);
    engineBox.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(engineBox);

    modelLabel.setText("Whisper model: (none — using Windows Speech / Segmenter)", juce::dontSendNotification);
    modelLabel.setColour(juce::Label::textColourId, juce::Colour(0xff9ca3af));
    addAndMakeVisible(modelLabel);

    browseModelBtn.onClick = [this] { if (browseModelCallback) browseModelCallback(); };
    addAndMakeVisible(browseModelBtn);

    lyricsPromptLabel.setText("Optional lyrics (edit or paste lyrics to align directly to detected vocal segments):", juce::dontSendNotification);
    lyricsPromptLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd4d4d8));
    addAndMakeVisible(lyricsPromptLabel);

    lyricsEditor.setMultiLine(true);
    lyricsEditor.setReturnKeyStartsNewLine(true);
    lyricsEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff111318));
    lyricsEditor.setTextToShowWhenEmpty("Leave blank to auto-transcribe spoken/sung audio, or paste lyrics here and click 'Align Typed Lyrics'...", juce::Colours::grey);
    addAndMakeVisible(lyricsEditor);

    statusLabel.setColour(juce::Label::textColourId, juce::Colour(0xff38bdf8));
    addAndMakeVisible(statusLabel);

    runTranscribeBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2563eb));
    runTranscribeBtn.onClick = [this] {
        if (!runCallback) return;
        const int id = engineBox.getSelectedId();
        auto mode = VocalTranscriber::EngineMode::Auto;
        if (id == 2) mode = VocalTranscriber::EngineMode::WhisperOnly;
        else if (id == 3) mode = VocalTranscriber::EngineMode::WindowsSapiOnly;
        else if (id == 4) mode = VocalTranscriber::EngineMode::SegmentOnly;
        runCallback(mode, lyricsEditor.getText(), selectedModelFile);
    };
    addAndMakeVisible(runTranscribeBtn);

    alignLyricsBtn.onClick = [this] {
        if (alignCallback) alignCallback(lyricsEditor.getText());
    };
    addAndMakeVisible(alignLyricsBtn);

    closeBtn.onClick = [this] { if (closeCallback) closeCallback(); };
    addAndMakeVisible(closeBtn);
}

void TranscribeOverlayPanel::setStatusText(const juce::String& text) {
    statusLabel.setText(text, juce::dontSendNotification);
}

void TranscribeOverlayPanel::setModelFile(const juce::File& file) {
    selectedModelFile = file;
    if (file.existsAsFile())
        modelLabel.setText("Whisper model: " + file.getFileName(), juce::dontSendNotification);
    else
        modelLabel.setText("Whisper model: (none — using Windows Speech / Segmenter)", juce::dontSendNotification);
}

void TranscribeOverlayPanel::setLyricsText(const juce::String& text) {
    lyricsEditor.setText(text, juce::dontSendNotification);
}

void TranscribeOverlayPanel::paint(juce::Graphics& g) {
    g.fillAll(juce::Colours::black.withAlpha(0.65f));
    const auto card = getLocalBounds().reduced(70, 50).toFloat();
    g.setColour(juce::Colour(0xff181b22));
    g.fillRoundedRectangle(card, 10.0f);
    g.setColour(juce::Colour(0xfff59e0b).withAlpha(0.65f));
    g.drawRoundedRectangle(card, 10.0f, 1.5f);
}

void TranscribeOverlayPanel::resized() {
    auto area = getLocalBounds().reduced(86, 64);
    titleLabel.setBounds(area.removeFromTop(28));
    area.removeFromTop(8);

    auto row1 = area.removeFromTop(26);
    engineLabel.setBounds(row1.removeFromLeft(70));
    engineBox.setBounds(row1);
    area.removeFromTop(8);

    auto row2 = area.removeFromTop(26);
    browseModelBtn.setBounds(row2.removeFromRight(170));
    row2.removeFromRight(8);
    modelLabel.setBounds(row2);
    area.removeFromTop(10);

    lyricsPromptLabel.setBounds(area.removeFromTop(22));
    area.removeFromTop(4);

    auto bottomRow = area.removeFromBottom(30);
    auto statusRow = area.removeFromBottom(24);
    area.removeFromBottom(6);
    lyricsEditor.setBounds(area);

    statusLabel.setBounds(statusRow);
    runTranscribeBtn.setBounds(bottomRow.removeFromLeft(160));
    bottomRow.removeFromLeft(10);
    alignLyricsBtn.setBounds(bottomRow.removeFromLeft(160));
    closeBtn.setBounds(bottomRow.removeFromRight(90));
}

// ============================================================================
// TranscribeWorker
// ============================================================================

class LyricMomentsEditor::TranscribeWorker : public juce::Thread {
public:
    TranscribeWorker(LyricMomentsEditor& ed,
                     std::vector<float> audio16k,
                     int64_t startSample,
                     double sourceRate,
                     VocalTranscriber::Options opts)
        : juce::Thread("HoneyVoiceTranscribe"),
          editor(ed),
          mono16k(std::move(audio16k)),
          timelineStartSample(startSample),
          sourceSampleRate(sourceRate),
          options(std::move(opts))
    {
        startThread();
    }

    ~TranscribeWorker() override {
        stopThread(5000);
    }

    void run() override {
        auto result = VocalTranscriber::transcribe(mono16k, timelineStartSample, sourceSampleRate, options);
        if (threadShouldExit())
            return;

        juce::Component::SafePointer<LyricMomentsEditor> safeEd(&editor);
        juce::MessageManager::callAsync([safeEd, result = std::move(result)]() mutable {
            if (!safeEd) return;
            safeEd->processorRef.setLastTranscriptionStatus(result.statusMessage);
            if (result.success) {
                safeEd->processorRef.setWords(std::move(result.words));
                safeEd->rebuildLyricLayout();
                safeEd->syncControlsFromSelection();
            }
            if (safeEd->transcribeOverlay) {
                safeEd->transcribeOverlay->setStatusText(result.statusMessage);
                if (result.success)
                    safeEd->transcribeOverlay->setLyricsText(buildLyricsTextFromWords(safeEd->processorRef.getWordsCopy()));
            }
            safeEd->repaint();
        });
    }

private:
    LyricMomentsEditor& editor;
    std::vector<float> mono16k;
    int64_t timelineStartSample = 0;
    double sourceSampleRate = 44100.0;
    VocalTranscriber::Options options;
};

// ============================================================================
// LyricMomentsEditor
// ============================================================================

LyricMomentsEditor::LyricMomentsEditor(LyricMomentsProcessor& p)
    : AudioProcessorEditor(&p),
      processorRef(p),
      waveformView(p,
                   [this](const juce::File& f) { loadAudioFileAsync(f); },
                   [this] { rebuildLyricLayout(); })
{
    setLookAndFeel(&customLookAndFeel);
    setSize(1000, 580);

    const auto foundModels = VocalTranscriber::findAvailableWhisperModels();
    if (!foundModels.isEmpty())
        selectedWhisperModel = foundModels.getFirst();

    searchBox.setTextToShowWhenEmpty("Find words...", juce::Colours::grey);
    searchBox.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff141518));
    searchBox.onTextChange = [this] {
        processorRef.updateSearchHighlights(searchBox.getText());
        updateLyricVisualStates();
    };
    addAndMakeVisible(searchBox);

    muteDryToggle.setToggleState(processorRef.getMuteDry(), juce::dontSendNotification);
    muteDryToggle.onClick = [this] { processorRef.setMuteDry(muteDryToggle.getToggleState()); };
    addAndMakeVisible(muteDryToggle);

    loadAudioBtn.onClick = [this] {
        activeFileChooser = std::make_shared<juce::FileChooser>(
            "Load Vocal Audio",
            juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            "*.wav;*.aiff;*.aif;*.flac;*.ogg;*.mp3");
        auto chooser = activeFileChooser;
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [this, chooser](const juce::FileChooser& fc) {
            const auto res = fc.getResult();
            if (res.existsAsFile())
                loadAudioFileAsync(res);
        });
    };
    addAndMakeVisible(loadAudioBtn);

    retranscribeBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2563eb));
    retranscribeBtn.onClick = [this] {
        if (!transcribeOverlay) {
            transcribeOverlay = std::make_unique<TranscribeOverlayPanel>(
                [this](VocalTranscriber::EngineMode mode, const juce::String& hint, const juce::File& model) {
                    startTranscription(mode, hint, model);
                },
                [this](const juce::String& lyrics) { alignTypedLyrics(lyrics); },
                [this] { openModelBrowser(); },
                [this] {
                    if (transcribeOverlay) transcribeOverlay->setVisible(false);
                });
            addChildComponent(*transcribeOverlay);
        }
        transcribeOverlay->setModelFile(selectedWhisperModel);
        transcribeOverlay->setStatusText(processorRef.getLastTranscriptionStatus());
        transcribeOverlay->setLyricsText(buildLyricsTextFromWords(processorRef.getWordsCopy()));
        transcribeOverlay->setBounds(getLocalBounds());
        transcribeOverlay->setVisible(true);
        transcribeOverlay->toFront(true);
    };
    addAndMakeVisible(retranscribeBtn);

    removeAllBtn.onClick = [this] {
        auto words = processorRef.getWordsCopy();
        for (auto& w : words)
            w.fx = WordEffectSettings{};
        processorRef.setWords(std::move(words));
        syncControlsFromSelection();
        updateLyricVisualStates();
        repaint();
    };
    addAndMakeVisible(removeAllBtn);

    // Lyric viewport
    lyricViewport.setViewedComponent(&lyricContainer, false);
    lyricViewport.setScrollBarsShown(true, false);
    addAndMakeVisible(lyricViewport);

    selectAllBtn.onClick = [this] {
        auto words = processorRef.getWordsCopy();
        for (auto& w : words) w.isSelected = true;
        processorRef.setWords(std::move(words));
        syncControlsFromSelection();
        updateLyricVisualStates();
    };
    clearSelBtn.onClick = [this] {
        processorRef.clearAllSelections();
        syncControlsFromSelection();
        updateLyricVisualStates();
    };
    liveCaptureToggle.setToggleState(processorRef.getTimeline().isLiveCaptureEnabled(), juce::dontSendNotification);
    liveCaptureToggle.onClick = [this] {
        processorRef.getTimeline().setLiveCaptureEnabled(liveCaptureToggle.getToggleState());
    };
    clearAudioBtn.onClick = [this] {
        processorRef.setStandalonePlaying(false);
        processorRef.getTimeline().clear();
        processorRef.setWords({});
        liveCaptureToggle.setToggleState(true, juce::dontSendNotification);
        rebuildLyricLayout();
        waveformView.repaint();
    };
    addAndMakeVisible(selectAllBtn);
    addAndMakeVisible(clearSelBtn);
    addAndMakeVisible(liveCaptureToggle);
    addAndMakeVisible(clearAudioBtn);

    // FX Rack Viewport & wiring
    fxRackViewport.setViewedComponent(&fxRackPanel, false);
    fxRackViewport.setScrollBarsShown(true, false);
    addAndMakeVisible(fxRackViewport);

    auto wireKnob = [this](juce::Slider& s) {
        s.onValueChange = [this] {
            if (!isSyncingControls) applyControlsToSelection();
        };
    };
    wireKnob(fxRackPanel.doublerAmountSlider);
    wireKnob(fxRackPanel.doublerDelaySlider);
    wireKnob(fxRackPanel.doublerDetuneSlider);
    wireKnob(fxRackPanel.delayAmountSlider);
    wireKnob(fxRackPanel.delayTimeSlider);
    wireKnob(fxRackPanel.delayFeedbackSlider);
    fxRackPanel.pingPongToggle.onClick = [this] {
        if (!isSyncingControls) applyControlsToSelection();
    };
    wireKnob(fxRackPanel.reverbAmountSlider);
    wireKnob(fxRackPanel.reverbDecaySlider);
    wireKnob(fxRackPanel.reverbWidthSlider);
    wireKnob(fxRackPanel.preverbAmountSlider);
    wireKnob(fxRackPanel.preverbLengthSlider);
    wireKnob(fxRackPanel.preverbDecaySlider);
    wireKnob(fxRackPanel.distortionAmountSlider);
    wireKnob(fxRackPanel.distortionDriveSlider);
    wireKnob(fxRackPanel.distortionToneSlider);
    wireKnob(fxRackPanel.harmonyAmountSlider);
    wireKnob(fxRackPanel.harmonyInt1Slider);
    wireKnob(fxRackPanel.harmonyInt2Slider);
    wireKnob(fxRackPanel.harmonyWidthSlider);
    wireKnob(fxRackPanel.pitchAmountSlider);
    wireKnob(fxRackPanel.pitchShiftSlider);

    auto setupFxTab = [this](juce::TextButton& btn, juce::Colour c, int cardIndex) {
        btn.setClickingTogglesState(true);
        btn.setColour(juce::TextButton::buttonOnColourId, c.withAlpha(0.32f));
        btn.onClick = [this, cardIndex] {
            if (!isSyncingControls)
                applyControlsToSelection();
            fxRackViewport.setViewPosition(0, juce::jmax(0, fxRackPanel.getCardY(cardIndex) - 8));
            repaint();
        };
        addAndMakeVisible(btn);
    };

    setupFxTab(doublerTabBtn,    juce::Colour(0xff2dd4bf), 0);
    setupFxTab(delayTabBtn,      juce::Colour(0xfff59e0b), 1);
    setupFxTab(reverbTabBtn,     juce::Colour(0xff3b82f6), 2);
    setupFxTab(preverbTabBtn,    juce::Colour(0xff8b5cf6), 3);
    setupFxTab(distortionTabBtn, juce::Colour(0xffef4444), 4);
    setupFxTab(harmonyTabBtn,    juce::Colour(0xffec4899), 5);
    setupFxTab(pitchTabBtn,      juce::Colour(0xffa855f7), 6);

    applyBtn.onClick = [this] { applyControlsToSelection(); };
    copyBtn.onClick = [this] {
        for (const auto& w : processorRef.getWordsCopy()) {
            if (w.isSelected) { clipboardSettings = w.fx; break; }
        }
    };
    pasteBtn.onClick = [this] {
        auto words = processorRef.getWordsCopy();
        for (auto& w : words) {
            if (w.isSelected) w.fx = clipboardSettings;
        }
        processorRef.setWords(std::move(words));
        syncControlsFromSelection();
        updateLyricVisualStates();
    };
    removeBtn.onClick = [this] {
        auto words = processorRef.getWordsCopy();
        for (auto& w : words) {
            if (w.isSelected) w.fx = WordEffectSettings{};
        }
        processorRef.setWords(std::move(words));
        syncControlsFromSelection();
        updateLyricVisualStates();
    };

    addAndMakeVisible(applyBtn);
    addAndMakeVisible(copyBtn);
    addAndMakeVisible(pasteBtn);
    addAndMakeVisible(removeBtn);

    // Transport & Waveform
    playStopBtn.onClick = [this] {
        if (!processorRef.getTimeline().hasAudio())
            return;
        processorRef.setStandalonePlaying(!processorRef.isStandalonePlaying());
    };
    addAndMakeVisible(playStopBtn);

    loopBtn.setToggleState(processorRef.isStandaloneLoop(), juce::dontSendNotification);
    loopBtn.onClick = [this] { processorRef.setStandaloneLoop(loopBtn.getToggleState()); };
    addAndMakeVisible(loopBtn);

    timeLabel.setFont(juce::Font(juce::FontOptions(11.5f)));
    timeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffd4d4d8));
    timeLabel.setText("00:00.0 / 00:00.0", juce::dontSendNotification);
    addAndMakeVisible(timeLabel);

    addAndMakeVisible(waveformView);

    inlineWordEditor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff0f172a));
    inlineWordEditor.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xfff59e0b));
    inlineWordEditor.onReturnKey = [this] {
        if (inlineEditingWordIndex >= 0) {
            auto words = processorRef.getWordsCopy();
            const auto newText = inlineWordEditor.getText().trim();
            if (inlineEditingWordIndex < (int)words.size() && newText.isNotEmpty()) {
                words[(size_t)inlineEditingWordIndex].word = newText;
                processorRef.setWords(std::move(words));
            }
        }
        inlineEditingWordIndex = -1;
        inlineWordEditor.setVisible(false);
        rebuildLyricLayout();
    };
    inlineWordEditor.onEscapeKey = [this] {
        inlineEditingWordIndex = -1;
        inlineWordEditor.setVisible(false);
    };
    inlineWordEditor.onFocusLost = [this] {
        inlineEditingWordIndex = -1;
        inlineWordEditor.setVisible(false);
    };
    lyricContainer.addChildComponent(inlineWordEditor);

    rebuildLyricLayout();
    syncControlsFromSelection();
    startTimerHz(30);
}

LyricMomentsEditor::~LyricMomentsEditor() {
    stopTimer();
    activeWorker.reset();
    setLookAndFeel(nullptr);
}

void LyricMomentsEditor::openModelBrowser() {
    activeFileChooser = std::make_shared<juce::FileChooser>(
        "Select Whisper GGML Model (.bin)",
        VocalTranscriber::getDefaultModelsDirectory(),
        "*.bin");
    auto chooser = activeFileChooser;
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this, chooser](const juce::FileChooser& fc) {
        const auto res = fc.getResult();
        if (res.existsAsFile()) {
            selectedWhisperModel = res;
            if (transcribeOverlay)
                transcribeOverlay->setModelFile(res);
        }
    });
}

void LyricMomentsEditor::loadAudioFileAsync(const juce::File& file) {
    juce::String err;
    if (!processorRef.getTimeline().loadFromFile(file, err)) {
        processorRef.setLastTranscriptionStatus("Load error: " + err);
        repaint();
        return;
    }

    liveCaptureToggle.setToggleState(false, juce::dontSendNotification);
    processorRef.seekStandaloneSample(processorRef.getTimeline().getValidStartSample());
    processorRef.setLastTranscriptionStatus("Loaded " + file.getFileName() + " — transcribing real audio...");
    waveformView.repaint();
    repaint();

    startTranscription(VocalTranscriber::EngineMode::Auto, {}, selectedWhisperModel);
}

void LyricMomentsEditor::startTranscription(VocalTranscriber::EngineMode mode,
                                            const juce::String& hintLyrics,
                                            const juce::File& modelFile) {
    std::vector<float> mono16k;
    int64_t startSample = 0;
    double sourceRate = processorRef.getCurrentSampleRate();

    if (!processorRef.getTimeline().extractMono16k(mono16k, startSample, sourceRate)) {
        const juce::String msg = "No audio captured yet — play audio in your DAW or load an audio file first.";
        processorRef.setLastTranscriptionStatus(msg);
        if (transcribeOverlay) transcribeOverlay->setStatusText(msg);
        repaint();
        return;
    }

    VocalTranscriber::Options opts;
    opts.mode = mode;
    opts.hintLyrics = hintLyrics;
    opts.whisperModelFile = modelFile;

    const juce::String msg = "Transcribing real audio (" + formatTimeSeconds((double)mono16k.size() / 16000.0) + ")...";
    processorRef.setLastTranscriptionStatus(msg);
    if (transcribeOverlay) transcribeOverlay->setStatusText(msg);
    repaint();

    activeWorker.reset();
    activeWorker = std::make_unique<TranscribeWorker>(*this, std::move(mono16k), startSample, sourceRate, std::move(opts));
}

void LyricMomentsEditor::alignTypedLyrics(const juce::String& lyricsText) {
    std::vector<float> mono16k;
    int64_t startSample = 0;
    double sourceRate = processorRef.getCurrentSampleRate();

    if (!processorRef.getTimeline().extractMono16k(mono16k, startSample, sourceRate)) {
        const juce::String msg = "No audio captured yet — play DAW audio or load a vocal file first.";
        processorRef.setLastTranscriptionStatus(msg);
        if (transcribeOverlay) transcribeOverlay->setStatusText(msg);
        return;
    }

    auto aligned = VocalTranscriber::alignLyricsToAudio(lyricsText, mono16k, startSample, sourceRate);
    const juce::String msg = aligned.empty()
        ? "Could not align lyrics — audio appears silent."
        : ("Aligned " + juce::String((int)aligned.size()) + " words to real vocal audio.");

    processorRef.setWords(std::move(aligned));
    processorRef.setLastTranscriptionStatus(msg);
    if (transcribeOverlay) transcribeOverlay->setStatusText(msg);
    rebuildLyricLayout();
    syncControlsFromSelection();
    repaint();
}

void LyricMomentsEditor::rebuildLyricLayout() {
    wordComponents.clear();
    const auto words = processorRef.getWordsCopy();
    const int activeIdx = processorRef.getActiveWordIndex();

    for (size_t i = 0; i < words.size(); ++i) {
        auto* comp = new LyricWordComponent(
            (int)i,
            words[i],
            (int)i == activeIdx,
            [this](int idx, const juce::MouseEvent& e) { handleWordClick(idx, e); },
            [this](int idx) { handleWordDoubleClick(idx); });
        lyricContainer.addAndMakeVisible(comp);
        wordComponents.add(comp);
    }

    // Flow layout inside lyricContainer
    const int containerW = juce::jmax(200, lyricViewport.getWidth() - 16);
    int cursorX = 12;
    int cursorY = 12;
    const int rowHeight = 34;

    for (int i = 0; i < wordComponents.size(); ++i) {
        const int wordW = juce::jmax(34, words[(size_t)i].word.length() * 9 + 16);
        if (cursorX + wordW > containerW - 10) {
            cursorX = 12;
            cursorY += rowHeight;
        }
        wordComponents[i]->setBounds(cursorX, cursorY, wordW, 28);
        cursorX += wordW + 6;
    }

    const int contentH = juce::jmax(lyricViewport.getHeight(), cursorY + rowHeight + 12);
    lyricContainer.setSize(containerW, contentH);
    waveformView.repaint();
}

void LyricMomentsEditor::updateLyricVisualStates() {
    const auto words = processorRef.getWordsCopy();
    const int activeIdx = processorRef.getActiveWordIndex();
    const int count = juce::jmin((int)words.size(), wordComponents.size());
    for (int i = 0; i < count; ++i)
        wordComponents[i]->setWordState(words[(size_t)i], i == activeIdx);
}

void LyricMomentsEditor::handleWordClick(int index, const juce::MouseEvent& e) {
    auto words = processorRef.getWordsCopy();
    if (index < 0 || index >= (int)words.size())
        return;

    if (e.mods.isShiftDown() && lastClickedWordIndex >= 0) {
        processorRef.selectWordRange(lastClickedWordIndex, index);
    } else {
        processorRef.setWordSelection(index, !words[(size_t)index].isSelected);
        lastClickedWordIndex = index;
    }

    syncControlsFromSelection();
    updateLyricVisualStates();
    waveformView.repaint();
}

void LyricMomentsEditor::handleWordDoubleClick(int index) {
    const auto words = processorRef.getWordsCopy();
    if (index < 0 || index >= (int)words.size() || index >= wordComponents.size())
        return;

    processorRef.seekStandaloneSample(words[(size_t)index].startSample);
    waveformView.repaint();

    inlineEditingWordIndex = index;
    const auto b = wordComponents[index]->getBounds();
    inlineWordEditor.setBounds(b.withWidth(juce::jmax(80, b.getWidth() + 24)));
    inlineWordEditor.setText(words[(size_t)index].word, juce::dontSendNotification);
    inlineWordEditor.setVisible(true);
    inlineWordEditor.toFront(true);
    inlineWordEditor.grabKeyboardFocus();
    inlineWordEditor.selectAll();
}

void LyricMomentsEditor::syncControlsFromSelection() {
    isSyncingControls = true;
    const auto words = processorRef.getWordsCopy();
    WordEffectSettings fx;
    bool foundSelected = false;
    for (const auto& w : words) {
        if (w.isSelected) {
            fx = w.fx;
            foundSelected = true;
            break;
        }
    }

    if (foundSelected) {
        doublerTabBtn.setToggleState(fx.doublerActive, juce::dontSendNotification);
        delayTabBtn.setToggleState(fx.delayActive, juce::dontSendNotification);
        reverbTabBtn.setToggleState(fx.reverbActive, juce::dontSendNotification);
        preverbTabBtn.setToggleState(fx.preverbActive, juce::dontSendNotification);
        distortionTabBtn.setToggleState(fx.distortionActive, juce::dontSendNotification);
        harmonyTabBtn.setToggleState(fx.harmonyActive, juce::dontSendNotification);
        pitchTabBtn.setToggleState(fx.pitchActive, juce::dontSendNotification);

        fxRackPanel.doublerAmountSlider.setValue(fx.doublerAmount * 100.0, juce::dontSendNotification);
        fxRackPanel.doublerDelaySlider.setValue(fx.doublerDelayMs, juce::dontSendNotification);
        fxRackPanel.doublerDetuneSlider.setValue(fx.doublerDetuneCents, juce::dontSendNotification);

        fxRackPanel.delayAmountSlider.setValue(fx.delayAmount * 100.0, juce::dontSendNotification);
        fxRackPanel.delayTimeSlider.setValue(fx.delayTimeMs, juce::dontSendNotification);
        fxRackPanel.delayFeedbackSlider.setValue(fx.delayFeedback * 100.0, juce::dontSendNotification);
        fxRackPanel.pingPongToggle.setToggleState(fx.pingPong, juce::dontSendNotification);

        fxRackPanel.reverbAmountSlider.setValue(fx.reverbAmount * 100.0, juce::dontSendNotification);
        fxRackPanel.reverbDecaySlider.setValue(fx.reverbDecaySec, juce::dontSendNotification);
        fxRackPanel.reverbWidthSlider.setValue(fx.reverbWidth * 100.0, juce::dontSendNotification);

        fxRackPanel.preverbAmountSlider.setValue(fx.preverbAmount * 100.0, juce::dontSendNotification);
        fxRackPanel.preverbLengthSlider.setValue(fx.preverbLengthMs, juce::dontSendNotification);
        fxRackPanel.preverbDecaySlider.setValue(fx.preverbDecaySec, juce::dontSendNotification);

        fxRackPanel.distortionAmountSlider.setValue(fx.distortionAmount * 100.0, juce::dontSendNotification);
        fxRackPanel.distortionDriveSlider.setValue(fx.drive * 100.0, juce::dontSendNotification);
        fxRackPanel.distortionToneSlider.setValue(fx.distortionTone * 100.0, juce::dontSendNotification);

        fxRackPanel.harmonyAmountSlider.setValue(fx.harmonyAmount * 100.0, juce::dontSendNotification);
        fxRackPanel.harmonyInt1Slider.setValue(fx.harmonyInterval1, juce::dontSendNotification);
        fxRackPanel.harmonyInt2Slider.setValue(fx.harmonyInterval2, juce::dontSendNotification);
        fxRackPanel.harmonyWidthSlider.setValue(fx.harmonyWidth * 100.0, juce::dontSendNotification);

        fxRackPanel.pitchAmountSlider.setValue(fx.pitchAmount * 100.0, juce::dontSendNotification);
        fxRackPanel.pitchShiftSlider.setValue(fx.semitoneShift, juce::dontSendNotification);
    }

    fxRackPanel.setActiveStates(fx);
    isSyncingControls = false;
}

void LyricMomentsEditor::applyControlsToSelection() {
    WordEffectSettings uiFx;
    uiFx.doublerActive    = doublerTabBtn.getToggleState();
    uiFx.delayActive      = delayTabBtn.getToggleState();
    uiFx.reverbActive     = reverbTabBtn.getToggleState();
    uiFx.preverbActive    = preverbTabBtn.getToggleState();
    uiFx.distortionActive = distortionTabBtn.getToggleState();
    uiFx.harmonyActive    = harmonyTabBtn.getToggleState();
    uiFx.pitchActive      = pitchTabBtn.getToggleState();

    uiFx.doublerAmount      = (float)fxRackPanel.doublerAmountSlider.getValue() / 100.0f;
    uiFx.doublerDelayMs     = (float)fxRackPanel.doublerDelaySlider.getValue();
    uiFx.doublerDetuneCents = (float)fxRackPanel.doublerDetuneSlider.getValue();

    uiFx.delayAmount   = (float)fxRackPanel.delayAmountSlider.getValue() / 100.0f;
    uiFx.delayTimeMs   = (float)fxRackPanel.delayTimeSlider.getValue();
    uiFx.delayFeedback = (float)fxRackPanel.delayFeedbackSlider.getValue() / 100.0f;
    uiFx.pingPong      = fxRackPanel.pingPongToggle.getToggleState();

    uiFx.reverbAmount   = (float)fxRackPanel.reverbAmountSlider.getValue() / 100.0f;
    uiFx.reverbDecaySec = (float)fxRackPanel.reverbDecaySlider.getValue();
    uiFx.reverbWidth    = (float)fxRackPanel.reverbWidthSlider.getValue() / 100.0f;

    uiFx.preverbAmount   = (float)fxRackPanel.preverbAmountSlider.getValue() / 100.0f;
    uiFx.preverbLengthMs = (float)fxRackPanel.preverbLengthSlider.getValue();
    uiFx.preverbDecaySec = (float)fxRackPanel.preverbDecaySlider.getValue();

    uiFx.distortionAmount = (float)fxRackPanel.distortionAmountSlider.getValue() / 100.0f;
    uiFx.drive            = (float)fxRackPanel.distortionDriveSlider.getValue() / 100.0f;
    uiFx.distortionTone   = (float)fxRackPanel.distortionToneSlider.getValue() / 100.0f;

    uiFx.harmonyAmount    = (float)fxRackPanel.harmonyAmountSlider.getValue() / 100.0f;
    uiFx.harmonyInterval1 = (int)std::llround(fxRackPanel.harmonyInt1Slider.getValue());
    uiFx.harmonyInterval2 = (int)std::llround(fxRackPanel.harmonyInt2Slider.getValue());
    uiFx.harmonyWidth     = (float)fxRackPanel.harmonyWidthSlider.getValue() / 100.0f;

    uiFx.pitchAmount   = (float)fxRackPanel.pitchAmountSlider.getValue() / 100.0f;
    uiFx.semitoneShift = (int)std::llround(fxRackPanel.pitchShiftSlider.getValue());

    auto words = processorRef.getWordsCopy();
    bool anySelected = false;
    for (auto& w : words) {
        if (w.isSelected) {
            w.fx = uiFx;
            anySelected = true;
        }
    }
    if (anySelected)
        processorRef.setWords(std::move(words));

    fxRackPanel.setActiveStates(uiFx);
    updateLyricVisualStates();
    waveformView.repaint();
}

void LyricMomentsEditor::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff121316));

    // Top bar
    g.setColour(juce::Colour(0xff1a1c20));
    g.fillRect(0, 0, getWidth(), 52);

    g.setColour(juce::Colour(0xfff59e0b));
    g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    g.drawText("HONEYVOICE", 16, 12, 130, 28, juce::Justification::centredLeft);

    // Lyric box background
    const auto lyricArea = juce::Rectangle<float>(16.0f, 62.0f, 480.0f, 348.0f);
    g.setColour(juce::Colour(0xff181a1f));
    g.fillRoundedRectangle(lyricArea, 8.0f);
    g.setColour(juce::Colour(0xff272b36));
    g.drawRoundedRectangle(lyricArea, 8.0f, 1.0f);

    if (processorRef.getWordCount() == 0) {
        g.setColour(juce::Colour(0xff9ca3af));
        g.setFont(juce::Font(juce::FontOptions(13.5f)));
        g.drawFittedText(
            "No transcribed lyrics yet.\n\n"
            "1. Play your vocal track in the DAW (live capture)\n"
            "   or click 'Load Audio' / drop an audio file below.\n"
            "2. Click 'Transcribe' to detect words & timestamps.",
            lyricArea.toNearestInt().reduced(24),
            juce::Justification::centred,
            6);
    }

    // Bottom waveform container
    const auto wavePanel = juce::Rectangle<float>(16.0f, 456.0f, 968.0f, 108.0f);
    g.setColour(juce::Colour(0xff181a1f));
    g.fillRoundedRectangle(wavePanel, 8.0f);

    // Status line inside waveform header
    g.setColour(juce::Colour(0xff9ca3af));
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    const auto& tl = processorRef.getTimeline();
    const juce::String srcText = tl.hasAudio()
        ? (tl.getSourceLabel() + " (" + formatTimeSeconds((double)tl.getValidLengthSamples() / processorRef.getCurrentSampleRate()) + ")  |  " + processorRef.getLastTranscriptionStatus())
        : processorRef.getLastTranscriptionStatus();
    g.drawText(srcText, 136, 458, 840, 18, juce::Justification::centredRight, true);
}

void LyricMomentsEditor::resized() {
    searchBox.setBounds      (152, 13, 195, 26);
    muteDryToggle.setBounds  (362, 13, 95,  26);
    loadAudioBtn.setBounds   (618, 13, 110, 26);
    retranscribeBtn.setBounds(740, 13, 116, 26);
    removeAllBtn.setBounds   (868, 13, 116, 26);

    lyricViewport.setBounds(20, 66, 472, 340);

    selectAllBtn.setBounds     (16,  414, 86,  28);
    clearSelBtn.setBounds      (108, 414, 86,  28);
    liveCaptureToggle.setBounds(204, 414, 118, 28);
    clearAudioBtn.setBounds    (406, 414, 90,  28);

    const int tabX = 508;
    const int tabY = 62;
    const int tabW = 112;
    const int tabH = 42;
    const int gap = 7;

    doublerTabBtn.setBounds   (tabX, tabY + (tabH + gap) * 0, tabW, tabH);
    delayTabBtn.setBounds     (tabX, tabY + (tabH + gap) * 1, tabW, tabH);
    reverbTabBtn.setBounds    (tabX, tabY + (tabH + gap) * 2, tabW, tabH);
    preverbTabBtn.setBounds   (tabX, tabY + (tabH + gap) * 3, tabW, tabH);
    distortionTabBtn.setBounds(tabX, tabY + (tabH + gap) * 4, tabW, tabH);
    harmonyTabBtn.setBounds   (tabX, tabY + (tabH + gap) * 5, tabW, tabH);
    pitchTabBtn.setBounds     (tabX, tabY + (tabH + gap) * 6, tabW, tabH);

    fxRackViewport.setBounds(630, 62, 354, 344);
    fxRackPanel.setSize(338, 7 * (110 + 8));

    applyBtn.setBounds (508, 414, 112, 28);
    copyBtn.setBounds  (630, 414, 112, 28);
    pasteBtn.setBounds (751, 414, 112, 28);
    removeBtn.setBounds(872, 414, 112, 28);

    playStopBtn.setBounds(24, 464, 58, 26);
    loopBtn.setBounds    (88, 464, 62, 26);
    timeLabel.setBounds  (24, 494, 126, 20);
    waveformView.setBounds(156, 478, 820, 80);

    if (transcribeOverlay)
        transcribeOverlay->setBounds(getLocalBounds());

    rebuildLyricLayout();
}

void LyricMomentsEditor::timerCallback() {
    playStopBtn.setButtonText(processorRef.isStandalonePlaying() ? "Stop" : "Play");

    const auto& tl = processorRef.getTimeline();
    const double sr = processorRef.getCurrentSampleRate();
    const double curSec = tl.hasAudio()
        ? (double)juce::jmax<int64_t>(0, processorRef.getCurrentPlayheadSample() - tl.getValidStartSample()) / sr
        : 0.0;
    const double totSec = tl.hasAudio()
        ? (double)tl.getValidLengthSamples() / sr
        : 0.0;
    timeLabel.setText(formatTimeSeconds(curSec) + " / " + formatTimeSeconds(totSec), juce::dontSendNotification);

    updateLyricVisualStates();
    waveformView.repaint();
    repaint(136, 456, 845, 22);
}
