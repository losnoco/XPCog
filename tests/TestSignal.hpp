// Shared constants for the synthetic signals the tests generate.
//
// M_PI is not standard C++ and MSVC does not define it without
// _USE_MATH_DEFINES, so every test that generated a sine broke the Windows
// build. Defining the macro would work; declaring the constant is what the
// language actually offers.

#pragma once

namespace xpcog::test {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;

}  // namespace xpcog::test
