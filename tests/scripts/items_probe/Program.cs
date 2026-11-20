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

// THE ITEMS ORACLE: the C# Kurt and ScriptRuntime (src/Mdk.Game) stepped together at 1/60 s,
// Kurt then the scripts as Viewer.RunSteps does, under a scripted input. It pins the pickups
// (ScriptRuntime.CollectPickups, Inventory.Collect, the collected pickup's shrink), the item keys
// (Kurt.UpdateItems: next, previous, use), the throw (THROW / K_SPWEP, ItemUsed on frame 8 or at
// once in the air) and what ItemUsed reaches (Items.UseItem, UpdateThrown, Activate and every
// item's active update, the blasts, Twister). Its output is tests/scripts/golden/L*_items_*.txt,
// and the C++ twin is `tick1 dump items <level> <steps> <schedule>` (tests/scripts/tick1.cpp,
// kurtItemsDump), which must print the same bytes.
//
//   Probe <level> <steps> <schedule>
//
// The schedule is comma-separated: `key:from-to` holds a key over steps [from, to) (keys fwd,
// back, left, right, sleft, sright, jump, turbo, fire, use, next, prev), and `give:step:MODEL`
// hands Kurt a pickup before that step (LevelCommands.Give: Kurt.Collect, as the console's
// `give` does), and `drop:step:MODEL:dx:dy:dz` spawns a pickup (Spawning.Flagged, the pickups'
// flags) in Kurt's arena at the level's start plus that offset, for him to run through (a MODEL
// of the form A>B spawns model A named B). The
// goldens' schedules are in tick1.cpp's kItemsCases table.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/fire_probe -- copy
// src/Mdk.Formats, src/Mdk.Engine, src/Mdk.Game and Directory.Build.props into a scratch tree with
// this directory beside src/; in the scratch Mdk.Engine.csproj drop the shader targets and the
// icon resource; `dotnet build -c Release -o probe-bin`, then
// `DOTNET_ROOT=<sdk> MDK_DATA_DIR=<the MDK folder> probe-bin/Probe ...`.
//
// TWO MORE VIEWS of the same run, chosen by a leading word (2026-11-20):
//
//   Probe hud <level> <steps> <schedule>     the HUD's message stream (Hud/Messages.cs)
//   Probe ribbon <level> <steps> <schedule>  the twisters' ribbons (Scripts/Ribbon.cs Triangles)
//
// `hud` builds the C# Messages over MDKFONT.FTI's FONTBIG/FONTSML exactly as HudView does, wired
// as Viewer wires it (Inventory.PickedUp pushes the name, zoomed, for 2 s; ScriptRuntime.Messages
// takes the scripts' hud_message), updates it once a step at 1/60 s (Viewer.DrawScene) and draws
// it on a 600-wide canvas (the 3DS top screen's). The renderer and the two FontViews are made
// without their constructors (no window, no GPU): the renderer's canvas lists are set by
// reflection, so Renderer.DrawImage records each glyph's quad, and the FontViews get the fields
// their constructor would compute (FontView.Atlas's x, the max ascent; textures 1 and 2). The
// schedule also takes `msg:step:NAME:flags:seconds`, Messages.Push before that step. Each line is
// the current message and its time and zoom, the queue, and the quads drawn: their count and an
// FNV-1a hash of each one's texture and left, top, right, bottom bits, and every 10th step the
// quads themselves (texture, then the four as float bits).
//
// `ribbon` prints each step's twisters: the first one's Triangles in full (positions as float
// bits, UVs in texels) and, for every twister, its corner count and an FNV-1a hash of all its
// corners' bits. The C++ twins are `tick1 dump hud|ribbon ...`.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
var mode = args[0] is "hud" or "ribbon" ? args[0] : "";
if (mode.Length != 0)
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
    Yaw = levelData.Dti.StartAngle,
};
var cmi = Cmi.Load(data.PathOf($"TRAVERSE/LEVEL{level}/LEVEL{level}.CMI"));
// The triangle groups as the Viewer registers them (LevelView's constructor: every arena of
// the level, before the runtime), so the blasts find the groups' triangles (GroupCenters).
var groups = new TriangleGroups();
foreach (var arena in levelData.Arenas)
{
    groups.Add(arena);
}

