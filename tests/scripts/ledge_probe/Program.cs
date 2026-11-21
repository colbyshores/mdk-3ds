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
using Mdk.Game.Scripts;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE LEDGE ORACLE: the C# Kurt, ScriptRuntime and FollowCamera (src/Mdk.Game) stepped together at
// 1/60 s as Viewer.RunSteps does -- Kurt, the scripts, ArenaSpace.SetSolid(SolidArenas), then the
// camera -- under a scripted input. It pins package 12 (docs/PORT_GAPS.md): Kurt.Ledge.cs
// (GrabLedge, HangFrom, UpdateClimb) and the ArenaSpace probes it stands on
// (ArenaSpace.Probes.cs: FloorCrossing, Edge -> Bsp.Edge, Free). Per step it prints Kurt's arena,
// state, feet, yaw, his three speeds, floor/chute flags, the animation frame and the climb's tick
// counter, his health, and the follow camera's climb, air pitch and arena pitch (the camera's
// position is not printed: the port's view trig is the LUT, which matches MathF only at the
// angles the movement goldens pin). Its output is tests/scripts/golden/L*_ledge_*.txt, and the
// C++ twin is `tick1 dump ledge <level> <steps> <schedule>` (tests/scripts/tick1.cpp,
// kurtLedgeDump), which must print the same bytes.
//
//   Probe <level> <steps> <schedule>
//   Probe scan <level> [arena|-] [any]   (searches for ledges Kurt grabs; not a golden)
//
// The schedule is comma-separated: `key:from-to` holds a key over steps [from, to) (fwd back left
// right sleft sright jump turbo fire), and `tp:step:ARENA:x:y:z:yaw` calls
// ScriptRuntime.TeleportKurt before that step. The goldens' schedules are in tick1.cpp's
// kLedgeCases table.
//
// THE SCAN. Every up-facing triangle (|n.z| >= 0.85) of every reachable arena (or of the one
// named), every level edge of it that runs exactly along +Y with the top towards +X (so the grab
// turns Kurt to a yaw of exactly 0, where the port's LUT facing equals MathF's) -- or, with
// `any`, every level edge at least 3 long, Kurt faced square at it (the 3DS harness walk, where
// no golden needs the yaw): Kurt is put on the floor under a point a few units before the edge's
// middle, runs at it and jumps (several distances and jump steps when an arena is named); each
// try prints where he started, the step he hangs on and the step the climb ends (-1 for none),
// and when no grab happened the first refusal of GrabLedge's chain. Kurt alone is stepped (no
// scripts), in that arena.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/items_probe -- copy
// src/Mdk.Formats, src/Mdk.Engine, src/Mdk.Game and Directory.Build.props into a scratch tree with
// this directory beside src/; in the scratch Mdk.Engine.csproj drop the shader targets and the
// icon resource; `dotnet build -c Release -o probe-bin`, then
// `DOTNET_ROOT=<sdk> MDK_DATA_DIR=<the MDK folder> probe-bin/Probe ...`.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
var scan = args[0] == "scan";
var level = int.Parse(scan ? args[1] : args[0]);
var steps = scan ? 0 : int.Parse(args[1]);
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
int P(float v) => (int)MathF.Floor(v * 64f);
string F(float v) => v.ToString("F4", CultureInfo.InvariantCulture);
int B(bool b) => b ? 1 : 0;
var climbField = typeof(KurtT).GetField("_climbTicks", BindingFlags.NonPublic | BindingFlags.Instance)!;
float Private(object o, string name) => (float)o.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(o)!;
var start = levelData.Dti.Arenas[levelData.Dti.StartArena].Name;

