using System.Globalization;
using System.Numerics;
using System.Reflection;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Hud;
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
//   Probe hud <level> <steps> <schedule>  sniper mode's screen and views (gap package #7)
//   Probe iris <level> <steps>            the overlay alone under a fixed sequence (#7)
//
// SNIPER MODE'S SCREEN AND VIEWS (`hud`, `iris`; golden/L*_sniper_hud*.txt, the C++ twin
// `tick1 dump sniperhud|sniperiris ...`). Hud/SniperOverlay.cs and SniperView.cs as Viewer runs
// them: each step SniperView.Draw's SniperHud (zoom, ammo, the rounds' cameras, Aim -- the C#'s
// own, by reflection) goes to SniperOverlay.Update(1/60), and SniperOverlay.Draw draws on a
// 600-wide canvas (the 3DS top screen's; `iris` on a 480-wide one, no side bars) with the HUD's
// panel at its place (HudView.Draw). The renderer and the overlay are made without their
// constructors (no window, no GPU): the renderer's canvas lists are set by reflection so
// FillRect/DrawImage record their quads, and the overlay's images get the texture ids the C++
// load hands out in the same order (frame 1, mask 2, CROSS 3, SNIP_RNG 4, SNIP_WEP 5, SNIP_TXT
// 6, SNIP_W1 7, SNIP_L1 8, ... SNIP_L6 18, SNIPERGA's frames from 19) and palette 7. Each step
// prints the overlay's state (iris, pulse, target, gauge, the zoom percent) and its quads' count
// and FNV-1a hash (texture, palette, colour, left, top, right, bottom, u0, v0, u1, v1 as bits),
// and every 60th step the quads; then each round camera's shot and, while it watches, the hash
// of its view (CameraMath.View: the view-projection and the projection) for its window
// (OnCanvas); then the clip's loaded rounds (ClipPlace: the rotation's hash, position, scale)
// and the clip view's hash. `iris` drives the overlay alone: the strike selected, the clip
// ready and a strike target on and off (Aim), a zoom sweep, the cameras' fills and the miss
// animation.
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
var hudMode = args[0] == "hud";
var irisMode = args[0] == "iris";
if (listMode || hudMode || irisMode)
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

// ---- Sniper mode's screen (`hud`, `iris`) ----
var npi = BindingFlags.NonPublic | BindingFlags.Instance;
var canvas = (Mdk.Engine.Render.Renderer)System.Runtime.CompilerServices.RuntimeHelpers.GetUninitializedObject(typeof(Mdk.Engine.Render.Renderer));
var canvasVertices = typeof(Mdk.Engine.Render.Renderer).GetField("_canvasVertices", npi)!;
var canvasCommands = typeof(Mdk.Engine.Render.Renderer).GetField("_canvasCommands", npi)!;
canvasVertices.SetValue(canvas, canvasVertices.FieldType.GetConstructor(Type.EmptyTypes)!.Invoke(null));
canvasCommands.SetValue(canvas, canvasCommands.FieldType.GetConstructor(Type.EmptyTypes)!.Invoke(null));
var overlay = (SniperOverlay)System.Runtime.CompilerServices.RuntimeHelpers.GetUninitializedObject(typeof(SniperOverlay));
{
    void Set(string field, object? value) => typeof(SniperOverlay).GetField(field, npi)!.SetValue(overlay, value);
    var imageType = typeof(SniperOverlay).GetNestedType("Image", BindingFlags.NonPublic)!;
    var texture = 0;
    object Image(Texture t, Vector2 hotspot) => Activator.CreateInstance(imageType, ++texture, new Vector2(t.Width, t.Height), hotspot)!;
    System.Collections.IList List() => (System.Collections.IList)Activator.CreateInstance(typeof(List<>).MakeGenericType(imageType))!;
    Set("_renderer", canvas);
    Set("_palette", 7);
    Set("_colours", levelData.Dti.Palette);
    Set("_frame", Image(SniperScreen.Frame(sprites), Vector2.Zero));
    Set("_mask", Image(SniperScreen.Mask(sprites), Vector2.Zero));
    var cross = sprites.GetAnimation("CROSS").GetFrame(0);
    Set("_cross", Image(cross.Image, new Vector2(cross.HotspotX, cross.HotspotY)));
    Set("_gauge", Image(sprites.GetImage("SNIP_RNG"), Vector2.Zero));
    Set("_weapon", Image(sprites.GetImage("SNIP_WEP"), Vector2.Zero));
    Set("_digits", Image(sprites.GetImage("SNIP_TXT"), Vector2.Zero));
    var icons = List();
    var labels = List();
    for (var i = 1; i <= 6; i++)
    {
        icons.Add(Image(sprites.GetImage($"SNIP_W{i}"), Vector2.Zero));
        labels.Add(Image(sprites.GetImage($"SNIP_L{i}"), Vector2.Zero));
    }

    var miss = List();
    var missAnimation = sprites.GetAnimation("SNIPERGA");
    for (var i = 0; i < missAnimation.FrameCount; i++)
    {
        miss.Add(Image(missAnimation.GetFrame(i).Image, Vector2.Zero));
    }

    Set("_icons", icons);
    Set("_labels", labels);
    Set("_miss", miss);
    Set("_iris", 1f);
    Set("_pulse", 1f);
}

