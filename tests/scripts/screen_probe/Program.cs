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
using Mdk.Game.Objects;
using Mdk.Game.Scripts;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE SCREEN ORACLE (PORT_GAPS package #15): the C# Kurt, ScriptRuntime and FollowCamera
// (src/Mdk.Game) stepped together at 1/60 s -- Kurt, then the scripts, then the camera, as
// Viewer.RunSteps does -- under a scripted input. It pins four things nothing drew before:
//
//   the town timer   ScriptRuntime.TownTicks (TownTicksByDifficulty, by Kurt's difficulty; none in
//                    the last level of the order) counted down in RunTick, and FlattenTown at 0:
//                    RaiseShake(5), the OOT_L<n>[A] message (FlagZoom | FlagFront, 5 s) and the
//                    flattened-town global flag the debriefing reads;
//   the shake        raise_573aa8 (opcode 135) and every RaiseShake -> ShakeRaised ->
//                    FollowCamera.RaiseShake, and FollowCamera.UpdateShake's offsets and drain. The
//                    camera's Random is `new Random()` (time-seeded); the probe swaps it for
//                    `new Random(camseed)` through reflection, so the offsets are reproducible --
//                    the C++ twin seeds its port of the same (legacy, seeded) generator the same;
//   the sky mode     set_574304 (opcode 202) and SkyModes.BackdropOf / ClearOf over the DTI's sky
//                    colour (Viewer.Background);
//   group textures   group_set_texture (opcode 140): TriangleGroups.SetMaterial, the group's
//                    material, which LevelView.Rebuild draws.
//
// A line is printed for every step whose state differs from the last printed one (the step number
// aside), then the step's events: a group whose state or material changed (`group`), and the HUD
// message queue when it changed (`msg`). Its output is tests/scripts/golden/L*_screen_*.txt, and
// the C++ twin is `tick1 dump screen <level> <steps> <schedule>` (tests/scripts/tick1.cpp,
// screenDump), which must print the same bytes.
//
//   Probe <level> <steps> <schedule>
//
// The schedule is comma-separated: `key:from-to` holds a key over steps [from, to) (keys fwd, back,
// left, right, jump, fire); `diff:N` the difficulty (0-2) before the runtime is built; `camseed:N`
// the camera's Random seed (default 1); before a step: `tp:step:ARENA:x:y:z` teleports Kurt,
// `town:step:N` sets TownTicks, `gflag:step:HEX` ors into GlobalFlags, `hp:step:ID:N` sets object
// ID's health and `kill:step:ID` kills it (ScriptRuntime.Kill: its death script, or it explodes;
// objects are numbered in the order first seen), `avar:step:ARENA:I:V` sets the arena's variable
// I. `list:step` (the probe's own, for finding the ids; no golden uses it) lists the objects
// after that step.
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

var keys = new Dictionary<string, Key>
{
    ["fwd"] = Key.Forward, ["back"] = Key.Back, ["left"] = Key.TurnLeft, ["right"] = Key.TurnRight,
    ["jump"] = Key.Jump, ["fire"] = Key.Fire,
};
var holds = new List<(Key Key, int From, int To)>();
var teleports = new List<(int Step, string Arena, Vector3 At)>();
var towns = new List<(int Step, int Ticks)>();
var gflags = new List<(int Step, int Flags)>();
var healths = new List<(int Step, int Id, int Health)>();
var kills = new List<(int Step, int Id)>();
var avars = new List<(int Step, string Arena, int Index, float Value)>();
var lists = new List<int>();
var difficulty = Difficulty.Normal;
var camSeed = 1;
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    switch (parts[0])
    {
        case "diff":
            difficulty = (Difficulty)int.Parse(parts[1]);
            continue;
        case "list":
            lists.Add(int.Parse(parts[1]));
            continue;
        case "camseed":
            camSeed = int.Parse(parts[1]);
            continue;
        case "tp":
            teleports.Add((int.Parse(parts[1]), parts[2], new Vector3(float.Parse(parts[3]), float.Parse(parts[4]), float.Parse(parts[5]))));
            continue;
        case "town":
            towns.Add((int.Parse(parts[1]), int.Parse(parts[2])));
            continue;
        case "gflag":
            gflags.Add((int.Parse(parts[1]), (int)uint.Parse(parts[2], NumberStyles.HexNumber)));
            continue;
        case "hp":
            healths.Add((int.Parse(parts[1]), int.Parse(parts[2]), int.Parse(parts[3])));
            continue;
        case "kill":
            kills.Add((int.Parse(parts[1]), int.Parse(parts[2])));
            continue;
        case "avar":
            avars.Add((int.Parse(parts[1]), parts[2], int.Parse(parts[3]), float.Parse(parts[4])));
            continue;
    }

    var range = parts[1].Split('-');
    holds.Add((keys[parts[0]], int.Parse(range[0]), int.Parse(range[1])));
}

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
// Viewer.cs:232: the difficulty decides the town's timer, before the scripts.
kurt.Inventory.Difficulty = difficulty;
var cmi = Cmi.Load(data.PathOf($"TRAVERSE/LEVEL{level}/LEVEL{level}.CMI"));
var groups = new TriangleGroups();
foreach (var arena in levelData.Arenas)
{
    groups.Add(arena);
}