// The scan's diagnosis: GrabLedge's chain replayed through the public probes, the first link
// that refused.
string Why(Vector3 before, KurtT kurt)
{
    var hands = new Vector3(0f, 0f, 4.6041665f);
    for (var i = 1; i <= 3; i++)
    {
        var ahead = kurt.Facing * i;
        if (space.FloorCrossing(before + hands + ahead, kurt.Feet + hands + ahead) is not { } top)
        {
            continue;
        }

        if (MathF.Abs(top.Normal.Z) < 0.85f)
        {
            return $"steep{i}";
        }

        if (ArenaSpace.Edge(top, top.Point - ahead) is not var (edge, direction))
        {
            return $"noedge{i}@{F(top.Point.X)},{F(top.Point.Y)},{F(top.Point.Z)}";
        }

        var yaw = float.RadiansToDegrees(MathF.Atan2(direction.Y, direction.X)) - 90f;
        var room = edge + new Vector3(0f, 0f, 2.5f);
        var free = space.Free(room - kurt.Facing, room + kurt.Facing * 0.5f, new Vector3(0.5f, 0.5f, 2f));
        return $"top{i} yaw={yaw:R} free={B(free)} edge={F(edge.X)},{F(edge.Y)},{F(edge.Z)}";
    }

    return "";
}

if (scan)
{
    // Kurt alone, in each arena in turn: run at the edge from 2 units before it and jump.
    var input = new Input();
    var only = args.Length > 2 && args[2] != "-" ? args[2] : null;
    var any = args.Length > 3 && args[3] == "any";
    foreach (var arena in levelData.Arenas.Where(a => levelData.IsReachable(a.Name) && (only == null || a.Name == only)))
    {
        var seen = new HashSet<(float, float, float, float)>();
        for (var t = 0; t < arena.TriangleIndices.Length / 3; t++)
        {
            var v = new Vector3[3];
            for (var k = 0; k < 3; k++)
            {
                v[k] = arena.Vertices[arena.TriangleIndices[t * 3 + k]];
            }

            var n = Vector3.Normalize(Vector3.Cross(v[1] - v[0], v[2] - v[0]));
            if (MathF.Abs(n.Z) < 0.85f)
            {
                continue;
            }

            for (var k = 0; k < 3; k++)
            {
                var a = v[k];
                var b = v[(k + 1) % 3];
                var c = v[(k + 2) % 3];
                // Into the top, square to the edge (XY): at yaw 0 only edges along Y with the top
                // towards +X; with `any`, every edge, Kurt faced at it (the 3DS harness's walk).
                var into = Vector3.Normalize(new Vector3(a.Y - b.Y, b.X - a.X, 0f));
                if (Vector3.Dot(into, c - a) < 0f)
                {
                    into = -into;
                }

                var along = any ? (b - a).Length() >= 3f
                    : a.X == b.X && MathF.Abs(b.Y - a.Y) >= 3f && c.X > a.X;
                if (!along || a.Z != b.Z || !seen.Add((MathF.Min(a.X, b.X) + MathF.Max(a.X, b.X) * 7f, MathF.Min(a.Y, b.Y), MathF.Max(a.Y, b.Y), a.Z)))
                {
                    continue;
                }

                var faceYaw = any ? float.RadiansToDegrees(MathF.Atan2(into.Y, into.X)) : 0f;
                var result = "";
                float[] distances = only == null ? [1.5f, 5f, 10f] : [1.5f, 3f, 5f, 7f, 10f, 14f, 20f];
                int[] jumps = only == null ? [2] : [1, 2, 6, 12];
                foreach (var distance in distances)
                foreach (var jump in jumps)
                {
                    var mid = (a + b) * 0.5f - into * distance - new Vector3(0f, 0f, 0.5f);
                    var down = mid - new Vector3(0f, 0f, 25f);
                    if (!space.Floor(mid, down, out var floor) || floor == down)
                    {
                        result += $" d{distance}:nofloor";
                        continue;
                    }

                    var height = a.Z - floor.Z;
                    if (height < 4f || height > 20f)
                    {
                        result += $" d{distance}:h{F(height)}";
                        continue;
                    }

                    space.SetSolid([arena.Name]);
                    var kurt = new KurtT(space, mixer, FrameCount);
                    kurt.Teleport(floor + new Vector3(0f, 0f, 0.1f), faceYaw);
                    var hung = -1;
                    var climbed = -1;
                    var why = "";
                    for (var step = 1; step <= 240; step++)
                    {
                        input.Hold(Key.Forward, step >= 10 && step < 60 ? Input.State.Down : Input.State.Up);
                        input.Hold(Key.Jump, step >= 10 + jump && step < 28 + jump ? Input.State.Down : Input.State.Up);
                        var before = kurt.Feet;
                        kurt.Update(input, 1f / 60f);
                        if (hung < 0 && why.Length == 0 && step < 60 && kurt.VerticalSpeed <= -0.25f && kurt.Current != KurtT.State.Hang)
                        {
                            why = Why(before, kurt);
                        }

                        if (hung < 0 && kurt.Current == KurtT.State.Hang)
                        {
                            hung = step;
                        }

                        if (hung > 0 && climbed < 0 && kurt.Current != KurtT.State.Hang)
                        {
                            climbed = step;
                        }
                    }

                    result += $" d{distance}j{jump}:at={F(floor.X)},{F(floor.Y)},{F(floor.Z)}:h{F(height)}:hang={hung}:climbed={climbed}:end={F(kurt.Feet.X)},{F(kurt.Feet.Y)},{F(kurt.Feet.Z)}:yaw={kurt.Yaw:R}:{kurt.Current}:{why}";
                }

                Console.WriteLine($"{arena.Name}{(arena.Name == start ? "*" : "")} edge={F(a.X)},{F(a.Y)}..{F(b.X)},{F(b.Y)},{F(a.Z)} face={F(faceYaw)}{result}");
            }
        }
    }

    return;
}

