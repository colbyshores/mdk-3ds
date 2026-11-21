using System.Globalization;
using System.Reflection;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Flow;
using Mdk.Game.Kurt;
using Mdk.Game.Level;
using Mdk.Game.Menu;
using Mdk.Game.Scripts;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE CHEATS' ORACLE (gap package #21): the C#'s own Cheats (Flow/Cheats.cs), Viewer.TypeCheats
// (Viewer.cs:862-882), MainMenu's debug keys (MainMenu.HandleKeys/DebugKey, MainMenu.cs:175-243)
// and Kurt's god mode and noclip (Kurt.Cheats.cs; the god guards in Kurt.Fall and Kurt.Damage).
// Its output is tests/scripts/golden/cheats_*.txt; the C++ twins are `vm_tests dump-cheats
// typed|menu ...` (tests/scripts/flow_tests.cpp) and `tick1 dump cheats <level> <steps>
// <schedule>` (tests/scripts/tick1.cpp), which must print the same bytes.
//
//   Probe typed <gore 0|1> <text>    each letter Viewer.TypeCheats hands Cheats.Type: the cheat it
//                                    completed, Settings.Gore, GameState.DebugKeys, the script
//                                    Option the Viewer sets, and the cheat's window (_typed)
//   Probe menu <debug 0|1> <text>    MainMenu.HandleKeys' typed half on the main page: whether a
//                                    debug key took the text, the event, the level, the counts
//   Probe kurt <level> <steps> <schedule>
//                                    Kurt, the scripts and the solid arenas at 1/60 s (Viewer.RunSteps),
//                                    the schedule `key:from-to` (fwd back left right sleft sright
//                                    jump turbo down fire), `god:step`, `mortal:step`,
//                                    `noclip:step`, `clip:step` (LevelCommands.SetGod/SetNoclip
//                                    before that step), `hurt:step:N` (Kurt.Hurt before that
//                                    step) and `yaw:DEG` (his yaw at the start)
//
// The menu's counts for D are Random.Shared's, so the probe prints only that all seven are below
// DebugCountLimit (100). MainMenu is made without its constructor (no Ui): DebugKey reads only
// _state and writes only _state and _next, which are set by reflection.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/items_probe -- copy
// src/Mdk.Formats, src/Mdk.Engine, src/Mdk.Game and Directory.Build.props into a scratch tree with
// this directory beside src/; in the scratch Mdk.Engine.csproj drop the shader targets and the
// icon resource; `dotnet build -c Release -o probe-bin`, then
// `DOTNET_ROOT=<sdk> MDK_DATA_DIR=<the MDK folder> probe-bin/Probe ...`.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
var flags = BindingFlags.NonPublic | BindingFlags.Instance;

if (args[0] == "typed")
{
    var settings = new Settings { Gore = args[1] == "1" };
    var state = new GameState();
    var cheats = new Cheats(settings, state);
    var window = typeof(Cheats).GetField("_typed", flags)!;
    // Viewer.cs:234: the runtime's Option is the setting's when the level starts.
    var option = settings.Gore ? 1 : 0;
    var text = args.Length > 2 ? args[2] : "";
    Console.WriteLine($"typed gore={args[1]} text={text}");
    // Viewer.TypeCheats: the text upper-cased, its ASCII capitals one at a time.
    foreach (var c in text.ToUpperInvariant().Where(char.IsAsciiLetterUpper))
    {
        var cheat = cheats.Type(c);
        if (cheat == Cheats.Cheat.Gore)
        {
            option = settings.Gore ? 1 : 0;
            Console.WriteLine($"Gore {(settings.Gore ? "on" : "off")}");
        }
        else if (cheat == Cheats.Cheat.DebugKeys)
        {
            Console.WriteLine($"Debug keys {(state.DebugKeys ? "on" : "off")}");
        }

        var typed = (string)window.GetValue(cheats)!;
        Console.WriteLine($"c={c} cheat={cheat} gore={(settings.Gore ? 1 : 0)} debug={(state.DebugKeys ? 1 : 0)} option={option} window={(typed.Length == 0 ? "-" : typed)}");
    }

    return;
}

