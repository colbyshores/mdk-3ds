#pragma once

namespace mdk {

// Inverts a row-major 4x4 matrix following the operation order of the
// System.Numerics.Matrix4x4.Invert path the C# oracle runs on x64. The sky
// clip-to-direction matrix is sensitive to last-bit differences, so this
// mirrors that path, including Vector4.Dot's pairwise reduction, rather than
// using a shorter cofactor form.
// Returns false for a singular matrix (or one below .NET's epsilon).
bool invertSystemNumericsMatrix4(const float in[16], float out[16]);

}  // namespace mdk
