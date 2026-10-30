#include "KeyFinder.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float majorProfile[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
    constexpr float minorProfile[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };
    constexpr int scale[7] = { 0, 2, 4, 5, 7, 9, 11 };

    float correlate (const float hist[12], const float profile[12], int tonic)
    {
        float mh = 0.0f, mp = 0.0f;
        for (int i = 0; i < 12; ++i)
        {
            mh += hist[i];
            mp += profile[i];
        }
        mh /= 12.0f;
        mp /= 12.0f;

        float num = 0.0f, dh = 0.0f, dp = 0.0f;
        for (int i = 0; i < 12; ++i)
        {
            const float h = hist[(i + tonic) % 12] - mh;
            const float p = profile[i] - mp;
            num += h * p;
            dh += h * h;
            dp += p * p;
        }
        return dh > 0.0f ? num / std::sqrt (dh * dp) : 0.0f;
    }

    // distance in semitones from x to the nearest note of the major scale on root (at most 1)
    float scaleDistance (float x, int root)
    {
        float rel = std::fmod (x - (float) root, 12.0f);
        if (rel < 0.0f)
            rel += 12.0f;
        float best = 12.0f;
        for (int s : scale)
            best = std::min ({ best, std::abs (rel - (float) s), std::abs (rel - (float) (s + 12)) });
        return best;
    }

    constexpr float establishSeconds = 1.5f;     // kernel-weighted seconds of singing
    constexpr float distanceWeight = 1.5f;       // penalty per squared semitone off-scale
    constexpr float outsideDistance = 0.7f;      // further than this from the scale = a different note, not sloppy intonation

    // switching because recent singing contradicts the current scale...
    constexpr float contradictedOutside = 0.2f, contradictedMargin = 0.06f, contradictedSeconds = 0.3f;   // long enough that one ornament or sloppy note can't flip the key
    // ...or because another scale fits the long window much better for a long time
    constexpr float betterMargin = 0.2f, betterSeconds = 1.5f;
    constexpr float slowSwitchOutside = 0.08f;   // share of the long window the current scale must fail to explain

    constexpr float offsetStep = 0.05f;          // 5-cent grid search
    constexpr float gridFollowSeconds = 2.0f;
}

void KeyFinder::reset()
{
    current = 0;
    candidate = -1;
    candidateRun = 0;
    grid = candidateGrid = 0.0f;
    currentMinor = false;
    established = false;
}

KeyFinder::Fit KeyFinder::evaluate (const WeightedNote* notes, int numNotes, int root, float offset)
{
    float hist[12] {};
    float total = 0.0f, cost = 0.0f, outside = 0.0f;
    for (int i = 0; i < numNotes; ++i)
    {
        const float x = notes[i].midi - offset, w = notes[i].weight;
        const float d = scaleDistance (x, root);
        total += w;
        cost += w * d * d;
        if (d > outsideDistance)
            outside += w;

        // the key profile sees each note as the semitone it rounds to on this grid, so it judges the
        // key; the fine placement of the grid is left to the distance term (soft bins pulled it off true)
        hist[(((int) std::lround (x) % 12) + 12) % 12] += w;
    }

    Fit f {};
    const float maj = correlate (hist, majorProfile, root);
    const float min = correlate (hist, minorProfile, (root + 9) % 12);
    f.minor = min > maj;
    f.outside = total > 0.0f ? outside / total : 0.0f;
    f.score = std::max (maj, min) - distanceWeight * (total > 0.0f ? cost / total : 0.0f);
    return f;
}

void KeyFinder::search (const WeightedNote* notes, int numNotes, float centre, float range, int& bestRoot, float& bestOffset,
                        float& bestScore)
{
    bestScore = -1.0e9f;
    const int steps = (int) std::lround (range / offsetStep);
    for (int k = -steps; k < steps; ++k)
    {
        const float offset = centre + (float) k * offsetStep;
        for (int r = 0; r < 12; ++r)
        {
            const float s = evaluate (notes, numNotes, r, offset).score;
            if (s > bestScore)
            {
                bestScore = s;
                bestRoot = r;
                bestOffset = offset;
            }
        }
    }
}

