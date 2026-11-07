// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre

/**
 * @file    InputNTV2.h
 * @brief   v0.42: the AJA SDK view of an input number (InputId.h).
 *
 * Kept apart from InputId.h so that header stays free of AJA SDK
 * headers. Include only from translation units that already use the
 * NTV2 enums.
 */
#pragma once

#include "InputId.h"

#include "ntv2enums.h"
#include "ntv2utils.h"

namespace ajainput
{
namespace input
{

/**
 * @brief Map an input number to the NTV2 input source, the connector /
 *        input widget channel and whether it is SDI.
 *
 * SDI n maps to NTV2_INPUTSOURCE_SDIn and NTV2Channel n-1 (the channel
 * SetSDITransmitEnable / GetSDIInput3GbPresent take). HDMI n maps to
 * NTV2_INPUTSOURCE_HDMIn and NTV2Channel n-1 (the channel the SDK's
 * HDMI input calls such as GetHDMIInputColor take). A quad-link input
 * maps to its first link.
 *
 * @return false for an input number that names no input.
 */
inline bool ToNTV2(int n, NTV2InputSource& outSource, NTV2Channel& outChannel,
                   bool& outIsSdi) noexcept
{
    const int sdi  = SdiNumber(n);
    const int hdmi = HdmiNumber(n);
    if (sdi >= 1 && sdi <= 8)
    {
        outChannel = static_cast<NTV2Channel>(sdi - 1);
        outSource  = ::NTV2ChannelToInputSource(outChannel, NTV2_IOKINDS_SDI);
        outIsSdi   = true;
        return NTV2_IS_VALID_INPUT_SOURCE(outSource);
    }
    if (hdmi >= 1 && hdmi <= 4)
    {
        outChannel = static_cast<NTV2Channel>(hdmi - 1);
        outSource  = ::NTV2ChannelToInputSource(outChannel, NTV2_IOKINDS_HDMI);
        outIsSdi   = false;
        return NTV2_IS_VALID_INPUT_SOURCE(outSource);
    }
    return false;
}

/// @return The NTV2 input source of SDI link k (0..3) of an input; for a
///         single-connector input only k = 0 is valid.
inline NTV2InputSource LinkSource(int n, int link) noexcept
{
    if (link < 0 || link >= ConnectorCount(n) || !UsesSdi(n))
    {
        NTV2InputSource s = NTV2_INPUTSOURCE_INVALID;
        NTV2Channel     c = NTV2_CHANNEL1;
        bool            sdi = false;
        if (link == 0 && ToNTV2(n, s, c, sdi))
            return s;
        return NTV2_INPUTSOURCE_INVALID;
    }
    const NTV2Channel ch = static_cast<NTV2Channel>(SdiNumber(n) - 1 + link);
    return ::NTV2ChannelToInputSource(ch, NTV2_IOKINDS_SDI);
}

} // namespace input
} // namespace ajainput
