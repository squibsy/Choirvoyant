#include "HarmonyEngine.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr double pi = 3.14159265358979323846;
    constexpr int chunkSize = 512;
    constexpr float minHz = 70.0f, maxHz = 1600.0f;   // up to ~G6: the top of a guitar neck, and any singer
    constexpr float deepMinHz = 30.0f;                // Instrument: down to bass guitar
    constexpr float consonantFloor = 0.001f;          // rms (-60 dBFS): below this, between notes is silence
                                                      // (only gates copying the source, so erring low is harmless)
    constexpr float minBeta = 0.6f, maxBeta = 1.7f;

    int64_t nextPow2 (int64_t x)
    {
        int64_t p = 1;
        while (p < x)
            p <<= 1;
        return p;
    }

    int64_t floorDiv (int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

    float dbToGain (float db, float floorDb) { return db <= floorDb ? 0.0f : std::pow (10.0f, db / 20.0f); }

    // Moves the lead by `steps` degrees of the major scale on `root`. The lead is first snapped to
    // the scale degree nearest its actual (fractional) pitch, so a note sung a little flat or sharp
    // is heard as the scale note it was aiming for; ties go down. `snapped` receives that degree.
    float diatonic (float lead, int root, int steps, int& snapped)
    {
        static const int scale[7] = { 0, 2, 4, 5, 7, 9, 11 };
        const int64_t base = floorDiv ((int64_t) std::floor (lead) - root, 12);

        int64_t bestOct = 0;
        int deg = 0;
        float bestDist = 1.0e9f;
        for (int64_t o = base - 1; o <= base + 1; ++o)
            for (int d = 0; d < 7; ++d)
            {
                const float dist = std::abs (lead - (float) (root + 12 * o + scale[d]));
                if (dist < bestDist)
                {
                    bestDist = dist;
                    bestOct = o;
                    deg = d;
                }
            }
        snapped = (int) (root + 12 * bestOct + scale[deg]);

        const int64_t d2 = deg + steps;
        const int64_t o2 = floorDiv (d2, 7);
        return (float) (root + 12 * (bestOct + o2) + scale[d2 - 7 * o2]);
    }
}

//==============================================================================
void HarmonyEngine::prepare (double sampleRate, double lookaheadSeconds)
{
    sr = sampleRate;
    hop = 256;
    while (sr / hop > 260.0)
        hop *= 2;
    // Voice: 70 Hz and up. Instrument: a frame twice as long reaches 30 Hz (bass guitar, baritone).
    // Live: a half-length frame, less delay but a higher floor (~94 Hz at 48 kHz). All allocated here.
    trackerFull.prepare (sr, hop * 8, minHz, maxHz);
    trackerDeep.prepare (sr, hop * 16, deepMinHz, maxHz);
    trackerLive.prepare (sr, hop * 4, minHz, maxHz);
    frameLen = hop * 16;

    maxHalf = (int) std::ceil (sr / deepMinHz / minBeta) + 4;
    uvSpacing = (float) (sr * 0.005);

    // lookahead + per-voice delay (250 ms) + entry lag (~200 ms) + analysis frame
    const int64_t bufferLen = nextPow2 ((int64_t) (sr * (maxLookaheadSeconds + 0.6)) + frameLen * 2);
    src.assign ((size_t) bufferLen, 0.0f);
    dryL.assign ((size_t) bufferLen, 0.0f);
    dryR.assign ((size_t) bufferLen, 0.0f);
    srcMask = bufferLen - 1;
    scratch.assign ((size_t) frameLen, 0.0f);

    const int64_t frameRing = nextPow2 (bufferLen / hop + 64);
    rawFrames.assign ((size_t) frameRing, {});
    frames.assign ((size_t) frameRing, {});
    keyTrack.assign ((size_t) frameRing, 0);
    keyMinor.assign ((size_t) frameRing, 0);
    keyTuning.assign ((size_t) frameRing, 0.0f);
    keyNotes.assign (256, {});
    frameMask = frameRing - 1;

    notes.assign (1024, {});
    noteMask = 1023;

    chordLog.assign (256, {});
    chordMask = 255;

    const int64_t markRing = nextPow2 ((int64_t) (bufferLen / (sr / maxHz)) + 64);
    marks.assign ((size_t) markRing, {});
    markMask = markRing - 1;

    const int64_t olaLen = nextPow2 (chunkSize + 4 * maxHalf + 16);
    olaMask = olaLen - 1;
    for (auto& in : inst)
        in.ola.assign ((size_t) olaLen, 0.0f);

    setLookahead (lookaheadSeconds);
}

void HarmonyEngine::setLookahead (double seconds)
{
    live = seconds <= 0.0;
    configureAnalysis();
    latency = (int) std::lround ((live ? liveLatencySeconds : std::clamp (seconds, 0.05, maxLookaheadSeconds)) * sr);
    reset();
}

void HarmonyEngine::configureAnalysis()
{
    tracker = live ? &trackerLive : deep ? &trackerDeep : &trackerFull;
    frameLen = tracker->getFrameLength();
    medianRadius = live ? 1 : 2;
    onsetFrames = live ? 2 : 3;
}

void HarmonyEngine::reset()
{
    std::fill (src.begin(), src.end(), 0.0f);
    std::fill (dryL.begin(), dryL.end(), 0.0f);
    std::fill (dryR.begin(), dryR.end(), 0.0f);
    head = 0;

    numRaw = numFinal = 0;
    keyFirst = keyLast = -1;
    analysedUpTo = 0;
    numNotes = 0;
    noteOpen = false;
    candRun = devRun = uvRun = 0;
    keyFinder.reset();
    forcedRoot = lockedRoot = -1;
    numMarks = 0;
    lastMark = 0.0;

    std::fill (std::begin (held), std::end (held), false);
    numChords = 0;

    rng = 0x9E3779B9u;
    for (int i = 0; i < maxInstances; ++i)
    {
        auto& in = inst[i];
        std::fill (in.ola.begin(), in.ola.end(), 0.0f);
        in.active = in.primed = false;
        in.nextMark = -1.0;
        in.shift = in.gain = in.formantInterval = in.drift = in.driftTarget = 0.0f;
        in.panL = in.panR = 0.0f;
        in.vibPhase = (float) (2.0 * pi * random01());
    }

    statusRoot = -1;
    statusMinor = false;
    statusLeadMidi = 0.0f;
    for (auto& t : statusTarget)
        t = 0.0f;
    statusChordRoot = -1;
    statusChordMask = 0;
}

uint32_t HarmonyEngine::signature (const HarmonySettings& s, bool isLive)
{
    // everything a cached note harmony depends on
    uint32_t h = 2166136261u;
    auto mix = [&h] (int x) { h = (h ^ (uint32_t) x) * 16777619u; };
    mix (s.keyMode);
    mix (s.scaleMode);
    mix (s.harmonyMode);
    mix ((int) std::lround (s.minNoteMs));
    mix (isLive ? 1 : 0);
    for (const auto& v : s.voice)
    {
        mix (v.interval);
        mix (v.position);
        mix (v.motion);
    }
    return h;
}

float HarmonyEngine::random01()
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (float) (rng >> 8) / 16777216.0f;
}

