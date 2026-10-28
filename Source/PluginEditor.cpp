#include "PluginEditor.h"

namespace
{
    const juce::Colour background (0xff15171c), panel (0xff1f232b), accent (0xff7fd1c7), text (0xffe6e8ec), dim (0xff8a909c);
    const juce::Colour voiceColour[3] = { juce::Colour (0xff7fd1c7), juce::Colour (0xffe0a96d), juce::Colour (0xffb59cf0) };

    juce::String noteName (float midi)
    {
        static const char* n[12] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        const int m = juce::roundToInt (midi);
        return juce::String (n[((m % 12) + 12) % 12]) + juce::String (m / 12 - 1);
    }

    juce::String keyName (int root, bool minor)
    {
        static const char* n[12] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        return minor ? juce::String (n[(root + 9) % 12]) + " minor" : juce::String (n[root]) + " major";
    }
}

//==============================================================================
ChoirvoyantEditor::Knob::Knob (juce::AudioProcessorValueTreeState& s, const juce::String& id, const juce::String& caption)
    : slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow),
      attachment (s, id, slider)
{
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 76, 16);
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    label.setText (caption, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (juce::FontOptions (12.0f));
    label.setColour (juce::Label::textColourId, dim);
    addAndMakeVisible (slider);
    addAndMakeVisible (label);
}

void ChoirvoyantEditor::Knob::resized()
{
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (16));
    slider.setBounds (r);
}

ChoirvoyantEditor::Choice::Choice (juce::AudioProcessorValueTreeState& s, const juce::String& id, const juce::String& caption)
    : attachment ((box.addItemList (dynamic_cast<juce::AudioParameterChoice*> (s.getParameter (id))->choices, 1), s), id, box)
{
    label.setText (caption, juce::dontSendNotification);
    label.setFont (juce::FontOptions (12.0f));
    label.setColour (juce::Label::textColourId, dim);
    addAndMakeVisible (box);
    addAndMakeVisible (label);
}

void ChoirvoyantEditor::Choice::resized()
{
    auto r = getLocalBounds();
    label.setBounds (r.removeFromTop (16));
    box.setBounds (r.removeFromTop (24));
}

//==============================================================================
ChoirvoyantEditor::ChoirvoyantEditor (ChoirvoyantProcessor& p) : AudioProcessorEditor (p), proc (p)
{
    lnf.setColourScheme ({ background, panel, panel.brighter (0.1f), dim, text, accent, juce::Colours::black, accent, text });
    lnf.setColour (juce::Slider::rotarySliderFillColourId, accent);
    lnf.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setLookAndFeel (&lnf);

    using namespace Params;
    key = &choice (Params::key, "Key");
    lookahead = &choice (Params::lookahead, "Lookahead");
    voices = &knob (Params::voices, "Voices");
    dry = &knob (Params::dry, "Dry");
    wet = &knob (Params::wet, "Harmony");

    for (int v = 0; v < 3; ++v)
    {
        voiceInterval[v] = &choice (vid (v, interval), "Interval");
        voiceKnobs[v][0] = &knob (vid (v, level), "Level");
        voiceKnobs[v][1] = &knob (vid (v, pan), "Pan");
        voiceKnobs[v][2] = &knob (vid (v, delay), "Delay");
        voiceKnobs[v][3] = &knob (vid (v, formant), "Formant");
        for (auto* k : voiceKnobs[v])
            k->slider.setColour (juce::Slider::rotarySliderFillColourId, voiceColour[v]);
    }

    character[0] = &knob (entryLag, "Entry Lag");
    character[1] = &knob (humanize, "Humanize");
    character[2] = &knob (autoFormant, "Auto Formant");
    character[3] = &knob (tightness, "Tightness");
    character[4] = &knob (minNote, "Min Note");
    character[5] = &knob (consonants, "Consonants");
    vibrato[0] = &knob (vibDepth, "Depth");
    vibrato[1] = &knob (vibRate, "Rate");
    vibrato[2] = &knob (vibDelay, "Delay");
    stack[0] = &knob (Params::stack, "Copies");
    stack[1] = &knob (stackDetune, "Detune");
    stack[2] = &knob (stackWidth, "Width");

    setSize (900, 640);
    startTimerHz (15);
}

ChoirvoyantEditor::~ChoirvoyantEditor()
{
    setLookAndFeel (nullptr);
}

ChoirvoyantEditor::Knob& ChoirvoyantEditor::knob (const juce::String& id, const juce::String& caption)
{
    auto* k = new Knob (proc.state, id, caption);
    owned.add (k);
    addAndMakeVisible (k);
    return *k;
}

ChoirvoyantEditor::Choice& ChoirvoyantEditor::choice (const juce::String& id, const juce::String& caption)
{
    auto* c = new Choice (proc.state, id, caption);
    owned.add (c);
    addAndMakeVisible (c);
    return *c;
}

