#include "HarmonyEngine.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr double pi = 3.14159265358979323846;
    constexpr int chunkSize = 512;
    constexpr float minHz = 70.0f, maxHz = 1100.0f;
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
    frameLen = hop * 8;
    tracker.prepare (sr, frameLen, minHz, maxHz);

    maxHalf = (int) std::ceil (sr / minHz / minBeta) + 4;
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
    latency = (int) std::lround (std::clamp (seconds, 0.05, maxLookaheadSeconds) * sr);
    reset();
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
    numMarks = 0;
    lastMark = 0.0;

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
                             const HarmonySettings& s)
{
    const float dryTarget = dbToGain (s.dryDb, -59.9f);
    const float wetTarget = dbToGain (s.wetDb, -59.9f);

    for (int done = 0; done < numSamples;)
    {
        const int len = std::min (chunkSize, numSamples - done);

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
        analysisKeyMode = s.keyMode;
        analyse();

        const int64_t t0 = head - len - latency;    // output covers source times [t0, t0 + len)
        const double genLimit = (double) (t0 + len + maxHalf);

        float panTargetL[maxInstances] {}, panTargetR[maxInstances] {};
        for (int i = 0; i < maxInstances; ++i)
        {
            auto& x = inst[i];
            const int v = i / 3, c = i % 3;
            const bool active = v < s.voices && c < s.stack;
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

            const auto& vs = s.voice[v];
            const float spread = c == 1 ? -0.5f : c == 2 ? 0.5f : 0.0f;
            const float pan = std::clamp (vs.pan + spread * s.stackWidth, -1.0f, 1.0f);
            const float angle = (float) ((pan + 1.0f) * pi * 0.25);
            const float level = dbToGain (vs.levelDb, -47.9f) / std::sqrt ((float) s.stack) * std::sqrt (2.0f);
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
        const int root = s.keyMode > 0 ? s.keyMode - 1 : keyAt (tOut);
        statusRoot = ((root + wholeSemis) % 12 + 12) % 12;
        const int64_t kk = std::clamp<int64_t> ((int64_t) std::floor (tOut / hop), keyFirst, keyLast);
        statusMinor = s.keyMode == 0 && keyMinor[(size_t) (kk & frameMask)] != 0;
    }
    const auto& f = frameAt (tOut);
    statusLeadMidi = f.voiced ? f.midi : 0.0f;
    statusTuning = grid - (float) wholeSemis;
    const int64_t ni = noteIndexAt (tOut);
    const bool inNote = ni >= 0 && (note (ni).end < 0 || tOut < (double) note (ni).end);
    for (int v = 0; v < 3; ++v)
        statusTarget[v] = inNote && v < s.voices ? targetFor (ni, v, s) + note (ni).tuning : 0.0f;   // sounding pitch
}

//==============================================================================
// Analysis
//==============================================================================
void HarmonyEngine::analyse()
{
    while (numRaw * hop + frameLen / 2 <= head)
    {
        const int64_t start = numRaw * hop - frameLen / 2;
        for (int k = 0; k < frameLen; ++k)
        {
            const int64_t idx = start + k;
            scratch[(size_t) k] = idx >= 0 ? src[(size_t) (idx & srcMask)] : 0.0f;
        }

        const auto r = tracker.analyse (scratch.data());
        Frame f;
        f.rms = r.rms;
        f.aperiodicity = r.aperiodicity;
        f.brightness = r.brightness;
        f.voiced = r.f0 > 0.0f && r.aperiodicity < 0.25f && r.rms > 0.002f;
        f.f0 = f.voiced ? r.f0 : 0.0f;
        rawFrames[(size_t) (numRaw & frameMask)] = f;
        ++numRaw;

        if (numRaw >= 3)
            finaliseFrame (numRaw - 3);   // needs raw frames j-2 .. j+2
    }
    placeMarks();
}

void HarmonyEngine::finaliseFrame (int64_t j)
{
    Frame f = rawFrames[(size_t) (j & frameMask)];

    // 5-frame median removes isolated octave jumps and single-frame voicing flips
    float f0s[5];
    int n = 0;
    for (int64_t k = j - 2; k <= j + 2; ++k)
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
    const int onsetFrames = 3;
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
                    closeNote (devStart);
                    openNote (devStart, devMidi);
                    devRun = 0;
                }
            }
            else
            {
                devRun = 0;
                if (n.numPitches < 32)
                {
                    n.pitches[n.numPitches++] = f.midi;
                    float tmp[32];
                    std::copy (n.pitches, n.pitches + n.numPitches, tmp);
                    std::nth_element (tmp, tmp + n.numPitches / 2, tmp + n.numPitches);
                    n.midi = tmp[n.numPitches / 2];
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
            if (b > a && e - (double) n.start >= 0.1 * sr)   // shorter fragments are mostly slides
            {
                const double d = 0.5 * (a + b) - tk, len = (b - a) / sr;
                const double zl = d / (d < 0.0 ? longPast : sigmaFuture), zs = d / (d < 0.0 ? shortPast : sigmaFuture);
                keyNotes[(size_t) numLong++] = { n.midi, (float) (len * std::exp (-0.5 * zl * zl)) };
                if (numShort < maxShort && d > -2.5 * shortPast)
                    shortNotes[numShort++] = { n.midi, (float) (len * std::exp (-0.5 * zs * zs)) };
            }
        }
        keyFinder.update (keyNotes.data(), numLong, shortNotes, numShort, (float) (sr / hop / every),
                          analysisKeyMode > 0 ? analysisKeyMode - 1 : -1);
    }

    keyTrack[(size_t) (k & frameMask)] = (int8_t) keyFinder.root();
    keyMinor[(size_t) (k & frameMask)] = keyFinder.minor() ? 1 : 0;
    keyTuning[(size_t) (k & frameMask)] = keyFinder.tuning();
    if (keyFirst < 0)
        keyFirst = k;
    keyLast = k;
}

