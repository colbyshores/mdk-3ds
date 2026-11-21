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

// THE AIR STRIKE ORACLE: the C# Kurt and ScriptRuntime (src/Mdk.Game) stepped together at 1/60 s,
// Kurt then the scripts, as Viewer.RunSteps does -- and, as Viewer.Update does (Viewer.cs:396-410),
// while the full-screen strike (ScriptRuntime.Strike) is Active the game WAITS: the step is the
// scene's StrikeScene.Update(1/60 s) instead (Esc: Skip), and a step on which the scene ends runs
// the game too. It pins Scripts/AirStrike.cs (FindTarget, Call, Start's five points and the curve
// by length, the teeth, the dive, its blasts), Scripts/StrikeScene.cs (Play, Update, Skip, the
// frame, the turn, the camera on the model's reference points, the plane's parts), and
// ScriptRuntime's PlayStrikeScene and FireSniper / SpecialEvent 51's strike half. Its output is
// tests/scripts/golden/L*_strike_*.txt, and the C++ twin is `tick1 dump strike <level> <steps>
// <schedule>` (tests/scripts/tick1.cpp, strikeDump), which must print the same bytes.
//
//   Probe <level> <steps> <schedule>
//
// The schedule is comma-separated:
//   at:x:y:z:yaw     Kurt starts there instead of the level's start (the C#'s --at);
//   give:step:MODEL  hands Kurt a pickup before that step (Kurt.Collect: SW_BONES is the strike);
//   aim:step:YAW:PITCH  Kurt's yaw and the scope's pitch (float bits, hex) before that step;
//   key:from-to      a key held over game steps [from, to) (snipe, fire, next, prev, zin, zout);
//   ev:step:value    special_event `value` for the controller of Kurt's arena before that step;
//   used:step        AirStrike.UsedUp = true before that step (the saved game's StrikeUsed);
//   skip:step        Esc on that step while the scene plays (StrikeScene.Skip);
//   show:step        print from that step on.
//
// Lines. A game step: `s=` Kurt (feet in 1/64, sniping, selected ammo, the strike's count, the
// clip, health), the shots, the objects' count, then `strike=` AirStrike's state (IsActive,
// UsedUp, the curve's length, the offset, the target and dive offsets, the teeth dropped,
// diving, the X_STRIKE's position, yaw, velocity, flags, contact), then `scene=` the last
// StrikeScene (Active, frame, yaw, the subject's type, frame and hidden parts, the camera's eye
// and target), all IEEE-754 bit patterns. A waiting step: `w=` and the scene alone. Then the
// objects whose health, hit type or hit event changed (`#id`), and the X_TOOTH grenades (`t#id`).
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/rounds_probe -- copy
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

string B(float v) => BitConverter.SingleToUInt32Bits(v).ToString("X8");
string V(Vector3 v) => $"{B(v.X)},{B(v.Y)},{B(v.Z)}";
float H(string hex) => BitConverter.UInt32BitsToSingle(uint.Parse(hex, NumberStyles.HexNumber));
int P(float v) => (int)MathF.Floor(v * 64f);

var keys = new Dictionary<string, Key>
{
    ["snipe"] = Key.Sniper, ["fire"] = Key.Fire, ["next"] = Key.ItemNext, ["prev"] = Key.ItemPrevious,
    ["zin"] = Key.ZoomIn, ["zout"] = Key.ZoomOut,
};
var holds = new List<(Key Key, int From, int To)>();
var gives = new List<(int Step, string Model)>();
var aims = new List<(int Step, float Yaw, float Pitch)>();
var events = new List<(int Step, int Value)>();
var used = new List<int>();
var skips = new List<int>();
var feet = levelData.Dti.StartPosition;
var startYaw = levelData.Dti.StartAngle;
var show = 1;
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    switch (parts[0])
    {
        case "at":
            feet = new Vector3(float.Parse(parts[1]), float.Parse(parts[2]), float.Parse(parts[3]));
            startYaw = float.Parse(parts[4]);
            continue;
        case "give":
            gives.Add((int.Parse(parts[1]), parts[2]));
            continue;
        case "aim":
            aims.Add((int.Parse(parts[1]), H(parts[2]), H(parts[3])));
            continue;
        case "ev":
            events.Add((int.Parse(parts[1]), int.Parse(parts[2])));
            continue;
        case "used":
            used.Add(int.Parse(parts[1]));
            continue;
        case "skip":
            skips.Add(int.Parse(parts[1]));
            continue;
        case "show":
            show = int.Parse(parts[1]);
            continue;
    }

    var range = parts[1].Split('-');
    holds.Add((keys[parts[0]], int.Parse(range[0]), int.Parse(range[1])));
}

