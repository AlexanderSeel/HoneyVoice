#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <memory>
#include <vector>

class MomentsLookAndFeel : public juce::LookAndFeel_V4 {
public:
    MomentsLookAndFeel() {
        setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff38bdf8));
        setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff27272a));
        setColour(juce::Slider::thumbColourId, juce::Colours::white);
        setColour(juce::TextButton::buttonColourId, juce::Colour(0xff20232b));
        setColour(juce::TextButton::textColourOffId, juce::Colour(0xffe4e4e7));
        setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        setColour(juce::ToggleButton::textColourId, juce::Colour(0xffe4e4e7));
        setColour(juce::ToggleButton::tickColourId, juce::Colour(0xfff59e0b));
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, const float rotaryStartAngle,
                          const float rotaryEndAngle, juce::Slider& slider) override {
        auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(4.0f);
        auto radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) / 2.0f;
        auto toAngle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        auto lineW = 3.0f;
        auto arcRadius = radius - lineW * 0.5f;

        juce::Path backgroundArc;
        backgroundArc.addCentredArc(bounds.getCentreX(), bounds.getCentreY(), arcRadius, arcRadius,
                                    0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(juce::Colour(0xff2a2d34));
        g.strokePath(backgroundArc, juce::PathStrokeType(lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (slider.isEnabled()) {
            juce::Path valueArc;
            valueArc.addCentredArc(bounds.getCentreX(), bounds.getCentreY(), arcRadius, arcRadius,
                                   0.0f, rotaryStartAngle, toAngle, true);
            g.setColour(slider.findColour(juce::Slider::rotarySliderFillColourId));
            g.strokePath(valueArc, juce::PathStrokeType(lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        g.setColour(juce::Colour(0xff18191c));
        g.fillEllipse(bounds.reduced(6.0f));

        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(11.0f, juce::Font::bold)));
        g.drawText(slider.getTextFromValue(slider.getValue()), bounds, juce::Justification::centred, false);
    }
};

class LyricWordComponent : public juce::Component {
public:
    LyricWordComponent(int index,
                       const LyricWord& wordSnapshot,
                       bool isPlayingNow,
                       std::function<void(int, const juce::MouseEvent&)> onClick,
                       std::function<void(int)> onDoubleClick)
        : wordIndex(index), word(wordSnapshot), playingNow(isPlayingNow),
          clickCallback(std::move(onClick)), doubleClickCallback(std::move(onDoubleClick)) {}

    void setWordState(const LyricWord& snapshot, bool isPlayingNow) {
        word = snapshot;
        playingNow = isPlayingNow;
        repaint();
    }

    void paint(juce::Graphics& g) override {
        auto area = getLocalBounds().toFloat().reduced(1.0f);

        if (word.isSelected) {
            g.setColour(juce::Colour(0xff3b82f6).withAlpha(0.35f));
            g.fillRoundedRectangle(area, 5.0f);
            g.setColour(juce::Colour(0xff60a5fa));
            g.drawRoundedRectangle(area, 5.0f, 1.5f);
        } else if (word.fx.hasAnyEffect()) {
            g.setColour(juce::Colour(0xff272a30));
            g.fillRoundedRectangle(area, 5.0f);
        }

        if (word.isSearchMatch) {
            g.setColour(juce::Colour(0xff2dd4bf));
            g.drawRoundedRectangle(area.reduced(1.0f), 4.0f, 1.4f);
        }

        if (playingNow) {
            g.setColour(juce::Colour(0xfff59e0b));
            g.drawRoundedRectangle(area, 5.0f, 2.0f);
        }

        g.setColour(playingNow ? juce::Colour(0xfffef08a)
                               : (word.isSelected ? juce::Colours::white : juce::Colour(0xffd4d4d8)));
        g.setFont(juce::Font(juce::FontOptions(15.0f, (word.isSelected || playingNow) ? juce::Font::bold : juce::Font::plain)));
        g.drawText(word.word, getLocalBounds().reduced(3, 2), juce::Justification::centred, false);

        float barX = 4.0f;
        const float barY = (float)getHeight() - 3.5f;
        const float barW = 8.0f;
        auto drawTag = [&](bool active, juce::uint32 col) {
            if (!active || barX + barW > (float)getWidth() - 3.0f) return;
            g.setColour(juce::Colour(col));
            g.fillRect(barX, barY, barW, 2.2f);
            barX += barW + 2.0f;
        };
        drawTag(word.fx.doublerActive,    0xff2dd4bf);
        drawTag(word.fx.delayActive,      0xfff59e0b);
        drawTag(word.fx.reverbActive,     0xff3b82f6);
        drawTag(word.fx.preverbActive,    0xff8b5cf6);
        drawTag(word.fx.distortionActive, 0xffef4444);
        drawTag(word.fx.harmonyActive,    0xffec4899);
        drawTag(word.fx.pitchActive,      0xffa855f7);
    }

    void mouseDown(const juce::MouseEvent& e) override {
        if (clickCallback) clickCallback(wordIndex, e);
    }

    void mouseDoubleClick(const juce::MouseEvent&) override {
        if (doubleClickCallback) doubleClickCallback(wordIndex);
    }

private:
    int wordIndex = 0;
    LyricWord word;
    bool playingNow = false;
    std::function<void(int, const juce::MouseEvent&)> clickCallback;
    std::function<void(int)> doubleClickCallback;
};

class WaveformTimelineComponent : public juce::Component,
                                  public juce::FileDragAndDropTarget {
public:
    WaveformTimelineComponent(LyricMomentsProcessor& proc,
                              std::function<void(const juce::File&)> onFileDropped,
                              std::function<void()> onWordsModified);

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray&, int, int) override { dragHover = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { dragHover = false; repaint(); }
    void filesDropped(const juce::StringArray& files, int, int) override;

    void refreshCacheIfNeeded();

private:
    void getViewRange(int64_t& outStart, int64_t& outEnd) const;
    int64_t xToSample(float x) const;
    float sampleToX(int64_t sample) const;

    LyricMomentsProcessor& processor;
    std::function<void(const juce::File&)> fileDroppedCallback;
    std::function<void()> wordsModifiedCallback;

    std::vector<AudioTimeline::PeakPair> cachedPeaks;
    uint64_t cachedVersion = 0;
    int cachedWidth = 0;
    int64_t cachedViewStart = 0;
    int64_t cachedViewEnd = 0;

    bool dragHover = false;
    int draggingWordIndex = -1;
    bool draggingStartEdge = false;
};

class FxRackPanel : public juce::Component {
public:
    FxRackPanel();
    void paint(juce::Graphics& g) override;
    void resized() override;

    void setActiveStates(const WordEffectSettings& fx);
    int getCardY(int cardIndex) const;

    // Doubler
    juce::Slider doublerAmountSlider, doublerDelaySlider, doublerDetuneSlider;
    // Delay
    juce::ToggleButton pingPongToggle { "PING PONG" };
    juce::Slider delayAmountSlider, delayTimeSlider, delayFeedbackSlider;
    // Reverb
    juce::Slider reverbAmountSlider, reverbDecaySlider, reverbWidthSlider;
    // Preverb
    juce::Slider preverbAmountSlider, preverbLengthSlider, preverbDecaySlider;
    // Distortion
    juce::Slider distortionAmountSlider, distortionDriveSlider, distortionToneSlider;
    // Harmony
    juce::Slider harmonyAmountSlider, harmonyInt1Slider, harmonyInt2Slider, harmonyWidthSlider;
    // Pitch
    juce::Slider pitchAmountSlider, pitchShiftSlider;

private:
    struct KnobWithLabel {
        juce::Slider* slider = nullptr;
        juce::Label label;
    };

    void addKnobLabel(juce::Slider& s, const juce::String& name);

    WordEffectSettings currentStates;
    std::vector<std::unique_ptr<KnobWithLabel>> knobLabels;

    static constexpr int kCardHeight = 110;
    static constexpr int kCardGap = 8;
};

class TranscribeOverlayPanel : public juce::Component {
public:
    TranscribeOverlayPanel(std::function<void(VocalTranscriber::EngineMode, const juce::String&, const juce::File&)> onRunTranscribe,
                           std::function<void(const juce::String&)> onAlignLyrics,
                           std::function<void()> onBrowseModel,
                           std::function<void()> onClose);

    void paint(juce::Graphics& g) override;
    void resized() override;

    void setStatusText(const juce::String& text);
    void setModelFile(const juce::File& file);
    void setLyricsText(const juce::String& text);
    juce::String getLyricsText() const { return lyricsEditor.getText(); }

private:
    juce::Label titleLabel;
    juce::Label engineLabel;
    juce::ComboBox engineBox;
    juce::Label modelLabel;
    juce::TextButton browseModelBtn { "Browse Model (.bin)..." };
    juce::Label lyricsPromptLabel;
    juce::TextEditor lyricsEditor;
    juce::Label statusLabel;
    juce::TextButton runTranscribeBtn { "Transcribe Audio" };
    juce::TextButton alignLyricsBtn { "Align Typed Lyrics" };
    juce::TextButton closeBtn { "Close" };

    juce::File selectedModelFile;
    std::function<void(VocalTranscriber::EngineMode, const juce::String&, const juce::File&)> runCallback;
    std::function<void(const juce::String&)> alignCallback;
    std::function<void()> browseModelCallback;
    std::function<void()> closeCallback;
};

class LyricMomentsEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit LyricMomentsEditor(LyricMomentsProcessor&);
    ~LyricMomentsEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

private:
    class TranscribeWorker;

    void rebuildLyricLayout();
    void updateLyricVisualStates();
    void syncControlsFromSelection();
    void applyControlsToSelection();
    void handleWordClick(int index, const juce::MouseEvent& e);
    void handleWordDoubleClick(int index);
    void loadAudioFileAsync(const juce::File& file);
    void startTranscription(VocalTranscriber::EngineMode mode,
                            const juce::String& hintLyrics,
                            const juce::File& modelFile);
    void alignTypedLyrics(const juce::String& lyricsText);
    void openModelBrowser();

    LyricMomentsProcessor& processorRef;
    MomentsLookAndFeel customLookAndFeel;

    juce::TextEditor searchBox;
    juce::ToggleButton muteDryToggle { "Mute Dry" };
    juce::TextButton loadAudioBtn { "Load Audio" };
    juce::TextButton retranscribeBtn { "Transcribe" };
    juce::TextButton removeAllBtn { "Clear FX" };

    juce::Viewport lyricViewport;
    juce::Component lyricContainer;
    juce::OwnedArray<LyricWordComponent> wordComponents;
    int lastClickedWordIndex = -1;

    juce::TextButton selectAllBtn { "Select All" };
    juce::TextButton clearSelBtn  { "Clear Sel" };
    juce::ToggleButton liveCaptureToggle { "DAW Capture" };
    juce::TextButton clearAudioBtn { "Clear Audio" };

    juce::TextButton doublerTabBtn    { "Doubler" };
    juce::TextButton delayTabBtn      { "Delay" };
    juce::TextButton reverbTabBtn     { "Reverb" };
    juce::TextButton preverbTabBtn    { "Preverb" };
    juce::TextButton distortionTabBtn { "Distortion" };
    juce::TextButton harmonyTabBtn    { "Harmony" };
    juce::TextButton pitchTabBtn      { "Pitch" };

    juce::Viewport fxRackViewport;
    FxRackPanel fxRackPanel;

    juce::TextButton applyBtn  { "Apply" };
    juce::TextButton copyBtn   { "Copy" };
    juce::TextButton pasteBtn  { "Paste" };
    juce::TextButton removeBtn { "Remove" };

    juce::TextButton playStopBtn { "Play" };
    juce::ToggleButton loopBtn   { "Loop" };
    juce::Label timeLabel;
    WaveformTimelineComponent waveformView;

    std::unique_ptr<TranscribeOverlayPanel> transcribeOverlay;
    std::unique_ptr<TranscribeWorker> activeWorker;
    std::shared_ptr<juce::FileChooser> activeFileChooser;
    juce::TextEditor inlineWordEditor;
    int inlineEditingWordIndex = -1;

    juce::File selectedWhisperModel;
    WordEffectSettings clipboardSettings;
    bool isSyncingControls = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LyricMomentsEditor)
};
