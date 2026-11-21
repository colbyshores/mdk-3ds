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

// THE SNOWBOARD ORACLE (package #10): the C# Kurt, ScriptRuntime and FollowCamera (src/Mdk.Game)
// stepped together at 1/60 s as Viewer.RunSteps does -- Kurt, the scripts, the camera, then
// ArenaSpace.SetSolid(ScriptRuntime.SolidArenas) -- under a scripted input. It pins LEVEL4's board
// (Scripts/Snowboard.cs whole; Scripts/Rides.cs MountBoard and the board's halves of Update,
// Dismount, Lost, TakesHits; Kurt/Kurt.Ride.cs Glide, GetOffBoard, Pose, CameraPivot;
// CameraRoll.Follow; ScriptRuntime.SolidArenas on the board): Kurt dropped onto the rideable
// XSNOWB gets on, rides it down CMEAT_3 steering with the keys, speeding up and braking, jumps, rams
// what he runs into, breaks the ice walls (KurtTouchesGroups), and gets off when its script clears
// the flag (the board thrown on). Its output is tests/scripts/golden/L4_board_*.txt, and the C++
// twin is `tick1 dump board <level> <steps> <schedule>` (tests/scripts/tick1.cpp, kurtBoardDump),
// which must print the same bytes.
//
//   Probe <level> <steps> <schedule>
//   Probe dis <level> <pc,...>     (the scripts reachable from those offsets; not a golden)
//   Probe scan <level>             (every XSNOWB: arena, place, flags, path; not a golden)
//
// The schedule is comma-separated: `key:from-to` holds a key over steps [from, to) (keys fwd,
// back, left, right, sleft, sright, jump, turbo, fire), `tp:step:ARENA:x:y:z:yaw` calls
// ScriptRuntime.TeleportKurt before that step (the tests' --teleport), `off:step:TYPE` clears that
// object's rideable flag (0x2000000) before that step, as its script does to let him off,
// `hit:step:DAMAGE` calls ScriptRuntime.HurtKurt and `kill:step:TYPE` ScriptRuntime.Kill. A long
// run prints only some steps: `every:N` the multiples of N and `show:from-to` the steps [from, to)
// (with either, the last step too; without both, every step). The goldens' schedules are in
// tick1.cpp's kBoardCases table.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/ride_probe -- copy
// src/Mdk.Formats, src/Mdk.Engine, src/Mdk.Game and Directory.Build.props into a scratch tree with
// this directory beside src/; in the scratch Mdk.Engine.csproj drop the shader targets and the
// icon resource; `dotnet build -c Release -o probe-bin`, then
// `DOTNET_ROOT=<sdk> MDK_DATA_DIR=<the MDK folder> probe-bin/Probe ...`.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
var dis = args[0] == "dis" || args[0] == "scan";
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

if (args[0] == "scan")
{
    foreach (var o in runtime.Objects.Where(o => o.TypeName.ToUpperInvariant() == "XSNOWB"))
    {
        Console.WriteLine($"{Name(o)} arena={o.Arena} at={F(o.Position.X)},{F(o.Position.Y)},{F(o.Position.Z)} yaw={F(o.Yaw)} flags={o.Flags:X} path={o.Path} t={F(o.PathTime)} origin={F(o.PathOrigin.X)},{F(o.PathOrigin.Y)},{F(o.PathOrigin.Z)}");
    }

    return;
}

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

object? Field(object o, string name) => o.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(o);
float Private(object o, string name) => (float)Field(o, name)!;

var keys = new Dictionary<string, Key>
{
    ["fwd"] = Key.Forward, ["back"] = Key.Back, ["left"] = Key.TurnLeft, ["right"] = Key.TurnRight,
    ["sleft"] = Key.StrafeLeft, ["sright"] = Key.StrafeRight, ["jump"] = Key.Jump, ["turbo"] = Key.Turbo,
    ["fire"] = Key.Fire,
};
var holds = new List<(Key Key, int From, int To)>();
var teleports = new List<(int Step, string Arena, Vector3 At, float Yaw)>();
var actions = new List<(int Step, string Kind, string Arg)>();
var every = 0;
var shows = new List<(int From, int To)>();
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    if (parts[0] == "tp")
    {
        teleports.Add((int.Parse(parts[1]), parts[2],
            new Vector3(float.Parse(parts[3]), float.Parse(parts[4]), float.Parse(parts[5])), float.Parse(parts[6])));
        continue;
    }

    if (parts[0] == "every")
    {
        every = int.Parse(parts[1]);
        continue;
    }

    if (parts[0] == "show")
    {
        var span = parts[1].Split('-');
        shows.Add((int.Parse(span[0]), int.Parse(span[1])));
        continue;
    }

    if (parts[0] is "off" or "hit" or "kill")
    {
        actions.Add((int.Parse(parts[1]), parts[0], parts[2]));
        continue;
    }

    var range = parts[1].Split('-');
    holds.Add((keys[parts[0]], int.Parse(range[0]), int.Parse(range[1])));
}

