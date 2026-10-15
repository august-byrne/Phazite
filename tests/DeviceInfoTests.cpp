// SPDX-License-Identifier: AGPL-3.0-or-later
// How devices describe themselves and their parameters, without any engine (src/devices/core/DeviceInfo.h).
#include "TestHelpers.h"
#include "devices/core/DeviceInfo.h"

#include <array>
#include <limits>

namespace
{
namespace devices = phazite::devices;

constexpr std::string_view shapes[] = { "Sine", "Saw", "Square" };

constexpr devices::ParameterInfo cutoff { .id = "cutoff",
                                          .name = "Cutoff",
                                          .minimum = 20.0f,
                                          .maximum = 20000.0f,
                                          .defaultValue = 2000.0f,
                                          .unit = devices::Unit::hertz,
                                          .midpoint = 1000.0f };

constexpr devices::ParameterInfo shape { .id = "shape",
                                         .name = "Shape",
                                         .minimum = 0.0f,
                                         .maximum = 2.0f,
                                         .defaultValue = 1.0f,
                                         .unit = devices::Unit::choice,
                                         .choices = shapes };

constexpr devices::ParameterInfo bypass { .id = "bypass", .name = "Bypass", .unit = devices::Unit::toggle };
} // namespace

TEST_CASE ("A device parameter's range is skewed around its midpoint, and whole for choices")
{
    REQUIRE (devices::isValid (cutoff));
    const auto range = devices::getRange (cutoff);
    CHECK (range.convertFrom0to1 (0.5f) == doctest::Approx (1000.0f).epsilon (0.001));
    CHECK (range.convertFrom0to1 (0.0f) == doctest::Approx (20.0f));
    CHECK (range.convertFrom0to1 (1.0f) == doctest::Approx (20000.0f));

    REQUIRE (devices::isValid (shape));
    CHECK (devices::getRange (shape).interval == doctest::Approx (1.0f));
    CHECK (devices::constrain (shape, 1.4f) == doctest::Approx (1.0f));
    CHECK (devices::constrain (shape, 7.0f) == doctest::Approx (2.0f));
    CHECK (devices::constrain (cutoff, 5.0f) == doctest::Approx (20.0f));
    CHECK (devices::constrain (cutoff, std::numeric_limits<float>::quiet_NaN()) == doctest::Approx (2000.0f));

    REQUIRE (devices::isValid (bypass));
    CHECK (devices::constrain (bypass, 0.7f) == doctest::Approx (1.0f));
}

TEST_CASE ("A device parameter's description must hold together")
{
    auto p = cutoff;
    p.defaultValue = 30000.0f; // outside the range
    CHECK_FALSE (devices::isValid (p));

    p = cutoff;
    p.midpoint = 20.0f; // not strictly inside
    CHECK_FALSE (devices::isValid (p));

    p = cutoff;
    p.minimum = 20000.0f; // an empty range
    CHECK_FALSE (devices::isValid (p));

    p = cutoff;
    p.id = {};
    CHECK_FALSE (devices::isValid (p));

    p = shape;
    p.maximum = 3.0f; // four values, three choices
    CHECK_FALSE (devices::isValid (p));

    p = shape;
    p.defaultValue = 0.5f; // not a whole number
    CHECK_FALSE (devices::isValid (p));

    p = shape;
    p.midpoint = 1.0f; // whole steps can't be skewed
    CHECK_FALSE (devices::isValid (p));

    p = cutoff;
    p.choices = shapes; // only choices have choices
    CHECK_FALSE (devices::isValid (p));

    p = bypass;
    p.maximum = 2.0f; // a toggle is 0 or 1
    CHECK_FALSE (devices::isValid (p));
}

TEST_CASE ("A device finds its parameters by ID, and their IDs must be unique")
{
    const std::array parameters { cutoff, shape, bypass };
    devices::DeviceInfo device { .typeID = "phaziteTest", .name = "Test", .kind = devices::DeviceKind::effect, .parameters = parameters };

    CHECK (devices::isValid (device));
    CHECK (devices::findParameter (device, "cutoff") == 0);
    CHECK (devices::findParameter (device, "bypass") == 2);
    CHECK (devices::findParameter (device, "resonance") == -1);

    const std::array twice { cutoff, shape, cutoff };
    device.parameters = twice;
    CHECK_FALSE (devices::isValid (device));

    device.parameters = parameters;
    device.typeID = {};
    CHECK_FALSE (devices::isValid (device));
}
