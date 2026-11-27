using System.Globalization;
using System.Numerics;
using System.Reflection;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Level;
using Mdk.Game.Objects;
using Mdk.Game.Scripts;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE HAZARDS ORACLE: the C# Kurt and ScriptRuntime (src/Mdk.Game) stepped together at 1/60 s,
// Kurt then the scripts as Viewer.RunSteps does, under a scripted input -- tests/scripts/items_probe
// with a teleport, a script runner and a watch list. It pins the combat and hazard opcodes
// (ScriptVm 158 touch_damage, 159 spawn_box, 167 move_to_bomb, 172 explosion, 178 explosion_damage,
// 184 explode; ScriptRuntime.TouchDamage, SpawnBox/BoxModel) and path_speed_by_kurt's consumer
// (ObjectMotion.UpdatePathSpeed). Its output is tests/scripts/golden/L*_hazards_*.txt, and the C++
// twin is `tick1 dump hazards <level> <steps> <schedule>` (tests/scripts/tick1.cpp,
// kurtHazardsDump), which must print the same bytes.
//
//   Probe <level> <steps> <schedule>
//
// The schedule is comma-separated. items_probe's tokens: `key:from-to` holds a key over steps
// [from, to), `give:step:MODEL` hands Kurt a pickup before that step, `drop:step:MODEL:dx:dy:dz`
// spawns a pickup (Spawning.Flagged) at the level's start plus that offset. And this probe's:
//   `tp:step:ARENA:x:y:z`        TeleportKurt into ARENA at (x, y, z), keeping his yaw, before
//                                that step (before the gives);
//   `run:step:MODEL:pc:dx:dy:dz` spawns MODEL (Spawning.Plain) in Kurt's arena at his feet plus
//                                the offset, running the retail script at CMI offset pc -- the
//                                bytecode the level's own objects run, entered at an opcode;
//   `watch:TYPE`                 prints every live object of that type each step;
//   `flying`                     appends ` fly=P,E`: FNV-1a hashes of every debris piece's
//                                centre, velocity and ticks and every effect's position,
//                                velocity and life (2026-11-20, docs/DIVERGENCES.md D5).
//   `rope:TYPE`                  appends ` rope=`: every live object of that type's place,
//                                pitch, yaw, swing speed and plane (jump_to, opcode 226),
//                                flags, rope colour and mask and RopeView.LinesOf's ends
//                                (blending off), at 1/10000, and `#` an FNV-1a over the bits of
//                                all those floats (2026-11-26, the opcode parity branch);
//   `frames`                     appends ` tex=`: AnimatedTextures.Frames (opcode 133) in
//                                ordinal (arena, texture) order, arena/texture:value:FrameOf.
// The goldens' schedules are in tick1.cpp's kHazardsCases table.
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
// The triangle groups as the Viewer registers them (LevelView's constructor), so the blasts find
// the groups' triangles.
var groups = new TriangleGroups();
foreach (var arena in levelData.Arenas)
{
    groups.Add(arena);
}

var runtime = new ScriptRuntime(levelData, cmi, sprites, space, groups, mixer, kurt, 1) { Option = 1 };
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

