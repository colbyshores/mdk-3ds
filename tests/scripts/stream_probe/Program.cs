using System.Globalization;
using System.Numerics;
using System.Reflection;
using System.Text;
using Mdk.Formats;
using Mdk.Game.Kurt;
using Mdk.Game.Stream;

// THE STREAM ORACLE: the C# stream simulation (src/Mdk.Game/Stream: Flight, StreamTube, TubeBasis,
// WatcomRandom) run on its own, as StreamScreen builds it but with no view, a fixed seed (the C#'s
// direct-start path seeds WatcomRandom itself) and a scripted steering schedule. Its output is
// tests/scripts/golden/stream_*.txt, and the C++ twin is `tick1 dump stream ...` / `tick1 dump
// streamrand ...` (tests/scripts/stream_dump.cpp), which must print the same bytes.
//
//   Probe rand 1,0,4294967295,305419896 256                     > golden/stream_rand.txt
//   Probe stream <level> <difficulty 0-2> <seed> <health> <max steps> <dt> <schedule>
//
// <dt> is 30 or 60 (a fixed 1/30 or 1/60 s), or mix (1/60, 1/30, 0.05, 0.15, 1/45 in turn: the
// 0.15 is clamped to the flight's 0.1 and the odd ones leave tick fractions), or fast (0.1, 0.07,
// 0.15, 1/30 in turn: a whole flight in ~450 steps, for the long runs). The schedule is
// comma-separated items, steps counted from 1, ranges inclusive:
//   right:a-b  left:a-b  up:a-b  down:a-b     the steering keys held
//   stop:n                                     Flight.Stop() before step n's update (Esc)
//   auto:a-b                                   a pilot: the keys that steer back to the axis
//                                              (|x| or |z| over 0.5 units), read off Kurt
//                                              before the update; added to any keys held
// It stops after the update that ends the flight. Every float is printed as its IEEE bits (%08X),
// every double as %016X: parity is exact, and no decimal formatting rule can differ.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404, linux-x64): the four oracle files are
// compiled straight from src/ (Probe.csproj links them; Difficulty from Kurt/Inventory.cs is restated
// below because that file needs the rest of Mdk.Game), so no scratch tree is needed:
//   dotnet build -c Release --artifacts-path <scratch> tests/scripts/stream_probe/Probe.csproj
//   DOTNET_ROOT=<the SDK> MDK_DATA_DIR=<the MDK folder> <scratch>/bin/Probe/release/Probe stream ...
// (--artifacts-path keeps bin/ and obj/ out of the tree.) Each golden's first line is its arguments.
// THE HOST MATTERS: on this machine .NET's Vector3.Lerp is a fused multiply-add (measured: 200000
// of 200000 random cases equal MathF.FusedMultiplyAdd(a, 1 - t, b * t), 90889 equal the unfused
// form), so the goldens are an FMA host's; stream/tube_basis.h says how the C++ matches it.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;

static string H(float v) => BitConverter.SingleToUInt32Bits(v).ToString("X8");
static string HD(double v) => BitConverter.DoubleToUInt64Bits(v).ToString("X16");
static string V(Vector3 v) => $"{H(v.X)},{H(v.Y)},{H(v.Z)}";

if (args[0] == "rand")
{
    var count = int.Parse(args[2]);
    foreach (var s in args[1].Split(','))
    {
        var seed = uint.Parse(s);
        var sequence = new WatcomRandom(seed);
        Console.WriteLine($"seed={seed} count={count}");
        var line = new StringBuilder();
        for (var i = 0; i < count; i++)
        {
            line.Append(i % 16 == 0 ? "" : " ").Append(sequence.Next().ToString("X4"));
            if (i % 16 == 15 || i == count - 1)
            {
                Console.WriteLine(line);
                line.Clear();
            }
        }
    }

    return;
}

var level = int.Parse(args[1]);
var difficulty = (Difficulty)int.Parse(args[2]);
var seedArg = uint.Parse(args[3]);
var health = int.Parse(args[4]);
var maxSteps = int.Parse(args[5]);
var dtMode = args[6];
var schedule = args.Length > 7 ? args[7] : "";

// StreamScreen's constructor: the level's index in GameState.Order, clamped to the four with
// statistics and Gunter's.
int[] order = [7, 6, 3, 4, 8, 5];
var index = Math.Clamp(Array.IndexOf(order, level), 0, 4);
var kind = index > 3 ? StreamTube.Kind.Gunter : StreamTube.Kind.Normal;

var data = MdkData.Find() ?? throw new Exception("no data");
var bni = Bni.Load(data.PathOf("STREAM/STREAM.BNI"));
Clip ClipOf(string name)
{
    var a = ModelAnimation.Parse(name, bni.Bytes, bni.Entries[name].Offset);
    return new Clip(a.FrameCount, a.Speed);
}

