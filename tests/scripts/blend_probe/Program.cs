using System.Globalization;
using System.Numerics;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Game;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Level;
using Mdk.Game.Objects;
using Mdk.Game.Scripts;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE FRAME BLEND ORACLE: the C# Kurt and ScriptRuntime stepped at 1/60 s (Kurt, then the scripts, as
// Viewer.RunSteps does; no follow camera), under platform_probe's schedule, with FRAMES at the
// given periods (milliseconds, cycled): each frame takes its elapsed into the pending time
// (Viewer.RunSteps' clamp), runs the whole steps (FrameBlend.Record after each, with
// Viewer.Carrier/Driven), then Viewer.BlendFrame's FrameBlend.Update(step = pending / Step,
// tick = TickBlend(pending)) and prints Kurt as drawn (FrameBlend.Feet/Yaw) and his carrier as
// drawn (FrameBlend.PositionOf, Viewer.Trace's carrier: the ride, else his platform).
//
//   Probe <level> <steps> <schedule> <periods-ms>
//
// The C++ twin is `tick1 dump blend <level> <steps> <schedule> <periods-ms>` (tests/scripts/
// tick1.cpp, kurtBlendDump), which must print the same bytes; golden/L7_blend_tank.txt is this
// probe's output for `7 300 tp:1:DANT_3:136:1292:101:0 33,41,29,37,52,34,31,45`.
//
// HOW IT WAS BUILT AND RUN (2026-11-27, .NET SDK 10.0.404): against UPSTREAM C# 1.2.6
// (`git archive 1094e273 src/Mdk.Formats src/Mdk.Engine src/Mdk.Game Directory.Build.props
// shaders assets` into a scratch tree, this directory beside its src/), not src/ (1.1.1): 1.2.6
// stands Kurt on platforms' triangles (e1fde558), as the port does, so the two simulations agree
// step for step; FrameBlend's Kurt half is the same in both. `dotnet build -c Release -o
// probe-bin -p:PrecompiledShaders=spirv` (with obj/shaders copied from a capture_oracle.sh tree),
// then `DOTNET_ROOT=<sdk> MDK_DATA_DIR=<the MDK folder> probe-bin/Probe ...`. On L6's lift and
// L3's hover ship the C++ matches it on all but a few frames, where the carrier's blended place
// is 1/64 apart: frame_blend::lerp's twice-rounded arithmetic against .NET's fused one (see
// frame_blend.cpp), not the blend's logic.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
var level = int.Parse(args[0]);
var steps = int.Parse(args[1]);
var schedule = args[2];
var periods = args[3].Split(',').Select(p => float.Parse(p) / 1000f).ToArray();
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
    Feet = levelData.Dti.StartPosition,
    Yaw = levelData.Dti.StartAngle,
};
var cmi = Cmi.Load(data.PathOf($"TRAVERSE/LEVEL{level}/LEVEL{level}.CMI"));
var groups = new TriangleGroups();
foreach (var arena in levelData.Arenas)
{
    groups.Add(arena);
}

var runtime = new ScriptRuntime(levelData, cmi, sprites, space, groups, mixer, kurt, 1) { Option = 1 };
int P(float v) => (int)MathF.Floor(v * 64f);
string Name(object? o) => o is MdkObject m ? $"{m.TypeName}#{m.InstanceId}" : o == null ? "-" : "?";

var keys = new Dictionary<string, Key>
{
    ["fwd"] = Key.Forward, ["back"] = Key.Back, ["left"] = Key.TurnLeft, ["right"] = Key.TurnRight,
    ["sleft"] = Key.StrafeLeft, ["sright"] = Key.StrafeRight, ["jump"] = Key.Jump, ["turbo"] = Key.Turbo,
    ["fire"] = Key.Fire,
};
var holds = new List<(Key Key, int From, int To)>();
var teleports = new List<(int Step, string Arena, Vector3 At, float Yaw)>();
var spawns = new List<(int Step, string Type, Vector3 At, float Yaw, int Script)>();
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    if (parts[0] == "spawn")
    {
        spawns.Add((int.Parse(parts[1]), parts[2],
            new Vector3(float.Parse(parts[3]), float.Parse(parts[4]), float.Parse(parts[5])), float.Parse(parts[6]),
            int.Parse(parts[7])));
        continue;
    }

    if (parts[0] == "tp")
    {
        teleports.Add((int.Parse(parts[1]), parts[2],
            new Vector3(float.Parse(parts[3]), float.Parse(parts[4]), float.Parse(parts[5])), float.Parse(parts[6])));
        continue;
    }

    var range = parts[1].Split('-');
    holds.Add((keys[parts[0]], int.Parse(range[0]), int.Parse(range[1])));
}

MdkObject? Carrier()
{
    if (runtime.Rides.Bomber != null)
    {
        return runtime.Rides.Ridden;
    }

    return kurt.OnFloor ? kurt.Platform as MdkObject : null;
}

MdkObject? Driven() => runtime.Rides.Bomber == null ? runtime.Rides.Ridden : null;

var blend = new FrameBlend(Blending.On);
var input = new Input();
const float Step = 1f / 60f;
var pending = 0f;
var step = 0;
Console.WriteLine($"level={level} steps={steps} schedule={schedule} periods={args[3]}");
for (var frame = 0; step < steps; frame++)
{
    pending = MathF.Min(pending + periods[frame % periods.Length], Step * 10);
    while (pending >= Step && step < steps)
    {
        pending -= Step;
        step++;
        foreach (var (s, arena, at, yaw) in teleports)
        {
            if (s == step)
            {
                runtime.TeleportKurt(arena, at, yaw);
            }
        }

        foreach (var (s, type, at, yaw, script) in spawns)
        {
            if (s == step)
            {
                runtime.Spawn(runtime.GetArenaState(runtime.CurrentArena).Controller, type, at, yaw, -1, script,
                    ScriptRuntime.Spawning.Plain);
            }
        }

        foreach (var key in keys.Values)
        {
            var down = holds.Any(h => h.Key == key && step >= h.From && step < h.To);
            input.Hold(key, down ? Input.State.Down : Input.State.Up);
        }

        kurt.Update(input, Step);
        runtime.Update(Step);
        blend.Record(kurt, Carrier(), Driven(), runtime.TickCount);
    }

    var alpha = pending / Step;
    var tick = runtime.TickBlend(pending);
    blend.Update(kurt, alpha, tick);
    var carrier = runtime.Rides.Ridden ?? kurt.Platform as MdkObject;
    var on = carrier is { } c ? $"{Name(c)}@{P(blend.PositionOf(c).X)},{P(blend.PositionOf(c).Y)},{P(blend.PositionOf(c).Z)}" : "-";
    Console.WriteLine($"f={frame} s={step} kurt64={P(blend.Feet.X)},{P(blend.Feet.Y)},{P(blend.Feet.Z)} yaw={blend.Yaw:F3} on={on}");
}