var runtime = new ScriptRuntime(levelData, cmi, sprites, space, groups, mixer, kurt, 1) { Option = 1 };
var picked = new List<string>();
Mdk.Game.Hud.Messages? messages = null;
System.Reflection.FieldInfo? canvasVertices = null;
System.Reflection.FieldInfo? canvasCommands = null;
Mdk.Engine.Render.Renderer? canvas = null;
if (mode == "hud")
{
    var fti = Fti.Load(data.PathOf("MISC/MDKFONT.FTI"));
    var flags = BindingFlags.NonPublic | BindingFlags.Instance;
    canvas = (Mdk.Engine.Render.Renderer)System.Runtime.CompilerServices.RuntimeHelpers.GetUninitializedObject(typeof(Mdk.Engine.Render.Renderer));
    canvasVertices = typeof(Mdk.Engine.Render.Renderer).GetField("_canvasVertices", flags)!;
    canvasCommands = typeof(Mdk.Engine.Render.Renderer).GetField("_canvasCommands", flags)!;
    canvasVertices.SetValue(canvas, canvasVertices.FieldType.GetConstructor(Type.EmptyTypes)!.Invoke(null));
    canvasCommands.SetValue(canvas, canvasCommands.FieldType.GetConstructor(Type.EmptyTypes)!.Invoke(null));
    Mdk.Game.Hud.FontView View(string name, int space, int texture)
    {
        var font = Font.Parse(fti.GetBytes(name), space);
        var view = (Mdk.Game.Hud.FontView)System.Runtime.CompilerServices.RuntimeHelpers.GetUninitializedObject(typeof(Mdk.Game.Hud.FontView));
        var x = new int[256];
        var atlas = Mdk.Game.Hud.FontView.Atlas(font, "", x);
        void Set(string field, object value) => typeof(Mdk.Game.Hud.FontView).GetField(field, flags)!.SetValue(view, value);
        Set("_renderer", canvas);
        Set("_font", font);
        Set("_texture", texture);
        Set("_palette", 0);
        Set("_x", x);
        Set("_maxAscent", font.Glyphs.Max(g => g?.Ascent ?? 0));
        Set("_size", new Vector2(atlas.Width, atlas.Height));
        return view;
    }

    // HudView's fonts (FontSpace: 6 and 4), Viewer's subscriptions.
    messages = new Mdk.Game.Hud.Messages(fti, View("FONTBIG", 6, 1), View("FONTSML", 4, 2));
    var hud = messages;
    kurt.Inventory.PickedUp += name => hud.Push(name, Mdk.Game.Hud.Messages.FlagZoom, 2f);
}
else
{
    kurt.Inventory.PickedUp += name => picked.Add(name);
}

runtime.Messages = messages;
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

var keys = new Dictionary<string, Key>
{
    ["fwd"] = Key.Forward, ["back"] = Key.Back, ["left"] = Key.TurnLeft, ["right"] = Key.TurnRight,
    ["sleft"] = Key.StrafeLeft, ["sright"] = Key.StrafeRight, ["jump"] = Key.Jump, ["turbo"] = Key.Turbo,
    ["fire"] = Key.Fire, ["use"] = Key.UseItem, ["next"] = Key.ItemNext, ["prev"] = Key.ItemPrevious,
};
var holds = new List<(Key Key, int From, int To)>();
var gives = new List<(int Step, string Model)>();
var drops = new List<(int Step, string Model, Vector3 Offset)>();
var pushes = new List<(int Step, string Name, int Flags, float Seconds)>();
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
    if (parts[0] == "msg")
    {
        pushes.Add((int.Parse(parts[1]), parts[2], int.Parse(parts[3]), float.Parse(parts[4])));
        continue;
    }

    if (parts[0] == "give")
    {
        gives.Add((int.Parse(parts[1]), parts[2]));
        continue;
    }

    if (parts[0] == "drop")
    {
        drops.Add((int.Parse(parts[1]), parts[2], new Vector3(float.Parse(parts[3]), float.Parse(parts[4]), float.Parse(parts[5]))));
        continue;
    }

    var range = parts[1].Split('-');
    holds.Add((keys[parts[0]], int.Parse(range[0]), int.Parse(range[1])));
}