var kurtClip = ClipOf("KURTANIM");
var rescueClip = ClipOf("BONESANIM");
var otherClip = ClipOf(kind == StreamTube.Kind.Gunter ? "GUNTANIM" : "SWHANM");

var random = new WatcomRandom(seedArg);
var tube = new StreamTube(index, difficulty, kind, random);
var flight = new Flight(tube, kind, difficulty, random, health, kurtClip, rescueClip, otherClip);

const BindingFlags Private = BindingFlags.NonPublic | BindingFlags.Instance;
var stateField = typeof(WatcomRandom).GetField("_state", Private)!;
var basesField = typeof(StreamTube).GetField("_bases", Private)!;
var radiusField = typeof(StreamTube).GetField("_radius", Private)!;
var shadeField = typeof(StreamTube).GetField("_shade", Private)!;
var shadeFromField = typeof(StreamTube).GetField("_shadeFrom", Private)!;
var shadeToField = typeof(StreamTube).GetField("_shadeTo", Private)!;
var directionField = typeof(Flight).GetField("_direction", Private)!;
var tickFractionField = typeof(Flight).GetField("_tickFraction", Private)!;
var fadeField = typeof(Flight).GetField("_fade", Private)!;
var endingField = typeof(Flight).GetField("_ending", Private)!;

// FNV-1a over 32-bit words, little-endian bytes.
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

void PrintRing(int n)
{
    var bases = (TubeBasis[])basesField.GetValue(tube)!;
    var b = bases[n & (StreamTube.Rings - 1)];
    var points = FnvStart;
    for (var j = 0; j < StreamTube.Points; j++)
    {
        points = FnvV(points, tube.Point(n, j));
    }

    var line = new StringBuilder($"R n={n} o={V(tube.Origin(n))} b={V(b.X)};{V(b.Y)};{V(b.Z)} pts={points:X8}");
    // The segment before ring n got its colours and planes when ring n was made.
    if (n > tube.Tail)
    {
        line.Append(" col=");
        for (var j = 0; j < StreamTube.Points; j++)
        {
            line.Append(tube.Colour(n - 1, j).ToString("X2"));
        }

        var planes = FnvStart;
        for (var i = 0; i < StreamTube.Points * 2; i++)
        {
            var (normal, distance) = tube.Plane(n - 1, i);
            planes = FnvF(FnvV(planes, normal), distance);
        }

        line.Append($" pl={planes:X8}");
    }

    Console.WriteLine(line);
}

void PrintWalk() =>
    Console.WriteLine($"W head={tube.Head} ang={V(tube.Angles)} rad={HD((double)radiusField.GetValue(tube)!)} " +
                      $"shade={HD((double)shadeField.GetValue(tube)!)} {(int)shadeFromField.GetValue(tube)!} {(int)shadeToField.GetValue(tube)!}");

var seen = new HashSet<Thing>();
void PrintSpawns()
{
    foreach (var (spawn, thing) in flight.Sprites)
    {
        if (seen.Add(thing))
        {
            Console.WriteLine($"P {spawn.Kind} t={H(spawn.T)} x={H(spawn.X)} z={H(spawn.Z)} size={H(spawn.Size)} speed={H(spawn.Speed)}");
        }
    }
}

static string ThingOf(Thing? t) => t == null ? "-" : $"{H(t.T)},{H(t.X)},{H(t.Z)},{H(t.Speed)},{H(t.Yaw)},{H(t.Frame)}";

static string M(Matrix4x4 m) =>
    $"{H(m.M11)},{H(m.M12)},{H(m.M13)},{H(m.M21)},{H(m.M22)},{H(m.M23)},{H(m.M31)},{H(m.M32)},{H(m.M33)},{H(m.M41)},{H(m.M42)},{H(m.M43)}";

var triangles = new List<TubeVertex>();
void PrintView(int step)
{
    triangles.Clear();
    foreach (var n in tube.DrawnSegments())
    {
        tube.AddTriangles(n, flight.Eye, triangles);
    }

    var tri = FnvStart;
    foreach (var v in triangles)
    {
        var (r, g, b, a) = tube.Rgba(v);
        tri = Fnv(FnvV(tri, v.Position), (uint)(v.Colour | (v.Level << 8) | (r << 16)));
        tri = Fnv(tri, (uint)(g | (b << 8) | (a << 16)));
    }

    var sprites = FnvStart;
    foreach (var (_, thing) in flight.Sprites)
    {
        var m = flight.Transform(thing);
        foreach (var f in new[] { m.M11, m.M12, m.M13, m.M21, m.M22, m.M23, m.M31, m.M32, m.M33, m.M41, m.M42, m.M43 })
        {
            sprites = FnvF(sprites, f);
        }

        sprites = Fnv(sprites, (uint)flight.OtherFrame(thing));
    }

    var other = flight.Other == null ? "-" : $"{M(flight.Transform(flight.Other))} of={flight.OtherFrame(flight.Other)}";
    Console.WriteLine($"V s={step} kurt={M(flight.KurtTransform())} kf={flight.KurtFrame()} bf={flight.BonesFrame()} " +
                      $"other={other} verts={triangles.Count} tri={tri:X8} spr={sprites:X8}");
}

