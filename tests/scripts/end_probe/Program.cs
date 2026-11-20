using System.Globalization;
using System.Numerics;
using System.Reflection;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Kurt;
using Mdk.Game.Level;
using Mdk.Game.Scripts;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE END-OF-LEVEL ORACLE: the C# Kurt, ScriptRuntime and FollowCamera (src/Mdk.Game) stepped
// together at 1/60 s, Kurt, then the scripts, then the camera, as Viewer.RunSteps does, under a
// scripted input. It pins the end of a level: special_event <= 50 (ScriptRuntime.SpecialEvent ->
// EndTheLevel: LevelOver, the town flags, StopFiring, NUKE and TORNADO) and EndLevel.cs whole --
// the triangles torn off highest first (0-7 a tick from the scripts' random), the pieces rising
// and spinning around Kurt until they are 500 above him, Kurt Frozen and Rising, his K_TAKEOF /
// K_FLOATC pose, his turn and rise, the camera's tilt up (FollowCamera's air pitch held at 0 while
// Rising, and no clearance while Frozen), the white flash, and the step LevelEnded fires. Its
// output is tests/scripts/golden/L*_end_*.txt, and the C++ twin is
// `tick1 dump end <level> <steps> <schedule>` (tests/scripts/tick1.cpp, levelEndDump), which must
// print the same bytes.
//
//   Probe <level> <steps> <schedule>
//
// The schedule is comma-separated: `key:from-to` holds a key over steps [from, to) (keys fwd,
// back, left, right, sleft, sright, jump, turbo, fire, use, next, prev), and `end:step:value`
// runs special_event `value` (opcode 131's ScriptRuntime.SpecialEvent) for the arena controller
// of Kurt's arena before that step -- the end trigger's own call, without walking Kurt to the
// object that makes it. The goldens' schedules are in tick1.cpp's kEndCases table.
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
var schedule = args.Length > 2 ? args[2] : "";
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
// Viewer.cs:237: the follow camera, with the space (its clearance) and no ClipView (the C++ has no
// solid-object pass yet, and the clearance is skipped while Kurt is Frozen anyway).
var follow = new FollowCamera(space);
var ended = 0;
runtime.LevelEnded += over => ended = over == ScriptRuntime.GameOver.No ? 1 : 2;
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
    ["fire"] = Key.Fire, ["use"] = Key.UseItem, ["next"] = Key.ItemNext, ["prev"] = Key.ItemPrevious,
};
var holds = new List<(Key Key, int From, int To)>();
var events = new List<(int Step, int Value)>();
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    if (parts[0] == "end")
    {
        events.Add((int.Parse(parts[1]), int.Parse(parts[2])));
        continue;
    }

    var range = parts[1].Split('-');
    holds.Add((keys[parts[0]], int.Parse(range[0]), int.Parse(range[1])));
}

int P(float v) => (int)MathF.Floor(v * 64f);
string F(float v) => v.ToString("F4", CultureInfo.InvariantCulture);
var input = new Input();
Console.WriteLine($"level={level} steps={steps} seed=1 schedule={schedule}");
for (var step = 1; step <= steps; step++)
{
    foreach (var (s, value) in events)
    {
        if (s == step)
        {
            runtime.SpecialEvent(runtime.GetArenaState(runtime.CurrentArena).Controller, value);
        }
    }

    foreach (var key in keys.Values)
    {
        var down = holds.Any(h => h.Key == key && step >= h.From && step < h.To);
        input.Hold(key, down ? Input.State.Down : Input.State.Up);
    }

    // Viewer.RunSteps: Kurt, the scripts, then the camera (PitchGoal: camera_track, else the
    // arena's rest pitch).
    kurt.Update(input, 1f / 60f);
    runtime.Update(1f / 60f);
    var goal = runtime.CameraTrackTicks > 0 ? runtime.CameraTrackPitch
        : levelData.Dti.Arenas.FirstOrDefault(a => a.Name == runtime.CurrentArena)?.Pitch ?? 4f;
    follow.Update(kurt, goal, input, 1f / 60f);

    var end = runtime.EndLevel;
    var pose = kurt.Pose is { } p ? $"{p.Name}:{p.Frame}" : "-";
    var line = $"s={step} state={kurt.Current} feet64={P(kurt.Feet.X)},{P(kurt.Feet.Y)},{P(kurt.Feet.Z)} yaw={F(kurt.Yaw)} " +
        $"vz={F(kurt.VerticalSpeed)} frozen={(kurt.Frozen ? 1 : 0)} rising={(kurt.Rising ? 1 : 0)} pose={pose} tilt={F(kurt.CameraTilt)} " +
        $"white={F(kurt.WhiteFlash)} fire={(kurt.Firing ? 1 : 0)} hp={kurt.Health} over={(runtime.LevelOver ? 1 : 0)} " +
        $"town={runtime.Stats.TownFlags} ended={ended} cam={F(Private(follow, "_arenaPitch"))},{F(Private(follow, "_airPitch"))} " +
        $"shake={F(runtime.Shake)} objs={runtime.Objects.Count}";
    if (end != null)
    {
        long tsum = 0;
        foreach (var t in end.Torn)
        {
            tsum += t;
        }

        long sx = 0, sy = 0, sz = 0, sa = 0;
        foreach (var piece in end.Pieces)
        {
            sx += P(piece.Centre.X);
            sy += P(piece.Centre.Y);
            sz += P(piece.Centre.Z);
            sa += P(piece.Angle);
        }

        var first = end.Pieces.Count == 0 ? "-"
            : $"{end.Pieces[0].Triangle}@{P(end.Pieces[0].Centre.X)},{P(end.Pieces[0].Centre.Y)},{P(end.Pieces[0].Centre.Z)}" +
              $"/a{F(end.Pieces[0].Angle)}/r{F(end.Pieces[0].RiseSpeed)}/s{F(end.Pieces[0].Spin)}";
        line += $" end={end.Arena} torn={end.Torn.Count} tsum={tsum} last={(end.Torn.Count == 0 ? -1 : end.Torn[^1])} " +
            $"pieces={end.Pieces.Count} psum={sx},{sy},{sz} asum={sa} first={first} rise={F(Private(end, "_riseSpeed"))} " +
            $"turn={F(Private(end, "_turn"))} flash={F(Private(end, "_flash"))} takeoff={F(Private(end, "_takeoff"))} " +
            $"pitch={F(end.PitchOffset)}";
    }

    Console.WriteLine(line + $" voices={Voices()}");
}