//==============================================================================
void HarmonyEngine::process (const float* const* in, int numIn, float* const* out, int numOut, int numSamples,
                             const HarmonySettings& settings, const MidiEvent* midi, int numMidi)
{
    // Instrument analysis reaches down to bass range (not live: the longer frame needs the lookahead).
    // Switching starts the analysis over, like changing the lookahead.
    if (const bool wantDeep = settings.instrument && ! live; wantDeep != deep)
    {
        deep = wantDeep;
        configureAnalysis();
        reset();
    }

    HarmonySettings s = settings;
    if (s.instrument)
    {
        s.entryLagMs = s.humanize = s.consonants = s.autoFormant = s.vibDepthCents = 0.0f;
        for (auto& v : s.voice)
            v.formantSemis = 0.0f;
    }

    // mute / solo: a muted source is silent; while anything is soloed, only soloed sources play
    bool anySolo = s.drySolo || s.doubleSolo;
    for (int v = 0; v < s.voices; ++v)
        anySolo = anySolo || s.voice[v].solo;
    auto audible = [anySolo] (bool mute, bool solo) { return ! mute && (! anySolo || solo); };

    const float dryTarget = audible (s.dryMute, s.drySolo) ? dbToGain (s.dryDb, -59.9f) : 0.0f;
    const float wetTarget = dbToGain (s.wetDb, -59.9f);
    currentSig = signature (s, live);

    // the scale the analysis fits to: fixed by the Key setting, frozen by Key Lock, or free
    if (s.keyMode > 0)
    {
        const int tonic = s.keyMode - 1;
        forcedRoot = s.scaleMode == 2 ? (tonic + 3) % 12 : tonic;
        lockedRoot = -1;
    }
    else if (s.keyLock)
    {
        if (lockedRoot < 0 && keyLast >= 0)   // nothing to lock until a key has been heard
            lockedRoot = keyFinder.root();
        forcedRoot = lockedRoot;
    }
    else
        forcedRoot = lockedRoot = -1;

    int midiPos = 0;
    for (int done = 0; done < numSamples;)
    {
        const int len = std::min (chunkSize, numSamples - done);

        while (midiPos < numMidi && midi[midiPos].offset < done + len)
        {
            handleMidi (midi[midiPos], head + std::max (0, midi[midiPos].offset - done));
            ++midiPos;
        }

        for (int j = 0; j < len; ++j)
        {
            const float l = numIn > 0 ? in[0][done + j] : 0.0f;
            const float r = numIn > 1 ? in[1][done + j] : l;
            const auto idx = (size_t) ((head + j) & srcMask);
            src[idx] = 0.5f * (l + r);
            dryL[idx] = l;
            dryR[idx] = r;
        }
        head += len;
        analyse();

        const int64_t t0 = head - len - latency;    // output covers source times [t0, t0 + len)
        const double genLimit = (double) (t0 + len + maxHalf);

        float panTargetL[maxInstances] {}, panTargetR[maxInstances] {};
        for (int i = 0; i < maxInstances; ++i)
        {
            auto& x = inst[i];
            const int v = i / 3, c = i % 3;
            const bool doubler = i >= firstDoubler;
            const bool active = doubler ? s.doubleDb > -59.9f : v < s.voices && c < s.stack;
            if (! active)
            {
                if (x.active)
                    std::fill (x.ola.begin(), x.ola.end(), 0.0f);
                x.active = false;
                continue;
            }
            if (! x.active)
            {
                std::fill (x.ola.begin(), x.ola.end(), 0.0f);
                x.active = true;
                x.primed = false;
                x.nextMark = (double) std::max<int64_t> (t0, 0);
                x.panL = x.panR = 0.0f;
            }
            synthesise (i, genLimit, s);

            float pan = 0.0f, level = 0.0f;
            if (doubler)
            {
                // the two copies of the lead sit either side of it
                pan = (i == firstDoubler ? -1.0f : 1.0f) * s.doubleWidth;
                level = audible (s.doubleMute, s.doubleSolo) ? dbToGain (s.doubleDb, -59.9f) * std::sqrt (2.0f) : 0.0f;
            }
            else
            {
                const auto& vs = s.voice[v];
                const float spread = c == 1 ? -0.5f : c == 2 ? 0.5f : 0.0f;
                pan = std::clamp (vs.pan + spread * s.stackWidth, -1.0f, 1.0f);
                level = audible (vs.mute, vs.solo) ? dbToGain (vs.levelDb, -47.9f) / std::sqrt ((float) s.stack) * std::sqrt (2.0f) : 0.0f;
            }
            const float angle = (float) ((pan + 1.0f) * pi * 0.25);
            panTargetL[i] = level * std::cos (angle);
            panTargetR[i] = level * std::sin (angle);
        }

        const float invLen = 1.0f / (float) len;
        for (int j = 0; j < len; ++j)
        {
            const int64_t t = t0 + j;
            const float ramp = (float) (j + 1) * invLen;
            float wl = 0.0f, wr = 0.0f;
            for (int i = 0; i < maxInstances; ++i)
            {
                auto& x = inst[i];
                if (! x.active)
                    continue;
                float& slot = x.ola[(size_t) (t & olaMask)];
                const float y = slot;
                slot = 0.0f;
                wl += y * (x.panL + (panTargetL[i] - x.panL) * ramp);
                wr += y * (x.panR + (panTargetR[i] - x.panR) * ramp);
            }

            dryGain += (dryTarget - dryGain) * 0.002f;
            wetGain += (wetTarget - wetGain) * 0.002f;
            const float dl = t >= 0 ? dryL[(size_t) (t & srcMask)] : 0.0f;
            const float dr = t >= 0 ? dryR[(size_t) (t & srcMask)] : 0.0f;
            const float ol = dryGain * dl + wetGain * wl;
            const float orr = dryGain * dr + wetGain * wr;
            if (numOut == 1)
                out[0][done + j] = 0.5f * (ol + orr);
            else
            {
                out[0][done + j] = ol;
                out[1][done + j] = orr;
            }
        }
        for (int i = 0; i < maxInstances; ++i)
        {
            inst[i].panL = panTargetL[i];
            inst[i].panR = panTargetR[i];
        }

        done += len;
    }

    // ---- status at the output position ----
    const double tOut = (double) (head - latency);
    // The grid is unwrapped internally (a singer may drift past a quarter tone); for display, fold
    // whole semitones of it into the key name so the readout shows the nearest real key.
    const float grid = tuningAt (tOut);
    const auto wholeSemis = (int) std::lround (grid);
    if (keyLast >= 0 && numNotes > 0)
    {
        int root = 0, tonic = 0;
        bool minor = false;
        scaleAt (tOut, s, root, tonic, minor);
        statusRoot = ((root + wholeSemis) % 12 + 12) % 12;
        statusMinor = minor;
    }
    const auto& f = frameAt (tOut);
    statusLeadMidi = f.voiced ? f.midi : 0.0f;
    statusTuning = grid - (float) wholeSemis;
    const int64_t ni = noteIndexAt (tOut);
    const bool inNote = ni >= 0 && (note (ni).end < 0 || tOut < (double) note (ni).end);
    if (inNote)
        harmonize (ni, s);
    for (int v = 0; v < 3; ++v)
    {
        const bool on = inNote && v < s.voices && note (ni).voiceOn[v] && noteSings (ni, tOut, s);
        statusTarget[v] = on ? note (ni).target[v] + note (ni).tuning : 0.0f;   // sounding pitch
    }
    const bool chordMode = s.harmonyMode == HarmonyModes::ChordsAuto || s.harmonyMode == HarmonyModes::ChordsMidi;
    if (inNote && chordMode && note (ni).chord.valid())
    {
        // shown on the same folded grid as the key
        auto c = note (ni).chord;
        c.root = ((c.root + wholeSemis) % 12 + 12) % 12;
        statusChordRoot = c.root;
        statusChordQuality = (int) c.quality;
    }
    else if (! inNote || ! chordMode)
        statusChordRoot = -1;
}

