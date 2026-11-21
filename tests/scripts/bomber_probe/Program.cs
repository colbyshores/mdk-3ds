using System.Globalization;
using System.Numerics;
using System.Reflection;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Hud;
using Mdk.Game.Kurt;
using Mdk.Game.Level;
using Mdk.Game.Objects;
using Mdk.Game.Scripts;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE BOMBER ORACLE (package #11): the C# Kurt, ScriptRuntime and FollowCamera (src/Mdk.Game)
// stepped together at 1/60 s -- Kurt, then the scripts, then the camera, as Viewer.RunSteps
// does -- under a scripted input, on level 7's DANT_5 XE ride. It pins Scripts/Bomber.cs whole
// (Update, MoveCursor, CursorStep, RefillBombs, Drop, Aim, AimDirection, FallTime, FirstObject,
// PassDamage), Rides.cs's bomber half (TryMount's Bombers, MountBomber, Follow, Hides,
// Dismount's drop), Kurt.Ride.cs's TopDownHeight/GetOffBoard, FollowCamera's LookDown, and
// Hud/BomberOverlay.cs's Draw: Kurt touching the rideable XE gets on (hidden, Kurt.Ride set),
// the view sinks 50 -> 0 above him in 2 s and the XE is then hidden (and not solid), the script
// unlocks the controls (flag 0x4000000 cleared), the keys move the cursor, fire drops an
// XBN_BOMB (Items' bomb kind 0x81) onto the point under the cursor, the bombs refill one a second,
// hits on Kurt go to the XE and back to him, and when the script clears the rideable flag he
// drops from it. Its output is tests/scripts/golden/L7_bomber_*.txt, and the C++ twin is
// `tick1 dump bomber <level> <steps> <schedule>` (tests/scripts/tick1.cpp, kurtBomberDump),
// which must print the same bytes.
//
//   Probe <level> <steps> <schedule>
//
// The schedule is comma-separated: `key:from-to` holds a key over steps [from, to) (keys fwd,
// back, left, right, sleft, sright, jump, turbo, fire), `mouse:from-to:dx:dy` sets the step's
// mouse motion (Input.MouseX/MouseY) over those steps, `tp:step:ARENA:x:y:z:yaw` calls
// ScriptRuntime.TeleportKurt before that step (the tests' --teleport), `call:step:ARENA:GROUP`
// hits that triangle group once with the chain gun (ScriptRuntime.HitGroup(arena, group, 1,
// HitChainGun, 0): RideTest.StepBomber's call of DANT_5's comm device, group 16), `board:step`
// puts Kurt 1 unit above the first rideable XE on the first step from then on that there is one
// (Kurt.Teleport, RideTest.StepBomber's boarding), `hit:step:DAMAGE` calls
// ScriptRuntime.HurtKurt (a shot at Kurt, which the ridden XE takes), and `print:step` prints
// only the steps from that one on (the ride is 6300 steps long; every step still runs). The
// goldens' schedules are in tick1.cpp's kBomberCases table.
//
// THE OVERLAY. BomberOverlay draws through the engine's Renderer (Vulkan) and a FontView; both
// are made without their constructors (no window, no GPU), as items_probe's `hud` view does: the
// renderer's canvas lists are set by reflection, so Renderer.DrawImage records each quad, the big
// font (FONTBIG, space 6) gets the fields its constructor would compute (texture 1; FONTSML is
// texture 2 in the twin), and the overlay its two images (BOMBTARG texture 3, CROSS texture 4,
// each frame 0's size and hotspot). Each step it draws Rides.Bomber.Shown on a 600-wide canvas
// (the 3DS top screen's), and the line ends with the quads' count and FNV-1a hash (texture, then
// left, top, right, bottom as float bits), and every 10th step the quads themselves.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/ride_probe -- copy
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
var groups = new TriangleGroups();
foreach (var arena in levelData.Arenas)
{
    groups.Add(arena);
}

var runtime = new ScriptRuntime(levelData, cmi, sprites, space, groups, mixer, kurt, 1) { Option = 1 };
int P(float v) => (int)MathF.Floor(v * 64f);
string F(float v) => v.ToString("F4", CultureInfo.InvariantCulture);
string B(float v) => BitConverter.SingleToInt32Bits(v).ToString("X8");
string Name(object? o) => o is MdkObject m ? $"{m.TypeName}#{m.InstanceId}" : o == null ? "-" : "?";
var flags = BindingFlags.NonPublic | BindingFlags.Instance;

// Viewer.cs:237: the follow camera over the same space (no ClipView in either tree).
var follow = new FollowCamera(space);
var voicesField = typeof(SoundMixer).GetField("_voices", flags)!;
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

// The overlay over a recording canvas (see THE OVERLAY above).
var fti = Fti.Load(data.PathOf("MISC/MDKFONT.FTI"));
var canvas = (Mdk.Engine.Render.Renderer)System.Runtime.CompilerServices.RuntimeHelpers.GetUninitializedObject(typeof(Mdk.Engine.Render.Renderer));
var canvasVertices = typeof(Mdk.Engine.Render.Renderer).GetField("_canvasVertices", flags)!;
var canvasCommands = typeof(Mdk.Engine.Render.Renderer).GetField("_canvasCommands", flags)!;
canvasVertices.SetValue(canvas, canvasVertices.FieldType.GetConstructor(Type.EmptyTypes)!.Invoke(null));
canvasCommands.SetValue(canvas, canvasCommands.FieldType.GetConstructor(Type.EmptyTypes)!.Invoke(null));
FontView View(string name, int fontSpace, int texture)
{
    var font = Font.Parse(fti.GetBytes(name), fontSpace);
    var view = (FontView)System.Runtime.CompilerServices.RuntimeHelpers.GetUninitializedObject(typeof(FontView));
    var x = new int[256];
    var atlas = FontView.Atlas(font, "", x);
    void Set(string field, object value) => typeof(FontView).GetField(field, flags)!.SetValue(view, value);
    Set("_renderer", canvas);
    Set("_font", font);
    Set("_texture", texture);
    Set("_palette", 0);
    Set("_x", x);
    Set("_maxAscent", font.Glyphs.Max(g => g?.Ascent ?? 0));
    Set("_size", new Vector2(atlas.Width, atlas.Height));
    return view;
}

var overlay = (BomberOverlay)System.Runtime.CompilerServices.RuntimeHelpers.GetUninitializedObject(typeof(BomberOverlay));
{
    var imageType = typeof(BomberOverlay).GetNestedType("Image", BindingFlags.NonPublic)!;
    object Image(string animation, int texture)
    {
        var frame = sprites.GetAnimation(animation).GetFrame(0);
        return Activator.CreateInstance(imageType, texture, new Vector2(frame.Image.Width, frame.Image.Height),
            new Vector2(frame.HotspotX, frame.HotspotY))!;
    }

    void Set(string field, object value) => typeof(BomberOverlay).GetField(field, flags)!.SetValue(overlay, value);
    Set("_renderer", canvas);
    Set("_palette", 0);
    Set("_font", View("FONTBIG", 6, 1));
    Set("_target", Image("BOMBTARG", 3));
    Set("_cross", Image("CROSS", 4));
}

// FNV-1a over an int's four little-endian bytes.
uint Fnv(uint hash, int value)
{
    foreach (var b in BitConverter.GetBytes(value))
    {
        hash = (hash ^ b) * 16777619u;
    }

    return hash;
}

string Overlay(int step)
{
    if (runtime.Rides.Bomber?.Shown is not { } sight)
    {
        return "draw=-";
    }

    var vertices = (List<Mdk.Engine.Render.Vertex>)canvasVertices.GetValue(canvas)!;
    var commands = (List<(int First, int Count, Mdk.Engine.Render.Material Material)>)canvasCommands.GetValue(canvas)!;
    vertices.Clear();
    commands.Clear();
    overlay.Draw(600f, sight);
    var quads = new List<string>();
    var hash = 2166136261u;
    foreach (var (first, count, material) in commands)
    {
        for (var q = first; q < first + count; q += 6)
        {
            var a = vertices[q].Position;
            var c = vertices[q + 2].Position;
            quads.Add($"T{material.Texture}:{B(a.X)},{B(a.Y)},{B(c.X)},{B(c.Y)}");
            hash = Fnv(hash, material.Texture);
            foreach (var v in new[] { a.X, a.Y, c.X, c.Y })
            {
                hash = Fnv(hash, BitConverter.SingleToInt32Bits(v));
            }
        }
    }

    return $"draw={quads.Count}:{hash:X8}" + (step % 10 == 0 && quads.Count != 0 ? $" quads={string.Join(";", quads)}" : "");
}

var keys = new Dictionary<string, Key>
{
    ["fwd"] = Key.Forward, ["back"] = Key.Back, ["left"] = Key.TurnLeft, ["right"] = Key.TurnRight,
    ["sleft"] = Key.StrafeLeft, ["sright"] = Key.StrafeRight, ["jump"] = Key.Jump, ["turbo"] = Key.Turbo,
    ["fire"] = Key.Fire,
};
var holds = new List<(Key Key, int From, int To)>();
var mice = new List<(int From, int To, float X, float Y)>();
var teleports = new List<(int Step, string Arena, Vector3 At, float Yaw)>();
var calls = new List<(int Step, string Arena, int Group)>();
var hits = new List<(int Step, int Damage)>();
var board = -1;
var from = 1;
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    switch (parts[0])
    {
        case "tp":
            teleports.Add((int.Parse(parts[1]), parts[2],
                new Vector3(float.Parse(parts[3]), float.Parse(parts[4]), float.Parse(parts[5])), float.Parse(parts[6])));
            continue;
        case "call":
            calls.Add((int.Parse(parts[1]), parts[2], int.Parse(parts[3])));
            continue;
        case "board":
            board = int.Parse(parts[1]);
            continue;
        case "print":
            from = int.Parse(parts[1]);
            continue;
        case "hit":
            hits.Add((int.Parse(parts[1]), int.Parse(parts[2])));
            continue;
        case "mouse":
        {
            var span = parts[1].Split('-');
            mice.Add((int.Parse(span[0]), int.Parse(span[1]), float.Parse(parts[2]), float.Parse(parts[3])));
            continue;
        }
    }

    var range = parts[1].Split('-');
    holds.Add((keys[parts[0]], int.Parse(range[0]), int.Parse(range[1])));
}

