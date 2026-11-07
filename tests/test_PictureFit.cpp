// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 Joaquin Villodre
//
// v0.42 Resize (src/PictureFit.h): where in the clip the input's picture
// is drawn for Fit / Fill / Stretch / Original.

#include "test_framework.h"
#include "PictureFit.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using ajainput::PixelRect;
using ajainput::PictureRect;
using ajainput::ResizeMode;

namespace ajainput::testing
{
std::string ToTestString(const PixelRect& r)
{
    char buf[80];
    std::snprintf(buf, sizeof(buf), "(%d, %d, %d x %d)", r.x, r.y, r.width, r.height);
    return buf;
}
} // namespace ajainput::testing

namespace
{
PixelRect R(int x, int y, int w, int h)
{
    PixelRect r;
    r.x      = x;
    r.y      = y;
    r.width  = w;
    r.height = h;
    return r;
}
} // namespace

TEST_CASE(PictureFit, OptionValues)
{
    // Saved in compositions: the values must keep their meaning.
    EXPECT_EQ(ajainput::ResizeModeFromOption(0.0f), ResizeMode::Fit);
    EXPECT_EQ(ajainput::ResizeModeFromOption(1.0f), ResizeMode::Fill);
    EXPECT_EQ(ajainput::ResizeModeFromOption(2.0f), ResizeMode::Stretch);
    EXPECT_EQ(ajainput::ResizeModeFromOption(3.0f), ResizeMode::Original);
    EXPECT_EQ(ajainput::ResizeModeFromOption(0.9999f), ResizeMode::Fill);
    // A value no option has (a later version's composition): Fit.
    EXPECT_EQ(ajainput::ResizeModeFromOption(4.0f), ResizeMode::Fit);
    EXPECT_EQ(ajainput::ResizeModeFromOption(-1.0f), ResizeMode::Fit);
    EXPECT_EQ(ajainput::ResizeModeFromOption(std::numeric_limits<float>::quiet_NaN()),
              ResizeMode::Fit);
    EXPECT_EQ(ajainput::ResizeModeFromOption(std::numeric_limits<float>::infinity()),
              ResizeMode::Fit);
    TEST_RETURN_OK;
}

TEST_CASE(PictureFit, SameAspectFillsTheClip)
{
    // The common case (an HD input on an HD composition) is the full-clip
    // draw of v0.41 in every mode but Original at another size.
    const PixelRect hd = R(0, 0, 1920, 1080);
    const PixelRect uhd = R(0, 0, 3840, 2160);
    for (ResizeMode m : { ResizeMode::Fit, ResizeMode::Fill, ResizeMode::Stretch,
                          ResizeMode::Original })
        EXPECT_EQ(PictureRect(m, 1920, 1080, hd), hd);
    for (ResizeMode m : { ResizeMode::Fit, ResizeMode::Fill, ResizeMode::Stretch })
    {
        EXPECT_EQ(PictureRect(m, 1920, 1080, uhd), uhd);
        EXPECT_EQ(PictureRect(m, 3840, 2160, hd), hd);
        EXPECT_EQ(PictureRect(m, 1280, 720, hd), hd);
    }
    TEST_RETURN_OK;
}

TEST_CASE(PictureFit, StretchIsAlwaysTheClip)
{
    const PixelRect vp = R(10, 20, 1920, 1200);
    EXPECT_EQ(PictureRect(ResizeMode::Stretch, 720, 576, vp), vp);
    EXPECT_EQ(PictureRect(ResizeMode::Stretch, 4096, 2160, vp), vp);
    TEST_RETURN_OK;
}

TEST_CASE(PictureFit, StandardDefinitionIsFourByThree)
{
    EXPECT_TRUE(ajainput::IsStandardDefinition(720, 576));
    EXPECT_TRUE(ajainput::IsStandardDefinition(720, 486));
    EXPECT_FALSE(ajainput::IsStandardDefinition(1280, 720));
    EXPECT_FALSE(ajainput::IsStandardDefinition(0, 576));
    EXPECT_EQ(ajainput::DisplayWidth(720, 576), 768.0);
    EXPECT_EQ(ajainput::DisplayWidth(720, 486), 648.0);
    EXPECT_EQ(ajainput::DisplayWidth(640, 480), 640.0);
    EXPECT_EQ(ajainput::DisplayWidth(1920, 1080), 1920.0);

    // PAL and NTSC on an HD composition: a 4:3 pillarbox.
    const PixelRect hd = R(0, 0, 1920, 1080);
    EXPECT_EQ(PictureRect(ResizeMode::Fit, 720, 576, hd), R(240, 0, 1440, 1080));
    EXPECT_EQ(PictureRect(ResizeMode::Fit, 720, 486, hd), R(240, 0, 1440, 1080));
    // On a 4:3 composition SD fills it.
    EXPECT_EQ(PictureRect(ResizeMode::Fit, 720, 576, R(0, 0, 1024, 768)), R(0, 0, 1024, 768));
    TEST_RETURN_OK;
}