//==============================================================================
// Analysis
//==============================================================================
void HarmonyEngine::analyse()
{
    // YIN measures the first half of its frame (its integration window, plus lags into the second
    // half), so the frame starts a quarter-frame before its time: the measured stretch is centred on
    // the time the pitch is filed under. (Centring the whole frame put every pitch ~11 ms early.)
    while (numRaw * hop + frameLen * 3 / 4 <= head)
    {
        const int64_t start = numRaw * hop - frameLen / 4;
        for (int k = 0; k < frameLen; ++k)
        {
            const int64_t idx = start + k;
            scratch[(size_t) k] = idx >= 0 ? src[(size_t) (idx & srcMask)] : 0.0f;
        }

        const auto r = tracker->analyse (scratch.data());
        Frame f;
        f.rms = r.rms;
        f.aperiodicity = r.aperiodicity;
        f.brightness = r.brightness;
        f.voiced = r.f0 > 0.0f && r.aperiodicity < 0.25f && r.rms > 0.002f;
        f.f0 = f.voiced ? r.f0 : 0.0f;
        rawFrames[(size_t) (numRaw & frameMask)] = f;
        ++numRaw;

        if (numRaw > medianRadius)
            finaliseFrame (numRaw - 1 - medianRadius);   // needs raw frames j-r .. j+r
    }
    placeMarks();
}

void HarmonyEngine::finaliseFrame (int64_t j)
{
    Frame f = rawFrames[(size_t) (j & frameMask)];

    // a median over 5 frames (3 in live mode) removes isolated octave jumps and single-frame voicing flips
    float f0s[5];
    int n = 0;
    for (int64_t k = j - medianRadius; k <= j + medianRadius; ++k)
        if (k >= 0)
        {
            const auto& g = rawFrames[(size_t) (k & frameMask)];
            if (g.voiced)
                f0s[n++] = g.f0;
        }
    if (f.voiced && n >= 2)
    {
        std::sort (f0s, f0s + n);
        f.f0 = n % 2 ? f0s[n / 2] : 0.5f * (f0s[n / 2 - 1] + f0s[n / 2]);
        f.midi = 69.0f + 12.0f * std::log2 (f.f0 / 440.0f);
    }
    else
    {
        f.voiced = false;
        f.f0 = 0.0f;
    }

    const float bright = std::clamp ((f.brightness - 0.15f) / 0.35f, 0.0f, 1.0f);
    f.cons = f.rms > 0.002f ? bright * (f.voiced ? 0.25f : 1.0f) : 0.0f;

    frames[(size_t) (j & frameMask)] = f;
    numFinal = j + 1;
    analysedUpTo = j * hop;

    segmentNotes (j, f);

    // the key track runs a little ahead of the output, so a note's key is ready before its first grain
    const int64_t ahead = std::min<int64_t> ((int64_t) (0.15 * sr), latency / 2);
    computeKey (j - (latency - ahead) / hop);
}

void HarmonyEngine::segmentNotes (int64_t j, const Frame& f)
{
    const int64_t t = j * hop;
    const int changeFrames = std::max (2, (int) std::lround (0.05 * sr / hop));
    const int offFrames = std::max (2, (int) std::lround (0.04 * sr / hop));

    if (f.voiced)
    {
        uvRun = 0;
        if (! noteOpen)
        {
            if (candRun == 0 || std::abs (f.midi - candMidi) > 0.6f)
            {
                candRun = 1;
                candStart = t;
                candMidi = f.midi;
            }
            else
            {
                ++candRun;
                candMidi += (f.midi - candMidi) * 0.3f;
            }
            if (candRun >= onsetFrames)
            {
                openNote (candStart, candMidi);
                candRun = 0;
            }
        }
        else
        {
            auto& n = note (numNotes - 1);
            if (std::abs (f.midi - n.midi) > 0.7f)
            {
                if (devRun == 0 || std::abs (f.midi - devMidi) > 0.6f)
                {
                    devRun = 1;
                    devStart = t;
                    devMidi = f.midi;
                }
                else
                {
                    ++devRun;
                    devMidi += (f.midi - devMidi) * 0.3f;
                }
                if (devRun >= changeFrames)
                {
                    if ((double) (devStart - n.start) < 0.2 * sr && std::abs (devMidi - n.midi) < 1.0f)
                    {
                        // a scoop or grace into the note, settling within a semitone: the same note,
                        // re-centred on where it settled (start kept), not a new one
                        n.midi = devMidi;
                        n.pitches[0] = devMidi;
                        n.numPitches = 1;
                    }
                    else
                    {
                        closeNote (devStart);
                        openNote (devStart, devMidi);
                    }
                    devRun = 0;
                }
            }
            else
            {
                devRun = 0;
                if (n.numPitches < Note::maxPitches)
                {
                    n.pitches[n.numPitches++] = f.midi;

                    // the note's pitch: median of the settled part (the attack, often a scoop, left out)
                    float tmp[Note::maxPitches];
                    const int skip = std::min (n.numPitches / 4, (int) (0.2 * sr / hop));
                    const int count = n.numPitches - skip;
                    std::copy (n.pitches + skip, n.pitches + n.numPitches, tmp);
                    std::nth_element (tmp, tmp + count / 2, tmp + count);
                    n.midi = tmp[count / 2];
                }
            }
        }
    }
    else
    {
        candRun = devRun = 0;
        if (noteOpen)
        {
            if (uvRun++ == 0)
                uvStart = t;
            if (uvRun >= offFrames)
                closeNote (uvStart);
        }
    }
}

void HarmonyEngine::openNote (int64_t start, float midi)
{
    auto& n = note (numNotes);
    n = Note {};
    n.start = start;
    n.midi = midi;
    n.pitches[0] = midi;
    n.numPitches = 1;
    for (int i = 0; i < maxInstances; ++i)
    {
        n.jitLag[i] = random01();
        n.jitDet[i] = random01();
    }
    ++numNotes;
    noteOpen = true;
}

void HarmonyEngine::closeNote (int64_t end)
{
    auto& n = note (numNotes - 1);
    n.end = std::max (end, n.start + hop);
    noteOpen = false;
    uvRun = 0;
}

void HarmonyEngine::computeKey (int64_t k)
{
    if (k < 0)
        return;

    // The fit runs every few frames; frames in between repeat the last result.
    constexpr int every = 4;
    if (k % every == 0)
    {
        // Notes are weighted by kernels centred on this frame: behind it, 6 s for the long window and
        // 1 s for the short one; in front of it, as much of the lookahead as is known.
        const double tk = (double) (k * hop), w1 = (double) analysedUpTo;
        const double sigmaFuture = std::max (0.1 * sr, 0.6 * (w1 - tk));
        const double longPast = 6.0 * sr, shortPast = 1.0 * sr;
        const double w0 = tk - 2.5 * longPast;

        int numLong = 0, numShort = 0;
        const int maxShort = (int) keyNotes.size() / 4, maxLong = (int) keyNotes.size() - maxShort;
        auto* shortNotes = keyNotes.data() + maxLong;
        for (int64_t i = numNotes - 1; noteValid (i) && numLong < maxLong; --i)
        {
            const auto& n = note (i);
            const double e = (double) noteEnd (n);
            if (e < w0)
                break;
            const double a = std::max ((double) n.start, w0), b = std::min (e, w1);
            if (b > a && e - (double) n.start >= 0.1 * sr && ! isGlide (n))   // short fragments and bends aren't key evidence
            {
                const double d = 0.5 * (a + b) - tk, len = (b - a) / sr;
                const double zl = d / (d < 0.0 ? longPast : sigmaFuture), zs = d / (d < 0.0 ? shortPast : sigmaFuture);
                keyNotes[(size_t) numLong++] = { n.midi, (float) (len * std::exp (-0.5 * zl * zl)) };
                // the short window (which can overturn the key) only trusts held notes: slide pieces and
                // ornaments under 250 ms can land on any note
                if (numShort < maxShort && d > -2.5 * shortPast && e - (double) n.start >= 0.25 * sr)
                    shortNotes[numShort++] = { n.midi, (float) (len * std::exp (-0.5 * zs * zs)) };
            }
        }
        keyFinder.update (keyNotes.data(), numLong, shortNotes, numShort, (float) (sr / hop / every),
                          forcedRoot);
    }

    keyTrack[(size_t) (k & frameMask)] = (int8_t) keyFinder.root();
    keyMinor[(size_t) (k & frameMask)] = keyFinder.minor() ? 1 : 0;
    keyTuning[(size_t) (k & frameMask)] = keyFinder.tuning();
    if (keyFirst < 0)
        keyFirst = k;
    keyLast = k;
}