var runtime = new ScriptRuntime(levelData, cmi, sprites, space, groups, mixer, kurt, 1) { Option = 1 };
// Viewer.cs:237-238: the follow camera over the space, shaken by the scripts.
var follow = new FollowCamera(space);
var randomField = typeof(FollowCamera).GetField("_random", BindingFlags.NonPublic | BindingFlags.Instance)!;
randomField.SetValue(follow, new Random(camSeed));
var shakeField = typeof(FollowCamera).GetField("_shake", BindingFlags.NonPublic | BindingFlags.Instance)!;
var raised = new List<float>();
runtime.ShakeRaised += follow.RaiseShake;
runtime.ShakeRaised += amount => raised.Add(amount);
// The HUD's message stream (Viewer.cs:245); only Push and its queue are read, not the fonts.
var fti = Fti.Load(data.PathOf("MISC/MDKFONT.FTI"));
var messages = new Mdk.Game.Hud.Messages(fti, null!, null!);
runtime.Messages = messages;

string B(float v) => BitConverter.SingleToUInt32Bits(v).ToString("X8");
string Text(byte[] bytes) => string.Concat(bytes.Select(b => b is >= 32 and <= 126 ? ((char)b).ToString() : $"\\x{b:X2}"));
var queueField = typeof(Mdk.Game.Hud.Messages).GetField("_queue", BindingFlags.NonPublic | BindingFlags.Instance)!;
string Queue()
{
    var parts = new List<string>();
    foreach (var m in (System.Collections.IEnumerable)queueField.GetValue(messages)!)
    {
        var t = m.GetType();
        var lines = (List<byte[]>)t.GetProperty("Lines")!.GetValue(m)!;
        parts.Add($"{(int)t.GetProperty("Flags")!.GetValue(m)!}:{string.Join("/", lines.Select(Text))}#{B((float)t.GetProperty("Time")!.GetValue(m)!)}");
    }

    return parts.Count == 0 ? "-" : string.Join("|", parts);
}

// SkyModes over the DTI's sky colour (Viewer.Background / SkyColour), as bytes.
string Clear(Vector4 c) => $"{(int)MathF.Round(c.X * 255f):X2}{(int)MathF.Round(c.Y * 255f):X2}{(int)MathF.Round(c.Z * 255f):X2}{(int)MathF.Round(c.W * 255f):X2}";
Vector4 SkyColour()
{
    var at = levelData.Dti.SkyTopColor * 4;
    var rgba = levelData.Dti.Palette.Rgba;
    return new Vector4(rgba[at], rgba[at + 1], rgba[at + 2], byte.MaxValue) / byte.MaxValue;
}

// Objects are numbered in the order they are first seen in the list (creation order).
var ids = new Dictionary<MdkObject, int>(ReferenceEqualityComparer.Instance);
int Id(MdkObject obj)
{
    if (!ids.TryGetValue(obj, out var id))
    {
        id = ids.Count;
        ids[obj] = id;
    }

    return id;
}

