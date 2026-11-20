using System.Globalization;
using System.Numerics;
using System.Reflection;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Formats.Scripts;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Level;
using Mdk.Game.Objects;
using Mdk.Game.Scripts;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE PLATFORMS ORACLE: the C# Kurt and ScriptRuntime (src/Mdk.Game) stepped together at 1/60 s,
// Kurt then the scripts as Viewer.RunSteps does, under a scripted input. It pins Kurt against the
// solid objects (Solids.Walk through ArenaSpace.Move, Solids.Platform through Kurt.FindPlatform),
// the platform he stands on (Kurt.Platform, GetKurtPlatform, opcode 114 if_player_on_me), the
// carry in the object loop (Kurt.Carry), the keeping platform (Kurt.Keeper.cs, KeeperAt), the
// touched objects (Kurt.UpdateTouched) and the triangle groups his moves run into
// (Kurt.TakeContacts, ScriptRuntime.KurtTouchesGroups), and the camera's sight against the objects
// that block it (ScriptRuntime.ClipView). Its output is
// tests/scripts/golden/L*_plat_*.txt, and the C++ twin is
// `tick1 dump platforms <level> <steps> <schedule>` (tests/scripts/tick1.cpp, kurtPlatformsDump),
// which must print the same bytes.
//
//   Probe <level> <steps> <schedule>
//   Probe scan <level>            (lists every arena's solid objects; not a golden)
//
// The schedule is comma-separated: `key:from-to` holds a key over steps [from, to) (keys fwd, back,
// left, right, sleft, sright, jump, turbo, fire), and `tp:step:ARENA:x:y:z:yaw` calls
// ScriptRuntime.TeleportKurt before that step (the tests' --teleport), and
// `spawn:step:TYPE:x:y:z:yaw:script`, ScriptRuntime.Spawn (Spawning.Plain, as spawn_relative does)
// of TYPE in Kurt's arena, its script at that CMI offset, before that step. The goldens' schedules are in
// tick1.cpp's kPlatformCases table.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/items_probe -- copy
// src/Mdk.Formats, src/Mdk.Engine, src/Mdk.Game and Directory.Build.props into a scratch tree with
// this directory beside src/; in the scratch Mdk.Engine.csproj drop the shader targets and the
// icon resource; `dotnet build -c Release -o probe-bin`, then
// `DOTNET_ROOT=<sdk> MDK_DATA_DIR=<the MDK folder> probe-bin/Probe ...`.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
var scan = args[0] == "scan";
var level = int.Parse(scan || args[0] == "dis" ? args[1] : args[0]);
var steps = scan || args[0] == "dis" ? 0 : int.Parse(args[1]);
var schedule = !scan && args.Length > 2 ? args[2] : "";
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

if (args[0] == "dis")
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