float HarmonyEngine::periodAt (double t) const
{
    // The source's period at t, or 0 where nothing is sung. Inside a note, a frame whose voicing
    // flickers (breathy tone, wide vibrato) still takes the note's pitch: an unvoiced grain is
    // copied unshifted, so the harmony would jump to the lead's own note and back.
    const auto& f = frameAt (t);
    if (f.voiced)
        return (float) (sr / f.f0);
    const int64_t ni = noteIndexAt (t);
    if (ni >= 0 && (note (ni).end < 0 || t < (double) note (ni).end))
        return (float) (sr / (440.0 * std::exp2 ((note (ni).midi - 69.0) / 12.0)));
    return 0.0f;
}

void HarmonyEngine::placeMarks()
{
    if (numFinal == 0)
        return;

    // pitch marks step through the source one period at a time (fixed spacing when unvoiced)
    const double limit = (double) ((numFinal - 1) * hop);
    for (;;)
    {
        const float here = periodAt (lastMark);
        const double t = lastMark + (here > 0.0f ? here : uvSpacing);
        if (t > limit)
            break;
        const float period = periodAt (t);
        auto& m = marks[(size_t) (numMarks & markMask)];
        m.t = t;
        m.voiced = period > 0.0f;
        m.period = period > 0.0f ? period : uvSpacing;
        ++numMarks;
        lastMark = t;
    }
}

//==============================================================================
// Lookups
//==============================================================================
float HarmonyEngine::midiAt (double t) const
{
    // the singer's pitch between two analysis frames, interpolated so corrections don't step
    if (numFinal < 2)
        return 0.0f;
    const int64_t lo = std::max<int64_t> (0, numFinal - (int64_t) frames.size() + 1);
    const double pos = std::clamp (t / hop, (double) lo, (double) (numFinal - 1));
    const auto j = (int64_t) std::floor (pos);
    const auto& a = frames[(size_t) (j & frameMask)];
    const auto& b = frames[(size_t) (std::min (j + 1, numFinal - 1) & frameMask)];
    if (a.voiced && b.voiced)
        return a.midi + (b.midi - a.midi) * (float) (pos - (double) j);
    return a.voiced ? a.midi : b.voiced ? b.midi : 0.0f;
}

const HarmonyEngine::Frame& HarmonyEngine::frameAt (double t) const
{
    static const Frame silent;
    if (numFinal == 0)
        return silent;
    const int64_t lo = std::max<int64_t> (0, numFinal - (int64_t) frames.size() + 1);
    const int64_t j = std::clamp<int64_t> ((int64_t) std::llround (t / hop), lo, numFinal - 1);
    return frames[(size_t) (j & frameMask)];
}

float HarmonyEngine::consonanceAt (double t) const
{
    if (numFinal == 0)
        return 0.0f;
    const int64_t lo = std::max<int64_t> (0, numFinal - (int64_t) frames.size() + 1);
    const int64_t c = (int64_t) std::llround (t / hop);
    float v = 0.0f;
    for (int64_t j = c - 2; j <= c + 2; ++j)
        if (j >= lo && j < numFinal)
            v = std::max (v, frames[(size_t) (j & frameMask)].cons);
    return v;
}

int HarmonyEngine::keyAt (double t) const
{
    if (keyLast < 0)
        return 0;
    const int64_t lo = std::max (keyFirst, keyLast - (int64_t) keyTrack.size() + 1);
    const int64_t k = std::clamp<int64_t> ((int64_t) std::floor (t / hop), lo, keyLast);
    return keyTrack[(size_t) (k & frameMask)];
}

float HarmonyEngine::tuningAt (double t) const
{
    if (keyLast < 0)
        return 0.0f;
    const int64_t lo = std::max (keyFirst, keyLast - (int64_t) keyTuning.size() + 1);
    const int64_t k = std::clamp<int64_t> ((int64_t) std::floor (t / hop), lo, keyLast);
    return keyTuning[(size_t) (k & frameMask)];
}

int64_t HarmonyEngine::noteIndexAt (double t) const
{
    int64_t lo = std::max<int64_t> (0, numNotes - (int64_t) notes.size() + 1), hi = numNotes - 1, found = -1;
    while (lo <= hi)
    {
        const int64_t mid = (lo + hi) / 2;
        if ((double) note (mid).start <= t)
        {
            found = mid;
            lo = mid + 1;
        }
        else
            hi = mid - 1;
    }
    return found;
}

int64_t HarmonyEngine::nearestMark (double t) const
{
    if (numMarks == 0)
        return -1;
    int64_t lo = std::max<int64_t> (0, numMarks - (int64_t) marks.size() + 1), hi = numMarks - 1;
    const int64_t first = lo;
    while (lo < hi)   // first mark at or after t
    {
        const int64_t mid = (lo + hi) / 2;
        if (marks[(size_t) (mid & markMask)].t < t)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo > first && std::abs (marks[(size_t) ((lo - 1) & markMask)].t - t) < std::abs (marks[(size_t) (lo & markMask)].t - t))
        --lo;
    return lo;
}

float HarmonyEngine::sourceAt (double t) const
{
    const double fl = std::floor (t);
    const auto i = (int64_t) fl;
    const float x = (float) (t - fl);
    auto at = [this] (int64_t k) { return k >= 0 ? src[(size_t) (k & srcMask)] : 0.0f; };
    const float y0 = at (i - 1), y1 = at (i), y2 = at (i + 1), y3 = at (i + 2);

    // 4-point Hermite
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * x + c2) * x + c1) * x + y1;
}

bool HarmonyEngine::keyMinorAt (double t) const
{
    if (keyLast < 0)
        return false;
    const int64_t lo = std::max (keyFirst, keyLast - (int64_t) keyMinor.size() + 1);
    const int64_t k = std::clamp<int64_t> ((int64_t) std::floor (t / hop), lo, keyLast);
    return keyMinor[(size_t) (k & frameMask)] != 0;
}

void HarmonyEngine::scaleAt (double t, const HarmonySettings& s, int& majorRoot, int& tonic, bool& minor) const
{
    if (s.keyMode > 0)
    {
        tonic = s.keyMode - 1;
        minor = s.scaleMode == 2;
        majorRoot = minor ? (tonic + 3) % 12 : tonic;
        return;
    }
    majorRoot = keyAt (t);
    minor = s.scaleMode == 0 ? keyMinorAt (t) : s.scaleMode == 2;
    tonic = minor ? (majorRoot + 9) % 12 : majorRoot;
}

void HarmonyEngine::handleMidi (const MidiEvent& e, int64_t t)
{
    if (e.note < 0 || e.note > 127 || held[e.note] == e.on)
        return;
    held[e.note] = e.on;

    // every change of the held notes is logged with its input time, so chords line up with the audio
    auto& c = chordLog[(size_t) (numChords & chordMask)];
    c.t = t;
    c.count = 0;
    for (int k = 0; k < 128 && c.count < 8; ++k)
        if (held[k])
            c.notes[c.count++] = k;
    ++numChords;
}

const HarmonyEngine::ChordEntry* HarmonyEngine::midiChordAt (double t) const
{
    int64_t lo = std::max<int64_t> (0, numChords - (int64_t) chordLog.size() + 1), hi = numChords - 1, found = -1;
    while (lo <= hi)
    {
        const int64_t mid = (lo + hi) / 2;
        if ((double) chordLog[(size_t) (mid & chordMask)].t <= t)
        {
            found = mid;
            lo = mid + 1;
        }
        else
            hi = mid - 1;
    }
    return found >= 0 ? &chordLog[(size_t) (found & chordMask)] : nullptr;
}

