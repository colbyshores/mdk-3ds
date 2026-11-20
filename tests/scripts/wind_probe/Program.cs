using System.Globalization;
using System.Numerics;
using System.Reflection;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Level;
using Mdk.Game.Scripts;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE UPDRAFT / WIND ORACLE: the C# Kurt and ScriptRuntime (src/Mdk.Game) stepped together at
// 1/60 s as Viewer.RunSteps does -- Kurt, then the scripts, then ArenaSpace.SetSolid(SolidArenas)
// -- from a placed start, under a key schedule. It pins package 8 (docs/PORT_GAPS.md): Kurt in
// the fans' updrafts (Kurt.Updraft.cs, ScriptRuntime.KurtUpdraft: his arena's fans, else the
// second arena's), the fans' sparks (ScriptRuntime.FanSpark -> Debris.Spark Still) that the
// updraft then lifts (Debris.Move's Updraft), the wind zones (opcode 224, ScriptRuntime.WindZone)
// and the slide they start (Kurt.Slide.cs, with FallMove's Sliding branch and Kurt.EnterArena).
// Per step it prints Kurt's arena, state, feet, yaw, vertical speed, floor/chute/updraft/slide
// flags, the slide's velocity, cap and its two tick counters, the view's roll, the fans' count
// and their shared wobble, the debris pieces (count, centre sums and vertical-speed sum), and the
// mixer's voices. Its output is tests/scripts/golden/L*_wind_*.txt, and the C++ twin is
// `tick1 dump wind <level> <steps> <x,y,z,yaw> <schedule>` (tests/scripts/tick1.cpp,
// kurtWindDump), which must print the same bytes.
//
//   Probe <level> <steps> <x,y,z,yaw> <schedule>
//
// The schedule is comma-separated `key:from-to` holds over steps [from, to) (fwd back left right
// sleft sright jump turbo fire).
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/items_probe -- copy
// src/Mdk.Formats, src/Mdk.Engine, src/Mdk.Game and Directory.Build.props into a scratch tree with
// this directory beside src/; in the scratch Mdk.Engine.csproj drop the shader targets and the
// icon resource; `dotnet build -c Release -o probe-bin`, then
// `DOTNET_ROOT=<sdk> MDK_DATA_DIR=<the MDK folder> probe-bin/Probe ...`.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
var level = int.Parse(args[0]);
var steps = int.Parse(args[1]);
var at = args[2].Split(',').Select(s => float.Parse(s, CultureInfo.InvariantCulture)).ToArray();
var schedule = args.Length > 3 ? args[3] : "";
var data = MdkData.Find() ?? throw new Exception("no data");
var levelData = new LevelData(data, level);
var space = new ArenaSpace();
foreach (var arena in levelData.Arenas.Where(a => levelData.IsReachable(a.Name)))
{
    space.Add(arena);
}

var bank = SoundBank.ForLevel(data, level);
bank.Preload();
var device = new AudioDevice(Output.Muted);
var mixer = new SoundMixer(device, bank.Get);
var sprites = Bni.Load(data.PathOf("TRAVERSE/TRAVSPRT.BNI"));
string[] names = ["K_STILL", "K_IDLE", "K_RUN", "K_SIDE", "K_TRN45", "K_JUMP", "K_RJMP", "K_FALL",
    "K_CHUTE", "K_LAND", "K_SHOT", "K_RUNFIR", "K_SPWEP", "K_BANG", "K_BFLIP", "K_BANG",
    "K_SLIP", "K_SLIDE", "K_FSLIDE", "K_BSLIDE", "K_HANG", "K_CRASHL", "K_ROLLL", "K_ROLLR", "K_HELM", "K_HELM"];
int FrameCount(KurtT.State s) => sprites.Has(names[(int)s]) ? sprites.GetAnimation(names[(int)s]).FrameCount : 0;
var kurt = new KurtT(space, mixer, FrameCount)
{
    Feet = new Vector3(at[0], at[1], at[2]),
    Yaw = at.Length > 3 ? at[3] : levelData.Dti.StartAngle,
};
var cmi = Cmi.Load(data.PathOf($"TRAVERSE/LEVEL{level}/LEVEL{level}.CMI"));
var groups = new TriangleGroups();
foreach (var arena in levelData.Arenas)
{
    groups.Add(arena);
}

