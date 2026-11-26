using System.Globalization;
using System.Reflection;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Level;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE KURT-STEPS ORACLE: the C# Kurt alone (no ScriptRuntime) stepped at 1/60 s from the level's
// start under the fixed schedule of tick1.cpp's dumpKurt. Its output is
// tests/scripts/golden/L3_kurt_steps.txt (`Probe <level> <steps>`) and L3_kurt_lethal.txt
// (`Probe <level> <steps> lethal`); the C++ twin is `tick1 dump kurt|kurtlethal <level> <steps>`.
// The original driver (/tmp/kilo/oracle/Driver) was not kept; this one was rebuilt on
// 2026-11-26 from dumpKurt and reproduces both goldens byte for byte against the C# 1.1.1
// (d5bb66ec). Built and run as the other probes (see ledge_probe/Program.cs).
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
var level = int.Parse(args[0]);
var steps = int.Parse(args[1]);
var lethal = args.Length > 2 && args[2] == "lethal";
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
var counts = string.Join(",", Enum.GetValues<KurtT.State>().Select(s => $"{s}={FrameCount(s)}"));
Console.Write($"level={level} steps={steps} counts={counts}\n");
const int HardLiftStep = 471, HardChuteStep = 642, HardChuteRelease = 662, DeathJumpStep = 770, MortalStep = 860;
const float HardLift = 300f;
var input = new Input();
for (var step = 1; step <= steps; step++)
{
    if (lethal && step == 1)
    {
        kurt.Feet = start + new System.Numerics.Vector3(0f, 0f, HardLift);
        kurt.VerticalSpeed = 0f;
        kurt.SetHealth(0);
    }

    if (!lethal && step == HardLiftStep)
    {
        kurt.Feet = start + new System.Numerics.Vector3(0f, 0f, HardLift);
        kurt.VerticalSpeed = 0f;
    }

    if (!lethal && step == MortalStep)
    {
        kurt.SetHealth(0);
    }

    var fwd = !lethal && ((step >= 10 && step < 46) || (step >= 190 && step < 285));
    var back = !lethal && step >= 100 && step < 190;
    var jump = !lethal && ((step >= 300 && step < 390) || (step >= HardChuteStep && step < HardChuteRelease) || step >= DeathJumpStep);
    input.Hold(Key.Forward, fwd ? Input.State.Down : Input.State.Up);
    input.Hold(Key.Back, back ? Input.State.Down : Input.State.Up);
    input.Hold(Key.Jump, jump ? Input.State.Down : Input.State.Up);
    kurt.Update(input, 1f / 60f);
    Console.Write($"s={step} state={kurt.Current} feet64={P(kurt.Feet.X)},{P(kurt.Feet.Y)},{P(kurt.Feet.Z)} yaw={F(kurt.Yaw)} " +
                  $"fwd={F(kurt.ForwardSpeed)} str={F(kurt.StrafeSpeed)} vert={F(kurt.VerticalSpeed)} frame={F(kurt.AnimationFrame)} " +
                  $"floor={(kurt.OnFloor ? 1 : 0)} chute={(kurt.ChuteOpen ? 1 : 0)} closing={(kurt.ChuteClosing ? 1 : 0)} hp={kurt.Health} voices={Voices()}\n");
}