var keys = new Dictionary<string, Key>
{
    ["fwd"] = Key.Forward, ["back"] = Key.Back, ["left"] = Key.TurnLeft, ["right"] = Key.TurnRight,
    ["sleft"] = Key.StrafeLeft, ["sright"] = Key.StrafeRight, ["jump"] = Key.Jump, ["turbo"] = Key.Turbo,
    ["fire"] = Key.Fire, ["use"] = Key.UseItem, ["next"] = Key.ItemNext, ["prev"] = Key.ItemPrevious,
};
var holds = new List<(Key Key, int From, int To)>();
var gives = new List<(int Step, string Model)>();
var drops = new List<(int Step, string Model, Vector3 Offset)>();
var teleports = new List<(int Step, string Arena, Vector3 Position)>();
var runs = new List<(int Step, string Model, int Pc, Vector3 Offset)>();
var watched = new List<string>();
var roped = new List<string>();
var textures = false;
var flying = false;
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    switch (parts[0])
    {
        case "give":
            gives.Add((int.Parse(parts[1]), parts[2]));
            continue;
        case "drop":
            drops.Add((int.Parse(parts[1]), parts[2], new Vector3(float.Parse(parts[3]), float.Parse(parts[4]), float.Parse(parts[5]))));
            continue;
        case "tp":
            teleports.Add((int.Parse(parts[1]), parts[2], new Vector3(float.Parse(parts[3]), float.Parse(parts[4]), float.Parse(parts[5]))));
            continue;
        case "run":
            runs.Add((int.Parse(parts[1]), parts[2], int.Parse(parts[3]), new Vector3(float.Parse(parts[4]), float.Parse(parts[5]), float.Parse(parts[6]))));
            continue;
        case "watch":
            watched.Add(parts[1]);
            continue;
        case "flying":
            flying = true;
            continue;
        case "rope":
            roped.Add(parts[1]);
            continue;
        case "frames":
            textures = true;
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

    foreach (var (s, model, offset) in drops)
    {
        if (s == step)
        {
            runtime.Spawn(runtime.GetArenaState(runtime.CurrentArena).Controller, model,
                levelData.Dti.StartPosition + offset, 0f, -1, 0, ScriptRuntime.Spawning.Flagged);
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

    var ohp = 0;
    long sx = 0, sy = 0, sz = 0;
    var watch = new List<string>();
    foreach (var o in runtime.Objects)
    {
        if (o.Dead)
        {
            continue;
        }

        ohp += o.Health;
        sx += P(o.Position.X);
        sy += P(o.Position.Y);
        sz += P(o.Position.Z);
        if (watched.Contains(o.TypeName))
        {
            watch.Add($"{o.TypeName}#{o.InstanceId}:h{o.Health}@{P(o.Position.X)},{P(o.Position.Y)},{P(o.Position.Z)}"
                + $"/m{o.MoveCommand}>{P(o.MoveDestination.X)},{P(o.MoveDestination.Y)},{P(o.MoveDestination.Z)}"
                + $"/v{F(o.PathSpeed)}/f{o.Flags:X}/e{o.HitEvent}/t{o.HitType}");
        }
    }

    var boxes = new List<string>();
    foreach (var b in runtime.Boxes)
    {
        if (!b.Dead)
        {
            var half = b.Model!.Bounds.Max;
            boxes.Add($"{b.TypeName}#{b.InstanceId}@{P(b.Position.X)},{P(b.Position.Y)},{P(b.Position.Z)}/{P(half.X)},{P(half.Y)},{P(half.Z)}");
        }
    }

    Console.WriteLine($"s={step} state={kurt.Current} arena={runtime.CurrentArena} feet64={P(kurt.Feet.X)},{P(kurt.Feet.Y)},{P(kurt.Feet.Z)} " +
        $"yaw={F(kurt.Yaw)} hp={kurt.Health} kd={F(kurt.KnockDamage)} objs={runtime.Objects.Count} ohp={ohp} " +
        $"opos={sx},{sy},{sz} kills={runtime.Stats.Kills} boxes={(boxes.Count == 0 ? "-" : string.Join(";", boxes))} " +
        $"watch={(watch.Count == 0 ? "-" : string.Join(";", watch))} " +
        $"bomb={(runtime.Items.Bomb is { Dead: false } ? 1 : 0)} pieces={runtime.Debris.Pieces.Count} effects={runtime.Effects.All.Count} " +
        $"white={F(kurt.WhiteFlash)} shake={F(runtime.Shake)} voices={Voices()}" + (flying ? Flying() : "")
        + (roped.Count > 0 ? Ropes() : "") + (textures ? Textures() : ""));
}

// The `frames` token's column (tick1's kurtHazardsDump prints the same, from a std::map).
string Textures()
{
    var all = runtime.AnimatedTextures.Frames
        .OrderBy(f => f.Key.Arena, StringComparer.Ordinal).ThenBy(f => f.Key.Texture, StringComparer.Ordinal)
        .Select(f => $"{f.Key.Arena}/{f.Key.Texture}:{F(f.Value)}:{runtime.AnimatedTextures.FrameOf(f.Key.Arena, f.Key.Texture)}")
        .ToList();
    return " tex=" + (all.Count == 0 ? "-" : string.Join(";", all));
}

// The `rope:TYPE` token's column (tick1's kurtHazardsDump prints the same).
string Ropes()
{
    var all = new List<string>();
    foreach (var o in runtime.Objects)
    {
        if (o.Dead || !roped.Contains(o.TypeName))
        {
            continue;
        }

        var bits = 2166136261u;
        foreach (var v in new[] { o.Position.X, o.Position.Y, o.Position.Z, o.Pitch, o.Yaw, o.SwingSpeed, o.SwingYaw })
        {
            bits = Fnv(bits, BitConverter.SingleToInt32Bits(v));
        }

        var ends = RopeView.LinesOf(o);
        var text = new List<string>();
        foreach (var e in ends)
        {
            text.Add($"{F(e.X)},{F(e.Y)},{F(e.Z)}");
            bits = Fnv(bits, BitConverter.SingleToInt32Bits(e.X));
            bits = Fnv(bits, BitConverter.SingleToInt32Bits(e.Y));
            bits = Fnv(bits, BitConverter.SingleToInt32Bits(e.Z));
        }

        all.Add($"{o.TypeName}#{o.InstanceId}@{F(o.Position.X)},{F(o.Position.Y)},{F(o.Position.Z)}/p{F(o.Pitch)}/y{F(o.Yaw)}"
            + $"/w{F(o.SwingSpeed)}/sy{F(o.SwingYaw)}/f{o.Flags:X}/c{o.RopeColor}/m{o.RopeMask:X}/l"
            + (ends.Count == 0 ? "-" : string.Join("|", text)) + $"#{bits:X8}");
    }

    return " rope=" + (all.Count == 0 ? "-" : string.Join(";", all));
}

// The `flying` token's column: FNV-1a over every piece's centre, velocity and ticks, and every
// effect's position, velocity and life, as bits, in list order (Debris.Move's and Effects.Move's
// bounces). The pieces' orientation is left out: Quaternion.CreateFromYawPitchRoll takes its sines
// from Vector3.SinCos, which the C++'s libm does not reproduce (docs/DIVERGENCES.md, D5).
string Flying()
{
    var pieces = 2166136261u;
    foreach (var p in runtime.Debris.Pieces)
    {
        foreach (var v in new[] { p.Center.X, p.Center.Y, p.Center.Z, p.Velocity.X, p.Velocity.Y, p.Velocity.Z })
        {
            pieces = Fnv(pieces, BitConverter.SingleToInt32Bits(v));
        }

        pieces = Fnv(pieces, p.Ticks);
    }

    var effects = 2166136261u;
    foreach (var e in runtime.Effects.All)
    {
        foreach (var v in new[] { e.Position.X, e.Position.Y, e.Position.Z, e.Velocity.X, e.Velocity.Y, e.Velocity.Z, e.Life })
        {
            effects = Fnv(effects, BitConverter.SingleToInt32Bits(v));
        }
    }

    return $" fly={pieces:X8},{effects:X8}";
}

uint Fnv(uint hash, int value)
{
    foreach (var b in BitConverter.GetBytes(value))
    {
        hash = (hash ^ b) * 16777619u;
    }

    return hash;
}
