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

// THE KURT-HURT ORACLE: the C# Kurt and the ScriptRuntime bound to him, stepped at 1/60 s from
// the level's start (Kurt, then the scripts, as Viewer.RunSteps), under one of the fixed schedules
// of tick1.cpp's kurtHurtDump. Each schedule writes Kurt the way a script would, between his
// update and the runtime's: opcode 244 (Kurt.Invulnerable = s), Kurt.SetHealth(0), opcode 215
// (Kurt.WhiteFlash = max(WhiteFlash, v)), opcode 109 (ScriptRuntime.HurtKurt) and opcode 248
// (ScriptRuntime.PushKurt), in that order. Kurt and the runtime share one mixer, so voices= is
// that mixer's list.
//
//   Probe <schedule> <level> <steps>   schedule: hurt hurtland knock invuln knockdown flashes push camroll
//
// Its output is tests/scripts/golden/L3_kurt_<schedule>.txt (all level 3: hurt 220, hurtland 340,
// knock 340, invuln 340, knockdown 340, flashes 620, push 360, camroll 200 steps); the C++ twin is
// `tick1 dump <schedule> <level> <steps>`. The original driver's `hurt` mode was not kept; this one
// was rebuilt on 2026-11-26 from tick1.cpp's kurtHurtDump and reproduces all eight goldens byte for
// byte against the C# 1.1.1 (d5bb66ec). Built and run as the other probes (see kurt_probe).
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
var schedule = args[0];
var level = int.Parse(args[1]);
var steps = int.Parse(args[2]);

// The schedules: kurtHurtDump's literals (see the comments there for what each number is for).
var lift = 300f;
(int Step, int Damage)[] hurts = [(20, 10), (40, 25), (60, 30), (80, 35), (200, 10)];
(int Step, float Seconds)[] sets = [];
(int Step, float Value)[] flashes = [];
var mortalStep = 0;
var airLift = 0f;
var airLiftStep = 0;
int forwardFrom = 0, forwardTo = 0;
(int Step, int Mode, float A, float B, float Up, float ObjYaw, float Dx, float Dy)[] pushes = [];
(int From, int To)[] turnRight = [];
(int From, int To)[] turnLeft = [];
var columns = "voices";
switch (schedule)
{
    case "hurt":
        break;
    case "hurtland":
        hurts = [(220, 10), (300, 4)];
        break;
    case "knock":
        hurts = [(20, 25), (175, 4), (300, 3)];
        columns = "knock";
        break;
    case "invuln":
        lift = 0f;
        hurts = [(40, 4), (210, 4), (280, 3)];
        sets = [(20, 3f), (305, 2f)];
        mortalStep = 300;
        columns = "invuln";
        break;
    case "knockdown":
        lift = 0f;
        hurts = [(20, 5), (40, 4), (156, 5)];
        sets = [(30, 0f)];
        airLift = 6f;
        airLiftStep = 150;
        forwardFrom = 25;
        forwardTo = 60;
        columns = "knockdown";
        break;
    case "flashes":
        lift = 0f;
        hurts = [(10, 1), (320, 4)];
        flashes = [(5, 60f), (400, 200f)];
        airLift = 100f;
        airLiftStep = 60;
        mortalStep = 330;
        columns = "flash";
        break;
    case "push":
        lift = 0f;
        hurts = [];
        pushes =
        [
            (20, 0, 20f, 5f, 3f, 37f, 0f, 0f),
            (40, 0, 25f, 5f, 3f, 200f, 0f, 0f),
            (130, 1, 15f, 0f, 8f, 0f, -5f, 0f),
            (240, 1, 12f, 0f, 0f, 0f, 0f, 0f),
            (355, 0, 30f, 0f, 20f, 15f, 0f, 0f),
        ];
        mortalStep = 350;
        columns = "push";
        break;
    case "camroll":
        lift = 0f;
        hurts = [];
        forwardFrom = 1;
        forwardTo = 189;
        turnRight = [(10, 99), (180, 189), (192, 196)];
        turnLeft = [(100, 179), (185, 189)];
        columns = "roll";
        break;
    default:
        throw new Exception($"unknown schedule {schedule}");
}

// The world: kurt_probe's (reachable arenas, the archive's frame counts, the level's sound bank)
// plus the CMI and the ScriptRuntime bound to this Kurt, seed 1, Option 1 (as cheats_probe).
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
int FrameCount(KurtT.State s) => (int)s < names.Length && sprites.Has(names[(int)s]) ? sprites.GetAnimation(names[(int)s]).FrameCount : 0;
var start = levelData.Dti.StartPosition;
var kurt = new KurtT(space, mixer, FrameCount) { Feet = start, Yaw = levelData.Dti.StartAngle };
var cmi = Cmi.Load(data.PathOf($"TRAVERSE/LEVEL{level}/LEVEL{level}.CMI"));
var groups = new TriangleGroups();
foreach (var arena in levelData.Arenas)
{
    groups.Add(arena);
}

var runtime = new ScriptRuntime(levelData, cmi, sprites, space, groups, mixer, kurt, 1) { Option = 1 };
var died = false;
kurt.Died += () => died = true;

var voicesField = typeof(SoundMixer).GetField("_voices", BindingFlags.NonPublic | BindingFlags.Instance)!;
string Voices()
{
    var parts = new List<string>();
    foreach (var v in (System.Collections.IList)voicesField.GetValue(mixer)!)
    {
        parts.Add((string)v!.GetType().GetField("Name")!.GetValue(v)!);
    }

    return parts.Count == 0 ? "-" : string.Join(",", parts);
}

