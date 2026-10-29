#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

class ChoirvoyantEditor : public juce::AudioProcessorEditor, private juce::Timer, private juce::AudioProcessorParameter::Listener
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

    // M / S toggle pair for one source
    struct MuteSolo : juce::Component
    {
        MuteSolo (juce::AudioProcessorValueTreeState& s, const juce::String& muteId, const juce::String& soloId);
        void resized() override;
        juce::TextButton mute { "M" }, solo { "S" };
        juce::AudioProcessorValueTreeState::ButtonAttachment muteAttachment, soloAttachment;
    };

    void timerCallback() override;
    void parameterValueChanged (int, float newValue) override;
    void parameterGestureChanged (int, bool starting) override;
    Knob& knob (const juce::String& id, const juce::String& caption);
    Choice& choice (const juce::String& id, const juce::String& caption);

    ChoirvoyantProcessor& proc;
    juce::LookAndFeel_V4 lnf;
    juce::OwnedArray<juce::Component> owned;

    Choice *key, *mode, *lookahead, *source, *harmonyMode, *singOn;
    Choice *voiceInterval[3], *voicePosition[3], *voiceMotion[3];
    juce::ToggleButton keyLock { "Key Lock" };
    juce::AudioProcessorValueTreeState::ButtonAttachment keyLockAttachment;
    Knob *voices, *dry, *wet, *longNote;
    Knob *voiceKnobs[3][4];
    Knob *character[6], *vibrato[3], *stack[3], *doubleKnobs[5];
    MuteSolo *voiceMuteSolo[3], *dryMuteSolo, *doubleMuteSolo;
    MuteSolo& muteSolo (const juce::String& muteId, const juce::String& soloId);

    juce::AudioProcessorParameter* sourceParam = nullptr;
    std::atomic<bool> sourceGesture { false }, switchToIntervals { false };

    juce::String keyText = "Listening...", chordText = "-", leadText, harmonyText;
    juce::Rectangle<int> headerArea, voiceArea[3], characterArea, harmonyArea, vibratoArea, stackArea, doubleArea, mixArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoirvoyantEditor)
};
