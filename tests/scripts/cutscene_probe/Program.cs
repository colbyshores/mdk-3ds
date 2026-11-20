using System.Globalization;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Kurt;
using Mdk.Game.Level;
using Mdk.Game.Objects;
using Mdk.Game.Scripts;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE CUTSCENE ORACLE: the C# Kurt and ScriptRuntime (src/Mdk.Game) stepped together at 1/60 s,
// Kurt then the scripts, as Viewer.RunSteps does. It pins special_event's arms above 50
// (ScriptRuntime.SpecialEvent, ScriptRuntime.cs:1009-1076): StartCutscene / EndCutscene, the
// Cutscene branch of RunTick (CutsceneTick: which objects run and which are drawn, Kurt's
// Visible), UpdateCutsceneCamera (the shots 0, 1, 2, 11 and 12, exact to the bit), Kurt Frozen,
// FindObjectNamed, and GameFinished. Its output is tests/scripts/golden/L*_cut_*.txt, and the C++
// twin is `tick1 dump cutscene <level> <steps> <schedule>` (tests/scripts/tick1.cpp,
// cutsceneDump), which must print the same bytes.
//
//   Probe <level> <steps> <schedule>
//
// The schedule is comma-separated:
//   at:x:y:z:yaw    Kurt starts there instead of the level's start (the C#'s --at): the arena
//                   under him becomes Kurt's on the first tick, so its controller and objects run;
//   ev:step:value   special_event `value` for the arena controller of Kurt's arena before that
//                   step -- the call the retail scripts make (every value above 50 is an arena
//                   controller's: L5 MUSE_5, L7 DANT_10, L8 GUNT_10), and the C#'s --event;
//   key:from-to     a key held over steps [from, to) (fwd, back, left, right, jump, fire);
//   show:step       print from that step on (a long cutscene's end: the shots 1 and 2 ease in
//                   for 1800 ticks, a minute, before their settled weights apply).
// The goldens' schedules are in tick1.cpp's kCutsceneCases table.
//
// Columns: Kurt (state, feet in 1/64 units, Frozen, Visible, firing), Kurt's arena, the cutscene
// (state in hex, its target, the shot, the blend timer, the camera's yaw, pitch, distance,
// stored position and point as IEEE-754 bit patterns), GameFinished's count, the objects (how
// many, how many Visible, an FNV-1a hash of the indices of the hidden ones, the sum of the
// objects' positions in 1/64 units -- which only moves for the objects that run).
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/end_probe -- copy
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

var keys = new Dictionary<string, Key>
{
    ["fwd"] = Key.Forward, ["back"] = Key.Back, ["left"] = Key.TurnLeft, ["right"] = Key.TurnRight,
    ["jump"] = Key.Jump, ["fire"] = Key.Fire,
};
var holds = new List<(Key Key, int From, int To)>();
var events = new List<(int Step, int Value)>();
var feet = levelData.Dti.StartPosition;
var yaw = levelData.Dti.StartAngle;
var show = 1;
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    if (parts[0] == "at")
    {
        feet = new System.Numerics.Vector3(float.Parse(parts[1]), float.Parse(parts[2]), float.Parse(parts[3]));
        yaw = float.Parse(parts[4]);
        continue;
    }

    if (parts[0] == "show")
    {
        show = int.Parse(parts[1]);
        continue;
    }

    if (parts[0] == "ev")
    {
        events.Add((int.Parse(parts[1]), int.Parse(parts[2])));
        continue;
    }

    var range = parts[1].Split('-');
    holds.Add((keys[parts[0]], int.Parse(range[0]), int.Parse(range[1])));
}

var kurt = new KurtT(space, mixer, FrameCount) { Feet = feet, Yaw = yaw };
var cmi = Cmi.Load(data.PathOf($"TRAVERSE/LEVEL{level}/LEVEL{level}.CMI"));
var groups = new TriangleGroups();
foreach (var arena in levelData.Arenas)
{
    groups.Add(arena);
}

var runtime = new ScriptRuntime(levelData, cmi, sprites, space, groups, mixer, kurt, 1) { Option = 1 };
var finished = 0;
runtime.GameFinished += () => finished++;

int P(float v) => (int)MathF.Floor(v * 64f);
string B(float v) => BitConverter.SingleToInt32Bits(v).ToString("x8");
string V(System.Numerics.Vector3 v) => $"{B(v.X)},{B(v.Y)},{B(v.Z)}";
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

    // Viewer.RunSteps: Kurt, then the scripts (the follow camera does not move the cutscene's).
    kurt.Update(input, 1f / 60f);
    runtime.Update(1f / 60f);
    if (step < show)
    {
        continue;
    }

    var target = runtime.CutsceneTarget is { } t ? (t.TypeName.Length != 0 ? $"{t.TypeName}_{t.InstanceId}" : $"@{t.Arena}") : "-";
    var count = 0;
    var shown = 0;
    var hidden = 2166136261u;
    long sum = 0;
    foreach (var obj in runtime.Objects)
    {
        if (obj.Visible)
        {
            shown++;
        }
        else
        {
            hidden = (hidden ^ (uint)count) * 16777619u;
        }

        sum += P(obj.Position.X) + P(obj.Position.Y) + P(obj.Position.Z);
        count++;
    }

    Console.WriteLine(
        $"s={step} state={kurt.Current} feet64={P(kurt.Feet.X)},{P(kurt.Feet.Y)},{P(kurt.Feet.Z)} " +
        $"frozen={(kurt.Frozen ? 1 : 0)} kvis={(kurt.Visible ? 1 : 0)} fire={(kurt.Firing ? 1 : 0)} arena={runtime.CurrentArena} " +
        $"cut={runtime.Cutscene:x} target={target} mode={runtime.CameraMode} timer={runtime.CameraTimer} " +
        $"yaw={B(runtime.CameraYaw)} pitch={B(runtime.CameraPitch)} dist={B(runtime.CameraDistance)} " +
        $"pos={V(runtime.CameraPosition)} point={V(runtime.CameraPoint)} finished={finished} " +
        $"objs={count} shown={shown} hid={hidden:x8} osum={sum}");
}