int P(float v) => (int)MathF.Floor(v * 64f);
string F(float v) => v.ToString("F4", CultureInfo.InvariantCulture);

// The header: every number of the schedule, as dumpKurtHurt prints it.
var header = $"level={level} steps={steps} seed=1 lift={F(lift)} hurts=" + string.Join(",", hurts.Select(h => $"{h.Step}:{h.Damage}"));
if (sets.Length > 0)
{
    header += " sets=" + string.Join(",", sets.Select(s => $"{s.Step}:{F(s.Seconds)}"));
}

if (flashes.Length > 0)
{
    header += " flashes=" + string.Join(",", flashes.Select(f => $"{f.Step}:{F(f.Value)}"));
}

if (mortalStep != 0)
{
    header += $" mortal={mortalStep}";
}

if (airLiftStep != 0)
{
    header += $" air={airLiftStep}:{F(airLift)}";
}

if (forwardFrom != 0)
{
    header += $" forward={forwardFrom}..{forwardTo}";
}

if (turnRight.Length > 0)
{
    header += " turnRight=" + string.Join(",", turnRight.Select(w => $"{w.From}..{w.To}"));
}

if (turnLeft.Length > 0)
{
    header += " turnLeft=" + string.Join(",", turnLeft.Select(w => $"{w.From}..{w.To}"));
}

if (pushes.Length > 0)
{
    header += " pushes=" + string.Join(",", pushes.Select(p =>
        $"{p.Step}:{p.Mode}:{F(p.A)},{F(p.B)},{F(p.Up)}:{F(p.ObjYaw)},{F(p.Dx)},{F(p.Dy)}"));
}

Console.Write(header + "\n");

var input = new Input();
for (var step = 1; step <= steps; step++)
{
    // The lifts are script teleports: the feet move and the vertical speed is zeroed with them.
    if (step == 1 && lift != 0f)
    {
        kurt.Feet = start + new Vector3(0f, 0f, lift);
        kurt.VerticalSpeed = 0f;
    }

    if (step == airLiftStep && airLift != 0f)
    {
        kurt.Feet = start + new Vector3(0f, 0f, airLift);
        kurt.VerticalSpeed = 0f;
    }

    var fwd = forwardFrom != 0 && step >= forwardFrom && step <= forwardTo;
    var right = turnRight.Any(w => step >= w.From && step <= w.To);
    var left = turnLeft.Any(w => step >= w.From && step <= w.To);
    input.Hold(Key.Forward, fwd ? Input.State.Down : Input.State.Up);
    input.Hold(Key.TurnRight, right ? Input.State.Down : Input.State.Up);
    input.Hold(Key.TurnLeft, left ? Input.State.Down : Input.State.Up);
    kurt.Update(input, 1f / 60f);

    // The scripts' writes, in kurtHurtDump's order, then the scripts' step.
    foreach (var s in sets.Where(s => s.Step == step))
    {
        kurt.Invulnerable = s.Seconds;  // opcode 244's set arm
    }

    if (step == mortalStep)
    {
        kurt.SetHealth(0);
    }

    foreach (var f in flashes.Where(f => f.Step == step))
    {
        kurt.WhiteFlash = MathF.Max(kurt.WhiteFlash, f.Value);  // opcode 215
    }

    foreach (var h in hurts.Where(h => h.Step == step))
    {
        runtime.HurtKurt(h.Damage);  // opcode 109
    }

    foreach (var p in pushes.Where(p => p.Step == step))
    {
        // The pushing object, placed relative to the runtime's own KurtPosition; the operand
        // list has the mode's arity (mode 0: [0, a, b, up]; otherwise [mode, a, up]).
        var obj = new MdkObject
        {
            Yaw = p.ObjYaw,
            Position = new Vector3(runtime.KurtPosition.X + p.Dx, runtime.KurtPosition.Y + p.Dy, runtime.KurtPosition.Z),
        };
        object?[] operands = p.Mode == 0 ? [p.Mode, p.A, p.B, p.Up] : [p.Mode, p.A, p.Up];
        runtime.PushKurt(obj, operands);  // opcode 248
    }

    runtime.Update(1f / 60f);

    var common = $"s={step} state={kurt.Current} feet64={P(kurt.Feet.X)},{P(kurt.Feet.Y)},{P(kurt.Feet.Z)} " +
                 $"vert={F(kurt.VerticalSpeed)} floor={(kurt.OnFloor ? 1 : 0)} chute={(kurt.ChuteOpen ? 1 : 0)} " +
                 $"hp={kurt.Health} vmhp={runtime.Kurt.Health}";
    var line = columns switch
    {
        "flash" => $"{common} hurt={F(kurt.HurtFlash)} white={F(kurt.WhiteFlash)} died={(died ? 1 : 0)}",
        "push" => $"{common} invuln={F(kurt.Invulnerable)} knock={F(kurt.KnockDamage)} frame={F(kurt.AnimationFrame)} " +
                  $"vel={F(kurt.Velocity.X)},{F(kurt.Velocity.Y)}",
        "knockdown" => $"{common} invuln={F(kurt.Invulnerable)} knock={F(kurt.KnockDamage)} frame={F(kurt.AnimationFrame)}",
        "invuln" => $"{common} invuln={F(kurt.Invulnerable)} knock={F(kurt.KnockDamage)}",
        "knock" => $"{common} knock={F(kurt.KnockDamage)}",
        "roll" => $"s={step} state={kurt.Current} roll={F(kurt.CameraRoll.Roll)}",
        _ => $"{common} voices={Voices()}",
    };
    Console.Write(line + "\n");
}
