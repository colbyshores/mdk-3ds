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

// THE BULLET HOLES ORACLE (PORT_GAPS package #16): the C# Kurt and ScriptRuntime (src/Mdk.Game)
// stepped together at 1/60 s, Kurt then the scripts as Viewer.RunSteps does, under the sniper
// rounds probe's scripted input (tests/scripts/rounds_probe). It pins Scripts/BulletHoles.cs
// (Nearest, Weights, Stamp) and ScriptRuntime.StampBulletHole / ArenaTexture: special_130, which
// the retail scripts run from an arena script's object when a sniper round hits it (if_hit_part
// ANY, gosub special_130: L5 MUSE_2 and MUSE_5, L6 OLYM_10, L8 GUNT_10). Every texture of the
// level is copied when the level loads; on each TextureStamped the texture is compared with its
// copy and the change is printed, then the copy is brought up to date:
//
//   stamp tex=NAME WxHxFRAMES before=FNV after=FNV rect=X0,Y0,X1,Y1 n=CHANGED
//
// (FNV-1a 32 of the texture's indices, every frame; the rectangle is the changed texels' bounds,
// inclusive, in the stacked-frame image; n how many changed). Its output is
// tests/scripts/golden/L*_holes_*.txt, and the C++ twin is `tick1 dump holes <level> <steps>
// <schedule>` (tests/scripts/tick1.cpp, bulletHolesDump), which must print the same bytes.
//
//   Probe <level> <steps> <schedule>
//   Probe list <level> <steps> <schedule>   after the steps, the objects of Kurt's arena with the
//                                           yaw and pitch from his sniper eye to their centres
//
// The schedule is the rounds probe's (key holds, give, aim, tp, hp) plus `opt:step:N`, which sets
// ScriptRuntime.Option (0: BHOLE2, the goreless hole) before that step, and `stampall:step:ID`,
// which calls StampBulletHole on object ID once per part of its model, with ShotPart that part + 1
// and ShotPoint its PartCenter (the special_130 a script would run, at every part of the model:
// the nearest face, its weights and the texel wrap over many more faces than a few rounds reach).
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/rounds_probe --
// copy src/Mdk.Formats, src/Mdk.Engine, src/Mdk.Game and Directory.Build.props into a scratch tree
// with this directory beside src/; in the scratch Mdk.Engine.csproj drop the shader targets and
// the icon resource; `dotnet build -c Release -o probe-bin`, then
// `DOTNET_ROOT=<sdk> MDK_DATA_DIR=<the MDK folder> probe-bin/Probe ...`.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
var listMode = args[0] == "list";
if (listMode)
{
    args = args[1..];
}

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
    Yaw = 0f,
};
var cmi = Cmi.Load(data.PathOf($"TRAVERSE/LEVEL{level}/LEVEL{level}.CMI"));
var groups = new TriangleGroups();
foreach (var arena in levelData.Arenas)
{
    groups.Add(arena);
}

var runtime = new ScriptRuntime(levelData, cmi, sprites, space, groups, mixer, kurt, 1) { Option = 1 };

string B(float v) => BitConverter.SingleToUInt32Bits(v).ToString("X8");
string V(Vector3 v) => $"{B(v.X)},{B(v.Y)},{B(v.Z)}";
float H(string hex) => BitConverter.UInt32BitsToSingle(uint.Parse(hex, NumberStyles.HexNumber));

uint Fnv(byte[] bytes)
{
    var hash = 2166136261u;
    foreach (var b in bytes)
    {
        hash = (hash ^ b) * 16777619u;
    }

    return hash;
}

// Every texture of the level, copied as it loaded (the level's archive, then each arena's).
var copies = new Dictionary<Texture, byte[]>(ReferenceEqualityComparer.Instance);
foreach (var archive in new[] { levelData.LevelTextures }.Concat(levelData.Arenas.Select(a => a.Textures)))
{
    foreach (var texture in archive.Textures.Values)
    {
        copies.TryAdd(texture, (byte[])texture.Indices.Clone());
    }
}

