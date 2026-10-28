#pragma once

#ifdef TEST_DEPTH
#include "depth_fixture.h"
using RasterFixture = DepthFixture;
static constexpr float raster_depths[] = {.25f, .75f};
#else
#include "portable_fixture.h"
using RasterFixture = PortableFixture;
static constexpr float raster_depths[] = {.5f, .2f};
#endif