void HarmonyEngine::placeMarks()
{
    if (numFinal == 0)
        return;

    // pitch marks step through the source one period at a time (fixed spacing when unvoiced)
    const double limit = (double) ((numFinal - 1) * hop);
    for (;;)
    {
        const auto& f = frameAt (lastMark);
        const double t = lastMark + (f.voiced ? sr / f.f0 : uvSpacing);
        if (t > limit)
            break;
        const auto& g = frameAt (t);
        auto& m = marks[(size_t) (numMarks & markMask)];
        m.t = t;
        m.voiced = g.voiced;
        m.period = g.voiced ? (float) (sr / g.f0) : uvSpacing;
        ++numMarks;
        lastMark = t;
    }
}

//==============================================================================
// Lookups
//==============================================================================
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

float HarmonyEngine::targetFor (int64_t noteIdx, int voice, const HarmonySettings& s, int depth)
{
    auto& n = note (noteIdx);
    const int interval = s.voice[voice].interval;
    if (n.targetDone[voice] && n.targetInterval[voice] == interval && n.targetKey[voice] == s.keyMode)
        return n.target[voice];

    // name the note on the singer's own tuning grid
    const double at = (double) n.start + 0.02 * sr;
    const int root = s.keyMode > 0 ? s.keyMode - 1 : keyAt (at);
    const float grid = tuningAt (at);
    n.tuning = grid;

    const int steps = Intervals::steps (interval);
    int lead = (int) std::lround (n.midi - grid);
    float target = steps % 7 == 0 ? (float) (lead + 12 * (steps / 7)) : diatonic (n.midi - grid, root, steps, lead);

    // a passing note keeps the previous harmony note instead of jumping with it
    const double len = (double) (noteEnd (n) - n.start);
    if (depth == 0 && len < s.minNoteMs * 0.001 * sr && noteValid (noteIdx - 1))
    {
        const auto& p = note (noteIdx - 1);
        if ((double) (n.start - noteEnd (p)) < 0.3 * sr)
        {
            const float held = targetFor (noteIdx - 1, voice, s, depth + 1);
            if (std::abs (held - (float) lead) <= 9.0f && std::lround (held) != lead)
                target = held;
        }
    }

    if (depth == 0)
    {
        n.targetDone[voice] = true;
        n.targetInterval[voice] = interval;
        n.targetKey[voice] = s.keyMode;
        n.target[voice] = target;
        n.leadRef[voice] = lead;
    }
    return target;
}

