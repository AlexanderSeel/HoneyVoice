#include "PluginEditor.h"

LyricMomentsEditor::LyricMomentsEditor(LyricMomentsProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p)
{
    setLookAndFeel(&customLookAndFeel);
    setSize(980, 560);

    if (processorRef.words.empty()) {
        std::vector<juce::String> demoLyrics = {
            "I'm", "a", "rebel", "with", "the", "cause,", "I", "fly", "the", "flag", "high",
            "I", "run", "and", "never", "pause,", "I'm", "riding", "til", "I", "die"
        };
        int64_t sampleCursor = 0;
        for (size_t i = 0; i < demoLyrics.size(); ++i) {
            LyricWord w;
            w.word = demoLyrics[i];
            w.startSample = sampleCursor;
            w.endSample = sampleCursor + 22050;
            sampleCursor += 24000;
            processorRef.words.push_back(w);
        }
    }

    searchBox.setTextToShowWhenEmpty("Find words...", juce::Colours::grey);
    searchBox.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff141518));
    addAndMakeVisible(searchBox);

    muteDryToggle.onClick = [this] { processorRef.muteDry = muteDryToggle.getToggleState(); };
    addAndMakeVisible(muteDryToggle);

    removeAllBtn.onClick = [this] {
        for (auto& w : processorRef.words) w.fx = WordEffectSettings{};
        repaint();
    };
    addAndMakeVisible(retranscribeBtn);
    addAndMakeVisible(removeAllBtn);

    auto setupKnob = [this](juce::Slider& s, double min, double max, double val, const juce::String& suffix, juce::Colour accent) {
        s.setSliderStyle(juce::Slider::RotaryVerticalDrag);
        s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        s.setRange(min, max, 0.01);
        s.setValue(val, juce::dontSendNotification);
        s.setTextValueSuffix(suffix);
        s.setColour(juce::Slider::rotarySliderFillColourId, accent);
        s.onValueChange = [this] { applyControlsToSelection(); };
        addAndMakeVisible(s);
    };

    setupKnob(doublerAmountSlider, 0.0, 100.0, 70.0, "%", juce::Colour(0xff2dd4bf));
    setupKnob(doublerDelaySlider, 1.0, 50.0, 10.0, "ms", juce::Colour(0xff2dd4bf));
    setupKnob(doublerDetuneSlider, 0.0, 50.0, 16.0, "c", juce::Colour(0xff2dd4bf));

    setupKnob(delayAmountSlider, 0.0, 100.0, 55.0, "%", juce::Colour(0xfff59e0b));
    setupKnob(delayTimeSlider, 20.0, 1000.0, 345.0, "ms", juce::Colour(0xfff59e0b));
    setupKnob(delayFeedbackSlider, 0.0, 95.0, 60.0, "%", juce::Colour(0xfff59e0b));
    pingPongToggle.setToggleState(true, juce::dontSendNotification);
    pingPongToggle.onClick = [this] { applyControlsToSelection(); };
    addAndMakeVisible(pingPongToggle);

    setupKnob(reverbAmountSlider, 0.0, 100.0, 40.0, "%", juce::Colour(0xff3b82f6));
    setupKnob(reverbDecaySlider, 0.2, 8.0, 3.8, "s", juce::Colour(0xff3b82f6));
    setupKnob(reverbWidthSlider, 0.0, 100.0, 60.0, "%", juce::Colour(0xff3b82f6));

    auto setupFxTab = [this](juce::TextButton& btn, juce::Colour c) {
        btn.setClickingTogglesState(true);
        btn.setColour(juce::TextButton::buttonOnColourId, c.withAlpha(0.3f));
        btn.onClick = [this] { applyControlsToSelection(); repaint(); };
        addAndMakeVisible(btn);
    };

    setupFxTab(doublerTabBtn,    juce::Colour(0xff2dd4bf));
    setupFxTab(delayTabBtn,      juce::Colour(0xfff59e0b));
    setupFxTab(reverbTabBtn,     juce::Colour(0xff3b82f6));
    setupFxTab(preverbTabBtn,    juce::Colour(0xff8b5cf6));
    setupFxTab(distortionTabBtn, juce::Colour(0xffef4444));
    setupFxTab(harmonyTabBtn,    juce::Colour(0xffec4899));
    setupFxTab(pitchTabBtn,      juce::Colour(0xffa855f7));

    addAndMakeVisible(applyBtn);
    addAndMakeVisible(copyBtn);
    addAndMakeVisible(pasteBtn);
    addAndMakeVisible(removeBtn);

    copyBtn.onClick = [this] {
        for (const auto& w : processorRef.words) {
            if (w.isSelected) { clipboardSettings = w.fx; break; }
        }
    };
    pasteBtn.onClick = [this] {
        for (auto& w : processorRef.words) {
            if (w.isSelected) w.fx = clipboardSettings;
        }
        syncControlsFromSelection();
        repaint();
    };
    removeBtn.onClick = [this] {
        for (auto& w : processorRef.words) {
            if (w.isSelected) w.fx = WordEffectSettings{};
        }
        syncControlsFromSelection();
        repaint();
    };

    rebuildLyricLayout();
    startTimerHz(30);
}