MdkObject? FirstOf(string type) => runtime.Objects.FirstOrDefault(o => !o.Dead && o.TypeName == type);

var input = new Input();
MdkObject? board = null;
var boardField = typeof(Rides).GetField("_board", BindingFlags.NonPublic | BindingFlags.Instance)!;
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

        if (kind == "off" && FirstOf(arg) is { } freed)
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

    // Viewer.RunSteps: Kurt, the scripts, the camera, then the collision world for the next step
    // (Viewer.cs:533; on the board it is Kurt's arena alone). The camera's goal is the arena's rest
    // pitch, as the rides probe's (opcode 203's atan2 is the LUT's in the C++). Only its height is
    // printed (`camz64`): its x and y stand behind Kurt along his facing, which the C++ takes from
    // the trig LUT (Player::facing, R6), and on the board's ever-changing yaw that is a 1/64 column
    // off the oracle now and then (measured: 3 steps of 700) -- rounding, not a behaviour.
    kurt.Update(input, 1f / 60f);
    runtime.Update(1f / 60f);
    var goal = levelData.Dti.Arenas.FirstOrDefault(a => a.Name == runtime.CurrentArena)?.Pitch ?? 4f;
    follow.Update(kurt, goal, input, 1f / 60f);
    space.SetSolid(runtime.SolidArenas);

    board ??= runtime.Objects.FirstOrDefault(o => o.TypeName.ToUpperInvariant() == "XSNOWB");
    var shown = (every == 0 && shows.Count == 0) || step == steps || (every > 0 && step % every == 0)
        || shows.Any(r => step >= r.From && step < r.To);
    if (!shown)
    {
        continue;
    }

    var touched = string.Join("|", kurt.Touched.Select(Name));
    var b = board == null ? "-"
        : $"{Name(board)}@{P(board.Position.X)},{P(board.Position.Y)},{P(board.Position.Z)}/y{F(board.Yaw)}/p{F(board.Pitch)}" +
          $"/f{board.Flags:X}/t{F(board.PathTime)}/ps{board.PathStop}/v{P(board.Velocity.X)},{P(board.Velocity.Y)},{P(board.Velocity.Z)}" +
          $"/g{F(board.Gravity)}/fr{F(board.Friction)}/r{board.Restart}/d{(board.Dead ? 1 : 0)}";
    var ride = boardField.GetValue(runtime.Rides);
    var sb = ride == null ? "-"
        : $"v{F(Private(ride, "_speed"))}/l{F(Private(ride, "_side"))}/h{F(Private(ride, "_heading"))}/s{F(Private(ride, "_steer"))}" +
          $"/p{F(Private(ride, "_pitch"))}/g{((bool)Field(ride, "_grounded")! ? 1 : 0)}/ph{F(Private(ride, "_pathHeading"))}" +
          $"/tt{F(Private(ride, "_turnTicks"))}/jt{F(Private(ride, "_jumpTicks"))}/at{F(Private(ride, "_airTicks"))}" +
          $"/{Field(ride, "_pose")}/fr{F(Private(ride, "_frame"))}";
    var pose = kurt.Pose is { } kp ? $"{kp.Name}:{kp.Frame}" : "-";
    // The flash's frame and jitter come from Kurt's unseeded Random (Kurt.Combat.cs:113), so only
    // whether one is up, and whether it sits at K_SURF's (-42, 12) + 0-4, is printed.
    var muzzle = kurt.Muzzle is { } m ? (m.X is >= -42 and <= -38 && m.Y is >= 12 and <= 16 ? "surf" : "state") : "-";
    var ridden = runtime.Rides.Ridden;
    var cam = follow.Position;
    Console.WriteLine($"s={step} arena={runtime.CurrentArena} solid={string.Join("|", runtime.SolidArenas)} state={kurt.Current} " +
        $"feet64={P(kurt.Feet.X)},{P(kurt.Feet.Y)},{P(kurt.Feet.Z)} yaw={F(kurt.Yaw)} fs={F(kurt.ForwardSpeed)} ss={F(kurt.StrafeSpeed)} " +
        $"vz={F(kurt.VerticalSpeed)} floor={(kurt.OnFloor ? 1 : 0)} vis={(kurt.Visible ? 1 : 0)} pose={pose} pivot={F(kurt.CameraPivot)} " +
        $"roll={F(kurt.CameraRoll.Roll)} fire={(kurt.Firing ? 1 : 0)} muzzle={muzzle} hp={kurt.Health} ride={Name(ridden)} " +
        $"onboard={(runtime.Rides.OnBoard() ? 1 : 0)} hits={(runtime.Rides.TakesHits() ? 1 : 0)} touched={(touched.Length == 0 ? "-" : touched)} " +
        $"board={b} sb={sb} camz64={P(cam.Z)} pitch={F(Private(follow, "_arenaPitch"))},{F(Private(follow, "_airPitch"))} " +
        $"objs={runtime.Objects.Count} voices={Voices()}");
}
