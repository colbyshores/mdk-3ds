using System.Globalization;
using System.Numerics;
using Mdk.Formats;

// THE STREAM VIEW'S MODEL ORACLE (package #18b). StreamView.cs draws KURT, BONES and the bonus
// (SWH150) or Gunter (GUNTA) of STREAM/STREAM.BNI in the poses of KURTANIM, BONESANIM, SWHANM and
// GUNTANIM: Model.Parse(name, bytes, offset, Header.NoFlags, Parts.Named), ModelAnimation.Parse and
// Bake, and Build's corner walk (part by part, triangle by triangle, `pose[p][TriangleIndices[t*3+k]]`
// with `TriangleUvs[t*3+k]`). This prints, for every (model, animation) pair StreamView.Draw can
// ask for, what that walk reads: the clip (frame count, speed), the material names, each part's
// triangle materials and UVs, and per frame a hash of the corners' positions. The C++ twin is
// `tick1 dump streamview` (tests/scripts/stream_dump.cpp), which must print the same bytes; the
// golden is tests/scripts/golden/stream_view_models.txt.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0, linux-x64):
//   dotnet build -c Release --artifacts-path <scratch> tests/scripts/stream_view_probe/Probe.csproj
//   DOTNET_ROOT=<the SDK> MDK_DATA_DIR=<the MDK folder> <scratch>/bin/Probe/release/Probe
// Every float is printed as its IEEE bits or hashed as them (FNV-1a over 32-bit words).
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;

static string H(float v) => BitConverter.SingleToUInt32Bits(v).ToString("X8");

static uint Fnv(uint h, uint word)
{
    for (var b = 0; b < 4; b++)
    {
        h ^= (word >> (8 * b)) & 0xFF;
        h *= 16777619u;
    }

    return h;
}

static uint FnvF(uint h, float v) => Fnv(h, BitConverter.SingleToUInt32Bits(v));
static uint FnvV(uint h, Vector3 v) => FnvF(FnvF(FnvF(h, v.X), v.Y), v.Z);
const uint FnvStart = 2166136261u;

var data = Environment.GetEnvironmentVariable("MDK_DATA_DIR") ?? "MDK";
var bni = Bni.Load(Path.Combine(data, "STREAM", "STREAM.BNI"));
Console.WriteLine("stream view models");
foreach (var image in new[] { "BG", "LIGHT", "PLANET", "SC_STAT", "SNIP_TXT" })
{
    var texture = bni.GetImage(image);
    var hash = FnvStart;
    foreach (var index in texture.Indices)
    {
        hash = Fnv(hash, index);
    }

    Console.WriteLine($"image {image} {texture.Width}x{texture.Height} {hash:X8}");
}

var pairs = new[]
{
    ("KURT", "KURTANIM"), ("KURT", "BONESANIM"), ("BONES", "BONESANIM"), ("SWH150", "SWHANM"), ("GUNTA", "GUNTANIM"),
};
foreach (var (name, animationName) in pairs)
{
    var model = Model.Parse(name, bni.Bytes, bni.Entries[name].Offset, Model.Header.NoFlags, Model.Parts.Named);
    var animation = ModelAnimation.Parse(animationName, bni.Bytes, bni.Entries[animationName].Offset);
    var frames = animation.Bake(model);
    Console.WriteLine($"pose {name} {animationName} frames {animation.FrameCount} speed {H(animation.Speed)} baked {frames.Length} parts {model.PartList.Count}");
    Console.WriteLine($"  materials {string.Join(",", model.Materials)}");
    for (var p = 0; p < model.PartList.Count; p++)
    {
        var part = model.PartList[p];
        var uvs = FnvStart;
        foreach (var uv in part.TriangleUvs)
        {
            uvs = FnvF(FnvF(uvs, uv.X), uv.Y);
        }

        var materials = FnvStart;
        foreach (var m in part.TriangleMaterials)
        {
            materials = Fnv(materials, (uint)m);
        }

        Console.WriteLine($"  part {p} {part.Name} vertices {part.Vertices.Length} triangles {part.TriangleMaterials.Length} materials {materials:X8} uvs {uvs:X8}");
    }

    for (var f = 0; f < frames.Length; f++)
    {
        var corners = FnvStart;
        var min = new Vector3(float.MaxValue);
        var max = new Vector3(float.MinValue);
        for (var p = 0; p < model.PartList.Count; p++)
        {
            var part = model.PartList[p];
            for (var c = 0; c < part.TriangleIndices.Length; c++)
            {
                var v = frames[f][p][part.TriangleIndices[c]];
                corners = FnvV(corners, v);
                min = Vector3.Min(min, v);
                max = Vector3.Max(max, v);
            }
        }

        Console.WriteLine($"  frame {f} {corners:X8} min {H(min.X)} {H(min.Y)} {H(min.Z)} max {H(max.X)} {H(max.Y)} {H(max.Z)}");
    }
}