TEST_CASE(PictureFit, FitLetterboxesAndPillarboxes)
{
    // DCI 4K on a UHD composition: bars top and bottom.
    EXPECT_EQ(PictureRect(ResizeMode::Fit, 4096, 2160, R(0, 0, 3840, 2160)),
              R(0, 67, 3840, 2025));
    // HD on a 16:10 composition.
    EXPECT_EQ(PictureRect(ResizeMode::Fit, 1920, 1080, R(0, 0, 1920, 1200)),
              R(0, 60, 1920, 1080));
    // HD on a portrait composition.
    EXPECT_EQ(PictureRect(ResizeMode::Fit, 1920, 1080, R(0, 0, 1080, 1920)),
              R(0, 656, 1080, 608));
    // Centred in a viewport that does not start at the origin.
    EXPECT_EQ(PictureRect(ResizeMode::Fit, 1920, 1080, R(100, 50, 1920, 1200)),
              R(100, 110, 1920, 1080));
    TEST_RETURN_OK;
}

TEST_CASE(PictureFit, FillCoversAndCuts)
{
    const PixelRect hd = R(0, 0, 1920, 1080);
    const PixelRect sd = PictureRect(ResizeMode::Fill, 720, 576, hd);
    EXPECT_EQ(sd, R(0, -180, 1920, 1440));
    EXPECT_TRUE(ajainput::Covers(sd, hd));

    const PixelRect wide = R(0, 0, 1920, 1200);
    const PixelRect r = PictureRect(ResizeMode::Fill, 1920, 1080, wide);
    EXPECT_EQ(r, R(-107, 0, 2133, 1200));
    EXPECT_TRUE(ajainput::Covers(r, wide));
    TEST_RETURN_OK;
}

TEST_CASE(PictureFit, OriginalIsNotScaled)
{
    EXPECT_EQ(PictureRect(ResizeMode::Original, 1920, 1080, R(0, 0, 3840, 2160)),
              R(960, 540, 1920, 1080));
    // Larger than the clip: centred, the rest is cut.
    const PixelRect hd = R(0, 0, 1920, 1080);
    const PixelRect big = PictureRect(ResizeMode::Original, 3840, 2160, hd);
    EXPECT_EQ(big, R(-960, -540, 3840, 2160));
    EXPECT_TRUE(ajainput::Covers(big, hd));
    // SD at its 4:3 display size.
    EXPECT_EQ(PictureRect(ResizeMode::Original, 720, 576, hd), R(576, 252, 768, 576));
    TEST_RETURN_OK;
}

TEST_CASE(PictureFit, NoOnePixelBars)
{
    // 1366 x 768 is almost 16:9: rounding must not leave a 1-pixel bar.
    const PixelRect vp = R(0, 0, 1366, 768);
    EXPECT_EQ(PictureRect(ResizeMode::Fit, 1920, 1080, vp), vp);
    EXPECT_EQ(PictureRect(ResizeMode::Fill, 1920, 1080, vp), vp);
    EXPECT_EQ(PictureRect(ResizeMode::Fit, 1920, 1080, R(0, 0, 1364, 768)), R(0, 0, 1364, 768));
    // A real 3-line difference is kept.
    EXPECT_EQ(PictureRect(ResizeMode::Fit, 1920, 1080, R(0, 0, 1920, 1083)),
              R(0, 1, 1920, 1080));
    TEST_RETURN_OK;
}

TEST_CASE(PictureFit, UnknownSizesGiveTheClip)
{
    const PixelRect vp = R(0, 0, 1920, 1080);
    EXPECT_EQ(PictureRect(ResizeMode::Fit, 0, 0, vp), vp);
    EXPECT_EQ(PictureRect(ResizeMode::Fill, 1920, 0, vp), vp);
    EXPECT_EQ(PictureRect(ResizeMode::Original, -5, 1080, vp), vp);
    EXPECT_EQ(PictureRect(ResizeMode::Fit, 1920, 1080, R(0, 0, 0, 0)), R(0, 0, 0, 0));
    EXPECT_EQ(PictureRect(ResizeMode::Fit, 1920, 1080, R(0, 0, 100, -1)), R(0, 0, 100, -1));
    TEST_RETURN_OK;
}

