using System.Globalization;
using System.Numerics;
using System.Reflection;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Formats.Scripts;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Kurt;
using Mdk.Game.Level;
using Mdk.Game.Objects;
using Mdk.Game.Scripts;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE RIDES ORACLE (package #9): the C# Kurt, ScriptRuntime and FollowCamera (src/Mdk.Game)
// stepped together at 1/60 s, Kurt, then the scripts, then the camera, as Viewer.RunSteps does,
// under a scripted input. It pins the walker ride (Scripts/Rides.cs: TryMount, MountWalker,
// UpdateWalker, Follow, Dismount, Lost; Kurt/Kurt.Ride.cs: RideWalker, JumpOff, Walk.Riding,
// Visible, Keys): Kurt touching a rideable XD2 on the floor gets in, walks it hidden (no strafe,
// no jump, no gun), it follows him each step, its sound plays while he steers, firing sounds the
// alarm, the hits go to it, and he jumps out when its script clears the flag or it goes. Opcode
// 171 (if_is_573c30, "the object Kurt rides") branches on Rides.Ridden: the `ride171` column is
// what it answers for the walker, and the walker's own script's position (`r`) shows the branch.
// Its output is tests/scripts/golden/L7_ride_*.txt, and the C++ twin is
// `tick1 dump ride <level> <steps> <schedule>` (tests/scripts/tick1.cpp, kurtRideDump), which
// must print the same bytes.
//
//   Probe <level> <steps> <schedule>
//   Probe dis <level> <pc,...>     (the scripts reachable from those offsets; not a golden)
//
// The schedule is comma-separated: `key:from-to` holds a key over steps [from, to) (keys fwd,
// back, left, right, sleft, sright, jump, turbo, fire), `tp:step:ARENA:x:y:z:yaw` calls
// ScriptRuntime.TeleportKurt before that step (the tests' --teleport), `ride:step:TYPE` calls
// Rides.RideWalker on the first object of that type before that step (the tests' --ride,
// RideTest.Start), `off:step:TYPE` clears that object's rideable flag (0x2000000) before that
// step, as its script does to let him out, and `hit:step:DAMAGE` calls ScriptRuntime.HurtKurt
// (a shot at Kurt, which the ridden walker takes). The goldens' schedules are in tick1.cpp's
// kRideCases table.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/items_probe -- copy
// src/Mdk.Formats, src/Mdk.Engine, src/Mdk.Game and Directory.Build.props into a scratch tree with
// this directory beside src/; in the scratch Mdk.Engine.csproj drop the shader targets and the
// icon resource; `dotnet build -c Release -o probe-bin`, then
// `DOTNET_ROOT=<sdk> MDK_DATA_DIR=<the MDK folder> probe-bin/Probe ...`.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
var dis = args[0] == "dis";
var level = int.Parse(dis ? args[1] : args[0]);
var steps = dis ? 0 : int.Parse(args[1]);
var schedule = !dis && args.Length > 2 ? args[2] : "";
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
string F(float v) => v.ToString("F4", CultureInfo.InvariantCulture);
string Name(object? o) => o is MdkObject m ? $"{m.TypeName}#{m.InstanceId}" : o == null ? "-" : "?";

if (dis)
{
    // Probe dis <level> <pc,...>: every instruction reachable from the offsets, in pc order.
    var decoder = ScriptDecoder.For(cmi);
    decoder.Preload(args[2].Split(',').Select(int.Parse));
    string Op(object? v) => v switch
    {
        null => "null",
        object?[] l => "[" + string.Join(",", l.Select(Op)) + "]",
        float f => F(f),
        _ => v.ToString() ?? "",
    };
    foreach (var ins in decoder.Decoded.OrderBy(i => i.Pc))
    {
        var name = ScriptOpcodes.Mnemonics.TryGetValue(ins.Opcode, out var n) ? n : ins.Opcode.ToString();
        var action = ins.Action is { } a ? $" -> {a.Type} {a.Target} {a.Else}" : "";
        Console.WriteLine($"{ins.Pc}: {ins.Opcode} {name} {string.Join(" ", ins.Operands.Select(Op))}{action}");
    }

    return;
}

// Viewer.cs:237: the follow camera over the same space (no ClipView in either tree).
var follow = new FollowCamera(space);
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
var teleports = new List<(int Step, string Arena, Vector3 At, float Yaw)>();
var actions = new List<(int Step, string Kind, string Arg)>();
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    if (parts[0] == "tp")
    {
        teleports.Add((int.Parse(parts[1]), parts[2],
            new Vector3(float.Parse(parts[3]), float.Parse(parts[4]), float.Parse(parts[5])), float.Parse(parts[6])));
        continue;
    }

    if (parts[0] is "ride" or "off" or "hit" or "kill")
    {
        actions.Add((int.Parse(parts[1]), parts[0], parts[2]));
        continue;
    }

    var range = parts[1].Split('-');
    holds.Add((keys[parts[0]], int.Parse(range[0]), int.Parse(range[1])));
}