var runtime = new ScriptRuntime(levelData, cmi, sprites, space, groups, mixer, kurt, 1) { Option = 1 };
var voicesField = typeof(SoundMixer).GetField("_voices", BindingFlags.NonPublic | BindingFlags.Instance)!;
string Voices()
{
    var list = (System.Collections.IList)voicesField.GetValue(mixer)!;
    var parts = new List<string>();
    foreach (var v in list)
    {
        parts.Add((string)v!.GetType().GetField("Name")!.GetValue(v)!);
    }

    return parts.Count == 0 ? "-" : string.Join(",", parts);
}

float Private(object o, string name) => (float)o.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(o)!;

var keys = new Dictionary<string, Key>
{
    ["fwd"] = Key.Forward, ["back"] = Key.Back, ["left"] = Key.TurnLeft, ["right"] = Key.TurnRight,
    ["sleft"] = Key.StrafeLeft, ["sright"] = Key.StrafeRight, ["jump"] = Key.Jump, ["turbo"] = Key.Turbo,
    ["fire"] = Key.Fire,
};
var holds = new List<(Key Key, int From, int To)>();
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    var range = parts[1].Split('-');
    holds.Add((keys[parts[0]], int.Parse(range[0]), int.Parse(range[1])));
}

const float Step = 1f / 60f;
int P(float v) => (int)MathF.Floor(v * 64f);
string F(float v) => v.ToString("F4", CultureInfo.InvariantCulture);
int B(bool b) => b ? 1 : 0;
var input = new Input();
Console.WriteLine($"level={level} steps={steps} at={args[2]} seed=1 schedule={schedule}");
for (var step = 1; step <= steps; step++)
{
    foreach (var key in keys.Values)
    {
        var down = holds.Any(h => h.Key == key && step >= h.From && step < h.To);
        input.Hold(key, down ? Input.State.Down : Input.State.Up);
    }

    kurt.Update(input, Step);
    runtime.Update(Step);
    space.SetSolid(runtime.SolidArenas);

    long sx = 0, sy = 0, sz = 0, vz = 0;
    var sparks = 0;
    foreach (var piece in runtime.Debris.Pieces)
    {
        sparks += piece.IsSpark ? 1 : 0;
        sx += P(piece.Center.X);
        sy += P(piece.Center.Y);
        sz += P(piece.Center.Z);
        vz += (long)MathF.Floor(piece.Velocity.Z * 4096f);
    }

    var second = runtime.SecondArena.Length == 0 ? "-" : runtime.SecondArena;
    Console.WriteLine(
        $"s={step} arena={(runtime.CurrentArena.Length == 0 ? "-" : runtime.CurrentArena)} second={second}{(runtime.SecondActive ? "+" : "")} " +
        $"state={kurt.Current} feet64={P(kurt.Feet.X)},{P(kurt.Feet.Y)},{P(kurt.Feet.Z)} yaw={F(kurt.Yaw)} vz={F(kurt.VerticalSpeed)} " +
        $"floor={B(kurt.OnFloor)} chute={B(kurt.ChuteOpen)} up={B(kurt.InUpdraft)} slide={B(kurt.Sliding)} " +
        $"sv={F(kurt.SlideVelocity.X)},{F(kurt.SlideVelocity.Y)} cap={F(Private(kurt, "_slideCap"))} " +
        $"left={F(Private(kurt, "_slideLeft"))} air={F(Private(kurt, "_slideAir"))} roll={F(kurt.CameraRoll.Roll)} hp={kurt.Health} " +
        $"fans={runtime.Fans.Count} wob={F(Private(runtime.Fans, "_wobble"))} debris={runtime.Debris.Pieces.Count}/{sparks} " +
        $"dsum={sx},{sy},{sz} dvz={vz} voices={Voices()}");
}