void ChoirvoyantEditor::timerCallback()
{
    const int root = proc.engine.statusRoot.load();
    const int cents = juce::roundToInt (proc.engine.statusTuning.load() * 100.0f);
    const auto newKey = root < 0 ? juce::String ("Listening...")
                                 : keyName (root, proc.engine.statusMinor.load()) + (cents == 0 ? juce::String() : juce::String::formatted ("  %+d ct", cents));
    const float lead = proc.engine.statusLeadMidi.load();
    const auto newLead = lead > 0.0f ? noteName (lead) : juce::String ("-");

    juce::StringArray h;
    const int numVoices = juce::roundToInt (proc.state.getRawParameterValue (Params::voices)->load());
    for (int v = 0; v < numVoices; ++v)
    {
        const float t = proc.engine.statusTarget[v].load();
        h.add (t > 0.0f ? noteName (t) : juce::String ("-"));
    }
    const auto newHarmony = h.joinIntoString ("  ");

    for (int v = 0; v < 3; ++v)
    {
        const float alpha = v < numVoices ? 1.0f : 0.35f;
        voiceInterval[v]->setAlpha (alpha);
        for (auto* k : voiceKnobs[v])
            k->setAlpha (alpha);
    }

    if (newKey != keyText || newLead != leadText || newHarmony != harmonyText)
    {
        keyText = newKey;
        leadText = newLead;
        harmonyText = newHarmony;
        repaint (headerArea);
    }
}

void ChoirvoyantEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);

    auto section = [&g] (juce::Rectangle<int> r, const juce::String& title, juce::Colour c) {
        g.setColour (panel);
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
        g.setColour (c);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (title, r.reduced (10, 6).removeFromTop (16), juce::Justification::left);
    };

    // header: name and live readout
    auto h = headerArea;
    g.setColour (text);
    g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    g.drawText ("Choirvoyant", h.removeFromLeft (200), juce::Justification::centredLeft);

    auto readout = h.removeFromLeft (300).reduced (0, 8);
    g.setColour (panel);
    g.fillRoundedRectangle (readout.toFloat(), 6.0f);
    auto rr = readout.reduced (12, 4);
    g.setFont (juce::FontOptions (12.0f));
    g.setColour (dim);
    g.drawText ("KEY", rr.removeFromLeft (30), juce::Justification::centredLeft);
    g.setColour (accent);
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    g.drawText (keyText, rr.removeFromLeft (150), juce::Justification::centredLeft);
    g.setFont (juce::FontOptions (12.0f));
    g.setColour (dim);
    g.drawText ("LEAD", rr.removeFromLeft (36), juce::Justification::centredLeft);
    g.setColour (text);
    g.drawText (leadText + "   " + harmonyText, rr, juce::Justification::centredLeft);

    for (int v = 0; v < 3; ++v)
        section (voiceArea[v], "VOICE " + juce::String (v + 1), voiceColour[v]);
    section (characterArea, "CHARACTER", text);
    section (vibratoArea, "VIBRATO", text);
    section (stackArea, "STACK", text);
    section (mixArea, "MIX", text);
}

void ChoirvoyantEditor::resized()
{
    auto r = getLocalBounds().reduced (12);

    // header row
    auto header = r.removeFromTop (60);
    headerArea = header.removeFromLeft (510);
    header.removeFromLeft (20);
    key->setBounds (header.removeFromLeft (180).withSizeKeepingCentre (170, 42));
    header.removeFromLeft (16);
    lookahead->setBounds (header.removeFromLeft (130).withSizeKeepingCentre (120, 42));
    r.removeFromTop (14);

    // voices
    auto voiceRow = r.removeFromTop (250);
    const int vw = (voiceRow.getWidth() - 20) / 3;
    for (int v = 0; v < 3; ++v)
    {
        voiceArea[v] = voiceRow.removeFromLeft (vw);
        voiceRow.removeFromLeft (10);
        auto a = voiceArea[v].reduced (10).withTrimmedTop (20);
        voiceInterval[v]->setBounds (a.removeFromTop (44));
        a.removeFromTop (6);
        auto top = a.removeFromTop (a.getHeight() / 2), bottom = a;
        voiceKnobs[v][0]->setBounds (top.removeFromLeft (top.getWidth() / 2));
        voiceKnobs[v][1]->setBounds (top);
        voiceKnobs[v][2]->setBounds (bottom.removeFromLeft (bottom.getWidth() / 2));
        voiceKnobs[v][3]->setBounds (bottom);
    }
    r.removeFromTop (10);

    // character row
    characterArea = r.removeFromTop (120);
    {
        auto a = characterArea.reduced (10).withTrimmedTop (18);
        const int w = a.getWidth() / 6;
        for (auto* k : character)
            k->setBounds (a.removeFromLeft (w));
    }
    r.removeFromTop (10);

    // vibrato + stack + mix
    const int third = (r.getWidth() - 20) / 3;
    vibratoArea = r.removeFromLeft (third);
    r.removeFromLeft (10);
    stackArea = r.removeFromLeft (third);
    r.removeFromLeft (10);
    mixArea = r;
    auto layoutThree = [] (juce::Rectangle<int> area, Knob* const* ks) {
        auto a = area.reduced (10).withTrimmedTop (18);
        const int w = a.getWidth() / 3;
        for (int i = 0; i < 3; ++i)
            ks[i]->setBounds (a.removeFromLeft (w));
    };
    layoutThree (vibratoArea, vibrato);
    layoutThree (stackArea, stack);
    Knob* const mix[3] = { voices, dry, wet };
    layoutThree (mixArea, mix);
}
