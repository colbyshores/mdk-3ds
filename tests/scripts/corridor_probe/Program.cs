using System.Globalization;
using System.Numerics;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Level;
using Mdk.Game.Objects;
using Mdk.Game.Scripts;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE CORRIDOR ORACLE: the C# Kurt and ScriptRuntime (src/Mdk.Game) stepped together at 1/60 s
// as Viewer.RunSteps does -- Kurt, then the scripts, then ArenaSpace.SetSolid(SolidArenas) -- from
// a placed start (the Linux frontend's --at), under a key schedule. Per step it prints Kurt's
// arena, the second arena (and whether it is active), the solid list his next move collides with,
// his feet, and whether a wall stopped his walk move this step: the move he meant (Facing *
// ForwardSpeed + Right * StrafeSpeed, over the step) swept again from where he stood, against the
// same solid list, and the arena and triangle it ran into. Its output is
// tests/scripts/golden/L7_corridor_*.txt, and the C++ twin is `tick1 dump corridor <level> <steps>
// <x,y,z,yaw> <schedule>` (tests/scripts/tick1.cpp, kurtCorridorDump), which must print the same
// bytes.
//
//   Probe <level> <steps> <x,y,z,yaw> <schedule>
//
// The schedule is items_probe's key holds: comma-separated `key:from-to` over steps [from, to)
// (fwd back left right sleft sright jump turbo fire), and `unlock:step`, which clears every
// door's locked bit (0x40) before that step. Level 7's DANT_1 -> CDANT_1 door (X7DOOR at
// 0,481,15) starts locked (door_set_flags 0x50) and nothing in a key-only run unlocks it; a
// locked door is solid to Kurt here (ScriptRuntime.SolidsWithin), so without the unlock he stops
// at y = 478.4 and never reaches the corridor.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/fire_probe -- copy
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
var keys = new Dictionary<string, Key>
{
    ["fwd"] = Key.Forward, ["back"] = Key.Back, ["left"] = Key.TurnLeft, ["right"] = Key.TurnRight,
    ["sleft"] = Key.StrafeLeft, ["sright"] = Key.StrafeRight, ["jump"] = Key.Jump, ["turbo"] = Key.Turbo,
    ["fire"] = Key.Fire,
};
var holds = new List<(Key Key, int From, int To)>();
var unlocks = new List<int>();
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    if (parts[0] == "unlock")
    {
        unlocks.Add(int.Parse(parts[1]));
        continue;
    }

    var range = parts[1].Split('-');
    holds.Add((keys[parts[0]], int.Parse(range[0]), int.Parse(range[1])));
}

const float Step = 1f / 60f;
// Kurt's walk slide (Kurt.WalkSlide): the replayed move slides as his did.
const float WalkSlide = 0.75f;
// Feet to whole units: Kurt's facing is libm here and the port's trig LUT there, which moves a
// diagonal walk by a 64th now and then -- nothing this probe is about.
string F0(float v) => v.ToString("F0", CultureInfo.InvariantCulture);
string F(float v) => v.ToString("F1", CultureInfo.InvariantCulture);
var input = new Input();
Console.WriteLine($"level={level} steps={steps} at={args[2]} schedule={schedule}");
var solid = new List<string>();
for (var step = 1; step <= steps; step++)
{
    foreach (var key in keys.Values)
    {
        var down = holds.Any(h => h.Key == key && step >= h.From && step < h.To);
        input.Hold(key, down ? Input.State.Down : Input.State.Up);
    }

    // The probe's one intervention: the doors' locked bit (0x40) cleared, as the level's own
    // unlock would (DANT_1's door to CDANT_1 starts locked, and a locked door is solid to Kurt).
    if (unlocks.Contains(step))
    {
        foreach (var o in runtime.Objects.Where(o => !o.Dead && (o.Flags & MdkObject.FlagDoor) != 0))
        {
            o.DoorState &= ~ObjectBehaviors.DoorLocked;
        }
    }

    var before = kurt.Feet;
    kurt.Update(input, Step);
    var meant = (kurt.Facing * kurt.ForwardSpeed + kurt.Right * kurt.StrafeSpeed) * Step;
    meant.Z = 0f;
    var wall = "-";
    if (meant != Vector3.Zero)
    {
        var replay = space.Move(before, meant, ArenaSpace.Motion.Walk, WalkSlide);
        if (replay.Hit && replay.Bsp != null)
        {
            wall = $"{replay.Bsp.Arena.Name}:{replay.Triangle}";
        }
    }

    runtime.Update(Step);
    space.SetSolid(runtime.SolidArenas);
    solid.Clear();
    solid.AddRange(runtime.SolidArenas);
    Console.WriteLine($"s={step} arena={(runtime.CurrentArena.Length == 0 ? "-" : runtime.CurrentArena)} " +
        $"second={(runtime.SecondArena.Length == 0 ? "-" : runtime.SecondArena)}{(runtime.SecondActive ? "+" : "")} " +
        $"solid={(solid.Count == 0 ? "-" : string.Join("|", solid))} feet={F0(kurt.Feet.X)},{F0(kurt.Feet.Y)},{F0(kurt.Feet.Z)} " +
        $"yaw={F(kurt.Yaw)} wall={wall}");
}