int P(float v) => (int)MathF.Floor(v * 64f);
string F(float v) => v.ToString("F4", CultureInfo.InvariantCulture);
var input = new Input();
Console.WriteLine($"{(mode.Length == 0 ? "" : mode + " ")}level={level} steps={steps} seed=1 schedule={schedule}");
for (var step = 1; step <= steps; step++)
{
    foreach (var (s, model) in gives)
    {
        if (s == step)
        {
            kurt.Collect(model);
        }
    }

    foreach (var (s, model, offset) in drops)
    {
        if (s == step)
        {
            // MODEL>NAME spawns MODEL and names it NAME: no retail arena has SW_EWJ's model, so
            // the cow it drops is reached through a pickup renamed to it.
            var spec = model.Split('>');
            var dropped = runtime.Spawn(runtime.GetArenaState(runtime.CurrentArena).Controller, spec[0],
                levelData.Dti.StartPosition + offset, 0f, -1, 0, ScriptRuntime.Spawning.Flagged);
            if (dropped != null && spec.Length > 1)
            {
                dropped.TypeName = spec[1];
            }
        }
    }

    foreach (var (s, name, flags, seconds) in pushes)
    {
        if (s == step)
        {
            messages?.Push(name, flags, seconds);
        }
    }

    foreach (var key in keys.Values)
    {
        var down = holds.Any(h => h.Key == key && step >= h.From && step < h.To);
        input.Hold(key, down ? Input.State.Down : Input.State.Up);
    }

    kurt.Update(input, 1f / 60f);
    runtime.Update(1f / 60f);
    if (mode == "hud")
    {
        messages!.Update(1f / 60f);
        HudLine(step);
        continue;
    }

    if (mode == "ribbon")
    {
        RibbonLine(step);
        continue;
    }

    var inventory = kurt.Inventory;
    var slots = string.Join("|", inventory.Slots.Select(s => $"{(int)s.Item}:{s.Count}"));
    var ammo = string.Join(":", inventory.Ammo);
    var ohp = 0;
    var thrown = new List<string>();
    var collected = new List<string>();
    foreach (var o in runtime.Objects)
    {
        if (o.Dead)
        {
            continue;
        }

        ohp += o.Health;
        if (o.ThrownKind > 0)
        {
            thrown.Add($"{o.ThrownKind}/{o.Flags & 0x5000:X}/{o.ItemTicks}/{o.TypeName}@{P(o.Position.X)},{P(o.Position.Y)},{P(o.Position.Z)}"
                + $"/y{F(o.Yaw)}/s{F(o.Scale)}/f{o.AnimationFrame}");
        }
        else if ((o.Flags & MdkObject.FlagCollected) != 0)
        {
            collected.Add($"{o.TypeName}/{F(o.Scale)}");
        }
    }

    var tw = Vector3.Zero;
    foreach (var t in runtime.Items.Twisters)
    {
        tw += t.Position;
    }

    var pickedText = picked.Count == 0 ? "-" : string.Join("|", picked);
    picked.Clear();
    Console.WriteLine($"s={step} state={kurt.Current} feet64={P(kurt.Feet.X)},{P(kurt.Feet.Y)},{P(kurt.Feet.Z)} yaw={F(kurt.Yaw)} " +
        $"frame={F(kurt.AnimationFrame)} hp={kurt.Health} kd={F(kurt.KnockDamage)} inv={(slots.Length == 0 ? "-" : slots)} sel={inventory.Selected} " +
        $"sgun={inventory.SuperChainGun} ammo={ammo}/{inventory.SelectedAmmo} picked={pickedText} objs={runtime.Objects.Count} ohp={ohp} " +
        $"kills={runtime.Stats.Kills} thrown={(thrown.Count == 0 ? "-" : string.Join(";", thrown))} " +
        $"collected={(collected.Count == 0 ? "-" : string.Join(";", collected))} " +
        $"bomb={(runtime.Items.Bomb is { Dead: false } ? 1 : 0)} decoy={(runtime.Items.Decoy is { Dead: false } ? 1 : 0)} " +
        $"tw={runtime.Items.Twisters.Count}@{P(tw.X)},{P(tw.Y)},{P(tw.Z)} pieces={runtime.Debris.Pieces.Count} effects={runtime.Effects.All.Count} " +
        $"white={F(kurt.WhiteFlash)} shake={F(runtime.Shake)} voices={Voices()}");
}