var lines = new List<string>();
runtime.TextureStamped += texture =>
{
    var before = copies[texture];
    var after = texture.Indices;
    int x0 = int.MaxValue, y0 = int.MaxValue, x1 = -1, y1 = -1, changed = 0;
    for (var i = 0; i < after.Length; i++)
    {
        if (after[i] == before[i])
        {
            continue;
        }

        changed++;
        var x = i % texture.Width;
        var y = i / texture.Width;
        x0 = Math.Min(x0, x);
        y0 = Math.Min(y0, y);
        x1 = Math.Max(x1, x);
        y1 = Math.Max(y1, y);
    }

    lines.Add($"  stamp tex={texture.Name} {texture.Width}x{texture.Height}x{texture.FrameCount} before={Fnv(before):X8} " +
        $"after={Fnv(after):X8} rect={(changed == 0 ? "-" : $"{x0},{y0},{x1},{y1}")} n={changed}");
    copies[texture] = (byte[])after.Clone();
};

// Objects are numbered in the order they are first seen in the list (creation order), as the
// rounds probe does.
var ids = new Dictionary<MdkObject, int>(ReferenceEqualityComparer.Instance);
int Id(MdkObject? obj)
{
    if (obj == null)
    {
        return -1;
    }

    if (!ids.TryGetValue(obj, out var id))
    {
        id = ids.Count;
        ids[obj] = id;
    }

    return id;
}

var keys = new Dictionary<string, Key>
{
    ["snipe"] = Key.Sniper, ["fire"] = Key.Fire, ["next"] = Key.ItemNext, ["prev"] = Key.ItemPrevious,
    ["zin"] = Key.ZoomIn, ["zout"] = Key.ZoomOut, ["left"] = Key.TurnLeft, ["right"] = Key.TurnRight,
    ["up"] = Key.Forward, ["down"] = Key.Back,
};
var holds = new List<(Key Key, int From, int To)>();
var gives = new List<(int Step, string Model)>();
var aims = new List<(int Step, float Yaw, float Pitch)>();
var teleports = new List<(int Step, string Arena, Vector3 At)>();
var healths = new List<(int Step, int Id, int Health)>();
var options = new List<(int Step, int Option)>();
var stampAlls = new List<(int Step, int Id)>();
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    switch (parts[0])
    {
        case "give":
            gives.Add((int.Parse(parts[1]), parts[2]));
            continue;
        case "aim":
            aims.Add((int.Parse(parts[1]), H(parts[2]), H(parts[3])));
            continue;
        case "hp":
            healths.Add((int.Parse(parts[1]), int.Parse(parts[2]), int.Parse(parts[3])));
            continue;
        case "tp":
            teleports.Add((int.Parse(parts[1]), parts[2], new Vector3(float.Parse(parts[3]), float.Parse(parts[4]), float.Parse(parts[5]))));
            continue;
        case "opt":
            options.Add((int.Parse(parts[1]), int.Parse(parts[2])));
            continue;
        case "stampall":
            stampAlls.Add((int.Parse(parts[1]), int.Parse(parts[2])));
            continue;
    }

    var range = parts[1].Split('-');
    holds.Add((keys[parts[0]], int.Parse(range[0]), int.Parse(range[1])));
}

var seen = new Dictionary<MdkObject, string>(ReferenceEqualityComparer.Instance);
var input = new Input();
var lastShots = -1;
var lastHits = -1;
if (!listMode)
{
    Console.WriteLine($"level={level} steps={steps} seed=1 schedule={schedule}");
}