var panelImage = sprites.GetImage("SC_STAT");
var panelSize = new Vector2(panelImage.Width, panelImage.Height);
var panelAt = new Vector2(SniperScreen.ViewWidth - (panelSize.X + 16f), SniperScreen.ViewHeight - (panelSize.Y + 10f));
var irisField = typeof(SniperOverlay).GetField("_iris", npi)!;
var pulseField = typeof(SniperOverlay).GetField("_pulse", npi)!;
var targetField = typeof(SniperOverlay).GetField("_target", npi)!;
var gaugeField = typeof(SniperOverlay).GetField("_gaugeShown", npi)!;
var aimMethod = typeof(Mdk.Game.SniperView).GetMethod("Aim", BindingFlags.NonPublic | BindingFlags.Static)!;
var clipPlace = typeof(Mdk.Game.SniperView).GetMethod("ClipPlace", BindingFlags.NonPublic | BindingFlags.Static)!;
var onCanvas = typeof(Mdk.Game.SniperView).GetMethod("OnCanvas", BindingFlags.NonPublic | BindingFlags.Static)!;
uint FnvM(uint hash, Matrix4x4 m)
{
    foreach (var v in new[] { m.M11, m.M12, m.M13, m.M14, m.M21, m.M22, m.M23, m.M24, m.M31, m.M32, m.M33, m.M34, m.M41, m.M42, m.M43, m.M44 })
    {
        hash = Fnv(hash, v);
    }

    return hash;
}

uint FnvI(uint hash, int v) => Fnv(hash, BitConverter.Int32BitsToSingle(v));

// The overlay's line: its state, then Draw's quads (count, hash; all of them when `full`).
string OverlayLine(float width, int health, bool full)
{
    var vertices = (List<Mdk.Engine.Render.Vertex>)canvasVertices.GetValue(canvas)!;
    var commands = (List<(int First, int Count, Mdk.Engine.Render.Material Material)>)canvasCommands.GetValue(canvas)!;
    vertices.Clear();
    commands.Clear();
    overlay.Draw(width, health, Blink.Shown, panelAt, panelSize);
    var hash = 2166136261u;
    var quads = new System.Text.StringBuilder();
    var count = 0;
    foreach (var (first, n, material) in commands)
    {
        for (var q = first; q < first + n; q += 6)
        {
            var a = vertices[q];
            var c = vertices[q + 2];
            float[] values = [material.Colour.X, material.Colour.Y, material.Colour.Z, material.Colour.W,
                a.Position.X, a.Position.Y, c.Position.X, c.Position.Y, a.Uv.X, a.Uv.Y, c.Uv.X, c.Uv.Y];
            hash = FnvI(FnvI(hash, material.Texture), material.Palette);
            foreach (var v in values)
            {
                hash = Fnv(hash, v);
            }

            if (full)
            {
                quads.Append($"\n    q{count} {material.Texture} {material.Palette} {string.Join(",", values.Select(B))}");
            }

            count++;
        }
    }

    return $"vis={(overlay.Visible ? 1 : 0)} iris={B((float)irisField.GetValue(overlay)!)} pulse={B((float)pulseField.GetValue(overlay)!)} " +
        $"tgt={((bool)targetField.GetValue(overlay)! ? 1 : 0)} gauge={(int)gaugeField.GetValue(overlay)!} quads={count} hash={hash:X8}{quads}";
}

