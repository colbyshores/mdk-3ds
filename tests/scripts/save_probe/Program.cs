using System.Globalization;
using System.Numerics;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Flow;
using Mdk.Game.Level;
using Mdk.Game.Scripts;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE SAVE ORACLE (package #19): the C# full save (F2) and its load, end to end. Kurt and the
// ScriptRuntime of src/Mdk.Game are stepped together at 1/60 s, Kurt then the scripts, as
// Viewer.RunSteps does and as tests/scripts/items_probe does (its setup and its schedule
// grammar), then:
//
//   1. the level's full save is taken (ScriptRuntime.Capture, Scripts/Snapshot.cs) and printed
//      whole, with Snapshot.Hash of it, then wrapped in the save file SaveGames writes
//      (SaveGame.ToJson, kind Snapshot, a fixed time so the bytes repeat) and that is printed;
//   2. a SECOND world is made exactly as the first (a fresh level, Kurt at the DTI's start, a
//      fresh runtime seeded 1), the save file is parsed back (SaveGame.Parse) and loaded into it
//      (GameState.Load, then ScriptRuntime.Load, as Viewer.RestoreSnapshot does), and its own
//      capture is printed (hash, and the text when it differs from the save);
//   3. the restored world runs on for <after> steps with no keys held, a line a step: Kurt's
//      state, feet and health and the hash of the whole capture; then the last capture whole.
//
//   Probe <level> <steps> <after> <schedule>
//   Probe load <file> <level> <after>        step 2-3 only, from a save file on disk (the C++
//                                            twin's, for the cross-load check)
//
// The schedule is items_probe's: `key:from-to` holds a key over steps [from, to) (fwd, back,
// left, right, sleft, sright, jump, turbo, fire, use, next, prev) and `give:step:MODEL` hands Kurt
// a pickup before that step; `yaw:DEG` turns Kurt to that yaw before the first step (the
// port walks Kurt along mdk::fastCos/fastSin, the C# along MathF.Cos/Sin, which agree to the
// bit only at yaw 0 -- AGENTS.md R6 -- so a save golden that moves Kurt walks him east). The
// `clip:ROUNDS:TIME` sets the sniper clip (Scope.ClipRounds/ClipTime, which a save carries)
// before the first step; `tp:step:ARENA:x:y:z` and `run:step:MODEL:pc:dx:dy:dz` are
// hazards_probe's (TeleportKurt before that step; MODEL spawned at Kurt's feet plus the offset
// running the CMI's script at pc). The C++ twin is `tick1 dump save ...` (tests/scripts/tick1.cpp,
// saveDump), which must print the same bytes; the goldens are tests/scripts/golden/L*_save_*.txt.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/items_probe -- copy
// src/Mdk.Formats, src/Mdk.Engine, src/Mdk.Game and Directory.Build.props into a scratch tree with
// this directory beside src/; `dotnet build -c Release -o probe-bin`, then
// `DOTNET_ROOT=<sdk> MDK_DATA_DIR=<the MDK folder> probe-bin/Probe ...`.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
const string FixedTime = "2026-11-20T12:00:00";
var data = MdkData.Find() ?? throw new Exception("no data");

(KurtT Kurt, ScriptRuntime Runtime) World(int level)
{
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
    return (kurt, runtime);
}

var keys = new Dictionary<string, Key>
{
    ["fwd"] = Key.Forward, ["back"] = Key.Back, ["left"] = Key.TurnLeft, ["right"] = Key.TurnRight,
    ["sleft"] = Key.StrafeLeft, ["sright"] = Key.StrafeRight, ["jump"] = Key.Jump, ["turbo"] = Key.Turbo,
    ["fire"] = Key.Fire, ["use"] = Key.UseItem, ["next"] = Key.ItemNext, ["prev"] = Key.ItemPrevious,
};
var holds = new List<(Key Key, int From, int To)>();
var gives = new List<(int Step, string Model)>();
float? startYaw = null;
(int Rounds, float Time)? clip = null;
var teleports = new List<(int Step, string Arena, Vector3 Position)>();
var runs = new List<(int Step, string Model, int Pc, Vector3 Offset)>();

void Step(KurtT kurt, ScriptRuntime runtime, Input input, int step)
{
    foreach (var (s, arena, position) in teleports)
    {
        if (s == step)
        {
            runtime.TeleportKurt(arena, position, kurt.Yaw);
        }
    }

    foreach (var (s, model) in gives)
    {
        if (s == step)
        {
            kurt.Collect(model);
        }
    }

    foreach (var (s, model, pc, offset) in runs)
    {
        if (s == step)
        {
            runtime.Spawn(runtime.GetArenaState(runtime.CurrentArena).Controller, model,
                kurt.Feet + offset, 0f, -1, pc, ScriptRuntime.Spawning.Plain);
        }
    }

    foreach (var key in keys.Values)
    {
        var down = holds.Any(h => h.Key == key && step >= h.From && step < h.To);
        input.Hold(key, down ? Input.State.Down : Input.State.Up);
    }

    kurt.Update(input, 1f / 60f);
    runtime.Update(1f / 60f);
}

int P(float v) => (int)MathF.Floor(v * 64f);
string F(float v) => v.ToString("F4", CultureInfo.InvariantCulture);

// Steps 2-3: a fresh world takes the save file and runs on.
void Restore(int level, string file, int after)
{
    var save = SaveGame.Parse(file) ?? throw new Exception("the save did not parse");
    var state = new GameState();
    state.Load(save);
    Console.WriteLine($"parsed kind={(int)save.Kind} level={save.Level} health={save.Health} deaths={save.Deaths} " +
        $"strike={save.StrikeUsed} time={save.Time} state={state.Snapshot?.Length ?? -1}");
    var (kurt, runtime) = World(level);
    runtime.Load(state.Snapshot!);
    var restored = runtime.Capture();
    Console.WriteLine($"restored {Snapshot.Hash(restored)} {restored.Length} objects {runtime.Objects.Count}");
    if (restored != state.Snapshot)
    {
        Console.WriteLine(restored);
    }

    var input = new Input();
    holds.Clear();
    gives.Clear();
    teleports.Clear();
    runs.Clear();
    for (var step = 1; step <= after; step++)
    {
        Step(kurt, runtime, input, step);
        var json = runtime.Capture();
        Console.WriteLine($"s={step} state={kurt.Current} feet64={P(kurt.Feet.X)},{P(kurt.Feet.Y)},{P(kurt.Feet.Z)} " +
            $"yaw={F(kurt.Yaw)} hp={kurt.Health} objs={runtime.Objects.Count} arena={runtime.CurrentArena} hash={Snapshot.Hash(json)}");
    }

    Console.WriteLine("final");
    Console.WriteLine(runtime.Capture());
}

if (args[0] == "flow")
{
    // `Probe flow <dir>`: the save record and the folder without a level (golden/save_flow.txt,
    // the C++ twin is flow_tests' saveFlowDump): light saves as GameState.Save writes them,
    // SaveGame.Parse over well- and ill-formed texts, GameState.Load of what parses, and the
    // folder's Write/Read/List/Delete -- then a death's LASTGAME as Game.Handle(KurtDied)
    // writes it and MainMenu.Load(LASTGAME) reads it back.
    var dir = args[1];
    if (Directory.Exists(dir))
    {
        Directory.Delete(dir, true);
    }

    var saves = new SaveGames(dir);
    var light = new GameState { Level = 4, Deaths = 2, StrikeUsed = true };
    foreach (var kind in new[] { SaveKind.LevelStart, SaveKind.BeforeLevel, SaveKind.Snapshot })
    {
        Console.WriteLine($"save {(int)kind} {(light.Save(kind) with { Time = FixedTime }).ToJson()}");
    }

    Console.WriteLine($"escaped {new SaveGame(SaveKind.LevelStart, 3, 100, 0, false, "a<b>&'\"+`\\/\u00e9\n\u0001").ToJson()}");
    string[] texts =
    [
        "", "{}", "[]", "not json", "{\"level\":5} x", "{\"level\":2}", "{\"level\":9}", "{\"level\":5}",
        "{\"level\":5.9,\"type\":6,\"health\":40.5,\"deaths\":-1,\"strike_used\":true,\"time\":\"x\"}",
        "{\"level\":5,\"strike_used\":1,\"time\":7,\"snapshot\":[1],\"health\":null}",
        "{ \"level\" : 5 , \"snapshot\" : {\"a\": [1, 2 ], \"b\":\"\\u0041\"} }",
        "{\"level\":7,\"type\":1003,\"snapshot\":{},\"time\":\"\\u00e9\\ud83d\\ude00\"}",
        "{\"level\":3,\"level\":8}", "{\"level\":8e0,\"type\":3.5}", "{\"level\":-0}",
    ];
    foreach (var sample in texts)
    {
        var parsed = SaveGame.Parse(sample);
        if (parsed == null)
        {
            Console.WriteLine("parse null");
            continue;
        }

        var loaded = new GameState();
        loaded.Load(parsed);
        Console.WriteLine($"parse kind={(int)parsed.Kind} level={parsed.Level} health={parsed.Health} deaths={parsed.Deaths} " +
            $"strike={parsed.StrikeUsed} time={parsed.Time} state={parsed.State ?? "null"} -> load level={loaded.Level} " +
            $"deaths={loaded.Deaths} strike={loaded.StrikeUsed} snapshot={loaded.Snapshot ?? "null"} json={parsed.ToJson()}");
    }

    Console.WriteLine($"list {string.Join(",", saves.List())} read {saves.Read("A")?.ToJson() ?? "null"}");
    Console.WriteLine($"write B {saves.Write("B", light.Save(SaveKind.BeforeLevel) with { Time = FixedTime })} " +
        $"write A {saves.Write("A", light.Save(SaveKind.LevelStart) with { Time = FixedTime })}");
    File.WriteAllText(Path.Combine(dir, "BAD.sav"), "{\"level\":1}");
    File.WriteAllText(Path.Combine(dir, "note.txt"), "not a save");
    Console.WriteLine($"list {string.Join(",", saves.List())} read A {saves.Read("A")?.ToJson() ?? "null"} " +
        $"read BAD {saves.Read("BAD")?.ToJson() ?? "null"} read C {saves.Read("C")?.ToJson() ?? "null"}");
    Console.WriteLine($"file A {File.ReadAllText(Path.Combine(dir, "A.sav"))}");
    saves.Delete("B");
    saves.Delete("C");
    Console.WriteLine($"deleted B: list {string.Join(",", saves.List())} quick(A) {saves.QuickSlot("A")}");

    // A death: Viewer.OnDied counts it, Game.Handle(KurtDied) writes LASTGAME (kind 3, the level's
    // start, health 100), the menu's Continue (MainMenu.Load) reads it back and plays the level.
    var died = new GameState { Level = 6, Deaths = 0 };
    died.Level = 6;
    died.Deaths++;
    died.StrikeUsed = false;
    saves.Write(SaveGames.LastGame, died.Save(SaveKind.LevelStart) with { Time = FixedTime });
    var last = saves.Read(SaveGames.LastGame)!;
    var next = new GameState();
    next.Load(last);
    Console.WriteLine($"died LASTGAME {File.ReadAllText(Path.Combine(dir, "LASTGAME.sav"))} -> level {next.Level} deaths {next.Deaths} " +
        $"next {(last.Kind == SaveKind.BeforeLevel ? "Briefing" : "Play")}");
    Directory.Delete(dir, true);
    return;
}

if (args[0] == "load")
{
    var file = File.ReadAllText(args[1]);
    var loadLevel = int.Parse(args[2]);
    var loadAfter = int.Parse(args[3]);
    Console.WriteLine($"save load level={loadLevel} after={loadAfter}");
    Restore(loadLevel, file, loadAfter);
    return;
}

var level = int.Parse(args[0]);
var steps = int.Parse(args[1]);
var after = int.Parse(args[2]);
var schedule = args.Length > 3 ? args[3] : "";
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    if (parts[0] == "give")
    {
        gives.Add((int.Parse(parts[1]), parts[2]));
        continue;
    }

    if (parts[0] == "yaw")
    {
        startYaw = float.Parse(parts[1], CultureInfo.InvariantCulture);
        continue;
    }

    if (parts[0] == "clip")
    {
        clip = (int.Parse(parts[1]), float.Parse(parts[2], CultureInfo.InvariantCulture));
        continue;
    }

    if (parts[0] == "tp")
    {
        teleports.Add((int.Parse(parts[1]), parts[2], new Vector3(float.Parse(parts[3]), float.Parse(parts[4]), float.Parse(parts[5]))));
        continue;
    }

    if (parts[0] == "run")
    {
        runs.Add((int.Parse(parts[1]), parts[2], int.Parse(parts[3]), new Vector3(float.Parse(parts[4]), float.Parse(parts[5]), float.Parse(parts[6]))));
        continue;
    }

    var range = parts[1].Split('-');
    holds.Add((keys[parts[0]], int.Parse(range[0]), int.Parse(range[1])));
}

Console.WriteLine($"save level={level} steps={steps} after={after} seed=1 schedule={schedule}");
var (firstKurt, first) = World(level);
if (startYaw is { } yaw)
{
    firstKurt.Yaw = yaw;
}

if (clip is { } c)
{
    firstKurt.Scope.ClipRounds = c.Rounds;
    firstKurt.Scope.ClipTime = c.Time;
}

var firstInput = new Input();
for (var step = 1; step <= steps; step++)
{
    Step(firstKurt, first, firstInput, step);
}

var capture = first.Capture();
Console.WriteLine($"capture {Snapshot.Hash(capture)} {capture.Length} objects {first.Objects.Count} can={first.CanSnapshot()}");
// Viewer.SnapshotSave: the light save of the level (GameState.Save) with Kurt's health and the state.
var game = new GameState { Level = level };
var saved = new SaveGame(SaveKind.Snapshot, game.Level, firstKurt.Health, game.Deaths, game.StrikeUsed, FixedTime, capture);
var text = saved.ToJson();
Console.WriteLine($"file {text.Length}");
Console.WriteLine(text);
if (Environment.GetEnvironmentVariable("SAVE_PROBE_WRITE") is { } path)
{
    File.WriteAllText(path, text);
}

Restore(level, text, after);