//==============================================================================
// Synthesis: TD-PSOLA, one grain per synthesis pitch mark
//==============================================================================
void HarmonyEngine::synthesise (int i, double genLimit, const HarmonySettings& s)
{
    auto& x = inst[i];
    const int v = i / 3, c = i % 3;
    const auto& vs = s.voice[v];

    if (numMarks == 0 || numFinal == 0)
    {
        x.nextMark = std::max (x.nextMark, genLimit);
        return;
    }

    const double fixedDelay = vs.delayMs * 0.001 * sr;
    const float stackDetune = (c == 1 ? s.stackDetuneCents : c == 2 ? -s.stackDetuneCents : 0.0f) * 0.01f;
    const float vibRate = s.vibRateHz * (1.0f + 0.03f * (float) (i - 4));   // stacked copies never lock in phase

    while (x.nextMark < genLimit)
    {
        const double ts = x.nextMark;
        const double u = ts - fixedDelay;

        // ---- late entry: jump back at each note onset, then catch up before the note ends ----
        double lagNow = 0.0;
        const int64_t ui = noteIndexAt (u);
        if (ui >= 0)
        {
            auto& n = note (ui);
            if (! n.entryDone[i])
            {
                const double len = (double) (noteEnd (n) - n.start);
                double lag = (s.entryLagMs + s.humanize * 35.0f * n.jitLag[i] + (c > 0 ? 12.0f * n.jitLag[i] : 0.0f)) * 0.001 * sr;
                lag = std::min (lag, 0.4 * len);
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
            const int64_t nx = ri + 1;
            if (noteValid (nx) && (double) note (nx).start - tr < 0.03 * sr)
            {
                use = nx;
                gate = 1.0f;
            }
            else if (ri >= 0 && tr - (double) noteEnd (note (ri)) < 0.06 * sr)
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

        float shiftTarget = 0.0f, vib = 0.0f, det = 0.0f;
        if (use >= 0)
        {
            const auto& n = note (use);
            // both the lead and the harmony note sit on the singer's own tuning grid
            const float target = targetFor (use, v, s) + n.tuning;
            const float lead = (float) n.leadRef[v] + n.tuning;   // the scale note the singer was aiming for
            float leadRef = lead;
            const auto& f = frameAt (tr);
            if (s.tightness > 0.0f && inside && f.voiced)
                leadRef = lead + s.tightness * std::clamp (f.midi - lead, -1.0f, 1.0f);
            shiftTarget = target - leadRef;

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
        x.shift += (shiftTarget - x.shift) * (1.0f - std::exp (-step0 / (float) (0.012 * sr)));
        if (gate > 0.0f)
            x.formantInterval += (shiftTarget - x.formantInterval) * (1.0f - std::exp (-step0 / (float) (1.5 * sr)));

        // humanize: slow random pitch drift of a few cents
        if (random01() < step0 / (float) (0.25 * sr))
            x.driftTarget = (2.0f * random01() - 1.0f) * s.humanize * 0.05f;
        x.drift += (x.driftTarget - x.drift) * (1.0f - std::exp (-step0 / (float) (0.2 * sr)));

        const float semis = x.shift + vib + det + stackDetune + x.drift;
        const float ratio = std::clamp (std::exp2 (semis / 12.0f), 0.25f, 4.0f);

        const float gateTarget = gate * (1.0f - s.consonants * consonanceAt (tr));
        x.gain += (gateTarget - x.gain) * (1.0f - std::exp (-step0 / (float) (0.008 * sr)));

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