// A float's bits, the exact value (no decimal rounding to disagree on).
string B(float v) => BitConverter.SingleToInt32Bits(v).ToString("X8");

// A message text's bytes: printable ASCII as is, the rest as \xNN.
string Text(byte[] bytes)
{
    var text = new System.Text.StringBuilder();
    foreach (var b in bytes)
    {
        text.Append(b is >= 32 and <= 126 ? ((char)b).ToString() : $"\\x{b:X2}");
    }

    return text.ToString();
}

// The HUD view: the current message (flags:lines#time), its time and zoom, the queue, and every
// quad Messages.Draw(600) makes (texture:left,top,right,bottom as bits).
void HudLine(int step)
{
    var flags = BindingFlags.NonPublic | BindingFlags.Instance;
    var type = typeof(Mdk.Game.Hud.Messages);
    string Message(object? m)
    {
        if (m == null)
        {
            return "-";
        }

        var t = m.GetType();
        var lines = (List<byte[]>)t.GetProperty("Lines")!.GetValue(m)!;
        return $"{(int)t.GetProperty("Flags")!.GetValue(m)!}:{string.Join("/", lines.Select(Text))}#{F((float)t.GetProperty("Time")!.GetValue(m)!)}";
    }

    var current = type.GetField("_current", flags)!.GetValue(messages);
    var time = (float)type.GetField("_time", flags)!.GetValue(messages)!;
    var zoom = (float)type.GetField("_zoom", flags)!.GetValue(messages)!;
    var queue = ((System.Collections.IEnumerable)type.GetField("_queue", flags)!.GetValue(messages)!).Cast<object>().Select(Message).ToList();
    var vertices = (List<Mdk.Engine.Render.Vertex>)canvasVertices!.GetValue(canvas)!;
    var commands = (List<(int First, int Count, Mdk.Engine.Render.Material Material)>)canvasCommands!.GetValue(canvas)!;
    vertices.Clear();
    commands.Clear();
    messages!.Draw(600f);
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

    // Every quad's hash each step; the quads themselves every 10th step (the golden's size).
    Console.WriteLine($"s={step} cur={Message(current)} t={F(time)} z={F(zoom)} q={(queue.Count == 0 ? "-" : string.Join("|", queue))} " +
        $"draw={quads.Count}:{hash:X8}" + (step % 10 == 0 && quads.Count != 0 ? $" quads={string.Join(";", quads)}" : ""));
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

// The ribbon view: the first twister's Ribbon.Triangles in full, and every twister's corner count
// and FNV-1a hash of its corners' bits (x, y, z, u, v, little-endian bytes).
void RibbonLine(int step)
{
    var twisters = runtime.Items.Twisters;
    var line = new System.Text.StringBuilder($"s={step} tw={twisters.Count}");
    var hashes = new List<string>();
    for (var i = 0; i < twisters.Count; i++)
    {
        var corners = twisters[i].Ribbon.Triangles();
        var hash = 2166136261u;
        foreach (var (position, uv) in corners)
        {
            foreach (var v in new[] { position.X, position.Y, position.Z, uv.X, uv.Y })
            {
                hash = Fnv(hash, BitConverter.SingleToInt32Bits(v));
            }
        }

        hashes.Add($"{corners.Count}:{hash:X8}");
        if (i == 0)
        {
            line.Append($" r0={corners.Count}:");
            line.Append(corners.Count == 0 ? "-" : string.Join(";", corners.Select(c =>
                $"{B(c.Position.X)},{B(c.Position.Y)},{B(c.Position.Z)}/{c.Uv.X.ToString("F1", CultureInfo.InvariantCulture)},{c.Uv.Y.ToString("F1", CultureInfo.InvariantCulture)}")));
        }
    }

    line.Append($" h={(hashes.Count == 0 ? "-" : string.Join(",", hashes))}");
    Console.WriteLine(line.ToString());
}