void HarmonyEngine::toneSpan (int64_t noteIdx, int64_t& first, int64_t& last) const
{
    // A held tone often reaches the note tracker as several pieces: a scoop into the note, the note,
    // a wobble at its end. Neighbours with no real gap, sung within 0.6 semitones of the tone's
    // longest piece, belong to the same tone.
    constexpr double maxGap = 0.05;
    constexpr float maxDiff = 0.6f;
    first = last = noteIdx;
    float anchor = note (noteIdx).midi;
    i64 longest = noteEnd (note (noteIdx)) - note (noteIdx).start;

    auto absorb = [&] (int64_t j) {
        const auto len = noteEnd (note (j)) - note (j).start;
        if (len > longest)
        {
            longest = len;
            anchor = note (j).midi;
        }
    };
    while (noteValid (first - 1) && (double) (note (first).start - noteEnd (note (first - 1))) < maxGap * sr
           && std::abs (note (first - 1).midi - anchor) < maxDiff)
        absorb (--first);
    while (noteValid (last + 1) && (double) (note (last + 1).start - noteEnd (note (last))) < maxGap * sr
           && std::abs (note (last + 1).midi - anchor) < maxDiff)
        absorb (++last);
}

bool HarmonyEngine::isGlide (const Note& n) const
{
    // A bend or slide: after the attack, the pitch moves steadily one way by more than half a
    // semitone (vibrato swings both ways, so its start and end thirds stay level).
    const int skip = std::min (n.numPitches / 4, (int) (0.2 * sr / hop));
    const int count = n.numPitches - skip;
    if (count < 6)
        return false;
    float a[Note::maxPitches], b[Note::maxPitches];
    const int third = count / 3;
    std::copy (n.pitches + skip, n.pitches + skip + third, a);
    std::copy (n.pitches + n.numPitches - third, n.pitches + n.numPitches, b);
    std::nth_element (a, a + third / 2, a + third);
    std::nth_element (b, b + third / 2, b + third);
    return std::abs (b[third / 2] - a[third / 2]) > 0.5f;
}

bool HarmonyEngine::noteSings (int64_t noteIdx, double t, const HarmonySettings& s) const
{
    if (s.singOn == SingModes::All)
        return true;

    // judged on the whole held tone, not on a fragment of it
    int64_t first = 0, last = 0;
    toneSpan (noteIdx, first, last);
    const double start = (double) note (first).start, end = (double) noteEnd (note (last));

    // once a tone is sung it stays sung to its end, so a later change of mind can't cut it off
    const uint32_t latch = (uint32_t) s.singOn * 100000u + (uint32_t) std::lround (s.longNoteMs) + 1u;
    if (note (first).singLatch == latch)
        return true;

    bool sings = true;
    switch (s.singOn)
    {
        case SingModes::LongNotes:
            // with lookahead the length is known before the tone starts; live, the voice fades in once it is long enough
            sings = end - start >= s.longNoteMs * 0.001 * sr && (! live || t - start >= s.longNoteMs * 0.001 * sr);
            break;

        case SingModes::PhraseEnds:
            sings = live || isPhraseEnd (first, last);   // live: needs the future, so sing everything
            break;

        default:
            break;
    }

    if (sings)
        note (first).singLatch = latch;
    return sings;
}

bool HarmonyEngine::isPhraseEnd (int64_t first, int64_t last) const
{
    // The phrase's last held tone: what follows before the next pause (250 ms) is at most a quick
    // syllable or two (each under 250 ms), which the backing voices let go.
    const double minTone = 0.25 * sr, pause = 0.25 * sr;
    const double start = (double) note (first).start, end = (double) noteEnd (note (last));

    // When the lookahead can't see far enough to tell (the tone is still sounding at its edge, or
    // has only just stopped), a long held tone is taken as the phrase end now, rather than finding
    // out and entering halfway through it. Short tones wait for the facts.
    const bool heldTone = end - start >= std::min (0.5 * sr, 0.6 * latency);
    if (note (last).end < 0)
        return heldTone;
    if (end - start < minTone)
        return false;

    double prevEnd = end;
    for (int64_t j = last + 1;; )
    {
        if (! noteValid (j))   // nothing sung since: a pause once long enough to tell, or a guess for a held tone
            return (double) analysedUpTo - prevEnd >= pause || heldTone;
        if ((double) note (j).start - prevEnd >= pause)
            return true;
        int64_t f2 = 0, l2 = 0;
        toneSpan (j, f2, l2);
        if (note (l2).end < 0 || (double) (noteEnd (note (l2)) - note (f2).start) >= minTone)
            return false;   // a real tone follows in the same phrase
        prevEnd = (double) noteEnd (note (l2));
        j = l2 + 1;
    }
}

namespace
{
    int pcOf (int p) { return ((p % 12) + 12) % 12; }
    bool dissonantWith (int interval) { const int i = pcOf (interval); return i == 1 || i == 2 || i == 10 || i == 11; }

    // MIDI chords are named on the A440 grid; the singer's grid may sit whole semitones away from it
    Chords::Chord shiftChord (Chords::Chord c, int semis)
    {
        if (! c.valid())
            return c;
        const int k = pcOf (semis);
        c.root = pcOf (c.root + semis);
        c.mask = (uint16_t) (((c.mask << k) | (c.mask >> (12 - k))) & 0x0FFF);
        return c;
    }
}

int HarmonyEngine::chooseDegree (int64_t noteIdx, int prevDegree, const Chords::Chord chords[], float grid, int majorRoot)
{
    // Viterbi over the known notes from this one to the edge of the lookahead (just this one when
    // live), starting from the previous note's chord. Only the first step is kept.
    constexpr int maxSteps = 12, nc = Chords::numDiatonic;
    int pcs[maxSteps];
    float lens[maxSteps];
    int steps = 0;
    const auto start = note (noteIdx).start;
    for (int64_t j = noteIdx; noteValid (j) && steps < maxSteps; ++j)
    {
        const auto& n = note (j);
        if (j > noteIdx && (live || n.start - start > latency))
            break;
        // A note clearly on a semitone keeps its name (a deliberate chromatic note is chord evidence);
        // one between semitones (a quarter-tone "blue" bend, a sloppy note) counts as the scale note it's nearest
        const float rel = n.midi - grid;
        int name = (int) std::lround (rel);
        if (std::abs (rel - (float) name) > 0.3f)
            diatonic (rel, majorRoot, 0, name);
        pcs[steps] = pcOf (name);
        // a bend or slide counts like a passing note: it shouldn't pull the chord its way
        lens[steps] = isGlide (n) ? 0.05f : (float) ((double) (noteEnd (n) - n.start) / sr);
        ++steps;
    }

    auto emit = [&] (int k, int c) {
        if (chords[c].has (pcs[k]))
            return pcs[k] == chords[c].root ? 0.0f : 0.1f;
        return 0.1f + 2.0f * std::min (lens[k], 1.0f);   // a short passing note barely counts against a chord
    };
    auto change = [&] (int k, int from, int to) {
        if (from == to)
            return 0.0f;
        const float shortNote = 0.8f * (1.0f - std::min (lens[k] / 0.3f, 1.0f));
        return (from >= 0 ? Chords::transitionCost (from, to) : 0.0f) + shortNote + 0.3f * Chords::priorCost (to);
    };

    float cost[maxSteps][nc];
    int back[maxSteps][nc];
    for (int c = 0; c < nc; ++c)
    {
        cost[0][c] = change (0, prevDegree, c) + emit (0, c);
        back[0][c] = -1;
    }
    for (int k = 1; k < steps; ++k)
        for (int c = 0; c < nc; ++c)
        {
            float best = 1.0e9f;
            int arg = 0;
            for (int p = 0; p < nc; ++p)
            {
                const float v = cost[k - 1][p] + change (k, p, c);
                if (v < best)
                {
                    best = v;
                    arg = p;
                }
            }
            cost[k][c] = best + emit (k, c);
            back[k][c] = arg;
        }

    int c = 0;
    for (int k = 1; k < nc; ++k)
        if (cost[steps - 1][k] < cost[steps - 1][c])
            c = k;
    for (int k = steps - 1; k > 0; --k)
        c = back[k][c];
    return c;
}

