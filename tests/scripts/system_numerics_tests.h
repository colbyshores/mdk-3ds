#pragma once

namespace mdk {
namespace test {

// The Linux sky's System.Numerics-compatible matrix inverse, checked against
// exact bits produced by the .NET 10 oracle.
int runSystemNumericsTests();

}  // namespace test
}  // namespace mdk
