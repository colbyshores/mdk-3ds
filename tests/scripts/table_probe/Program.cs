using System.Collections;
using System.Globalization;
using System.Reflection;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Level;
using Mdk.Game.Scripts;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE LEVEL TICK TABLES' ORACLE: the C#'s own ScriptRuntime run with no Kurt input for N ticks
// at 1/30 s (seed 1, Option 1), and its object list printed. Its output is
// tests/scripts/golden/L<level>_t<ticks>.txt (the object table) and
// golden/L<level>_t<ticks>_script.txt (the script register file and the per-object VM state);
// the C++ twins are tick1.cpp's dumpTable and dumpScript, which must print the same bytes.
//
//   Probe <level> <ticks>           the object table: position, yaw, health, flags, the move
//                                   command, the door's connection and the path
//   Probe <level> <ticks> script    the target, the global variables, Kurt's combat state, the
//                                   mixer's live voices, the debris pieces, and per object its
//                                   script flags, variables, restart/wait, gosub stacks, level
//                                   timers and the two sounds it holds
//
// The goldens were first made by a scratch driver that was lost; this probe was rebuilt
// 2026-11-26 from tick1.cpp's dumpTable/dumpScript and checked byte for byte against the
// goldens. Kurt is placed at the level's start and never updated (no input); the mixer runs
// on a muted device and is never updated, so its voice list is what the scripts started.
// Removed objects leave ScriptRuntime.Objects, so every listed object is printed.
//
// HOW IT IS BUILT AND RUN: as tests/scripts/cheats_probe -- copy src/Mdk.Formats,
// src/Mdk.Engine, src/Mdk.Game and Directory.Build.props (upstream C# 1.1.1, d5bb66ec) into a
// scratch tree with this directory beside src/; in the scratch Mdk.Engine.csproj drop the shader
// targets and the icon resource; `dotnet build -c Release -o probe-bin`, then
// `DOTNET_ROOT=<sdk> MDK_DATA_DIR=<the MDK folder> probe-bin/Probe 5 590`.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
var flags = BindingFlags.NonPublic | BindingFlags.Instance;

var level = int.Parse(args[0]);
var ticks = int.Parse(args[1]);
var script = args.Length > 2 && args[2] == "script";

// The world, as cheats_probe builds it: the reachable arenas for the space, every arena's
// triangle groups, the level's sounds preloaded, the sprites archive.
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
var kurt = new KurtT(space, mixer, _ => 0)
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
for (var t = 0; t < ticks; t++)
{
    runtime.Update(1f / 30f);
}

string F(float v) => v.ToString("F4", CultureInfo.InvariantCulture);
var objects = runtime.Objects;

if (!script)
{
    Console.Write($"level={level} ticks={ticks} seed=1 currentArena={runtime.CurrentArena} objects={objects.Count}\n");
    for (var i = 0; i < objects.Count; i++)
    {
        var o = objects[i];
        Console.Write($"{i:D3} type={o.TypeName} arena={o.Arena} pos=({F(o.Position.X)},{F(o.Position.Y)},{F(o.Position.Z)}) " +
                      $"yaw={F(o.Yaw)} hp={o.Health} flags=0x{o.Flags:X8} dead={(o.Dead ? 1 : 0)} " +
                      $"move={o.MoveCommand}/{o.MoveParameter} connects={o.Connects} " +
                      $"path={o.Path}/{F(o.PathTime)}/{F(o.PathSpeed)}/{o.PathStop}\n");
    }

    return;
}

// SCRIPT: the target the scripts aim at this tick, and the aliens' target's index in the list.
var target = runtime.TargetPosition;
var alien = runtime.AlienTarget == null ? -1 : objects.IndexOf(runtime.AlienTarget);
Console.Write($"SCRIPT level={level} ticks={ticks} seed=1 currentArena={runtime.CurrentArena} objects={objects.Count} " +
              $"target_pos={F(target.X)},{F(target.Y)},{F(target.Z)} target_yaw={F(runtime.TargetYaw)} alien_target={alien}\n");
var g = runtime.GlobalVariables;
Console.Write($"GVAR {F(g[0])} {F(g[1])} {F(g[2])} {F(g[3])}\n");

// KURT: what hurt_kurt (109) writes and if_inventory (175) reads.
Console.Write($"KURT health={kurt.Health} hurt_flash={F(kurt.HurtFlash)} invulnerable={F(kurt.Invulnerable)} " +
              $"knock={F(kurt.KnockDamage)} state={(int)kurt.Current} slots={kurt.Inventory.Slots.Count}\n");

// AUDIO: the mixer's voices (SoundMixer._voices, a private list of a private class), id:name.
var voices = (IList)typeof(SoundMixer).GetField("_voices", flags)!.GetValue(mixer)!;
var list = new List<string>();
foreach (var voice in voices)
{
    var type = voice!.GetType();
    list.Add($"{type.GetField("Id")!.GetValue(voice)}:{type.GetField("Name")!.GetValue(voice)}");
}

Console.Write($"AUDIO voices={voices.Count} list={string.Join(",", list)}\n");

// DEBRIS: the shattered groups and sparks in flight.
var pieces = runtime.Debris.Pieces.Select(p =>
    $"{p.Arena}@{F(p.Center.X)},{F(p.Center.Y)},{F(p.Center.Z)}v{F(p.Velocity.X)},{F(p.Velocity.Y)},{F(p.Velocity.Z)}t{p.Ticks}");
Console.Write($"DEBRIS pieces={runtime.Debris.Pieces.Count} list={string.Join(",", pieces)}\n");

for (var i = 0; i < objects.Count; i++)
{
    var o = objects[i];
    var v = o.Variables;
    var lt = o.LevelTimers;
    Console.Write($"OBJ {i:D3} type={o.TypeName} sflags=0x{o.ScriptFlags:X8} vars={F(v[0])},{F(v[1])},{F(v[2])},{F(v[3])} " +
                  $"restart={o.Restart} wait={F(o.WaitTime)} wresume={o.WaitResume} death={o.DeathScript} cmdtgt={o.CommandTarget} " +
                  $"gret={string.Join(",", o.GosubReturns)} gres={string.Join(",", o.GosubRestarts)} " +
                  $"timers={F(lt[0])},{F(lt[1])},{F(lt[2])},{F(lt[3])},{F(lt[4])} " +
                  $"tsnd={o.TrackedSound} tvoice={o.TrackedVoice} tplay={(mixer.IsVoicePlaying(o.TrackedVoice) ? 1 : 0)} " +
                  $"lsnd={o.LoopSoundName} lvoice={o.LoopSound} lplay={(mixer.IsVoicePlaying(o.LoopSound) ? 1 : 0)}\n");
}