float DtOf(int step) => dtMode switch
{
    "30" => 1f / 30f,
    "60" => 1f / 60f,
    "fast" => new[] { 0.1f, 0.07f, 0.15f, 1f / 30f }[(step - 1) % 4],
    _ => new[] { 1f / 60f, 1f / 30f, 0.05f, 0.15f, 1f / 45f }[(step - 1) % 5],
};

var keys = schedule.Split(',', StringSplitOptions.RemoveEmptyEntries)
    .Select(item => item.Split(':'))
    .Select(p => (Name: p[0], From: int.Parse(p[1].Split('-')[0]), To: int.Parse(p[1].Split('-')[^1])))
    .ToList();
bool Held(string name, int step) => keys.Any(k => k.Name == name && step >= k.From && step <= k.To);

Console.WriteLine($"stream level={level} index={index} kind={kind} difficulty={(int)difficulty} seed={seedArg} health={health} " +
                  $"steps={maxSteps} dt={dtMode} schedule={(schedule.Length == 0 ? "-" : schedule)}");
Console.WriteLine($"clips kurt={kurtClip.FrameCount}/{H(kurtClip.Speed)} rescue={rescueClip.FrameCount}/{H(rescueClip.Speed)} " +
                  $"other={otherClip.FrameCount}/{H(otherClip.Speed)}");
Console.WriteLine($"tube turn={H(tube.MaxTurn)} radius={H(tube.MinRadius)}-{H(tube.MaxRadius)} lights={flight.LightCount} " +
                  $"ramp={string.Concat(tube.Ramp.Select(c => $"{c.R:X2}{c.G:X2}{c.B:X2}"))}");
for (var n = tube.Tail; n < tube.Head; n++)
{
    PrintRing(n);
}

PrintWalk();
PrintSpawns();
PrintView(0);

for (var step = 1; step <= maxSteps; step++)
{
    if (keys.Any(k => k.Name == "stop" && k.From == step))
    {
        flight.Stop();
    }

    var right = (Held("right", step) ? 1 : 0) - (Held("left", step) ? 1 : 0);
    var up = (Held("up", step) ? 1 : 0) - (Held("down", step) ? 1 : 0);
    if (Held("auto", step))
    {
        // Right moves Kurt to +x and up to +z (Flight.Steer), so steer against the offset.
        right = Math.Clamp(right + (flight.Kurt.X > 0.5f ? -1 : flight.Kurt.X < -0.5f ? 1 : 0), -1, 1);
        up = Math.Clamp(up + (flight.Kurt.Z > 0.5f ? -1 : flight.Kurt.Z < -0.5f ? 1 : 0), -1, 1);
    }
    var head = tube.Head;
    var tail = tube.Tail;
    flight.Update(DtOf(step), new Steering(right, up));

    for (var n = head; n < tube.Head; n++)
    {
        PrintRing(n);
    }

    if (tube.Tail != tail)
    {
        PrintWalk();
    }

    PrintSpawns();
    var k = flight.Kurt;
    var fade = flight.Fade;
    var bg = flight.BackgroundOffset;
    Console.WriteLine(
        $"s={step} seg={tube.Tail}/{tube.Head} rng={(uint)stateField.GetValue(random)!:X8} k={ThingOf(k)} p={H(flight.Pitch)} " +
        $"at={V(flight.KurtPosition)} dir={V((Vector3)directionField.GetValue(flight)!)} hp={flight.Health} " +
        $"blink={flight.Blink} tf={H((float)tickFractionField.GetValue(flight)!)} fade={H((float)fadeField.GetValue(flight)!)}" +
        $"{((bool)endingField.GetValue(flight)! ? "!" : "")} f={H(fade.Whiten)},{H(fade.Dark)},{H(fade.Red)} time={H(flight.Time)} " +
        $"eye={V(flight.Eye)} fwd={V(flight.Forward)} right={V(flight.Right)} up={V(flight.Up)} bg={H(bg.X)},{H(bg.Y)} " +
        $"other={ThingOf(flight.Other)} bones={ThingOf(flight.Bones)} spr={flight.Sprites.Count()} " +
        $"ev={(flight.Events.Count == 0 ? "-" : string.Join("+", flight.Events))} " +
        $"snd={(flight.Sounds.Count == 0 ? "-" : string.Join("+", flight.Sounds))}" +
        (flight.Ended ? $" END {flight.Outcome}" : ""));
    if (step % 15 == 0 || flight.Ended)
    {
        PrintView(step);
    }

    if (flight.Ended)
    {
        break;
    }
}

namespace Mdk.Game.Kurt
{
    /// <summary>Kurt/Inventory.cs's enum, restated (see the csproj).</summary>
    public enum Difficulty { Easy, Normal, Hard }
}