LyricMomentsEditor::~LyricMomentsEditor() {
    setLookAndFeel(nullptr);
}

void LyricMomentsEditor::rebuildLyricLayout() {
    wordComponents.clear();
    for (auto& w : processorRef.words) {
        auto* comp = new LyricWordComponent(w, [this] { syncControlsFromSelection(); });
        addAndMakeVisible(comp);
        wordComponents.add(comp);
    }
    resized();
}

void LyricMomentsEditor::syncControlsFromSelection() {
    for (const auto& w : processorRef.words) {
        if (w.isSelected) {
            doublerTabBtn.setToggleState(w.fx.doublerActive, juce::dontSendNotification);
            delayTabBtn.setToggleState(w.fx.delayActive, juce::dontSendNotification);
            reverbTabBtn.setToggleState(w.fx.reverbActive, juce::dontSendNotification);
            distortionTabBtn.setToggleState(w.fx.distortionActive, juce::dontSendNotification);

            doublerAmountSlider.setValue(w.fx.doublerAmount * 100.0, juce::dontSendNotification);
            doublerDelaySlider.setValue(w.fx.doublerDelayMs, juce::dontSendNotification);
            doublerDetuneSlider.setValue(w.fx.doublerDetuneCents, juce::dontSendNotification);

            delayAmountSlider.setValue(w.fx.delayAmount * 100.0, juce::dontSendNotification);
            delayTimeSlider.setValue(w.fx.delayTimeMs, juce::dontSendNotification);
            delayFeedbackSlider.setValue(w.fx.delayFeedback * 100.0, juce::dontSendNotification);
            pingPongToggle.setToggleState(w.fx.pingPong, juce::dontSendNotification);

            reverbAmountSlider.setValue(w.fx.reverbAmount * 100.0, juce::dontSendNotification);
            reverbDecaySlider.setValue(w.fx.reverbDecaySec, juce::dontSendNotification);
            reverbWidthSlider.setValue(w.fx.reverbWidth * 100.0, juce::dontSendNotification);
            break;
        }
    }
}

void LyricMomentsEditor::applyControlsToSelection() {
    for (auto& w : processorRef.words) {
        if (w.isSelected) {
            w.fx.doublerActive    = doublerTabBtn.getToggleState();
            w.fx.delayActive      = delayTabBtn.getToggleState();
            w.fx.reverbActive     = reverbTabBtn.getToggleState();
            w.fx.distortionActive = distortionTabBtn.getToggleState();

            w.fx.doublerAmount      = (float)doublerAmountSlider.getValue() / 100.0f;
            w.fx.doublerDelayMs     = (float)doublerDelaySlider.getValue();
            w.fx.doublerDetuneCents = (float)doublerDetuneSlider.getValue();

            w.fx.delayAmount   = (float)delayAmountSlider.getValue() / 100.0f;
            w.fx.delayTimeMs   = (float)delayTimeSlider.getValue();
            w.fx.delayFeedback = (float)delayFeedbackSlider.getValue() / 100.0f;
            w.fx.pingPong      = pingPongToggle.getToggleState();

            w.fx.reverbAmount   = (float)reverbAmountSlider.getValue() / 100.0f;
            w.fx.reverbDecaySec = (float)reverbDecaySlider.getValue();
            w.fx.reverbWidth    = (float)reverbWidthSlider.getValue() / 100.0f;
        }
    }
    repaint();
}

