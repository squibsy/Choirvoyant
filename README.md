# Choirvoyant

A lookahead vocal harmonizer (VST3). Choirvoyant delays its output by a lookahead
time (250 ms to 2 s, reported to the host as latency), so every decision about a sung
note (its key, its chord, how long it lasts, whether it is a passing note) is made
knowing what comes next. A live mode cuts the latency to 30 ms for stage use.

## How it works

1. **Pitch tracking**: FFT-based YIN every ~5 ms, with an octave-error guard for vowels
   whose first formant sits on the second harmonic.
2. **Note segmentation**: frames are grouped into notes (onset, end, median pitch).
3. **Key and tuning detection**: the scale *and* the singer's tuning grid are fitted together
   (every root, at 5-cent grid offsets), scoring how close the sung notes sit to the scale
   plus Krumhansl-Kessler profiles for the tonic. This handles singers who aren't tuned to
   A440 or who drift. A long window (about 6 s plus the lookahead) decides the key, and a
   short one (about 1 s plus the lookahead) watches for notes clearly outside it, so real
   key changes are still followed within a note or two.
4. **Harmony**, one of four modes:
   - **Chords (Auto)** (default): a chord is chosen for every lead note from the key's
     triads (with the major V in minor keys). The choice is a small Viterbi search over the
     notes in the lookahead, using functional progression costs (I→IV→V→I cheap, V→IV
     dear), so a passing note doesn't change the chord. The voices are then voiced together
     from that chord: each stays in its register (Position), moves as little as possible,
     the chord keeps its 3rd, no two voices double the 3rd or 5th, and parallel 5ths and
     octaves against the lead are avoided.
   - **Chords (MIDI)**: the chord held on MIDI at each note's start (up to 100 ms late
     still counts). With no chord held, it falls back to Auto.
   - **MIDI Notes**: each voice sings a held MIDI note exactly (voice 1 the highest).
   - **Intervals**: v1 behaviour, each voice a fixed number of scale steps from the lead.
5. **Synthesis**: TD-PSOLA per voice, which keeps formants by default. *Auto Formant*
   shifts formants by a share of each voice's long-term interval, so a harmony a 3rd up
   sounds like a slightly smaller singer rather than a sped-up one.

## Controls

| Section | Controls |
|---|---|
| Header | Key (Auto or a tonic), Mode (Auto / Major / Minor), Lookahead (Live 30 ms, 250 ms to 2 s), Key Lock, and a live readout of key, tuning offset, chord, lead and harmony notes |
| Voice 1-3 | Chord modes: Position (Above, Above Wide, Below, Below Wide, Bass) and Motion (Follow, Hold, Contrary). Intervals mode: Interval. Always: Level, Pan, Delay, Formant offset |
| Character | Entry Lag (late entry that catches up before the note ends), Humanize (timing and pitch drift), Auto Formant, Tightness (0 = follow the singer's inflections, 100% = harmonies exactly in tune), Min Note, Consonants (pulls sibilants and breaths out of the harmonies) |
| Harmony | Harmony mode, Sing On (All Notes, Long Notes, Phrase Ends), Long Note threshold |
| Vibrato | Depth, Rate, Delay. Only on notes long enough to carry it, out of phase between voices |
| Stack | Copies per voice (1-3), Detune, Width |
| Mix | Voices (1-3), Dry, Harmony |

**Motion**: *Follow* voice-leads with the lead; *Hold* keeps its note while it still fits
the chord (sustained "ooh" lines); *Contrary* prefers moving against the lead.
**Key Lock** freezes the key currently detected; the tuning grid keeps following the singer.

**Live mode** keeps only what the pitch analysis needs (30 ms). Without lookahead the key
needs a couple of seconds to settle, the lowest trackable pitch rises to about G2 (94 Hz),
Entry Lag and Min Note are off, Long Notes fades a voice in once the note has lasted long
enough, and Phrase Ends acts like All Notes.

To drive Chords (MIDI) or MIDI Notes, route a MIDI track to Choirvoyant's MIDI input.

## Build

```
git submodule update --init
cmake -S . -B build -A x64
cmake --build build --config Release --target Choirvoyant_VST3
```

The plugin lands in `build/Choirvoyant_artefacts/Release/VST3/Choirvoyant.vst3`.

## Tests

`EngineTest` drives the engine on synthetic sung melodies:

```
build/EngineTest_artefacts/Release/EngineTest.exe                       # all checks (key, intervals, chords, MIDI, Sing On, live, CPU)
build/EngineTest_artefacts/Release/EngineTest.exe --in vocal.wav        # harmonize a file into renders/ (default settings)
build/EngineTest_artefacts/Release/EngineTest.exe --vst3 <plugin.vst3>  # load and render through the plugin
build/EngineTest_artefacts/Release/EngineTest.exe --ui <plugin.vst3> ui.png
```

`--in` options via environment: `CHV_KEY=1..12` fixes the tonic, `CHV_TAG=name` tags the output
file, `CHV_NOTES=1` prints the detected notes and chords, `CHV_PITCH=1` prints the raw pitch track.

## Known limits

- Monophonic sources only. Chords or a bleed-heavy mic will confuse the tracker.
- Auto key works well on steady singers. With untrained singers whose notes scatter
  by ±50 cents it can flip between neighbouring keys, so set **Key** to the song's key;
  the tuning grid still follows the singer's drift.
- A melody that never sings the note separating two keys (for example, no 7th degree)
  can flip between those two. Chords (MIDI) avoids the question entirely.
- Automatic chords are diatonic triads only (no secondary dominants or borrowed chords);
  play them on MIDI for anything more colourful.
- Intervals mode uses the key's major / natural-minor scale.
- Changing Lookahead resets the engine and changes the reported latency.
- Checked on synthetic vocals and two Wikimedia Commons recordings (see `EngineTest --in`);
  not yet tried in a DAW session.