void HarmonyEngine::voiceChord (const Note* prev, int lead, int prevLead, const Chords::Chord& chord, int majorRoot,
                                const HarmonySettings& s, float out[3]) const
{
    // Each voice gets a short list of chord tones in its register, each with its own cost (movement,
    // register, clashes with the lead, parallels). The voicing is then chosen jointly, so the voices
    // share the chord between them: no two on the same note, and the chord keeps its 3rd.
    //
    // When the lead sings a note outside the chord (a passing "re" over the tonic, say), chord tones
    // a 2nd away would clash, so the upper voices may also take a scale note a 3rd or 6th from the
    // lead and move in parallel with it, as backing singers do over passing notes.
    constexpr int maxCand = 16;
    int cand[3][maxCand];
    float candCost[3][maxCand];
    int numCand[3] {};
    const int dl = prev != nullptr ? lead - prevLead : 0;
    const bool leadInChord = chord.has (lead);
    auto inScale = [majorRoot] (int p) {
        const int d = pcOf (p - majorRoot);
        return d == 0 || d == 2 || d == 4 || d == 5 || d == 7 || d == 9 || d == 11;
    };
    auto thirdOrSixth = [lead] (int p) { const int iv = pcOf (p - lead); return iv == 3 || iv == 4 || iv == 8 || iv == 9; };

    for (int v = 0; v < 3; ++v)
    {
        const int pos = s.voice[v].position, motion = s.voice[v].motion;
        int lo = lead + 1, hi = lead + 9;
        float nominal = 4.0f;
        switch (pos)
        {
            case Positions::AboveWide: lo = lead + 5;  hi = lead + 16; nominal = 9.0f;   break;
            case Positions::Below:     lo = lead - 9;  hi = lead - 1;  nominal = -5.0f;  break;
            case Positions::BelowWide: lo = lead - 16; hi = lead - 5;  nominal = -9.0f;  break;
            case Positions::Bass:      lo = lead - 26; hi = lead - 12; nominal = -19.0f; break;
            default: break;
        }

        const bool bass = pos == Positions::Bass;
        auto passingTone = [&] (int p) { return ! bass && ! leadInChord && ! chord.has (p) && inScale (p) && thirdOrSixth (p); };
        auto fits = [&] (int p) { return bass ? pcOf (p) == chord.root : chord.has (p) || passingTone (p); };
        const bool hasPrev = prev != nullptr && prev->voiceOn[v];
        const int pv = hasPrev ? (int) std::lround (prev->target[v]) : 0;

        // Hold: stay put while the old note is still a chord tone, near its register (the window moves
        // with every lead note, so allow some slack) and not clashing
        if (motion == Motions::Hold && hasPrev && fits (pv) && pv >= lo - 5 && pv <= hi + 5 && ! dissonantWith (pv - lead)
            && (bass || pcOf (pv) != pcOf (lead)))
        {
            cand[v][0] = pv;
            candCost[v][0] = 0.0f;
            numCand[v] = 1;
            continue;
        }

        for (int widen = 0; widen <= 12 && numCand[v] == 0; widen += 12)
            for (int p = lo - widen; p <= hi + widen && numCand[v] < maxCand; ++p)
            {
                if (! fits (p))
                    continue;
                float cost = 0.25f * std::abs ((float) p - ((float) lead + nominal));
                if (passingTone (p))
                    cost += 0.5f;                                // a chord tone is preferred when it doesn't clash
                if (hasPrev)
                    cost += 0.7f * (float) std::abs (p - pv);
                if (pcOf (p) == pcOf (lead))
                    cost += bass ? 1.0f : 3.0f;                  // don't double the melody
                if (dissonantWith (p - lead))
                    cost += 2.0f;
                if (hasPrev && dl != 0 && p != pv)
                {
                    const int was = pcOf (pv - prevLead), now = pcOf (p - lead);
                    if (was == now && (now == 0 || now == 7))
                        cost += 4.0f;                            // parallel octaves / fifths with the lead
                    if (motion == Motions::Contrary && (p - pv) * dl > 0)
                        cost += 2.5f;
                }
                cand[v][numCand[v]] = p;
                candCost[v][numCand[v]++] = cost;
            }
        if (numCand[v] == 0)   // no chord tone anywhere near: should not happen with a valid chord
        {
            cand[v][0] = lead + (int) nominal;
            candCost[v][0] = 0.0f;
            numCand[v] = 1;
        }
    }

    // the chord's 3rd gives it its colour
    int third = -1;
    if (chord.has (chord.root + 4))
        third = pcOf (chord.root + 4);
    else if (chord.has (chord.root + 3))
        third = pcOf (chord.root + 3);

    // only the voices in use share the chord; the rest are voiced on their own
    const int active = std::clamp (s.voices, 1, 3);
    int pick[3] {}, best[3] {};
    float bestCost = 1.0e9f;
    for (pick[0] = 0; pick[0] < numCand[0]; ++pick[0])
        for (pick[1] = 0; pick[1] < (active > 1 ? numCand[1] : 1); ++pick[1])
            for (pick[2] = 0; pick[2] < (active > 2 ? numCand[2] : 1); ++pick[2])
            {
                float cost = 0.0f;
                bool hasThird = third < 0 || pcOf (lead) == third;
                for (int v = 0; v < active; ++v)
                {
                    const int p = cand[v][pick[v]];
                    cost += candCost[v][pick[v]];
                    hasThird = hasThird || pcOf (p) == third;
                    for (int w = 0; w < v; ++w)
                        if (pcOf (p) == pcOf (cand[w][pick[w]]) && s.voice[v].position != Positions::Bass
                            && s.voice[w].position != Positions::Bass)
                            cost += pcOf (p) == chord.root ? 1.0f : 4.0f;   // two voices on one note: fine for the root only
                }
                if (! hasThird && leadInChord)   // over a passing note the voices follow the lead instead
                    cost += 2.0f;
                if (cost < bestCost)
                {
                    bestCost = cost;
                    std::copy (pick, pick + 3, best);
                }
            }

    for (int v = 0; v < 3; ++v)
    {
        int k = v < active ? best[v] : 0;
        if (v >= active)   // unused voice: its own cheapest note
            for (int c = 1; c < numCand[v]; ++c)
                if (candCost[v][c] < candCost[v][k])
                    k = c;
        out[v] = (float) cand[v][k];
    }
}