for (var step = 1; step <= steps; step++)
{
    foreach (var (s, model) in gives)
    {
        if (s == step)
        {
            kurt.Collect(model);
        }
    }

    foreach (var (s, oid, health) in healths)
    {
        if (s == step)
        {
            ids.First(kv => kv.Value == oid).Key.Health = health;
        }
    }

    foreach (var (s, arena, at) in teleports)
    {
        if (s == step)
        {
            runtime.TeleportKurt(arena, at, 0f);
        }
    }

    foreach (var (s, yaw, pitch) in aims)
    {
        if (s == step)
        {
            kurt.Yaw = yaw;
            kurt.Scope.Pitch = pitch;
        }
    }

    foreach (var (s, option) in options)
    {
        if (s == step)
        {
            runtime.Option = option;
        }
    }

    foreach (var (s, oid) in stampAlls)
    {
        if (s != step)
        {
            continue;
        }

        var obj = ids.First(kv => kv.Value == oid).Key;
        for (var part = 0; part < (obj.Model?.PartList.Count ?? 0); part++)
        {
            obj.ShotPart = part + 1;
            obj.ShotPoint = obj.PartCenter(part);
            lines.Add($"  direct #{oid} {obj.TypeName} part={part} pt={V(obj.ShotPoint)}");
            runtime.StampBulletHole(obj);
        }
    }

    foreach (var key in holds.Select(h => h.Key).Distinct())
    {
        var down = holds.Any(h => h.Key == key && step >= h.From && step < h.To);
        input.Hold(key, down ? Input.State.Down : Input.State.Up);
    }

    kurt.Update(input, 1f / 60f);
    runtime.Update(1f / 60f);

    // Number every object the step made, in list order.
    foreach (var obj in runtime.Objects)
    {
        Id(obj);
    }

    if (listMode)
    {
        continue;
    }

    var stats = runtime.Stats;
    var changes = new List<string>();
    foreach (var obj in runtime.Objects)
    {
        var id = Id(obj);
        var hitText = $"hp={obj.Health} ht={obj.HitType} sp={obj.ShotPart} pt={V(obj.ShotPoint)}";
        if (!seen.TryGetValue(obj, out var before))
        {
            seen[obj] = hitText;
            continue;
        }

        if (before != hitText)
        {
            seen[obj] = hitText;
            changes.Add($"  #{id} {obj.TypeName} {hitText} pos={V(obj.Position)} yaw={B(obj.Yaw)}");
        }
    }

    if (stats.SniperShots != lastShots || stats.SniperHits != lastHits || lines.Count > 0 || changes.Count > 0)
    {
        Console.WriteLine($"s={step} arena={runtime.CurrentArena} snipe={(kurt.Sniping ? 1 : 0)} shots={stats.SniperShots} hits={stats.SniperHits} objs={runtime.Objects.Count}");
        lastShots = stats.SniperShots;
        lastHits = stats.SniperHits;
        foreach (var line in lines.Concat(changes))
        {
            Console.WriteLine(line);
        }

        lines.Clear();
    }
}

if (listMode)
{
    var eye = kurt.SniperEye;
    Console.WriteLine($"level={level} arena={runtime.CurrentArena} feet={kurt.Feet} eye={eye}");
    foreach (var obj in runtime.Objects)
    {
        var id = Id(obj);
        if (obj.Dead || obj.Model == null)
        {
            continue;
        }

        var wb = runtime.GetWorldBounds(obj);
        var c = (wb.Min + wb.Max) * 0.5f;
        var d = c - eye;
        var yaw = float.RadiansToDegrees(MathF.Atan2(d.Y, d.X));
        yaw = (yaw % 360f + 360f) % 360f;
        var pitch = -float.RadiansToDegrees(MathF.Atan2(d.Z, new Vector2(d.X, d.Y).Length()));
        var clear = runtime.Raycast(eye, c) == null;
        Console.WriteLine($"#{id} {obj.TypeName} restart={obj.Restart} arena={obj.Arena} parts={obj.Model.PartList.Count} pos={obj.Position} c={c} dist={d.Length():F1} " +
            $"yaw={yaw:F3}/{B(yaw)} pitch={pitch:F3}/{B(pitch)} clear={clear}");
    }
}
