#pragma once

namespace mdk {

// Inverts a row-major 4x4 matrix using the same operation order as the
// System.Numerics.Matrix4x4.Invert path the C# oracle runs on x64. The sky
// clip-to-direction matrix is sensitive to last-bit differences, so this is
// deliberately a translation of that path and not a shorter cofactor form.
// Returns false for a singular matrix (or one below .NET's epsilon).
bool invertSystemNumericsMatrix4(const float in[16], float out[16]);

}  // namespace mdk