var kurt = new KurtT(space, mixer, FrameCount) { Feet = feet, Yaw = startYaw };
var cmi = Cmi.Load(data.PathOf($"TRAVERSE/LEVEL{level}/LEVEL{level}.CMI"));
var groups = new TriangleGroups();
foreach (var arena in levelData.Arenas)
{
    groups.Add(arena);
}

var runtime = new ScriptRuntime(levelData, cmi, sprites, space, groups, mixer, kurt, 1) { Option = 1 };

// Objects are numbered in the order they are first seen in the list (creation order), as the
// rounds probe does: the C++ deque keeps removed objects in their slots.
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

// AirStrike's and StrikeScene's state is private: read by reflection.
var flags = BindingFlags.NonPublic | BindingFlags.Instance;
object? F(object o, string name) => o.GetType().GetField(name, flags)!.GetValue(o);
var strikeObj = typeof(AirStrike);

string StrikeLine()
{
    var air = runtime.AirStrike;
    var strike = (MdkObject?)F(air, "_strike");
    var curve = (BezierPath?)F(air, "_curve");
    var text = $"strike={(air.IsActive() ? 1 : 0)} used={(air.UsedUp ? 1 : 0)} len={(curve == null ? "-" : B(curve.Length))} " +
        $"off={B((float)F(air, "_offset")!)} toff={B((float)F(air, "_targetOffset")!)} doff={B((float)F(air, "_diveOffset")!)} " +
        $"teeth={(int)F(air, "_teeth")!} diving={((bool)F(air, "_diving")! ? 1 : 0)}";
    if (strike != null)
    {
        text += $" x#{Id(strike)}={strike.TypeName}:{(strike.Dead ? "dead" : "live")}:{V(strike.Position)}:{B(strike.Yaw)}:" +
            $"{V(strike.Velocity)}:{strike.Flags:X}:{strike.ContactFlags:X}:{strike.Health}";
    }

    return text;
}

string SceneLine()
{
    if (runtime.Strike is not { } scene)
    {
        return "scene=-";
    }

    var (eye, target) = scene.Camera;
    var subject = scene.Subject;
    return $"scene={(scene.Active ? 1 : 0)} f={B((float)F(scene, "_frame")!)} y={B((float)F(scene, "_yaw")!)} " +
        $"sub={(subject.TypeName.Length == 0 ? "-" : subject.TypeName)}:{subject.AnimationFrame}:{subject.HiddenParts:X}:{B(subject.Yaw)} " +
        $"eye={V(eye)} at={V(target)}";
}

// SniperView.Aim's test (SniperView.cs:81-88, the overlay's strike-target bit): the point
// AirStrike.FindTarget finds from the sniper eye along the scope's line, or `-`.
string Target() => runtime.AirStrike.FindTarget(kurt.SniperEye, kurt.SniperForward) is { } t ? V(t) : "-";

if (Environment.GetEnvironmentVariable("STRIKE_HUNT") != null)
{
    // A search aid (not a golden): the scripts that test if_hit_weapon -6 (the dive's blast).
    var decoder = new Mdk.Formats.Scripts.ScriptDecoder(cmi.Bytes);
    var starts = new List<(int Offset, string Name)>();
    foreach (var (name, offset) in cmi.ObjectScripts) starts.Add((offset, "obj " + name));
    foreach (var (name, offset) in cmi.ArenaScripts) starts.Add((offset, "arena " + name));
    foreach (var (name, offset) in cmi.AlienScripts) starts.Add((offset, "alien " + name));
    decoder.Preload(starts.Select(s => s.Offset).Where(o => o > 0));
    foreach (var ins in decoder.Decoded.Where(i => i.Opcode is 195 or 131).OrderBy(i => i.Pc))
    {
        var owner = starts.Where(s => s.Offset <= ins.Pc).OrderByDescending(s => s.Offset).FirstOrDefault();
        Console.WriteLine($"{ins.Pc:X} op={ins.Opcode} {string.Join(" ", ins.Operands.Select(o => o?.ToString()))} in {owner.Name}@{owner.Offset:X}");
    }

    foreach (var a in levelData.Arenas.Where(a => a.Vertices.Length > 0))
    {
        Console.WriteLine($"{a.Name} min=<{a.Vertices.Min(v => v.X)},{a.Vertices.Min(v => v.Y)},{a.Vertices.Min(v => v.Z)}> " +
            $"max=<{a.Vertices.Max(v => v.X)},{a.Vertices.Max(v => v.Y)},{a.Vertices.Max(v => v.Z)}>");
    }

    return;
}