if (irisMode)
{
    // The overlay alone (SniperOverlay.Update and Draw): the strike (type 5) selected from step
    // 40, the clip reloading (NotChecked) to 50, a target 50-150, none 151-170, a target again
    // 171-260 (the pulse runs once the iris is shut), NotChecked after; the zoom sweeps 1 -> 0.2
    // and back; the cameras show empty, the miss animation (its time the step) and a hit.
    Console.WriteLine($"iris level={level} steps={steps}");
    for (var step = 1; step <= steps; step++)
    {
        var selected = step < 40 ? 0 : 5;
        var aim = step < 50 ? StrikeAim.NotChecked
            : step <= 150 ? StrikeAim.Target
            : step <= 170 ? StrikeAim.None
            : step <= 260 ? StrikeAim.Target
            : StrikeAim.NotChecked;
        var zoom = step < 150 ? 1f - step * 0.0055f : 0.2f + (step - 150) * 0.004f;
        SniperRounds.RoundCamera[] cameras =
        [
            new(SniperRounds.Shot.Empty, default, default, 0f),
            new(SniperRounds.Shot.Miss, default, default, step * 0.5f),
            new(step % 90 < 45 ? SniperRounds.Shot.Hit : SniperRounds.Shot.Kill, default, default, 0f),
        ];
        int[] ammo = [0, 4, 0, 2, 3];
        overlay.Update(1f / 60f, new SniperHud(zoom, ammo, selected, cameras, aim));
        Console.WriteLine($"s={step} zoom={B(zoom)} sel={selected} aim={(int)aim} pct={(int)MathF.Round(100f * Math.Clamp((1f - zoom) * (1f - zoom) * 1.05194f, 0f, 1f))} " +
            OverlayLine(480f, 37, step % 60 == 0 || step == 100 || step == 200));
    }

    return;
}

var seen = new Dictionary<MdkObject, string>(ReferenceEqualityComparer.Instance);
void Out(string line)
{
    if (!hudMode)
    {
        Console.WriteLine(line);
    }
}