TEST_CASE(PictureFit, StaysWithinTheViewportLimit)
{
    // Original of a picture larger than the GL takes: shrunk evenly.
    const PixelRect r = PictureRect(ResizeMode::Original, 8192, 4320,
                                    R(0, 0, 1920, 1080), 4096);
    EXPECT_EQ(r, R(-1088, -540, 4096, 2160));
    // Fill of HD into a tall clip, with the default limit (16384).
    const PixelRect t = PictureRect(ResizeMode::Fill, 1920, 1080, R(0, 0, 2048, 16384));
    EXPECT_TRUE(t.width <= ajainput::kDefaultMaxViewportDim);
    EXPECT_TRUE(t.height <= ajainput::kDefaultMaxViewportDim);
    EXPECT_EQ(t, R(-7168, 3584, 16384, 9216));
    TEST_RETURN_OK;
}

TEST_CASE(PictureFit, PropertiesOverManySizes)
{
    const int pictures[][2] = { {720, 486}, {720, 576}, {1280, 720}, {1920, 1080},
                                {2048, 1080}, {3840, 2160}, {4096, 2160}, {640, 480},
                                {1024, 768} };
    const int viewports[][2] = { {1920, 1080}, {1920, 1200}, {1024, 768}, {3840, 2160},
                                 {1366, 768}, {800, 600}, {7680, 1080}, {1080, 1920},
                                 {1, 1}, {3, 2}, {333, 777} };
    for (const auto& p : pictures)
    {
        const double aspect = ajainput::DisplayWidth(p[0], p[1]) / p[1];
        for (const auto& v : viewports)
        {
            const PixelRect vp = R(7, 3, v[0], v[1]);
            const PixelRect fit  = PictureRect(ResizeMode::Fit,  p[0], p[1], vp);
            const PixelRect fill = PictureRect(ResizeMode::Fill, p[0], p[1], vp);
            // Fit: inside the clip, touching two opposite sides.
            EXPECT_TRUE(fit.x >= vp.x && fit.y >= vp.y &&
                        fit.x + fit.width  <= vp.x + vp.width &&
                        fit.y + fit.height <= vp.y + vp.height);
            EXPECT_TRUE(fit.width == vp.width || fit.height == vp.height);
            // Fill: covers the clip.
            EXPECT_TRUE(ajainput::Covers(fill, vp));
            // Both centred (within a pixel).
            for (const PixelRect& r : { fit, fill })
            {
                const int left   = r.x - vp.x;
                const int right  = (vp.x + vp.width) - (r.x + r.width);
                const int bottom = r.y - vp.y;
                const int top    = (vp.y + vp.height) - (r.y + r.height);
                EXPECT_TRUE(std::abs(left - right) <= 1);
                EXPECT_TRUE(std::abs(bottom - top) <= 1);
                // The input's aspect, unless snapped to the clip or tiny.
                if (r != vp && r.width >= 16 && r.height >= 16)
                {
                    const double got = static_cast<double>(r.width) / r.height;
                    const double tol = 1.0 / r.width + aspect / r.height;
                    EXPECT_TRUE(std::fabs(got - aspect) <= tol * 1.01);
                }
            }
        }
    }
    TEST_RETURN_OK;
}

TEST_CASE(PictureFit, IntersectAndCovers)
{
    EXPECT_EQ(ajainput::Intersect(R(0, 0, 100, 100), R(50, 25, 100, 100)), R(50, 25, 50, 75));
    const PixelRect none = ajainput::Intersect(R(0, 0, 10, 10), R(20, 20, 5, 5));
    EXPECT_EQ(none.width, 0);
    EXPECT_EQ(none.height, 0);
    EXPECT_TRUE(ajainput::Covers(R(-10, -10, 120, 120), R(0, 0, 100, 100)));
    EXPECT_TRUE(ajainput::Covers(R(0, 0, 100, 100), R(0, 0, 100, 100)));
    EXPECT_FALSE(ajainput::Covers(R(0, 1, 100, 99), R(0, 0, 100, 100)));
    TEST_RETURN_OK;
}
