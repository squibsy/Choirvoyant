#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "HarmonyEngine.h"
#include "Params.h"

class ChoirvoyantProcessor : public juce::AudioProcessor
{
public:
    ChoirvoyantProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override { engine.reset(); }
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }   // chords for the chord / MIDI-notes harmony modes
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.5; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState state;
    HarmonyEngine engine;

private:
    struct VoicePtrs { std::atomic<float> *interval, *level, *pan, *delay, *formant, *position, *motion; };
    VoicePtrs vp[3] {};
    std::atomic<float> *key, *mode, *keyLock, *harmony, *singOn, *longNote, *lookahead, *voices, *stack, *stackDetune,
                       *stackWidth, *entryLag, *humanize, *autoFormant, *tightness, *minNote, *vibDepth, *vibRate, *vibDelay,
                       *consonants, *dry, *wet;

    HarmonySettings snapshot() const;
    std::vector<HarmonyEngine::MidiEvent> midiEvents;

    int currentLookahead = -1;
    double currentSampleRate = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoirvoyantProcessor)
};