Console.WriteLine($"{(hudMode ? "hud " : "")}level={level} steps={steps} seed=1 schedule={schedule}");
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
    // The `hud` lines of this step (a local function: its early returns end it, not the step).
    void HudStep()
    {
        // SniperView.Draw: out of sniper mode the overlay is told so; in it, its SniperHud.
        var cameras = new List<SniperRounds.RoundCamera>();
        for (var i = 0; i < SniperRounds.Slots; i++)
        {
            cameras.Add(runtime.SniperRounds.Camera(i));
        }

        if (!kurt.Sniping)
        {
            overlay.Update(1f / 60f, null);
        }
        else
        {
            var aimNow = (StrikeAim)aimMethod.Invoke(null, [kurt, runtime])!;
            overlay.Update(1f / 60f, new SniperHud(scope.Zoom, kurt.Inventory.Ammo, kurt.Inventory.SelectedAmmo, cameras, aimNow));
        }

        Console.WriteLine($"s={step} snipe={(kurt.Sniping ? 1 : 0)} zoom={B(scope.Zoom)} clip={scope.ClipRounds}/{B(scope.ClipTime)} " +
            $"sel={kurt.Inventory.SelectedAmmo} " + OverlayLine(600f, kurt.Health, kurt.Sniping && step % 60 == 0));
        if (!kurt.Sniping)
        {
            return;
        }

        var (scale, left) = SniperOverlay.Placement(600f);
        for (var i = 0; i < cameras.Count; i++)
        {
            var camera = cameras[i];
            var text = $"  cam{i} shot={(int)camera.Shot} time={B(camera.Time)}";
            if (camera.Shot == SniperRounds.Shot.Watching)
            {
                // DrawRoundCamera's view for its window.
                var area = (System.Drawing.RectangleF)onCanvas.Invoke(null, [SniperOverlay.RoundViews[i], scale, left])!;
                var aspect = area.Width / area.Height;
                var fieldOfView = float.RadiansToDegrees(2f * MathF.Atan(MathF.Tan(float.DegreesToRadians(90f) / 2f) / aspect));
                var view = Mdk.Game.Kurt.CameraMath.View(camera.Position, camera.Forward, Mdk.Game.Kurt.FollowCamera.UpOf(camera.Forward), fieldOfView, aspect, 0.5f, 20000f);
                text += $" area={B(area.X)},{B(area.Y)},{B(area.Width)},{B(area.Height)} at={V(camera.Position)} " +
                    $"view={FnvM(2166136261u, view.ViewProjection):X8} proj={FnvM(2166136261u, view.Camera!.Value.Projection):X8}";
            }

            Console.WriteLine(text);
        }

        // DrawClip: the loaded rounds of the selected type and the clip's view.
        string[] roundModels = ["SW_SHOT", "SW_HOME", "SW_SGREN", "SW_HGREN", "SW_LGREN", "SW_BONES"];
        if (runtime.FindModel(runtime.CurrentArena, roundModels[kurt.Inventory.SelectedAmmo]) == null)
        {
            return;
        }

        var clipArea = (System.Drawing.RectangleF)onCanvas.Invoke(null, [new System.Drawing.RectangleF(0f, 0f, SniperScreen.ViewWidth, SniperScreen.ViewHeight), scale, left])!;
        var shown = false;
        for (var i = 0; i < Mdk.Game.Kurt.Scope.ClipSize; i++)
        {
            var t = scope.ClipTime + i;
            if (i >= scope.ClipRounds || t == 0f || t >= 3f)
            {
                continue;
            }

            shown = true;
            var (rotation, position, roundScale) = ((Matrix4x4, Vector3, float))clipPlace.Invoke(null, [t])!;
            Console.WriteLine($"  clip{i} t={B(t)} rot={FnvM(2166136261u, rotation):X8} at={V(position)} scale={B(roundScale)}");
        }

        if (shown)
        {
            var clipFov = float.RadiansToDegrees(2f * MathF.Atan(SniperScreen.ViewHeight / 2f / 250f));
            var clipView = Mdk.Game.Kurt.CameraMath.View(Vector3.Zero, -Vector3.UnitY, -Vector3.UnitZ, clipFov, clipArea.Width / clipArea.Height, 1f, 1000f);
            Console.WriteLine($"  clipview area={B(clipArea.X)},{B(clipArea.Y)},{B(clipArea.Width)},{B(clipArea.Height)} " +
                $"view={FnvM(2166136261u, clipView.ViewProjection):X8} proj={FnvM(2166136261u, clipView.Camera!.Value.Projection):X8}");
        }
    }

    // In `hud` mode the rounds' lines are not printed, but they are still made: they number
    // the objects (Id), which the schedule's `hp:` refers to.
    if (hudMode)
    {
        HudStep();
    }

    var stats = runtime.Stats;
    Out($"s={step} snipe={(kurt.Sniping ? 1 : 0)} yaw={B(kurt.Yaw)} pitch={B(scope.Pitch)} zoom={B(scope.Zoom)} " +
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

        Out($"  r{i} {state} t={(int)Get(r, "Type")!} p={V((Vector3)Get(r, "Position")!)} yaw={B(Fl(r, "Yaw"))} " +
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
            Out($"  #{id} {obj.TypeName} {text}");
        }
    }
}
