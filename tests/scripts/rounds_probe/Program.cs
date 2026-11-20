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

// THE SNIPER ROUNDS ORACLE: the C# Kurt and ScriptRuntime (src/Mdk.Game) stepped together at
// 1/60 s, Kurt then the scripts as Viewer.RunSteps does, under a scripted input that enters sniper
// mode, aims and fires. It pins Scripts/SniperRounds.cs (the five round types: flight, homing,
// the mortar's drag, gravity and bounces, bomb_follow_path), ScriptRuntime.FireSniper and
// UpdateSniperTarget (the lock, Scope.ZoomLimit), and what a round does to what it hits: the
// object's hit_event / hit_type / hit_direction / shot_part / shot_point, its health and the
// kill, the explosions' blasts, the walls' groups and sparks. Its output is
// tests/scripts/golden/L*_rounds_*.txt, and the C++ twin is `tick1 dump rounds <level> <steps>
// <schedule>` (tests/scripts/tick1.cpp, kurtRoundsDump), which must print the same bytes.
//
//   Probe <level> <steps> <schedule>
//   Probe list <level> <steps>            the candidate targets after <steps> idle steps
//
// The schedule is comma-separated: `key:from-to` holds a key over steps [from, to) (keys snipe,
// fire, next, prev, zin, zout, left, right, up, down), `give:step:MODEL` hands Kurt a pickup
// before that step (Kurt.Collect, as the console's give), `aim:step:YAW:PITCH` sets Kurt's yaw
// and the scope's pitch (float bits, hex) before that step, and `tp:step:ARENA:x:y:z` teleports
// Kurt there facing yaw 0 (ScriptRuntime.TeleportKurt, as the level console's teleport), and
// `hp:step:ID:HEALTH` sets the health of object ID (numbered as printed, below) before that
// step, so a single round can reach the kill. The goldens' schedules are in
// tick1.cpp's kRoundsCases table.
//
// YAW, PITCH AND EXACTNESS. Kurt stays at the level's start. The port's Player::facing() is the
// trig LUT (docs/OPEN_ITEMS.md, owner-gated) and it reaches the sniper eye only while the scope
// looks DOWN (Scope.Eye: the forward shift for a positive pitch), so every aim here keeps the
// pitch at or above level; the round's own line (Scope.Direction) is libm on both sides, so any
// yaw is exact.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/fire_probe -- copy
// src/Mdk.Formats, src/Mdk.Engine, src/Mdk.Game and Directory.Build.props into a scratch tree with
// this directory beside src/; in the scratch Mdk.Engine.csproj drop the shader targets and the
// icon resource; `dotnet build -c Release -o probe-bin`, then
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

// Objects are numbered in the order they are first seen in the list (creation order): the C++
// deque keeps removed objects in their slots, the C# list drops them, so a list index is not
// shared but this number is.
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

var input = new Input();
if (listMode)
{
    for (var step = 1; step <= steps; step++)
    {
        kurt.Update(input, 1f / 60f);
        runtime.Update(1f / 60f);
    }

    var eye = levelData.Dti.StartPosition + new Vector3(0f, 0f, 4f);
    Console.WriteLine($"level={level} arena={runtime.CurrentArena} eye={eye}");
    foreach (var obj in runtime.Objects)
    {
        var id = Id(obj);
        if (obj.Dead || obj.Arena != runtime.CurrentArena || obj.Health == 0 || (obj.Flags & 0x30) != 0)
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
        Console.WriteLine($"#{id} {obj.TypeName} hp={obj.Health} flags={obj.Flags:X} c={c} dist={d.Length():F1} yaw={yaw:F3}/{B(yaw)} pitch={pitch:F3}/{B(pitch)} clear={clear}");
    }

    return;
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
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    if (parts[0] == "give")
    {
        gives.Add((int.Parse(parts[1]), parts[2]));
        continue;
    }

    if (parts[0] == "aim")
    {
        aims.Add((int.Parse(parts[1]), H(parts[2]), H(parts[3])));
        continue;
    }

    if (parts[0] == "hp")
    {
        healths.Add((int.Parse(parts[1]), int.Parse(parts[2]), int.Parse(parts[3])));
        continue;
    }

    if (parts[0] == "tp")
    {
        teleports.Add((int.Parse(parts[1]), parts[2], new Vector3(float.Parse(parts[3]), float.Parse(parts[4]), float.Parse(parts[5]))));
        continue;
    }

    var range = parts[1].Split('-');
    holds.Add((keys[parts[0]], int.Parse(range[0]), int.Parse(range[1])));
}

