#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

class ChoirvoyantEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ChoirvoyantEditor (ChoirvoyantProcessor&);
    ~ChoirvoyantEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Knob : juce::Component
    {
        Knob (juce::AudioProcessorValueTreeState& s, const juce::String& id, const juce::String& caption);
        void resized() override;
        juce::Slider slider;
        juce::Label label;
        juce::AudioProcessorValueTreeState::SliderAttachment attachment;
    };

    struct Choice : juce::Component
    {
        Choice (juce::AudioProcessorValueTreeState& s, const juce::String& id, const juce::String& caption);
        void resized() override;
        juce::ComboBox box;
        juce::Label label;
        juce::AudioProcessorValueTreeState::ComboBoxAttachment attachment;
    };

    void timerCallback() override;
    Knob& knob (const juce::String& id, const juce::String& caption);
    Choice& choice (const juce::String& id, const juce::String& caption);

    ChoirvoyantProcessor& proc;
    juce::LookAndFeel_V4 lnf;
    juce::OwnedArray<juce::Component> owned;

    Choice *key, *lookahead, *voiceInterval[3];
    Knob *voices, *dry, *wet;
    Knob *voiceKnobs[3][4];
    Knob *character[6], *vibrato[3], *stack[3];

    juce::String keyText = "Listening...", leadText, harmonyText;
    juce::Rectangle<int> headerArea, voiceArea[3], characterArea, vibratoArea, stackArea, mixArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoirvoyantEditor)
};