var kurtMain = new KurtT(space, mixer, FrameCount)
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

var runtime = new ScriptRuntime(levelData, cmi, sprites, space, groups, mixer, kurtMain, 1) { Option = 1 };
// Viewer.cs:237: the follow camera, with the space (its clearance) and no ClipView, as the end
// probe does.
var follow = new FollowCamera(space);
var keys = new Dictionary<string, Key>
{
    ["fwd"] = Key.Forward, ["back"] = Key.Back, ["left"] = Key.TurnLeft, ["right"] = Key.TurnRight,
    ["sleft"] = Key.StrafeLeft, ["sright"] = Key.StrafeRight, ["jump"] = Key.Jump, ["turbo"] = Key.Turbo,
    ["fire"] = Key.Fire,
};
var holds = new List<(Key Key, int From, int To)>();
var teleports = new List<(int Step, string Arena, Vector3 At, float Yaw)>();
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    if (parts[0] == "tp")
    {
        teleports.Add((int.Parse(parts[1]), parts[2],
            new Vector3(float.Parse(parts[3]), float.Parse(parts[4]), float.Parse(parts[5])), float.Parse(parts[6])));
        continue;
    }

    var range = parts[1].Split('-');
    holds.Add((keys[parts[0]], int.Parse(range[0]), int.Parse(range[1])));
}

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

var held = new Input();
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

    foreach (var key in keys.Values)
    {
        var down = holds.Any(h => h.Key == key && step >= h.From && step < h.To);
        held.Hold(key, down ? Input.State.Down : Input.State.Up);
    }

    // Viewer.RunSteps: Kurt, the scripts, the solid arenas, then the camera (PitchGoal:
    // camera_track, else the arena's rest pitch).
    kurtMain.Update(held, 1f / 60f);
    runtime.Update(1f / 60f);
    space.SetSolid(runtime.SolidArenas);
    var goal = runtime.CameraTrackTicks > 0 ? runtime.CameraTrackPitch
        : levelData.Dti.Arenas.FirstOrDefault(a => a.Name == runtime.CurrentArena)?.Pitch ?? 4f;
    follow.Update(kurtMain, goal, held, 1f / 60f);

    Console.WriteLine(
        $"s={step} arena={(runtime.CurrentArena.Length == 0 ? "-" : runtime.CurrentArena)} state={kurtMain.Current} " +
        $"feet64={P(kurtMain.Feet.X)},{P(kurtMain.Feet.Y)},{P(kurtMain.Feet.Z)} yaw={F(kurtMain.Yaw)} " +
        $"v={F(kurtMain.ForwardSpeed)},{F(kurtMain.StrafeSpeed)},{F(kurtMain.VerticalSpeed)} floor={B(kurtMain.OnFloor)} " +
        $"chute={B(kurtMain.ChuteOpen)} frame={F(kurtMain.AnimationFrame)} climb={(int)climbField.GetValue(kurtMain)!} " +
        $"hp={kurtMain.Health} cam={F(Private(follow, "_climb"))},{F(Private(follow, "_airPitch"))},{F(Private(follow, "_arenaPitch"))} " +
        $"voices={Voices()}");
}
