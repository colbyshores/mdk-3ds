using System.Globalization;
using System.Numerics;
using System.Reflection;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Level;
using Mdk.Game.Kurt;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE SNIPER-MODE ORACLE: the C# Kurt (src/Mdk.Game/Kurt/Kurt.Sniper.cs, Scope.cs, Inventory's
// SelectAmmo/UseAmmo/NextAmmo) stepped alone at 1/60 s under a scripted input that enters sniper
// mode, tilts, sidesteps, zooms (keys and wheel), fires the clip, turns, picks the ammo and leaves,
// and FollowCamera.SniperView's view for the 3DS top screen's aspect. Its output is
// tests/scripts/golden/L*_sniper.txt, and the C++ twin is `tick1 dump sniper <level> <steps>`
// (tests/scripts/tick1.cpp, kurtSniperDump), which must print the same bytes.
//
//   Probe <level> <steps>
//
// NO SCRIPTS RUNTIME. ScriptRuntime's constructor binds Kurt.SniperFire to FireSniper (the rounds,
// gap-inventory package #6) and its tick narrows Scope.ZoomLimit on a locked target
// (UpdateSniperTarget, also #6); neither is this package's. Without it SniperFire is null, which
// Kurt.UpdateSniper reads as `_ => true` (every shot finds a slot), and ZoomLimit stays ZoomMin.
//
// YAW 0, AND WHY. Kurt starts at the level's start position facing +X (yaw 0), not the level's
// start angle: the port's Player::facing() is the trig LUT (docs/OPEN_ITEMS.md, owner-gated), and
// facing enters this run in two places only -- the sidestep (Right) and the eye's forward shift
// while looking down (Scope.Eye). At yaw 0 the LUT's cos/sin are exactly 1 and 0, so both are
// bit-exact; the schedule does all of its looking down and sidestepping before the first turn, and
// keeps the pitch at or above level after it. Every float is printed as its exact bits.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/fire_probe -- copy
// src/Mdk.Formats, src/Mdk.Engine, src/Mdk.Game and Directory.Build.props into a scratch tree with
// this directory beside src/; in the scratch Mdk.Engine.csproj drop the shader targets and the
// icon resource; `dotnet build -c Release -o probe-bin`, then
// `DOTNET_ROOT=<sdk> MDK_DATA_DIR=<the MDK folder> probe-bin/Probe <level> <steps>`.
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
    Yaw = 0f,
};
var voicesField = typeof(SoundMixer).GetField("_voices", BindingFlags.NonPublic | BindingFlags.Instance)!;
string Voices()
{
    var list = (System.Collections.IList)voicesField.GetValue(mixer)!;
    var parts = new List<string>();
    foreach (var v in list)
    {
        parts.Add((string)v!.GetType().GetField("Name")!.GetValue(v)!);
    }

    // Compactly: how many voices the mixer holds (nothing finishes one here, so every launch adds
    // one), the newest one's name, and whether the two loops (BREATH, ZOOM) are live.
    var last = parts.Count == 0 ? "-" : parts[^1];
    return $"{parts.Count}:{last} breath={(parts.Contains("BREATH") ? 1 : 0)} zoom={(parts.Contains("ZOOM") ? 1 : 0)}";
}

string B(float v) => BitConverter.SingleToUInt32Bits(v).ToString("X8");
string V(Vector3 v) => $"{B(v.X)},{B(v.Y)},{B(v.Z)}";
uint Fnv(uint hash, float v)
{
    var bits = BitConverter.SingleToUInt32Bits(v);
    for (var i = 0; i < 4; i++)
    {
        hash = (hash ^ ((bits >> (8 * i)) & 0xFF)) * 16777619u;
    }

    return hash;
}

float[] M(Matrix4x4 m) => [m.M11, m.M12, m.M13, m.M14, m.M21, m.M22, m.M23, m.M24, m.M31, m.M32, m.M33, m.M34, m.M41, m.M42, m.M43, m.M44];

