#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

class MomentsLookAndFeel : public juce::LookAndFeel_V4 {
public:
    MomentsLookAndFeel() {
        setColour(juce::Slider::rotarySliderFillColourId, juce::Colour(0xff38bdf8));
        setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff27272a));
        setColour(juce::Slider::thumbColourId, juce::Colours::white);
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
    LyricWordComponent(LyricWord& wordRef, std::function<void()> onSelectionChanged)
        : word(wordRef), callback(std::move(onSelectionChanged)) {}

    void paint(juce::Graphics& g) override {
        auto area = getLocalBounds().toFloat().reduced(1.0f);

        if (word.isSelected) {
            g.setColour(juce::Colour(0xff3b82f6).withAlpha(0.35f));
            g.fillRoundedRectangle(area, 4.0f);
            g.setColour(juce::Colour(0xff60a5fa));
            g.drawRoundedRectangle(area, 4.0f, 1.5f);
        } else if (hasAnyEffect()) {
            g.setColour(juce::Colour(0xff272a30));
            g.fillRoundedRectangle(area, 4.0f);
        }

        g.setColour(word.isSelected ? juce::Colours::white : juce::Colour(0xffd4d4d8));
        g.setFont(juce::Font(juce::FontOptions(16.0f, word.isSelected ? juce::Font::bold : juce::Font::plain)));
        g.drawText(word.word, getLocalBounds(), juce::Justification::centred, false);

        float barX = 3.0f;
        float barY = (float)getHeight() - 3.0f;
        float barW = 10.0f;
        if (word.fx.doublerActive)    { g.setColour(juce::Colour(0xff2dd4bf)); g.fillRect(barX, barY, barW, 2.0f); barX += barW + 2.0f; }
        if (word.fx.delayActive)      { g.setColour(juce::Colour(0xfff59e0b)); g.fillRect(barX, barY, barW, 2.0f); barX += barW + 2.0f; }
        if (word.fx.reverbActive)     { g.setColour(juce::Colour(0xff3b82f6)); g.fillRect(barX, barY, barW, 2.0f); barX += barW + 2.0f; }
        if (word.fx.distortionActive) { g.setColour(juce::Colour(0xffef4444)); g.fillRect(barX, barY, barW, 2.0f); }
    }

    void mouseDown(const juce::MouseEvent&) override {
        word.isSelected = !word.isSelected;
        repaint();
        if (callback) callback();
    }

private:
    bool hasAnyEffect() const {
        return word.fx.doublerActive || word.fx.delayActive || word.fx.reverbActive || word.fx.distortionActive;
    }

    LyricWord& word;
    std::function<void()> callback;
};

class LyricMomentsEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit LyricMomentsEditor(LyricMomentsProcessor&);
    ~LyricMomentsEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

private:
    void rebuildLyricLayout();
    void syncControlsFromSelection();
    void applyControlsToSelection();

    LyricMomentsProcessor& processorRef;
    MomentsLookAndFeel customLookAndFeel;

    juce::TextEditor searchBox;
    juce::ToggleButton muteDryToggle { "Mute Dry" };
    juce::TextButton retranscribeBtn { "Re-transcribe" };
    juce::TextButton removeAllBtn { "Remove All" };

    juce::OwnedArray<LyricWordComponent> wordComponents;

    juce::TextButton doublerTabBtn    { "Doubler" };
    juce::TextButton delayTabBtn      { "Delay" };
    juce::TextButton reverbTabBtn     { "Reverb" };
    juce::TextButton preverbTabBtn    { "Preverb" };
    juce::TextButton distortionTabBtn { "Distortion" };
    juce::TextButton harmonyTabBtn    { "Harmony" };
    juce::TextButton pitchTabBtn      { "Pitch" };

    juce::Slider doublerAmountSlider, doublerDelaySlider, doublerDetuneSlider;

    juce::ToggleButton pingPongToggle { "PING PONG" };
    juce::Slider delayAmountSlider, delayTimeSlider, delayFeedbackSlider;

    juce::Slider reverbAmountSlider, reverbDecaySlider, reverbWidthSlider;

    juce::TextButton applyBtn  { "Apply" };
    juce::TextButton copyBtn   { "Copy" };
    juce::TextButton pasteBtn  { "Paste" };
    juce::TextButton removeBtn { "Remove" };

    WordEffectSettings clipboardSettings;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LyricMomentsEditor)
};