var mouseX = typeof(Input).GetField("<MouseX>k__BackingField", flags)!;
var mouseY = typeof(Input).GetField("<MouseY>k__BackingField", flags)!;
var speedField = typeof(Bomber).GetField("_speed", flags)!;
var refillField = typeof(Bomber).GetField("_refill", flags)!;
var heldField = typeof(Bomber).GetField("_fireHeld", flags)!;
var input = new Input();
MdkObject? xe = null;
var boarded = false;
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

    foreach (var (s, arena, group) in calls)
    {
        if (s == step)
        {
            runtime.HitGroup(arena, group, 1, ScriptRuntime.HitChainGun, 0);
        }
    }

    // RideTest.StepBomber's boarding: once an XE is rideable, Kurt 1 unit above its box.
    if (board >= 0 && step >= board && !boarded &&
        runtime.Objects.FirstOrDefault(o => o.TypeName == "XE" && (o.Flags & Rides.FlagRideable) != 0) is { } rideable)
    {
        boarded = true;
        var roof = runtime.GetWorldBounds(rideable).Max.Z;
        kurt.Teleport(rideable.Position with { Z = roof + 1f }, kurt.Yaw);
    }

    foreach (var (s, damage) in hits)
    {
        if (s == step)
        {
            runtime.HurtKurt(damage);
        }
    }

    foreach (var key in keys.Values)
    {
        var down = holds.Any(h => h.Key == key && step >= h.From && step < h.To);
        input.Hold(key, down ? Input.State.Down : Input.State.Up);
    }

    var mouse = mice.FirstOrDefault(m => step >= m.From && step < m.To);
    mouseX.SetValue(input, mouse.X);
    mouseY.SetValue(input, mouse.Y);

    // Viewer.RunSteps: Kurt, the scripts (whose Update ends with Rides.Follow), then the camera
    // at the arena's rest pitch (see ride_probe on camera_track; the top-down view ignores it).
    kurt.Update(input, 1f / 60f);
    runtime.Update(1f / 60f);
    var goal = levelData.Dti.Arenas.FirstOrDefault(a => a.Name == runtime.CurrentArena)?.Pitch ?? 4f;
    follow.Update(kurt, goal, input, 1f / 60f);

    xe ??= runtime.Objects.FirstOrDefault(o => o.TypeName == "XE");
    if (step < from)
    {
        continue;
    }

    var x = xe == null ? "-"
        : $"{Name(xe)}@{P(xe.Position.X)},{P(xe.Position.Y)},{P(xe.Position.Z)}/y{F(xe.Yaw)}" +
          $"/f{xe.Flags:X}/hp{xe.Health}/r{xe.Restart}/d{(xe.Dead ? 1 : 0)}/v{(xe.Visible ? 1 : 0)}";
    var bomber = runtime.Rides.Bomber;
    var b = "-";
    if (bomber != null)
    {
        var speed = (Vector2)speedField.GetValue(bomber)!;
        b = $"{F(bomber.ViewHeight)}/l{(bomber.Locked ? 1 : 0)}/h{(bomber.HidesXe ? 1 : 0)}/c{F(bomber.Cursor.X)},{F(bomber.Cursor.Y)}" +
            $"/v{F(speed.X)},{F(speed.Y)}/n{bomber.BombsLeft}/t{F((float)refillField.GetValue(bomber)!)}/held{((bool)heldField.GetValue(bomber)! ? 1 : 0)}";
    }

    var bombs = new List<string>();
    var live = 0;
    var ohp = 0;
    foreach (var o in runtime.Objects)
    {
        if (o.Dead)
        {
            continue;
        }

        live++;
        ohp += o.Health;
        if (o.TypeName == "XBN_BOMB")
        {
            bombs.Add($"{Name(o)}@{P(o.Position.X)},{P(o.Position.Y)},{P(o.Position.Z)}/v{F(o.Velocity.X)},{F(o.Velocity.Y)},{F(o.Velocity.Z)}" +
                $"/k{o.ThrownKind}/t{o.ItemTicks}/f{o.Flags:X}/p{F(o.Pitch)}");
        }
    }

    // The camera while it looks down (LookDown): where it is, its forward, and whether its
    // up is Kurt's facing (`up=facing`, else the vector) -- the facing's digits are MathF
    // here and the trig table there (AGENTS.md R6), apart in the fourth place (measured),
    // and the yaw column already pins them. Off the XE it is the ordinary follow camera,
    // whose pitch also goes through the table (1/64 apart once at step 640, measured):
    // `cam=follow`.
    var cam = follow.Position;
    var up = follow.Up == kurt.Facing ? "facing" : $"{F(follow.Up.X)},{F(follow.Up.Y)},{F(follow.Up.Z)}";
    var view = kurt.TopDownHeight == null ? "cam=follow"
        : $"cam64={P(cam.X)},{P(cam.Y)},{P(cam.Z)} fwd={F(follow.Forward.X)},{F(follow.Forward.Y)},{F(follow.Forward.Z)} up={up}";
    var top = kurt.TopDownHeight is { } h ? F(h) : "-";
    Console.WriteLine($"s={step} arena={runtime.CurrentArena} state={kurt.Current} feet64={P(kurt.Feet.X)},{P(kurt.Feet.Y)},{P(kurt.Feet.Z)} " +
        $"yaw={F(kurt.Yaw)} fs={F(kurt.ForwardSpeed)} vz={F(kurt.VerticalSpeed)} floor={(kurt.OnFloor ? 1 : 0)} vis={(kurt.Visible ? 1 : 0)} " +
        $"fire={(kurt.Firing ? 1 : 0)} hp={kurt.Health} ride={Name(runtime.Rides.Ridden)} hits={(runtime.Rides.TakesHits() ? 1 : 0)} " +
        $"topdown={top} bomber={b} xe={x} {view} " +
        $"objs={runtime.Objects.Count} live={live} ohp={ohp} kills={runtime.Stats.Kills} bombs={(bombs.Count == 0 ? "-" : string.Join(";", bombs))} voices={Voices()} {Overlay(step)}");
}
