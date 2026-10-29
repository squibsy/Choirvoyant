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

    int intParam (juce::AudioProcessorValueTreeState& s, const char* id) { return juce::roundToInt (s.getRawParameterValue (id)->load()); }
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
ChoirvoyantEditor::ChoirvoyantEditor (ChoirvoyantProcessor& p)
    : AudioProcessorEditor (p), proc (p), keyLockAttachment (p.state, Params::keyLock, keyLock)
{
    lnf.setColourScheme ({ background, panel, panel.brighter (0.1f), dim, text, accent, juce::Colours::black, accent, text });
    lnf.setColour (juce::Slider::rotarySliderFillColourId, accent);
    lnf.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    lnf.setColour (juce::ToggleButton::tickColourId, accent);
    setLookAndFeel (&lnf);

    using namespace Params;
    key = &choice (Params::key, "Key");
    mode = &choice (Params::mode, "Mode");
    lookahead = &choice (Params::lookahead, "Lookahead");
    addAndMakeVisible (keyLock);

    harmonyMode = &choice (harmony, "Harmony");
    singOn = &choice (Params::singOn, "Sing On");
    longNote = &knob (Params::longNote, "Long Note");

    voices = &knob (Params::voices, "Voices");
    dry = &knob (Params::dry, "Dry");
    wet = &knob (Params::wet, "Harmony");

    for (int v = 0; v < 3; ++v)
    {
        voiceInterval[v] = &choice (vid (v, interval), "Interval");
        voicePosition[v] = &choice (vid (v, position), "Position");
        voiceMotion[v] = &choice (vid (v, motion), "Motion");
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

    setSize (1080, 630);
    timerCallback();
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
    auto& e = proc.engine;
    const int root = e.statusRoot.load();
    const int cents = juce::roundToInt (e.statusTuning.load() * 100.0f);
    const auto newKey = root < 0 ? juce::String ("Listening...")
                                 : keyName (root, e.statusMinor.load()) + (cents == 0 ? juce::String() : juce::String::formatted ("  %+d ct", cents));

    Chords::Chord c;
    c.root = e.statusChordRoot.load();
    c.quality = (Chords::Quality) e.statusChordQuality.load();
    c.mask = c.root >= 0 ? (uint16_t) 1 : (uint16_t) 0;
    char chordBuf[16];
    Chords::name (c, chordBuf, (int) sizeof (chordBuf));
    const juce::String newChord (chordBuf);

    const float lead = e.statusLeadMidi.load();
    const auto newLead = lead > 0.0f ? noteName (lead) : juce::String ("-");

    juce::StringArray h;
    const int numVoices = intParam (proc.state, Params::voices);
    for (int v = 0; v < numVoices; ++v)
    {
        const float t = e.statusTarget[v].load();
        h.add (t > 0.0f ? noteName (t) : juce::String ("-"));
    }
    const auto newHarmony = h.joinIntoString ("  ");

    // show the controls the current modes use; dim the ones that do nothing right now
    const int hm = intParam (proc.state, Params::harmony);
    const bool intervals = hm == HarmonyModes::Intervals, midiNotes = hm == HarmonyModes::MidiNotes;
    const bool live = intParam (proc.state, Params::lookahead) == 0;
    for (int v = 0; v < 3; ++v)
    {
        const float alpha = v < numVoices ? 1.0f : 0.35f;
        voiceInterval[v]->setVisible (intervals);
        voicePosition[v]->setVisible (! intervals);
        voiceMotion[v]->setVisible (! intervals);
        voiceInterval[v]->setAlpha (alpha);
        voicePosition[v]->setAlpha (midiNotes ? 0.35f : alpha);
        voiceMotion[v]->setAlpha (midiNotes ? 0.35f : alpha);
        for (auto* k : voiceKnobs[v])
            k->setAlpha (alpha);
    }
    const int sing = intParam (proc.state, Params::singOn);
    longNote->setAlpha (sing == SingModes::LongNotes ? 1.0f : 0.35f);
    // Phrase Ends needs to see the pause after a phrase's last note, which 1 s of lookahead often can't
    const bool phraseHint = sing == SingModes::PhraseEnds && intParam (proc.state, Params::lookahead) < 4;
    singOn->label.setText (phraseHint ? "Sing On  (best with 2 s)" : "Sing On", juce::dontSendNotification);
    singOn->label.setColour (juce::Label::textColourId, phraseHint ? accent : dim);
    character[0]->setAlpha (live ? 0.35f : 1.0f);   // Entry Lag and Min Note need the lookahead
    character[4]->setAlpha (live ? 0.35f : 1.0f);
    mode->setAlpha (1.0f);

    if (newKey != keyText || newChord != chordText || newLead != leadText || newHarmony != harmonyText)
    {
        keyText = newKey;
        chordText = newChord;
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
    g.drawText ("Choirvoyant", h.removeFromLeft (190), juce::Justification::centredLeft);

    auto readout = h.reduced (0, 8);
    g.setColour (panel);
    g.fillRoundedRectangle (readout.toFloat(), 6.0f);
    auto rr = readout.reduced (12, 4);
    auto label = [&g, &rr] (const char* s, int w) {
        g.setFont (juce::FontOptions (12.0f));
        g.setColour (dim);
        g.drawText (s, rr.removeFromLeft (w), juce::Justification::centredLeft);
    };
    label ("KEY", 30);
    g.setColour (accent);
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    g.drawText (keyText, rr.removeFromLeft (150), juce::Justification::centredLeft);
    label ("CHORD", 46);
    g.setColour (accent);
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    g.drawText (chordText, rr.removeFromLeft (56), juce::Justification::centredLeft);
    label ("LEAD", 36);
    g.setColour (text);
    g.drawText (leadText + "   " + harmonyText, rr, juce::Justification::centredLeft);

    for (int v = 0; v < 3; ++v)
        section (voiceArea[v], "VOICE " + juce::String (v + 1), voiceColour[v]);
    section (characterArea, "CHARACTER", text);
    section (harmonyArea, "HARMONY", text);
    section (vibratoArea, "VIBRATO", text);
    section (stackArea, "STACK", text);
    section (mixArea, "MIX", text);
}

void ChoirvoyantEditor::resized()
{
    auto r = getLocalBounds().reduced (12);

    // header row: name + readout, then the key controls
    auto header = r.removeFromTop (60);
    auto controls = header.removeFromRight (440);
    headerArea = header;
    controls.removeFromLeft (14);
    key->setBounds (controls.removeFromLeft (90).withSizeKeepingCentre (84, 42));
    mode->setBounds (controls.removeFromLeft (96).withSizeKeepingCentre (90, 42));
    lookahead->setBounds (controls.removeFromLeft (130).withSizeKeepingCentre (124, 42));
    keyLock.setBounds (controls.withSizeKeepingCentre (controls.getWidth() - 8, 24).translated (0, 8));
    r.removeFromTop (14);

    // voices: Interval (Intervals mode) or Position + Motion (chord modes) share the top line
    auto voiceRow = r.removeFromTop (240);
    const int vw = (voiceRow.getWidth() - 20) / 3;
    for (int v = 0; v < 3; ++v)
    {
        voiceArea[v] = voiceRow.removeFromLeft (vw);
        voiceRow.removeFromLeft (10);
        auto a = voiceArea[v].reduced (10).withTrimmedTop (20);
        auto top = a.removeFromTop (44);
        voiceInterval[v]->setBounds (top);
        voicePosition[v]->setBounds (top.removeFromLeft (top.getWidth() / 2 - 4));
        top.removeFromLeft (8);
        voiceMotion[v]->setBounds (top);
        a.removeFromTop (4);
        auto upper = a.removeFromTop (a.getHeight() / 2), lower = a;
        voiceKnobs[v][0]->setBounds (upper.removeFromLeft (upper.getWidth() / 2));
        voiceKnobs[v][1]->setBounds (upper);
        voiceKnobs[v][2]->setBounds (lower.removeFromLeft (lower.getWidth() / 2));
        voiceKnobs[v][3]->setBounds (lower);
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

    // harmony + vibrato + stack + mix
    const int quarter = (r.getWidth() - 30) / 4;
    harmonyArea = r.removeFromLeft (quarter);
    r.removeFromLeft (10);
    vibratoArea = r.removeFromLeft (quarter);
    r.removeFromLeft (10);
    stackArea = r.removeFromLeft (quarter);
    r.removeFromLeft (10);
    mixArea = r;
    {
        auto a = harmonyArea.reduced (10).withTrimmedTop (18);
        longNote->setBounds (a.removeFromRight (80));
        a.removeFromRight (6);
        harmonyMode->setBounds (a.removeFromTop (a.getHeight() / 2));
        singOn->setBounds (a);
    }
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
