using System.Globalization;
using System.Numerics;
using System.Reflection;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Level;
using Mdk.Game.Scripts;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE FIRING ORACLE: the C# Kurt and ScriptRuntime (src/Mdk.Game) stepped together at
// 1/60 s, Kurt then the scripts as Viewer.RunSteps does, with a scripted input that holds
// the chain gun. Its output is tests/scripts/golden/{L8_fire,L8_fire_run,L7_fire_run}.txt,
// and the C++ twin is `tick1 dump fire|firerun <level> <steps>` (tests/scripts/tick1.cpp,
// kurtFireDump), which must print the same bytes.
//
//   Probe 8 420                                    > golden/L8_fire.txt
//   Probe 8 480 10,460,200,230,250,280 nopc        > golden/L8_fire_run.txt
//   Probe 7 480 10,460,200,230,250,280 nopc        > golden/L7_fire_run.txt
//
// The schedule argument is fire-from,fire-to,turn-from,turn-to,run-from,run-to (steps);
// `nopc` drops the pieces' centre sum, which a moving Kurt cannot match at one decimal
// while the port's Player::facing() uses the trig LUT (see kurtFireDump).
//
// HOW IT WAS BUILT AND RUN (2026-11-19, .NET SDK 10.0.404): copy src/Mdk.Formats,
// src/Mdk.Engine, src/Mdk.Game and Directory.Build.props into a scratch tree with this
// directory beside src/; in the scratch Mdk.Engine.csproj drop the shader targets and the
// icon resource (no dxc is needed: nothing here draws); then
//   dotnet build -c Release -o probe-bin   and   MDK_DATA_DIR=<the MDK folder> probe-bin/Probe ...
// The ProjectReference paths below are the scratch tree's (../src/...). It is not built in
// this repository, like the oracle itself.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
var level = int.Parse(args[0]);
var steps = int.Parse(args[1]);
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
var runtime = new ScriptRuntime(levelData, cmi, sprites, space, new TriangleGroups(), mixer, kurt, 1) { Option = 1 };
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

string F(float v) => v.ToString("F4", CultureInfo.InvariantCulture);
string F2(float v) => v.ToString("F1", CultureInfo.InvariantCulture);
int[] p = args.Length > 2 ? args[2].Split(',').Select(int.Parse).ToArray() : [10, 400, 200, 230, 300, 330];
var input = new Input();
Console.WriteLine($"level={level} steps={steps} seed=1 fire={p[0]}-{p[1]} turn={p[2]}-{p[3]} run={p[4]}-{p[5]}");
for (var step = 1; step <= steps; step++)
{
    input.Hold(Key.Fire, step >= p[0] && step < p[1] ? Input.State.Down : Input.State.Up);
    input.Hold(Key.TurnLeft, step >= p[2] && step < p[3] ? Input.State.Down : Input.State.Up);
    input.Hold(Key.Forward, step >= p[4] && step < p[5] ? Input.State.Down : Input.State.Up);
    kurt.Update(input, 1f / 60f);
    runtime.Update(1f / 60f);
    var ohp = 0;
    foreach (var o in runtime.Objects)
    {
        if (!o.Dead)
        {
            ohp += o.Health;
        }
    }

    var pc = Vector3.Zero;
    var life = 0;
    foreach (var piece in runtime.Debris.Pieces)
    {
        pc += piece.Center;
        life += piece.Ticks;
    }

    var bar = runtime.GetBar();
    var centres = args.Length > 3 && args[3] == "nopc" ? "" : $" pc={F2(pc.X)},{F2(pc.Y)},{F2(pc.Z)}";
    Console.WriteLine($"s={step} state={kurt.Current} feet64={(int)MathF.Floor(kurt.Feet.X * 64f)},{(int)MathF.Floor(kurt.Feet.Y * 64f)},{(int)MathF.Floor(kurt.Feet.Z * 64f)} " +
        $"frame={F(kurt.AnimationFrame)} fire={(kurt.Firing ? 1 : 0)} muzzle={(kurt.Muzzle != null ? 1 : 0)} hp={kurt.Health} " +
        $"shots={runtime.Stats.Shots} hits={runtime.Stats.ShotHits} kills={runtime.Stats.Kills} objs={runtime.Objects.Count} ohp={ohp} " +
        $"bar={bar.Health}/{bar.Max} pieces={runtime.Debris.Pieces.Count} life={life}{centres} effects={runtime.Effects.All.Count} " +
        $"white={F(kurt.WhiteFlash)} voices={Voices()}");
}