void LyricMomentsEditor::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff121316));

    g.setColour(juce::Colour(0xff1a1c20));
    g.fillRect(0, 0, getWidth(), 48);

    g.setColour(juce::Colour(0xfff59e0b));
    g.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
    g.drawText("HONEYVOICE", 16, 10, 140, 28, juce::Justification::centredLeft);

    auto lyricArea = juce::Rectangle<float>(16, 60, 480, 390);
    g.setColour(juce::Colour(0xff181a1f));
    g.fillRoundedRectangle(lyricArea, 8.0f);

    auto drawRackCard = [&](juce::Rectangle<float> r, juce::Colour borderCol, const juce::String& title) {
        g.setColour(juce::Colour(0xff1a1d24));
        g.fillRoundedRectangle(r, 8.0f);
        g.setColour(borderCol.withAlpha(0.5f));
        g.drawRoundedRectangle(r, 8.0f, 1.2f);
        g.setColour(borderCol);
        g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
        g.drawText(title, r.reduced(12, 8).removeFromTop(20), juce::Justification::centredLeft);
    };

    drawRackCard({ 635, 60,  329, 112 }, juce::Colour(0xff2dd4bf), "Doubler");
    drawRackCard({ 635, 180, 329, 112 }, juce::Colour(0xfff59e0b), "Delay");
    drawRackCard({ 635, 300, 329, 112 }, juce::Colour(0xff3b82f6), "Reverb");

    auto waveArea = juce::Rectangle<float>(16, 464, 948, 80);
    g.setColour(juce::Colour(0xff181a1f));
    g.fillRoundedRectangle(waveArea, 8.0f);

    g.setColour(juce::Colour(0xff9ca3af).withAlpha(0.45f));
    for (int x = 26; x < 950; x += 4) {
        float h = 10.0f + 26.0f * std::abs(std::sin(x * 0.04f) * std::cos(x * 0.015f));
        g.fillRect((float)x, waveArea.getCentreY() - h * 0.5f, 2.0f, h);
    }
}

void LyricMomentsEditor::resized() {
    searchBox.setBounds(160, 11, 200, 26);
    muteDryToggle.setBounds(450, 11, 100, 26);
    retranscribeBtn.setBounds(730, 11, 110, 26);
    removeAllBtn.setBounds(854, 11, 110, 26);

    int cursorX = 32;
    int cursorY = 78;
    const int maxRight = 480;
    const int rowHeight = 34;

    for (auto* comp : wordComponents) {
        int wordW = 54;
        for (size_t i = 0; i < processorRef.words.size(); ++i) {
            if (wordComponents[(int)i] == comp) {
                wordW = juce::jmax(28, processorRef.words[i].word.length() * 9 + 14);
                break;
            }
        }

        if (cursorX + wordW > maxRight) {
            cursorX = 32;
            cursorY += rowHeight;
        }
        comp->setBounds(cursorX, cursorY, wordW, 28);
        cursorX += wordW + 6;
    }

    int tabX = 510;
    int tabY = 60;
    int tabW = 112;
    int tabH = 38;
    int gap = 8;

    doublerTabBtn.setBounds   (tabX, tabY + (tabH + gap) * 0, tabW, tabH);
    delayTabBtn.setBounds     (tabX, tabY + (tabH + gap) * 1, tabW, tabH);
    reverbTabBtn.setBounds    (tabX, tabY + (tabH + gap) * 2, tabW, tabH);
    preverbTabBtn.setBounds   (tabX, tabY + (tabH + gap) * 3, tabW, tabH);
    distortionTabBtn.setBounds(tabX, tabY + (tabH + gap) * 4, tabW, tabH);
    harmonyTabBtn.setBounds   (tabX, tabY + (tabH + gap) * 5, tabW, tabH);
    pitchTabBtn.setBounds     (tabX, tabY + (tabH + gap) * 6, tabW, tabH);

    doublerAmountSlider.setBounds(655, 92, 64, 64);
    doublerDelaySlider.setBounds (745, 92, 64, 64);
    doublerDetuneSlider.setBounds(835, 92, 64, 64);

    pingPongToggle.setBounds     (750, 185, 110, 22);
    delayAmountSlider.setBounds  (655, 212, 64, 64);
    delayTimeSlider.setBounds    (745, 212, 64, 64);
    delayFeedbackSlider.setBounds(835, 212, 64, 64);

    reverbAmountSlider.setBounds (655, 332, 64, 64);
    reverbDecaySlider.setBounds  (745, 332, 64, 64);
    reverbWidthSlider.setBounds  (835, 332, 64, 64);

    applyBtn.setBounds (510, 420, 112, 30);
    copyBtn.setBounds  (635, 420, 102, 30);
    pasteBtn.setBounds (748, 420, 102, 30);
    removeBtn.setBounds(862, 420, 102, 30);
}

void LyricMomentsEditor::timerCallback() {
    repaint(16, 464, 948, 80);
}