if (args[0] == "menu")
{
    var state = new GameState { DebugKeys = args[1] == "1" };
    var text = args.Length > 2 ? args[2] : "";
    var menu = (MainMenu)System.Runtime.CompilerServices.RuntimeHelpers.GetUninitializedObject(typeof(MainMenu));
    typeof(MainMenu).GetField("_state", flags)!.SetValue(menu, state);
    typeof(MainMenu).GetField("_next", flags)!.SetValue(menu, Event.None);
    var debugKey = typeof(MainMenu).GetMethod("DebugKey", flags)!;
    // HandleKeys: `var typed = input.Typed.ToUpperInvariant(); if (_page == Page.Main &&
    // _state.DebugKeys && DebugKey(typed)) return;` -- on the main page.
    var typed = text.ToUpperInvariant();
    var handled = state.DebugKeys && (bool)debugKey.Invoke(menu, [typed])!;
    var next = (Event)typeof(MainMenu).GetField("_next", flags)!.GetValue(menu)!;
    var stats = state.Stats;
    int[] counts = [stats.Shots, stats.ShotHits, stats.SniperShots, stats.SniperHits, stats.HeadShots, stats.Enemies, stats.Kills];
    var drawn = next == Event.Statistics ? (counts.All(n => n is >= 0 and < 100) ? "below100" : "OUT") : "-";
    Console.WriteLine($"menu debug={args[1]} text={text} handled={(handled ? 1 : 0)} next={next} level={state.Level} counts={drawn}");
    return;
}

// kurt <level> <steps> <schedule>
var level = int.Parse(args[1]);
var steps = int.Parse(args[2]);
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

var keys = new Dictionary<string, Key>
{
    ["fwd"] = Key.Forward, ["back"] = Key.Back, ["left"] = Key.TurnLeft, ["right"] = Key.TurnRight,
    ["sleft"] = Key.StrafeLeft, ["sright"] = Key.StrafeRight, ["jump"] = Key.Jump, ["turbo"] = Key.Turbo,
    ["down"] = Key.Down, ["fire"] = Key.Fire,
};
var holds = new List<(Key Key, int From, int To)>();
var switches = new List<(int Step, string What)>();
var hurts = new List<(int Step, int Damage)>();
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    switch (parts[0])
    {
        case "god" or "mortal" or "noclip" or "clip":
            switches.Add((int.Parse(parts[1]), parts[0]));
            continue;
        case "hurt":
            hurts.Add((int.Parse(parts[1]), int.Parse(parts[2])));
            continue;
        case "yaw":
            kurt.Yaw = float.Parse(parts[1]);
            continue;
    }

    var range = parts[1].Split('-');
    holds.Add((keys[parts[0]], int.Parse(range[0]), int.Parse(range[1])));
}

int P(float v) => (int)MathF.Floor(v * 64f);
string F(float v) => v.ToString("F4", CultureInfo.InvariantCulture);
var input = new Input();
Console.WriteLine($"cheats level={level} steps={steps} seed=1 schedule={schedule}");
for (var step = 1; step <= steps; step++)
{
    foreach (var (s, what) in switches)
    {
        if (s != step)
        {
            continue;
        }

        // LevelCommands.SetGod / SetNoclip (DevTools/LevelCommands.cs:37-39).
        switch (what)
        {
            case "god": kurt.Mortality = Mortality.God; break;
            case "mortal": kurt.Mortality = Mortality.Mortal; break;
            case "noclip": kurt.Clipping = Clipping.Off; break;
            case "clip": kurt.Clipping = Clipping.On; break;
        }
    }

    foreach (var (s, damage) in hurts)
    {
        if (s == step)
        {
            kurt.Hurt(damage);
        }
    }

    foreach (var key in keys.Values)
    {
        var down = holds.Any(h => h.Key == key && step >= h.From && step < h.To);
        input.Hold(key, down ? Input.State.Down : Input.State.Up);
    }

    // Viewer.RunSteps: Kurt, the scripts, then Kurt's solid arenas (Viewer.cs:533), which
    // Kurt.Fall's Bottom reads.
    kurt.Update(input, 1f / 60f);
    runtime.Update(1f / 60f);
    space.SetSolid(runtime.SolidArenas);
    Console.WriteLine($"s={step} state={kurt.Current} feet64={P(kurt.Feet.X)},{P(kurt.Feet.Y)},{P(kurt.Feet.Z)} yaw={F(kurt.Yaw)} " +
                      $"hp={kurt.Health} floor={(kurt.OnFloor ? 1 : 0)} speeds={F(kurt.ForwardSpeed)},{F(kurt.StrafeSpeed)},{F(kurt.VerticalSpeed)} " +
                      $"god={(kurt.Mortality == Mortality.God ? 1 : 0)} clip={(kurt.Clipping == Clipping.On ? 1 : 0)}");
}
