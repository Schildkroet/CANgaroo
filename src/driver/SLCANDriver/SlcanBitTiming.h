/*

  Copyright (c) 2026 Schildkroet

  This file is part of cangaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.

*/

#pragma once

// Exact CAN bit timing for adapters that take prescaler and segments directly,
// such as the ElmueSoft CANable 2.5 Slcan firmware ("sBRP,Seg1,Seg2,Sjw" and
// "y..."). The presets "S6" etc. there all use a 75 % sample point, which does
// not fit a bus at the common 87.5 %: a CAN FD node switches to the data rate at
// its nominal sample point, so BRS frames then fail on the wire.
//
// Header-only and free of Qt, so it can be tested on its own.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace slcan
{

struct TimingLimits
{
    uint32_t brpMax{0};
    uint32_t seg1Max{0};
    uint32_t seg2Max{0};
    uint32_t sjwMax{0};
};

struct BitTiming
{
    uint32_t brp{0};
    uint32_t seg1{0};  // propagation + phase 1, in time quanta
    uint32_t seg2{0};
    uint32_t sjw{0};

    [[nodiscard]] constexpr uint32_t quanta() const noexcept { return 1 + seg1 + seg2; }
    [[nodiscard]] double bitrate(uint32_t clockHz) const noexcept
    {
        return static_cast<double>(clockHz) / (static_cast<double>(brp) * quanta());
    }
    // Sample point in permille, as MeasurementInterface stores it
    [[nodiscard]] constexpr uint32_t samplePointPermille() const noexcept
    {
        return ((1 + seg1) * 1000 + quanta() / 2) / quanta();
    }
};

// The timing closest to `bitrate` (at most 0.1 % off) with the sample point
// nearest to `samplePointPermille`. Among equally close bitrates the smallest
// prescaler wins: more quanta per bit, a finer sample point. SJW follows the
// ElmueSoft firmware's own rule for its presets: min(Seg1, Seg2, max) - 1.
// Empty if the clock cannot produce the bitrate within the limits.
[[nodiscard]] inline std::optional<BitTiming> calcBitTiming(uint32_t clockHz, uint32_t bitrate,
                                                            uint32_t samplePointPermille,
                                                            const TimingLimits &lim)
{
    if (clockHz == 0 || bitrate == 0 || samplePointPermille == 0 || samplePointPermille >= 1000)
        return std::nullopt;

    std::optional<BitTiming> best;
    double bestError = 0;
    for (uint32_t brp = 1; brp <= lim.brpMax; ++brp)
    {
        const double ideal = static_cast<double>(clockHz) / (static_cast<double>(brp) * bitrate);
        const auto quanta = static_cast<uint32_t>(std::lround(ideal));
        if (quanta < 3)
            break;  // larger prescalers only make it fewer
        if (quanta > 1 + lim.seg1Max + lim.seg2Max)
            continue;

        const double error = std::abs(static_cast<double>(clockHz) / (static_cast<double>(brp) * quanta) - bitrate) / bitrate;
        if (error > 0.001)
            continue;

        // Sample point after the sync quantum and seg1
        const auto beforeSample = static_cast<uint32_t>(std::lround(quanta * samplePointPermille / 1000.0));
        if (beforeSample < 2 || beforeSample >= quanta)
            continue;
        const uint32_t seg1 = beforeSample - 1;
        const uint32_t seg2 = quanta - beforeSample;
        if (seg1 > lim.seg1Max || seg2 > lim.seg2Max)
            continue;

        if (!best || error < bestError - 1e-12)
        {
            uint32_t sjw = std::min({seg1, seg2, lim.sjwMax});
            if (sjw > 1)
                --sjw;
            best = BitTiming{.brp = brp, .seg1 = seg1, .seg2 = seg2, .sjw = sjw};
            bestError = error;
        }
    }
    return best;
}

} // namespace slcan