// The rounds are private (SniperRounds._rounds, the Round class): read by reflection.
var flags = BindingFlags.NonPublic | BindingFlags.Instance;
var roundsField = typeof(SniperRounds).GetField("_rounds", flags)!;
var roundType = typeof(SniperRounds).GetNestedType("Round", BindingFlags.NonPublic)!;
object? Get(object o, string name) => roundType.GetField(name)!.GetValue(o);
float Fl(object o, string name) => (float)Get(o, name)!;

uint Fnv(uint hash, float v)
{
    var bits = BitConverter.SingleToUInt32Bits(v);
    for (var i = 0; i < 4; i++)
    {
        hash = (hash ^ ((bits >> (8 * i)) & 0xFF)) * 16777619u;
    }

    return hash;
}

var seen = new Dictionary<MdkObject, string>(ReferenceEqualityComparer.Instance);
Console.WriteLine($"level={level} steps={steps} seed=1 schedule={schedule}");
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

    foreach (var key in holds.Select(h => h.Key).Distinct())
    {
        var down = holds.Any(h => h.Key == key && step >= h.From && step < h.To);
        input.Hold(key, down ? Input.State.Down : Input.State.Up);
    }

    kurt.Update(input, 1f / 60f);
    runtime.Update(1f / 60f);

    var scope = kurt.Scope;
    var stats = runtime.Stats;
    Console.WriteLine($"s={step} snipe={(kurt.Sniping ? 1 : 0)} yaw={B(kurt.Yaw)} pitch={B(scope.Pitch)} zoom={B(scope.Zoom)} " +
        $"limit={B(scope.ZoomLimit)} clip={scope.ClipRounds}/{B(scope.ClipTime)} sel={kurt.Inventory.SelectedAmmo} " +
        $"ammo={string.Join(",", kurt.Inventory.Ammo)} target={Id(runtime.SniperTarget)} hp={kurt.Health} " +
        $"shots={stats.SniperShots} hits={stats.SniperHits} kills={stats.Kills} objs={runtime.Objects.Count} " +
        $"effects={runtime.Effects.All.Count} pieces={runtime.Debris.Pieces.Count}");
    var rounds = (Array)roundsField.GetValue(runtime.SniperRounds)!;
    for (var i = 0; i < rounds.Length; i++)
    {
        var r = rounds.GetValue(i)!;
        var state = Get(r, "State")!.ToString();
        if (state == "Free")
        {
            continue;
        }

        var visual = (MdkObject?)Get(r, "Visual");
        var basis = 2166136261u;
        if (visual != null)
        {
            var m = visual.RollingBasis;
            foreach (var v in new[] { m.M11, m.M12, m.M13, m.M14, m.M21, m.M22, m.M23, m.M24, m.M31, m.M32, m.M33, m.M34, m.M41, m.M42, m.M43, m.M44 })
            {
                basis = Fnv(basis, v);
            }
        }

        Console.WriteLine($"  r{i} {state} t={(int)Get(r, "Type")!} p={V((Vector3)Get(r, "Position")!)} yaw={B(Fl(r, "Yaw"))} " +
            $"pitch={B(Fl(r, "Pitch"))} roll={B(Fl(r, "Roll"))} v={B(Fl(r, "Speed"))},{B(Fl(r, "Vertical"))} " +
            $"life={B(Fl(r, "Life"))} linger={B(Fl(r, "Linger"))} tgt={Id((MdkObject?)Get(r, "Target"))}/{(int)Get(r, "TargetPart")!} " +
            $"yr={B(Fl(r, "YawRate"))} path={(int)Get(r, "Path")!}/{B(Fl(r, "PathTime"))} " +
            $"cam={B(Fl(r, "CameraDistance"))}:{V((Vector3)Get(r, "CameraPosition")!)} watch={B(Fl(r, "Watch"))} " +
            $"vis={(visual == null ? "-" : visual.TypeName + ":" + V(visual.Position) + ":" + basis.ToString("X8"))}");
    }

    // The objects whose hit state changed this step.
    foreach (var obj in runtime.Objects)
    {
        var id = Id(obj);
        var text = $"hp={obj.Health} ev={obj.HitEvent} ht={obj.HitType} hd={B(obj.HitDirection)} sp={obj.ShotPart} " +
            $"pt={V(obj.ShotPoint)} pos={V(obj.Position)}";
        var hitText = $"hp={obj.Health} ht={obj.HitType} hd={B(obj.HitDirection)} sp={obj.ShotPart} pt={V(obj.ShotPoint)}";
        if (!seen.TryGetValue(obj, out var before))
        {
            seen[obj] = hitText;
            continue;
        }

        if (before != hitText)
        {
            seen[obj] = hitText;
            Console.WriteLine($"  #{id} {obj.TypeName} {text}");
        }
    }
}