void HarmonyEngine::harmonize (int64_t noteIdx, const HarmonySettings& s, int depth)
{
    auto& n = note (noteIdx);
    if (n.harmDone && n.harmSig == currentSig)
        return;

    // name the note on the singer's own tuning grid
    const double at = (double) n.start + 0.02 * sr;
    const float grid = tuningAt (at);
    n.tuning = grid;
    int majorRoot = 0, tonic = 0;
    bool minor = false;
    scaleAt (at, s, majorRoot, tonic, minor);

    // The note the singer meant: a note sung between semitones counts as the scale note it's nearest
    // (sloppy intonation); one sung squarely on a semitone keeps its name, even outside the scale (a
    // deliberate chromatic note). Snapping that too would put the harmony a semitone off its target.
    const float rel = n.midi - grid;
    const int onGrid = (int) std::lround (rel);
    const bool meantAsSung = std::abs (rel - (float) onGrid) <= 0.3f;
    int snapped = onGrid;
    if (! meantAsSung)
        diatonic (rel, majorRoot, 0, snapped);

    // the previous note, for voice leading and passing-note holds (only harmonized one level deep)
    const Note* prev = nullptr;
    if (depth == 0 && noteValid (noteIdx - 1) && (double) (n.start - noteEnd (note (noteIdx - 1))) < 1.5 * sr)
    {
        harmonize (noteIdx - 1, s, 1);
        prev = &note (noteIdx - 1);
    }

    const double midiTolerance = live ? 0.0 : 0.1 * sr;   // a chord played just after the singer still counts
    n.chord = {};
    n.chordDegree = -1;

    switch (s.harmonyMode)
    {
        case HarmonyModes::Intervals:
            for (int v = 0; v < 3; ++v)
            {
                const int steps = Intervals::steps (s.voice[v].interval);
                int lead = (int) std::lround (rel);
                n.target[v] = steps % 7 == 0 ? (float) (lead + 12 * (steps / 7)) : diatonic (rel, majorRoot, steps, lead);
                n.leadRef[v] = meantAsSung ? onGrid : lead;
                n.voiceOn[v] = true;
            }
            break;

        case HarmonyModes::MidiNotes:
        {
            // each voice sings a held note exactly: voice 1 the highest, then downwards
            const auto* e = midiChordAt ((double) n.start + midiTolerance);
            const int count = e != nullptr ? e->count : 0;
            for (int v = 0; v < 3; ++v)
            {
                n.voiceOn[v] = v < count;
                n.target[v] = v < count ? (float) e->notes[count - 1 - v] - grid : 0.0f;
                n.leadRef[v] = (int) std::lround (rel);
            }
            break;
        }

        default:   // chord modes
        {
            if (s.harmonyMode == HarmonyModes::ChordsMidi)
                if (const auto* e = midiChordAt ((double) n.start + midiTolerance); e != nullptr && e->count > 0)
                    n.chord = shiftChord (Chords::fromNotes (e->notes, e->count), -(int) std::lround (grid));

            if (! n.chord.valid())
            {
                Chords::Chord chords[Chords::numDiatonic];
                Chords::diatonic (tonic, minor, chords);
                n.chordDegree = chooseDegree (noteIdx, prev != nullptr ? prev->chordDegree : -1, chords, grid, majorRoot);
                n.chord = chords[n.chordDegree];
            }

            voiceChord (prev, snapped, prev != nullptr ? prev->leadRef[0] : snapped, n.chord, majorRoot, s, n.target);
            for (int v = 0; v < 3; ++v)
            {
                n.leadRef[v] = snapped;
                n.voiceOn[v] = true;
            }
            break;
        }
    }

    const double len = (double) (noteEnd (n) - n.start);
    const bool follows = depth == 0 && s.harmonyMode != HarmonyModes::MidiNotes && prev != nullptr
                         && (double) (n.start - noteEnd (*prev)) < 0.3 * sr;
    const bool continuation = follows && (double) (n.start - noteEnd (*prev)) < 0.05 * sr
                              && std::lround (n.midi - grid) == std::lround (prev->midi - prev->tuning);

    if (continuation)
    {
        // The same sung note, split in two by the tracker (a wobble, a breath of a consonant):
        // the harmony carries on as it was rather than re-voicing in the middle of the note
        for (int v = 0; v < 3; ++v)
        {
            n.target[v] = prev->target[v];
            n.leadRef[v] = prev->leadRef[v];
            n.voiceOn[v] = prev->voiceOn[v];
        }
        n.chord = prev->chord;
        n.chordDegree = prev->chordDegree;
    }
    else if (follows && isGlide (n))
    {
        // A bend or slide is on its way somewhere, not a note to harmonize: the voices keep the
        // previous note's interval, so they bend and slide along with the lead
        for (int v = 0; v < 3; ++v)
            if (prev->voiceOn[v])
            {
                n.target[v] = prev->target[v];
                n.leadRef[v] = prev->leadRef[v];
            }
        if (prev->chord.valid() && s.harmonyMode != HarmonyModes::Intervals)
        {
            n.chord = prev->chord;
            n.chordDegree = prev->chordDegree;
        }
    }
    else if (follows && ! live && len < s.minNoteMs * 0.001 * sr)
    {
        // a passing note keeps the previous harmony notes (and chord) instead of jumping with it,
        // as long as a held note doesn't clash with it (a 2nd, 7th or tritone); a clashing voice
        // takes the note it was just voiced to instead (a chord tone, or a 3rd/6th from the lead)
        const bool upperOnly = s.harmonyMode != HarmonyModes::Intervals;
        for (int v = 0; v < 3; ++v)
        {
            const float heldNote = prev->target[v];
            const bool bassVoice = upperOnly && s.voice[v].position == Positions::Bass;
            const bool clashes = ! bassVoice && dissonantWith ((int) std::lround (heldNote) - n.leadRef[v]);
            if (prev->voiceOn[v] && ! clashes && std::abs (heldNote - (float) n.leadRef[v]) <= 9.0f && std::lround (heldNote) != n.leadRef[v])
                n.target[v] = heldNote;
        }
        if (prev->chord.valid() && s.harmonyMode != HarmonyModes::Intervals)
        {
            n.chord = prev->chord;
            n.chordDegree = prev->chordDegree;
        }
    }

    // a note harmonized only as context for the next one is recomputed properly when it is read
    n.harmDone = depth == 0;
    n.harmSig = currentSig;
}

