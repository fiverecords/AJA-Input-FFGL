// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// Input numbers (src/InputId.h). The Input parameter's option values
// are stored in compositions: once published they must keep naming the
// same inputs.

#include "test_framework.h"
#include "InputId.h"

#include <cstring>

namespace in = ajainput::input;

TEST_CASE(InputId, OptionValuesNameFixedInputs)
{
    // The published numbering: SDI 1-8 = 0-7, HDMI 1-4 = 8-11,
    // quad-link SDI 1-4 = 12, SDI 5-8 = 13.
    for (int sdi = 1; sdi <= 8; ++sdi)
        EXPECT_EQ(in::FromOptionValue(static_cast<float>(sdi - 1)), in::FromSdi(sdi));
    for (int hdmi = 1; hdmi <= 4; ++hdmi)
        EXPECT_EQ(in::FromOptionValue(static_cast<float>(hdmi + 7)), in::FromHdmi(hdmi));
    EXPECT_EQ(in::FromOptionValue(12.0f), in::QuadFromFirstSdi(1));
    EXPECT_EQ(in::FromOptionValue(13.0f), in::QuadFromFirstSdi(5));
    EXPECT_TRUE(in::IsHdmi(9));
    EXPECT_EQ(in::HdmiNumber(9), 1);
    EXPECT_EQ(in::HdmiNumber(12), 4);
    EXPECT_EQ(in::SdiNumber(2), 2);
    EXPECT_EQ(in::SdiNumber(8), 8);
    // Rounding and out-of-range values.
    EXPECT_EQ(in::FromOptionValue(0.9999f), 2);
    EXPECT_EQ(in::FromOptionValue(-1.0f), 0);
    EXPECT_EQ(in::FromOptionValue(14.0f), 0);
    EXPECT_EQ(in::FromOptionValue(13.0f), 14);
    TEST_RETURN_OK;
}

TEST_CASE(InputId, EveryInputRoundTrips)
{
    for (int sdi = 1; sdi <= 8; ++sdi)
    {
        const int n = in::FromSdi(sdi);
        EXPECT_TRUE(in::IsSdi(n));
        EXPECT_EQ(in::SdiNumber(n), sdi);
        EXPECT_EQ(in::HdmiNumber(n), 0);
        EXPECT_EQ(in::FromOptionValue(in::ToOptionValue(n)), n);
        EXPECT_EQ(in::ConnectorCount(n), 1);
    }
    for (int hdmi = 1; hdmi <= 4; ++hdmi)
    {
        const int n = in::FromHdmi(hdmi);
        EXPECT_TRUE(in::IsHdmi(n));
        EXPECT_EQ(in::HdmiNumber(n), hdmi);
        EXPECT_EQ(in::SdiNumber(n), 0);
        EXPECT_EQ(in::FromOptionValue(in::ToOptionValue(n)), n);
        EXPECT_FALSE(in::UsesSdi(n));
    }
    EXPECT_EQ(in::FromSdi(0), 0);
    EXPECT_EQ(in::FromSdi(9), 0);
    EXPECT_EQ(in::FromHdmi(5), 0);
    TEST_RETURN_OK;
}

TEST_CASE(InputId, QuadLinkInputs)
{
    const int q1 = in::QuadFromFirstSdi(1);
    const int q5 = in::QuadFromFirstSdi(5);
    EXPECT_EQ(q1, 13);
    EXPECT_EQ(q5, 14);
    EXPECT_TRUE(in::IsQuad(q1));
    EXPECT_TRUE(in::UsesSdi(q1));
    EXPECT_FALSE(in::IsSdi(q1));
    EXPECT_EQ(in::SdiNumber(q1), 1);
    EXPECT_EQ(in::SdiNumber(q5), 5);
    EXPECT_EQ(in::ConnectorCount(q5), 4);
    EXPECT_EQ(in::QuadFromFirstSdi(2), 0);
    EXPECT_FALSE(in::IsValid(0));
    EXPECT_FALSE(in::IsValid(15));
    EXPECT_EQ(in::ConnectorCount(0), 0);
    TEST_RETURN_OK;
}

TEST_CASE(InputId, LabelsAndMenu)
{
    EXPECT_TRUE(std::strcmp(in::LabelUpper(in::FromHdmi(1)), "HDMI") == 0);
    EXPECT_TRUE(std::strcmp(in::LabelUpper(in::FromSdi(5)), "SDI5") == 0);
    EXPECT_TRUE(std::strcmp(in::LabelUpper(in::FromHdmi(3)), "HDMI3") == 0);
    EXPECT_TRUE(std::strcmp(in::LabelUpper(in::FromSdi(7)), "SDI7") == 0);
    EXPECT_TRUE(std::strcmp(in::LabelLower(13), "sdi1-4") == 0);
    EXPECT_TRUE(std::strcmp(in::LabelUpper(99), "input") == 0);
    EXPECT_TRUE(std::strcmp(in::MenuName(in::FromHdmi(1), false), "HDMI") == 0);
    EXPECT_TRUE(std::strcmp(in::MenuName(in::FromHdmi(1), true), "HDMI 1") == 0);
    EXPECT_TRUE(std::strcmp(in::MenuName(in::FromHdmi(2), true), "HDMI 2") == 0);
    EXPECT_TRUE(std::strcmp(in::MenuName(in::FromSdi(5), false), "SDI 5") == 0);
    EXPECT_TRUE(std::strcmp(in::MenuName(14, false), "SDI 5-8 (4K)") == 0);

    // The menu order lists every input once.
    bool seen[in::kMaxInputNumber + 1] = {};
    for (int k = 0; k < in::kMaxInputNumber; ++k)
    {
        const int n = in::kMenuOrder[k];
        EXPECT_TRUE(in::IsValid(n));
        EXPECT_FALSE(seen[n]);
        seen[n] = true;
    }
    TEST_RETURN_OK;
}