void KeyFinder::update (const WeightedNote* longNotes, int numLong, const WeightedNote* shortNotes, int numShort,
                        float updatesPerSecond, int forcedRoot)
{
    float seconds = 0.0f;
    for (int i = 0; i < numLong; ++i)
        seconds += longNotes[i].weight;
    if (seconds < 0.05f)
        return;

    if (forcedRoot >= 0)
    {
        // fixed key: the grid stays within a quarter tone of A440, then follows the singer slowly
        if (current != forcedRoot || ! established)
            grid = std::remainder (grid, 1.0f);
        float best = grid, bestScore = -1.0e9f;
        const float range = established && current == forcedRoot ? 0.2f : 0.5f;
        for (float offset = -range; offset < range; offset += offsetStep)
        {
            const float s = evaluate (longNotes, numLong, forcedRoot, grid + offset).score;
            if (s > bestScore)
            {
                bestScore = s;
                best = grid + offset;
            }
        }
        const bool settled = established && current == forcedRoot;
        grid = settled ? grid + (best - grid) * std::min (1.0f, 1.0f / (gridFollowSeconds * updatesPerSecond))
                       : std::remainder (best, 1.0f);
        current = forcedRoot;
        established = seconds >= establishSeconds;
        candidate = -1;
        candidateRun = 0;
        currentMinor = evaluate (longNotes, numLong, current, grid).minor;
        return;
    }

    int bestRoot = current;
    float bestOffset = grid, bestScore = 0.0f;

    if (! established)
    {
        // early on, follow the best guess freely (one semitone of grid either way); lock once there is enough material
        search (longNotes, numLong, grid, 0.5f, bestRoot, bestOffset, bestScore);
        current = bestRoot;
        grid = bestOffset;
        established = seconds >= establishSeconds;
        currentMinor = evaluate (longNotes, numLong, current, grid).minor;
        return;
    }

    // let the current key's grid follow the singer's drift, slowly
    {
        float follow = grid, followScore = -1.0e9f;
        for (int k = -4; k <= 4; ++k)
        {
            const float offset = grid + (float) k * offsetStep;
            const float s = evaluate (longNotes, numLong, current, offset).score;
            if (s > followScore)
            {
                followScore = s;
                follow = offset;
            }
        }
        grid += (follow - grid) * std::min (1.0f, 1.0f / (gridFollowSeconds * updatesPerSecond));
    }

    const auto recent = evaluate (shortNotes, numShort, current, grid);
    const bool contradicted = numShort > 0 && recent.outside > contradictedOutside;

    float currentScore;
    bool allowed = true;
    if (contradicted)
    {
        search (shortNotes, numShort, grid, 0.5f, bestRoot, bestOffset, bestScore);
        currentScore = recent.score;
    }
    else
    {
        search (longNotes, numLong, grid, 0.5f, bestRoot, bestOffset, bestScore);
        const auto cur = evaluate (longNotes, numLong, current, grid);
        currentScore = cur.score;

        // A melody that never sings the note telling two keys apart (Amazing Grace never sings F or
        // F#) fits both; the profile's sense of "home" shifts with every phrase that rests on the 5th.
        // So a slow switch needs the current scale to miss notes that were sung and the new one to
        // explain them, not just a better profile score.
        const auto alt = evaluate (longNotes, numLong, bestRoot, bestOffset);
        allowed = cur.outside > slowSwitchOutside && alt.outside < cur.outside - slowSwitchOutside / 2.0f;
    }

    const float needMargin = contradicted ? contradictedMargin : betterMargin;
    const int needUpdates = std::max (1, (int) std::lround ((contradicted ? contradictedSeconds : betterSeconds) * updatesPerSecond));

    if (allowed && bestRoot != current && bestScore - currentScore > needMargin)
    {
        candidateRun = bestRoot == candidate ? candidateRun + 1 : 1;
        candidate = bestRoot;
        candidateGrid = bestOffset;
        if (candidateRun >= needUpdates)
        {
            current = candidate;
            grid = candidateGrid;
            candidate = -1;
            candidateRun = 0;
        }
    }
    else
    {
        candidate = -1;
        candidateRun = 0;
    }

    currentMinor = evaluate (longNotes, numLong, current, grid).minor;
}
