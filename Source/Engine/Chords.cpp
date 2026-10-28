#include "Chords.h"

#include <cstdio>

namespace Chords
{
namespace
{
    uint16_t maskOf (int root, const int* intervals, int count)
    {
        uint16_t m = 0;
        for (int i = 0; i < count; ++i)
            m |= (uint16_t) (1u << ((root + intervals[i]) % 12));
        return m;
    }

    Chord triad (int root, Quality q)
    {
        static const int maj[] = { 0, 4, 7 }, min[] = { 0, 3, 7 }, dim[] = { 0, 3, 6 };
        Chord c;
        c.root = root % 12;
        c.quality = q;
        c.mask = maskOf (c.root, q == Major ? maj : q == Minor ? min : dim, 3);
        return c;
    }

    int popcount (uint16_t m)
    {
        int n = 0;
        for (; m != 0; m &= (uint16_t) (m - 1))
            ++n;
        return n;
    }

    struct Template
    {
        Quality q;
        int intervals[4];
        int count;
    };

    const Template templates[] = {
        { Dom7, { 0, 4, 7, 10 }, 4 }, { Maj7, { 0, 4, 7, 11 }, 4 }, { Min7, { 0, 3, 7, 10 }, 4 },
        { HalfDim7, { 0, 3, 6, 10 }, 4 }, { Dim7, { 0, 3, 6, 9 }, 4 },
        { Major, { 0, 4, 7 }, 3 }, { Minor, { 0, 3, 7 }, 3 }, { Diminished, { 0, 3, 6 }, 3 },
        { Augmented, { 0, 4, 8 }, 3 }, { Sus4, { 0, 5, 7 }, 3 }, { Sus2, { 0, 2, 7 }, 3 },
    };
}

void diatonic (int tonic, bool minor, Chord out[numDiatonic])
{
    static const int majOffsets[numDiatonic] = { 0, 2, 4, 5, 7, 9, 11 };
    static const Quality majQ[numDiatonic] = { Major, Minor, Minor, Major, Major, Minor, Diminished };
    static const int minOffsets[numDiatonic] = { 0, 2, 3, 5, 7, 8, 10 };
    static const Quality minQ[numDiatonic] = { Minor, Diminished, Major, Minor, Major, Major, Major };

    for (int d = 0; d < numDiatonic; ++d)
        out[d] = triad (tonic + (minor ? minOffsets[d] : majOffsets[d]), minor ? minQ[d] : majQ[d]);
}

float transitionCost (int from, int to)
{
    if (from == to)
        return 0.0f;

    // degrees: 0 I, 1 ii, 2 iii, 3 IV, 4 V, 5 vi, 6 vii
    // negative = a strong, common move; positive = a weak one
    static const float pref[numDiatonic][numDiatonic] = {
        //  I      ii     iii    IV     V      vi     vii
        {  0.0f, -0.2f,  0.1f, -0.5f, -0.5f, -0.3f,  0.2f },   // from I
        {  0.1f,  0.0f,  0.2f,  0.0f, -0.5f,  0.1f, -0.2f },   // from ii
        {  0.1f,  0.1f,  0.0f, -0.2f,  0.1f, -0.4f,  0.3f },   // from iii
        { -0.4f, -0.1f,  0.2f,  0.0f, -0.5f,  0.1f, -0.1f },   // from IV
        { -0.6f,  0.5f,  0.3f,  0.5f,  0.0f, -0.2f,  0.3f },   // from V
        {  0.0f, -0.4f,  0.1f, -0.4f,  0.0f,  0.0f,  0.3f },   // from vi
        { -0.5f,  0.3f,  0.2f,  0.4f,  0.2f,  0.1f,  0.0f },   // from vii
    };
    return 1.0f + pref[from][to];
}

float priorCost (int degree)
{
    static const float prior[numDiatonic] = { 0.0f, 0.4f, 0.7f, 0.1f, 0.1f, 0.3f, 1.0f };
    return prior[degree];
}

Chord fromNotes (const int* notes, int count)
{
    Chord c;
    if (count <= 0)
        return c;

    int lowest = notes[0];
    for (int i = 0; i < count; ++i)
    {
        c.mask |= (uint16_t) (1u << (((notes[i] % 12) + 12) % 12));
        if (notes[i] < lowest)
            lowest = notes[i];
    }
    const int bass = ((lowest % 12) + 12) % 12;

    // best template: most played notes explained, fewest template notes missing, bass as root preferred
    float best = -1.0e9f;
    for (const auto& t : templates)
        for (int root = 0; root < 12; ++root)
        {
            const uint16_t tm = maskOf (root, t.intervals, t.count);
            const int explained = popcount ((uint16_t) (tm & c.mask));
            const int missing = popcount ((uint16_t) (tm & ~c.mask));
            const int extra = popcount ((uint16_t) (c.mask & ~tm));
            if (explained < 2)
                continue;
            const float score = (float) explained * 2.0f - (float) missing * 1.5f - (float) extra * 1.0f + (root == bass ? 0.75f : 0.0f);
            if (score > best)
            {
                best = score;
                c.root = root;
                c.quality = t.q;
            }
        }

    if (c.root < 0)   // a single note or an interval no template explains: name it after the bass
    {
        c.root = bass;
        c.quality = Other;
    }
    return c;
}

void name (const Chord& c, char* buf, int bufSize)
{
    static const char* roots[12] = { "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    static const char* suffix[] = { "", "m", "dim", "aug", "sus2", "sus4", "7", "maj7", "m7", "m7b5", "dim7", "" };
    if (! c.valid())
    {
        std::snprintf (buf, (size_t) bufSize, "-");
        return;
    }
    std::snprintf (buf, (size_t) bufSize, "%s%s", roots[c.root], suffix[(int) c.quality]);
}
}