if (scan)
{
    // Every reachable arena in turn: Kurt is put at its centre (no Kurt step), its controller runs
    // a few ticks, and its solid objects are listed with whether their scripts use opcode 114.
    foreach (var arena in levelData.Arenas.Where(a => levelData.IsReachable(a.Name)))
    {
        var min = arena.Vertices.Aggregate(Vector3.Min);
        var max = arena.Vertices.Aggregate(Vector3.Max);
        runtime.TeleportKurt(arena.Name, (min + max) * 0.5f, 0f);
        for (var i = 0; i < 8; i++)
        {
            runtime.Update(1f / 60f);
        }

        var state = runtime.GetArenaState(arena.Name);
        for (var g = 0; g < 16; g++)
        {
            if (((state.GroupHitMasks[g] | state.GroupHitFlags[g]) & ScriptRuntime.HitKurt) == 0)
            {
                continue;
            }

            // The group's triangles: their count and the centre of the first up-facing one.
            var count = 0;
            var first = "";
            for (var t = 0; t < arena.TriangleFlags.Length; t++)
            {
                if ((int)(arena.TriangleFlags[t] >> 24) != g + 1)
                {
                    continue;
                }

                count++;
                var a = arena.Vertices[arena.TriangleIndices[t * 3]];
                var b = arena.Vertices[arena.TriangleIndices[t * 3 + 1]];
                var c = arena.Vertices[arena.TriangleIndices[t * 3 + 2]];
                var n = Vector3.Cross(b - a, c - a);
                if (first.Length == 0)
                {
                    var m = (a + b + c) / 3f;
                    first = $"{F(m.X)},{F(m.Y)},{F(m.Z)} n={F(n.X)},{F(n.Y)},{F(n.Z)}";
                }
            }

            Console.WriteLine($"{arena.Name} group={g + 1} mask={state.GroupHitMasks[g]:X} flags={state.GroupHitFlags[g]:X} script={state.GroupHitScripts[g]} tris={count} at={first}");
        }

        foreach (var o in runtime.Objects.Where(o => o.Arena == arena.Name && !o.Dead))
        {
            var decoder = ScriptDecoder.For(cmi);
            var ops = new HashSet<int>();
            try
            {
                decoder.Preload(new[] { o.Restart, o.DeathScript }.Where(x => x > 0));
                ops = decoder.Decoded.Select(i => i.Opcode).ToHashSet();
            }
            catch (IndexOutOfRangeException)
            {
            }

            var b = runtime.GetWorldBounds(o);
            Console.WriteLine($"{arena.Name} {Name(o)} foot={o.Footing} flags={o.Flags:X} hp={o.Health} pos={F(o.Position.X)},{F(o.Position.Y)},{F(o.Position.Z)} "
                + $"box={F(b.Min.X)},{F(b.Min.Y)},{F(b.Min.Z)}..{F(b.Max.X)},{F(b.Max.Y)},{F(b.Max.Z)} op114={(ops.Contains(114) ? 1 : 0)} op209={(ops.Contains(209) ? 1 : 0)}");
        }
    }

    return;
}

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

var keptField = typeof(KurtT).GetField("_keeper", BindingFlags.NonPublic | BindingFlags.Instance)!;
var input = new Input();
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

    kurt.Update(input, 1f / 60f);
    runtime.Update(1f / 60f);

    var touched = string.Join("|", kurt.Touched.Select(Name));
    var hits = runtime.GetArenaState(runtime.CurrentArena).HitScripts;
    // The solid objects within 40 units of Kurt in his arena (squared, so no square root decides
    // the list): where they are, their yaw, hidden parts and flags.
    var near = new List<string>();
    foreach (var o in runtime.Objects)
    {
        if (o.Dead || o.Arena != runtime.CurrentArena || o.Footing == null || Vector3.DistanceSquared(o.Position, kurt.Feet) > 1600f)
        {
            continue;
        }

        near.Add($"{Name(o)}@{P(o.Position.X)},{P(o.Position.Y)},{P(o.Position.Z)}/y{F(o.Yaw)}/h{o.HiddenParts:X}/f{o.Flags:X}/r{o.Restart}");
    }

    // ScriptRuntime.ClipView (the follow camera's object pass) from his head (FollowCamera's
    // HeadHeight 5.5) to 30 units north of it: where a blocking object's part cuts the sight.
    var head = kurt.Feet + Vector3.UnitZ * 5.5f;
    var clip = runtime.ClipView(head, head + new Vector3(0f, 30f, 0f));
    Console.WriteLine($"s={step} arena={runtime.CurrentArena} state={kurt.Current} feet64={P(kurt.Feet.X)},{P(kurt.Feet.Y)},{P(kurt.Feet.Z)} "
        + $"yaw={F(kurt.Yaw)} vz={F(kurt.VerticalSpeed)} floor={(kurt.OnFloor ? 1 : 0)} plat={Name(kurt.Platform)} on={Name(runtime.GetKurtPlatform())} "
        + $"kept={Name(keptField.GetValue(kurt))} touched={(touched.Length == 0 ? "-" : touched)} hit={hits.HitType}/{hits.Restart} "
        + $"clip={P(clip.X)},{P(clip.Y)},{P(clip.Z)} objs={runtime.Objects.Count} near={(near.Count == 0 ? "-" : string.Join(";", near))}");
}
