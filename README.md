# Choirvoyant

A lookahead vocal harmonizer (VST3). Choirvoyant delays its output by a lookahead
time (250 ms to 2 s, reported to the host as latency), so every decision about a sung
note (its key, how long it lasts, whether it is a passing note) is made knowing what
comes next.

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
4. **Harmony**: each voice moves the lead by scale steps of the detected key. A note
   shorter than *Min Note* keeps the previous harmony note.
5. **Synthesis**: TD-PSOLA per voice, which keeps formants by default. *Auto Formant*
   shifts formants by a share of each voice's long-term interval, so a harmony a 3rd up
   sounds like a slightly smaller singer rather than a sped-up one.

## Controls

| Section | Controls |
|---|---|
| Header | Key (Auto or fixed scale), Lookahead, live key, lead and harmony readout |
| Voice 1-3 | Interval (Unison, 2nd to 6th up, Octave up, 3rd to 6th down, Octave down), Level, Pan, Delay, Formant offset |
| Character | Entry Lag (late entry that catches up before the note ends), Humanize (timing and pitch drift), Auto Formant, Tightness (0 = follow the singer's inflections, 100% = harmonies exactly in tune), Min Note, Consonants (pulls sibilants and breaths out of the harmonies) |
| Vibrato | Depth, Rate, Delay. Only on notes long enough to carry it, out of phase between voices |
| Stack | Copies per voice (1-3), Detune, Width |
| Mix | Voices (1-3), Dry, Harmony |

## Build

```
git submodule update --init
cmake -S . -B build -A x64
cmake --build build --config Release --target Choirvoyant_VST3
```

The plugin lands in `build/Choirvoyant_artefacts/Release/VST3/Choirvoyant.vst3`.

## Tests

`EngineTest` drives the engine on a synthetic sung melody:

```
build/EngineTest_artefacts/Release/EngineTest.exe                       # key + harmony pitch checks, CPU
build/EngineTest_artefacts/Release/EngineTest.exe --in vocal.wav        # harmonize a file into renders/
build/EngineTest_artefacts/Release/EngineTest.exe --vst3 <plugin.vst3>  # load and render through the plugin
build/EngineTest_artefacts/Release/EngineTest.exe --ui <plugin.vst3> ui.png
```

## Known limits (v1)

- Monophonic sources only. Chords or a bleed-heavy mic will confuse the tracker.
- Harmonies use the major scale of the detected key (natural minor for minor keys);
  harmonic minor and chord-aware voicing are not in v1.
- Changing Lookahead resets the engine and changes the reported latency.
- Auto key works well on steady singers. With untrained singers whose notes scatter
  by ±50 cents it can flip between neighbouring keys, so set **Key** to the song's key;
  the tuning grid still follows the singer's drift.
- A melody that never sings the note separating two keys (for example, no 7th degree)
  can flip between those two; only harmonies that land on that note differ.
- Checked on synthetic vocals and two Wikimedia Commons recordings (see `EngineTest --in`);
  not yet tried in a DAW session.