// Every group's (state, material) as the level loaded, to print what a step changed.
var groupSeen = new Dictionary<(string, int), string>();
string GroupText(TriangleGroups.Group g) => $"state={(int)g.State} mat={(g.Material is { } m ? m.ToString(CultureInfo.InvariantCulture) : "-")}";
foreach (var arena in levelData.Arenas)
{
    foreach (var g in groups.Of(arena.Name))
    {
        groupSeen[(arena.Name, g.Number)] = GroupText(g);
    }
}

Console.WriteLine($"level={level} steps={steps} seed=1 schedule={schedule}");
Console.WriteLine($"start town={runtime.TownTicks} sky={runtime.SkyMode} skycolour={Clear(SkyColour())}");
var input = new Input();
var lastState = "";
var lastQueue = "-";
for (var step = 1; step <= steps; step++)
{
    foreach (var (s, arena, at) in teleports)
    {
        if (s == step)
        {
            runtime.TeleportKurt(arena, at, 0f);
        }
    }

    foreach (var (s, ticks) in towns)
    {
        if (s == step)
        {
            runtime.TownTicks = ticks;
        }
    }

    foreach (var (s, value) in gflags)
    {
        if (s == step)
        {
            runtime.GlobalFlags |= value;
        }
    }

    foreach (var (s, oid, health) in healths)
    {
        if (s == step)
        {
            ids.First(kv => kv.Value == oid).Key.Health = health;
        }
    }

    foreach (var (s, oid) in kills)
    {
        if (s == step)
        {
            runtime.Kill(ids.First(kv => kv.Value == oid).Key);
        }
    }

    foreach (var (s, arena, index, value) in avars)
    {
        if (s == step)
        {
            runtime.GetArenaState(arena).Variables[index] = value;
        }
    }

    foreach (var key in keys.Values)
    {
        var down = holds.Any(h => h.Key == key && step >= h.From && step < h.To);
        input.Hold(key, down ? Input.State.Down : Input.State.Up);
    }

    kurt.Update(input, 1f / 60f);
    runtime.Update(1f / 60f);
    var goal = runtime.CameraTrackTicks > 0 ? runtime.CameraTrackPitch
        : levelData.Dti.Arenas.FirstOrDefault(a => a.Name == runtime.CurrentArena)?.Pitch ?? 4f;
    follow.Update(kurt, goal, input, 1f / 60f);
    foreach (var obj in runtime.Objects)
    {
        Id(obj);
    }

    var backdrop = SkyModes.BackdropOf(runtime.SkyMode);
    var clear = SkyModes.ClearOf(runtime.SkyMode, SkyColour());
    var offset = follow.ShakeOffset;
    var state = $"arena={runtime.CurrentArena} town={runtime.TownTicks} gflags={runtime.GlobalFlags:X8} sky={runtime.SkyMode} " +
        $"backdrop={backdrop} clear={Clear(clear)} shake={B(runtime.Shake)} cam={B((float)shakeField.GetValue(follow)!)} " +
        $"off={(int)offset.X},{(int)offset.Y}";
    var events = new List<string>();
    foreach (var amount in raised)
    {
        events.Add($"  raise {B(amount)}");
    }

    raised.Clear();
    foreach (var arena in levelData.Arenas)
    {
        foreach (var g in groups.Of(arena.Name).OrderBy(g => g.Number))
        {
            var text = GroupText(g);
            if (groupSeen[(arena.Name, g.Number)] != text)
            {
                groupSeen[(arena.Name, g.Number)] = text;
                events.Add($"  group {arena.Name} {g.Number} {text}");
            }
        }
    }

    var queue = Queue();
    if (queue != lastQueue)
    {
        lastQueue = queue;
        events.Add($"  msg {queue}");
    }

    if (lists.Contains(step))
    {
        foreach (var obj in runtime.Objects)
        {
            events.Add($"  obj #{Id(obj)} {obj.TypeName} arena={obj.Arena} hp={obj.Health} dead={obj.Dead} pos={obj.Position}");
        }
    }

    if (state != lastState || events.Count > 0)
    {
        Console.WriteLine($"s={step} {state} objs={runtime.Objects.Count}");
        lastState = state;
        foreach (var line in events)
        {
            Console.WriteLine(line);
        }
    }
}
