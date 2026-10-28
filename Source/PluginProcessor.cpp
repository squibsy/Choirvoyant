#include "PluginProcessor.h"
#include "PluginEditor.h"

ChoirvoyantProcessor::ChoirvoyantProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      state (*this, nullptr, "Choirvoyant", Params::createLayout())
{
    auto get = [this] (const juce::String& id) {
        auto* p = state.getRawParameterValue (id);
        jassert (p != nullptr);
        return p;
    };

    using namespace Params;
    for (int v = 0; v < 3; ++v)
        vp[v] = { get (vid (v, interval)), get (vid (v, level)), get (vid (v, pan)), get (vid (v, delay)), get (vid (v, formant)),
                  get (vid (v, position)), get (vid (v, motion)) };

    key = get (Params::key);                 mode = get (Params::mode);             keyLock = get (Params::keyLock);
    harmony = get (Params::harmony);         singOn = get (Params::singOn);         longNote = get (Params::longNote);
    lookahead = get (Params::lookahead);     voices = get (Params::voices);
    stack = get (Params::stack);             stackDetune = get (Params::stackDetune); stackWidth = get (Params::stackWidth);
    entryLag = get (Params::entryLag);       humanize = get (Params::humanize);     autoFormant = get (Params::autoFormant);
    tightness = get (Params::tightness);     minNote = get (Params::minNote);       consonants = get (Params::consonants);
    vibDepth = get (Params::vibDepth);       vibRate = get (Params::vibRate);       vibDelay = get (Params::vibDelay);
    dry = get (Params::dry);                 wet = get (Params::wet);
}

bool ChoirvoyantProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    const bool inOk = in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
    const bool outOk = out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
    return inOk && outOk;
}

HarmonySettings ChoirvoyantProcessor::snapshot() const
{
    auto i = [] (const std::atomic<float>* p) { return juce::roundToInt (p->load()); };
    auto f = [] (const std::atomic<float>* p) { return p->load(); };

    HarmonySettings s;
    for (int v = 0; v < 3; ++v)
    {
        auto& o = s.voice[v];
        o.interval = i (vp[v].interval);
        o.levelDb = f (vp[v].level);
        o.pan = f (vp[v].pan);
        o.delayMs = f (vp[v].delay);
        o.formantSemis = f (vp[v].formant);
        o.position = i (vp[v].position);
        o.motion = i (vp[v].motion);
    }
    s.keyMode = i (key);
    s.scaleMode = i (mode);
    s.keyLock = f (keyLock) > 0.5f;
    s.harmonyMode = i (harmony);
    s.singOn = i (singOn);
    s.longNoteMs = f (longNote);
    s.voices = i (voices);
    s.stack = i (stack);
    s.stackDetuneCents = f (stackDetune);
    s.stackWidth = f (stackWidth);
    s.entryLagMs = f (entryLag);
    s.humanize = f (humanize);
    s.autoFormant = f (autoFormant);
    s.tightness = f (tightness);
    s.minNoteMs = f (minNote);
    s.vibDepthCents = f (vibDepth);
    s.vibRateHz = f (vibRate);
    s.vibDelayMs = f (vibDelay);
    s.consonants = f (consonants);
    s.dryDb = f (dry);
    s.wetDb = f (wet);
    return s;
}

void ChoirvoyantProcessor::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    currentLookahead = juce::roundToInt (lookahead->load());
    engine.prepare (sampleRate, Params::lookaheadSeconds (currentLookahead));
    setLatencySamples (engine.getLatencySamples());
    midiEvents.clear();
    midiEvents.reserve (1024);
}

void ChoirvoyantProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int la = juce::roundToInt (lookahead->load());
    if (la != currentLookahead)
    {
        currentLookahead = la;
        engine.setLookahead (Params::lookaheadSeconds (la));
        setLatencySamples (engine.getLatencySamples());
    }

    const int numIn = juce::jmin (getTotalNumInputChannels(), 2);
    const int numOut = juce::jmin (getTotalNumOutputChannels(), 2);
    if (numOut == 0)
        return;

    const float* ins[2] = { buffer.getReadPointer (0), buffer.getReadPointer (juce::jmin (1, buffer.getNumChannels() - 1)) };
    float* outs[2] = { buffer.getWritePointer (0), buffer.getWritePointer (juce::jmin (1, buffer.getNumChannels() - 1)) };
    // MIDI chords for the chord and MIDI-notes harmony modes (capacity reserved in prepareToPlay)
    midiEvents.clear();
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (midiEvents.size() + 128 > midiEvents.capacity())
            break;
        if (m.isNoteOn())
            midiEvents.push_back ({ meta.samplePosition, true, m.getNoteNumber() });
        else if (m.isNoteOff())
            midiEvents.push_back ({ meta.samplePosition, false, m.getNoteNumber() });
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            for (int n = 0; n < 128; ++n)
                midiEvents.push_back ({ meta.samplePosition, false, n });
    }

    engine.process (ins, juce::jmax (1, numIn), outs, numOut, buffer.getNumSamples(), snapshot(),
                    midiEvents.data(), (int) midiEvents.size());
    midi.clear();
}

void ChoirvoyantProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = state.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void ChoirvoyantProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (state.state.getType()))
            state.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* ChoirvoyantProcessor::createEditor()
{
    return new ChoirvoyantEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ChoirvoyantProcessor();
}