MdkObject? FirstOf(string type) => runtime.Objects.FirstOrDefault(o => !o.Dead && o.TypeName == type);

var input = new Input();
MdkObject? walker = null;
Console.WriteLine($"level={level} steps={steps} seed=1 schedule={schedule}");
for (var step = 1; step <= steps; step++)
{
    foreach (var (s, arena, at, yaw) in teleports)
    {
        if (s == step)
        {
            runtime.TeleportKurt(arena, at, yaw);
        }
    }

    foreach (var (s, kind, arg) in actions)
    {
        if (s != step)
        {
            continue;
        }

        if (kind == "ride" && FirstOf(arg) is { } mounted)
        {
            runtime.Rides.RideWalker(mounted);
        }
        else if (kind == "off" && FirstOf(arg) is { } freed)
        {
            freed.Flags &= ~Rides.FlagRideable;
        }
        else if (kind == "kill" && FirstOf(arg) is { } victim)
        {
            runtime.Kill(victim);
        }
        else if (kind == "hit")
        {
            runtime.HurtKurt(int.Parse(arg));
        }
    }

    foreach (var key in keys.Values)
    {
        var down = holds.Any(h => h.Key == key && step >= h.From && step < h.To);
        input.Hold(key, down ? Input.State.Down : Input.State.Up);
    }

    // Viewer.RunSteps: Kurt, the scripts (whose Update ends with Rides.Follow), then the camera.
    // Its goal is the arena's rest pitch, NOT Viewer.PitchGoal's camera_track pitch: opcode 203
    // (ScriptRuntime.CameraTrack) takes MathF.Atan2 and the C++ fastAtan2 (R6), a ULP or two
    // apart (DANT_9's track at step 300: -24.1330566 here, -24.1330547 there), which the eased
    // pitch carries into the camera's 1/64 columns. The track is opcode 203's, pinned by its own
    // tests; this golden pins the camera following the ride. `track` prints its ticks.
    kurt.Update(input, 1f / 60f);
    runtime.Update(1f / 60f);
    var goal = levelData.Dti.Arenas.FirstOrDefault(a => a.Name == runtime.CurrentArena)?.Pitch ?? 4f;
    follow.Update(kurt, goal, input, 1f / 60f);

    walker ??= runtime.Objects.FirstOrDefault(o => o.TypeName is "XD2" or "XD");
    var touched = string.Join("|", kurt.Touched.Select(Name));
    var w = walker == null ? "-"
        : $"{Name(walker)}@{P(walker.Position.X)},{P(walker.Position.Y)},{P(walker.Position.Z)}/y{F(walker.Yaw)}" +
          $"/f{walker.Flags:X}/a{F(walker.AnimationRate)}/hp{walker.Health}/r{walker.Restart}/d{(walker.Dead ? 1 : 0)}";
    var ridden = runtime.Rides.Ridden;
    var cam = follow.Position;
    Console.WriteLine($"s={step} arena={runtime.CurrentArena} state={kurt.Current} feet64={P(kurt.Feet.X)},{P(kurt.Feet.Y)},{P(kurt.Feet.Z)} " +
        $"yaw={F(kurt.Yaw)} fs={F(kurt.ForwardSpeed)} ss={F(kurt.StrafeSpeed)} vz={F(kurt.VerticalSpeed)} floor={(kurt.OnFloor ? 1 : 0)} " +
        $"walk={(kurt.Walking == KurtT.Walk.Riding ? "R" : "N")} vis={(kurt.Visible ? 1 : 0)} keys={(kurt.Keys.Steer ? 1 : 0)}{(kurt.Keys.Fire ? 1 : 0)} " +
        $"fire={(kurt.Firing ? 1 : 0)} hp={kurt.Health} ride={Name(ridden)} ride171={(walker != null && ridden == walker ? 1 : 0)} " +
        $"hits={(runtime.Rides.TakesHits() ? 1 : 0)} alarm={runtime.AlarmTicks} touched={(touched.Length == 0 ? "-" : touched)} " +
        $"walker={w} cam64={P(cam.X)},{P(cam.Y)},{P(cam.Z)} pitch={F(Private(follow, "_arenaPitch"))},{F(Private(follow, "_airPitch"))} track={runtime.CameraTrackTicks} " +
        $"objs={runtime.Objects.Count} voices={Voices()}");
}