// The schedule: keys held over [from, to), the wheel's notches before a step, pickups handed over
// (Kurt.Collect, the console's give) before a step. tick1.cpp's kSniperHolds is the same table.
(Key Key, int From, int To)[] holds =
[
    (Key.Sniper, 40, 42),       // enter
    (Key.Back, 50, 80),         // look down (the eye moves forward)
    (Key.StrafeRight, 60, 90),  // sidestep at a quarter of the speed
    (Key.ZoomIn, 90, 150),      // zoom in to the limit
    (Key.Fire, 110, 125),       // the strike round (type 5), then the clip empties
    (Key.Forward, 160, 290),    // look up past level: the pitch stays below 0 from here on
    (Key.Fire, 200, 262),       // normal bullets, a shot every quarter second
    (Key.TurnRight, 300, 330),  // turn
    (Key.Turbo, 310, 320),
    (Key.ZoomOut, 320, 360),
    (Key.TurnLeft, 340, 360),
    (Key.ItemNext, 390, 392),   // the next type with rounds (3)
    (Key.ItemPrevious, 410, 412), // back to bullets
    (Key.ItemNext, 430, 432),   // 3 again
    (Key.Fire, 450, 530),       // homing rounds until they run out
    (Key.Sniper, 540, 542),     // leave (SNIPEROFF)
    (Key.Jump, 560, 566),
    (Key.Sniper, 564, 566),     // in the air: refused
];
(int Step, float Notches)[] wheels = [(140, 1f), (365, -2f), (375, 1f)];
(int Step, string Model)[] gives = [(5, "SW_HGREN"), (6, "SW_BONES")];

var wheelProperty = typeof(Input).GetProperty("Wheel")!;
// Upstream 1618b8c3 (C# 1.2.0) zooms the scope with Input.ZoomWheel, which Input.AddWheel fills
// while sniping under the default bindings; the notches are written there too when the C# has it,
// so the probe drives the same zoom before and after that commit.
var zoomWheelProperty = typeof(Input).GetProperty("ZoomWheel");
const float Aspect = 400f / 240f;
// Two fixed points for Scope.ToScreen: one ahead and a little to the side (it locks at zoom 1
// and leaves the lock square zoomed in), one behind the eye.
Vector3[] probes = [levelData.Dti.StartPosition + new Vector3(60f, 4f, 5f), levelData.Dti.StartPosition - new Vector3(60f, 0f, 0f)];
Console.WriteLine($"level={level} steps={steps} aspect={B(Aspect)} lock={B(Scope.LockLimit(12f, 80f))},{B(Scope.LockLimit(4f, 0.5f))}");
var input = new Input();
for (var step = 1; step <= steps; step++)
{
    foreach (var give in gives.Where(g => g.Step == step))
    {
        kurt.Collect(give.Model);
    }

    foreach (var key in holds.Select(h => h.Key).Distinct())
    {
        var down = holds.Any(h => h.Key == key && step >= h.From && step < h.To);
        input.Hold(key, down ? Input.State.Down : Input.State.Up);
    }

    foreach (var wheel in wheels.Where(w => w.Step == step))
    {
        wheelProperty.SetValue(input, wheel.Notches);
        if (zoomWheelProperty != null && kurt.Sniping)
        {
            zoomWheelProperty.SetValue(input, wheel.Notches);
        }
    }

    kurt.Update(input, 1f / 60f);
    input.ClearMouse();

    var scope = kurt.Scope;
    var view = FollowCamera.SniperView(kurt, Aspect);
    var camera = view.Camera!.Value;
    var vp = M(view.ViewProjection);
    var proj = M(camera.Projection);
    var hash = 2166136261u;
    foreach (var v in vp)
    {
        hash = Fnv(hash, v);
    }

    foreach (var v in proj)
    {
        hash = Fnv(hash, v);
    }

    var screenText = string.Join(";", probes.Select(point =>
        Scope.ToScreen(kurt.SniperEye, kurt.SniperForward, scope.Zoom, point) is { } p
            ? $"{B(p.X)},{B(p.Y)}:{(Scope.Locks(p - new Vector2(8f), p + new Vector2(8f)) ? 1 : 0)}"
            : "-"));
    var ammo = string.Join(",", kurt.Inventory.Ammo);
    Console.WriteLine($"s={step} state={kurt.Current} floor={(kurt.OnFloor ? 1 : 0)} snipe={(kurt.Sniping ? 1 : 0)} " +
        $"feet={V(kurt.Feet)} yaw={B(kurt.Yaw)} vz={B(kurt.VerticalSpeed)} strafe={B(kurt.StrafeSpeed)} " +
        $"pitch={B(scope.Pitch)} zoom={B(scope.Zoom)} limit={B(scope.ZoomLimit)} clip={scope.ClipRounds}/{B(scope.ClipTime)} " +
        $"sel={kurt.Inventory.SelectedAmmo} ammo={ammo} eye={V(kurt.SniperEye)} fwd={V(kurt.SniperForward)} " +
        $"cam={V(view.Position)}/{V(camera.Forward)}/{V(camera.Up)}/{B(camera.Near)} scr={screenText} m={hash:X8} voices={Voices()}");
    if (kurt.Sniping && step % 20 == 0)
    {
        Console.WriteLine($"  vp={string.Join(",", vp.Select(B))}");
        Console.WriteLine($"  proj={string.Join(",", proj.Select(B))}");
    }
}