//==============================================================================
// Synthesis: TD-PSOLA, one grain per synthesis pitch mark
//==============================================================================
void HarmonyEngine::synthesise (int i, double genLimit, const HarmonySettings& s)
{
    auto& x = inst[i];
    const int v = i / 3, c = i % 3;
    static const VoiceSettings doublerVoice {};
    const bool doubler = i >= firstDoubler;   // a copy of the lead itself (Double)
    const auto& vs = doubler ? doublerVoice : s.voice[v];

    if (numMarks == 0 || numFinal == 0)
    {
        x.nextMark = std::max (x.nextMark, genLimit);
        return;
    }

    // the two doubles sit at different short delays, so they don't comb against each other
    const double fixedDelay = (doubler ? s.doubleDelayMs * (i == firstDoubler ? 1.0 : 0.62) : vs.delayMs) * 0.001 * sr;
    const float stackDetune = doubler ? 0.0f : (c == 1 ? s.stackDetuneCents : c == 2 ? -s.stackDetuneCents : 0.0f) * 0.01f;
    // the doubles' detune LFOs run at slightly different rates; stacked copies' vibratos never lock in phase
    const float vibRate = doubler ? s.doubleRateHz * (i == firstDoubler ? 1.0f : 1.17f) : s.vibRateHz * (1.0f + 0.03f * (float) (i - 4));

    while (x.nextMark < genLimit)
    {
        const double ts = x.nextMark;
        const double u = ts - fixedDelay;

        // ---- late entry: jump back at each note onset, then catch up before the note ends ----
        double lagNow = 0.0;
        const int64_t ui = doubler ? -1 : noteIndexAt (u);   // a double comes in with the lead
        if (ui >= 0)
        {
            auto& n = note (ui);
            if (! n.entryDone[i])
            {
                const double len = (double) (noteEnd (n) - n.start);
                double lag = (s.entryLagMs + s.humanize * 35.0f * n.jitLag[i] + (c > 0 ? 12.0f * n.jitLag[i] : 0.0f)) * 0.001 * sr;
                lag = live ? 0.0 : std::min (lag, 0.4 * len);   // live, the note's length (and so the catch-up) is unknown
                double catchUp = std::min (len, std::max (3.0 * lag, 0.15 * sr));
                catchUp = std::max (catchUp, 2.5 * lag);
                n.lag[i] = (float) lag;
                n.catchUp[i] = (float) std::max (catchUp, 1.0);
                n.entryDone[i] = true;
            }
            const double into = u - (double) n.start;
            if (into < n.catchUp[i])
                lagNow = n.lag[i] * (1.0 - into / n.catchUp[i]);
        }
        const double tr = u - lagNow;

        // ---- which lead note is being read ----
        const int64_t ri = noteIndexAt (tr);
        int64_t use = -1;
        float gate = 0.0f;
        bool inside = false;
        if (ri >= 0 && (note (ri).end < 0 || tr < (double) note (ri).end))
        {
            use = ri;
            gate = 1.0f;
            inside = true;
        }
        else
        {
            // Between notes the voice still sings the consonants attached to them: the "h" leading
            // into a note (up to 200 ms) and the "s" trailing after one (up to 250 ms), as long as
            // the singer is making sound all the way to the note. Silence closes the voice.
            auto soundedBetween = [this] (double a, double b) {
                for (double t = a; t <= b; t += hop)
                    if (frameAt (t).rms < consonantFloor)
                        return false;
                return true;
            };
            const int64_t nx = ri + 1;
            const double toNext = noteValid (nx) ? (double) note (nx).start - tr : 1.0e9;
            const double sinceEnd = ri >= 0 ? tr - (double) noteEnd (note (ri)) : 1.0e9;
            if (toNext < 0.03 * sr || (toNext < 0.2 * sr && soundedBetween (tr, (double) note (nx).start)))
            {
                use = nx;
                gate = 1.0f;
            }
            else if (sinceEnd < 0.06 * sr || (sinceEnd < 0.25 * sr && soundedBetween ((double) noteEnd (note (ri)), tr)))
            {
                use = ri;
                gate = 1.0f;
            }
            else
                use = ri >= 0 ? ri : (noteValid (nx) ? nx : -1);
        }

        const int64_t mi = nearestMark (tr);
        if (mi < 0)
        {
            x.nextMark = ts + uvSpacing;
            continue;
        }
        const Mark m = marks[(size_t) (mi & markMask)];

        float shiftTarget = x.shift, vib = 0.0f, det = 0.0f, tightComp = 0.0f;
        if (use >= 0 && ! doubler)
            harmonize (use, s);
        // Sing On is decided on the voice's own timeline (u), not on the audio it re-reads while
        // entering late, or a voice would flicker on the tail of a note it doesn't sing. With
        // lookahead it also starts fading just before a note it won't sing.
        bool sings = true;
        if (s.singOn != SingModes::All && ! doubler)
        {
            auto around = [this] (double t) -> int64_t {
                const int64_t at = noteIndexAt (t);
                if (at >= 0 && (note (at).end < 0 || t < (double) note (at).end))
                    return at;
                if (noteValid (at + 1) && (double) note (at + 1).start - t < 0.03 * sr)
                    return at + 1;
                if (at >= 0 && t - (double) noteEnd (note (at)) < 0.06 * sr)
                    return at;
                return -1;
            };
            const int64_t now = around (u), soon = live ? now : around (u + 0.03 * sr);
            sings = now >= 0 && noteSings (now, u, s) && (soon < 0 || soon == now || noteSings (soon, u + 0.03 * sr, s));
        }

        if (doubler)
        {
            // unison with the lead, slowly detuned by an LFO; the two doubles move in opposite directions
            shiftTarget = 0.0f;
            det = s.doubleDetuneCents * 0.01f * std::sin (x.vibPhase + (i == firstDoubler ? 0.0f : (float) pi));
        }
        else if (use >= 0 && ! (note (use).voiceOn[v] && sings))
            gate = 0.0f;   // this voice rests here (Sing On, or fewer MIDI notes held than voices)
        else if (use >= 0)
        {
            const auto& n = note (use);
            // both the lead and the harmony note sit on the singer's own tuning grid
            const float target = n.target[v] + n.tuning;
            const float lead = (float) n.leadRef[v] + n.tuning;   // the scale note the singer was aiming for
            shiftTarget = target - lead;

            // Tightness cancels the singer's own pitch movement (vibrato, wobble) around the note.
            // Applied directly, not through the note-change smoothing: smoothing would lag the
            // vibrato it cancels and leave a wobble of its own.
            if (s.tightness > 0.0f && inside)
            {
                const float now = midiAt (tr);
                if (now > 0.0f)
                    tightComp = -s.tightness * std::clamp (now - lead, -1.0f, 1.0f);
            }

            const double len = (double) (noteEnd (n) - n.start);
            const double vibDelay = s.vibDelayMs * 0.001 * sr;
            if (s.vibDepthCents > 0.0f && inside && len >= vibDelay + 0.2 * sr)
            {
                float env = (float) std::clamp ((tr - (double) n.start - vibDelay) / (0.3 * sr), 0.0, 1.0);
                if (n.end >= 0)
                    env *= (float) std::clamp (((double) n.end - tr) / (0.08 * sr), 0.0, 1.0);
                vib = s.vibDepthCents * 0.01f * env * std::sin (x.vibPhase);
            }
            det = s.humanize * 0.06f * (2.0f * n.jitDet[i] - 1.0f);
        }

        const float step0 = m.period;
        if (! x.primed)
        {
            x.shift = x.formantInterval = shiftTarget;
            x.gain = 0.0f;
            x.primed = true;
        }
        // glide between harmony notes like a singer (~35 ms), not a hard-tuned step
        x.shift += (shiftTarget - x.shift) * (1.0f - std::exp (-step0 / (float) (0.035 * sr)));
        if (gate > 0.0f)
            x.formantInterval += (shiftTarget - x.formantInterval) * (1.0f - std::exp (-step0 / (float) (1.5 * sr)));

        // humanize: slow random pitch drift of a few cents
        if (random01() < step0 / (float) (0.25 * sr))
            x.driftTarget = (2.0f * random01() - 1.0f) * s.humanize * 0.05f;
        x.drift += (x.driftTarget - x.drift) * (1.0f - std::exp (-step0 / (float) (0.2 * sr)));

        const float semis = x.shift + tightComp + vib + det + stackDetune + x.drift;
        const float ratio = std::clamp (std::exp2 (semis / 12.0f), 0.25f, 4.0f);

        // backing-singer modes fade voices in and out rather than cutting them
        const float gateTarget = gate * (1.0f - s.consonants * consonanceAt (tr));
        const double gateTime = s.singOn != SingModes::All || s.harmonyMode == HarmonyModes::MidiNotes ? 0.012 : 0.008;
        x.gain += (gateTarget - x.gain) * (1.0f - std::exp (-step0 / (float) (gateTime * sr)));

        const float beta = m.voiced ? std::clamp (std::exp2 ((s.autoFormant * x.formantInterval + vs.formantSemis) / 12.0f), minBeta, maxBeta)
                                    : 1.0f;
        const float amp = x.gain * (m.voiced ? std::sqrt (beta / ratio) : 1.0f);
        if (amp > 1.0e-5f)
            addGrain (x, ts, m, beta, amp);

        const double step = m.voiced ? m.period / ratio : m.period;
        x.vibPhase += (float) (2.0 * pi * vibRate * step / sr);
        if (x.vibPhase > 2.0 * pi)
            x.vibPhase -= (float) (2.0 * pi);
        x.nextMark = ts + step;
    }
}

void HarmonyEngine::addGrain (Instance& x, double ts, const Mark& m, float beta, float amp)
{
    // Hann grain two analysis periods wide; reading the source at rate beta moves the formants by beta
    const double hwS = std::min ((double) m.period / beta, (double) maxHalf - 2.0);
    const auto n0 = (int64_t) std::ceil (ts - hwS), n1 = (int64_t) std::floor (ts + hwS);
    const double invHw = pi / hwS;
    for (int64_t n = n0; n <= n1; ++n)
    {
        const double k = (double) n - ts;
        const float w = 0.5f * (1.0f + (float) std::cos (k * invHw));
        x.ola[(size_t) (n & olaMask)] += amp * w * sourceAt (m.t + k * beta);
    }
}