if (Environment.GetEnvironmentVariable("STRIKE_SCAN") is { } scan)
{
    // A search aid (not a golden): after idle steps, the yaws and pitches whose line finds a target.
    var idle = new Input();
    for (var step = 1; step <= steps; step++)
    {
        kurt.Update(idle, 1f / 60f);
        runtime.Update(1f / 60f);
    }

    Console.WriteLine($"arena={runtime.CurrentArena} feet={kurt.Feet}");
    foreach (var obj in runtime.Objects.Where(o => !o.Dead && o.Arena == runtime.CurrentArena))
    {
        var wb = runtime.GetWorldBounds(obj);
        var c = (wb.Min + wb.Max) * 0.5f;
        var d = c - kurt.SniperEye;
        Console.WriteLine($"  {obj.TypeName} hp={obj.Health} pos={obj.Position} c={c} yaw={float.RadiansToDegrees(MathF.Atan2(d.Y, d.X)):F2} " +
            $"pitch={-float.RadiansToDegrees(MathF.Atan2(d.Z, new Vector2(d.X, d.Y).Length())):F2}");
    }

    for (var y = 0; y < 360; y += int.Parse(scan))
    {
        var pitchStep = float.Parse(Environment.GetEnvironmentVariable("STRIKE_PSTEP") ?? "5");
        for (var p = -30f; p <= 80f; p += pitchStep)
        {
            kurt.Scope.Pitch = p;
            kurt.Yaw = y;
            if (runtime.AirStrike.FindTarget(kurt.SniperEye, kurt.SniperForward) is { } t)
            {
                Console.WriteLine($"yaw={y} pitch={p} -> {t} dist={(t - kurt.SniperEye).Length():F1}");
            }
        }
    }

    return;
}

var seen = new Dictionary<MdkObject, string>(ReferenceEqualityComparer.Instance);
var input = new Input();
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

    foreach (var (s, yaw, pitch) in aims)
    {
        if (s == step)
        {
            kurt.Yaw = yaw;
            kurt.Scope.Pitch = pitch;
        }
    }

    foreach (var s in used)
    {
        if (s == step)
        {
            runtime.AirStrike.UsedUp = true;
        }
    }

    foreach (var (s, value) in events)
    {
        if (s == step)
        {
            runtime.SpecialEvent(runtime.GetArenaState(runtime.CurrentArena).Controller, value);
        }
    }

    // Viewer.Update (Viewer.cs:396-410) through SniperView.DrawStrike (SniperView.cs:167-190): the
    // scene advances (or Esc skips it) and the game waits while it plays.
    if (runtime.Strike is { Active: true } scene)
    {
        if (skips.Contains(step))
        {
            scene.Skip();
        }
        else
        {
            scene.Update(1f / 60f);
        }

        if (scene.Active)
        {
            if (step >= show)
            {
                Console.WriteLine($"w={step} {SceneLine()}");
            }

            continue;
        }
    }

    foreach (var key in keys.Values)
    {
        var down = holds.Any(h => h.Key == key && step >= h.From && step < h.To);
        input.Hold(key, down ? Input.State.Down : Input.State.Up);
    }

    kurt.Update(input, 1f / 60f);
    runtime.Update(1f / 60f);
    if (step < show)
    {
        continue;
    }

    var stats = runtime.Stats;
    var live = 0;
    foreach (var obj in runtime.Objects)
    {
        Id(obj);
        if (!obj.Dead)
        {
            live++;
        }
    }

    Console.WriteLine(
        $"s={step} feet64={P(kurt.Feet.X)},{P(kurt.Feet.Y)},{P(kurt.Feet.Z)} snipe={(kurt.Sniping ? 1 : 0)} " +
        $"sel={kurt.Inventory.SelectedAmmo} ammo5={kurt.Inventory.Ammo[4]} clip={kurt.Scope.ClipRounds}/{B(kurt.Scope.ClipTime)} " +
        $"hp={kurt.Health} shots={stats.SniperShots} kills={stats.Kills} objs={live} effects={runtime.Effects.All.Count} " +
        $"cut={runtime.Cutscene:x} arena={runtime.CurrentArena} tgt={Target()} {StrikeLine()} {SceneLine()}");

    foreach (var obj in runtime.Objects)
    {
        var id = Id(obj);
        // A tooth from its drop on, while it changes; any other object when its hit state does.
        var tooth = obj.TypeName == "X_TOOTH";
        var text = tooth
            ? $"{(obj.Dead ? "dead" : "live")} p={V(obj.Position)} v={V(obj.Velocity)} f={obj.Flags:X} k={obj.ThrownKind} " +
              $"it={obj.ItemTicks} c={obj.ContactFlags:X}"
            : $"hp={obj.Health} ev={obj.HitEvent} ht={obj.HitType} dead={(obj.Dead ? 1 : 0)}";
        var known = seen.TryGetValue(obj, out var before);
        if (known && before == text)
        {
            continue;
        }

        seen[obj] = text;
        if (tooth)
        {
            Console.WriteLine($"  t#{id} {text}");
        }
        else if (known)
        {
            Console.WriteLine($"  #{id} {obj.TypeName} {text} pos={V(obj.Position)}");
        }
    }
}
