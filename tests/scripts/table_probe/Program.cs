using System.Collections;
using System.Globalization;
using System.Numerics;
using System.Reflection;
using Mdk.Engine.Render;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Formats;
using Mdk.Game.Audio;
using Mdk.Game.Collision;
using Mdk.Game.Level;
using Mdk.Game.Objects;
using Mdk.Game.Scripts;
using KurtT = Mdk.Game.Kurt.Kurt;

// THE LEVEL TICK TABLES' ORACLE: the C#'s own ScriptRuntime run with no Kurt input for N ticks
// at 1/30 s (seed 1, Option 1), and its object list printed. Its output is
// tests/scripts/golden/L<level>_t<ticks>.txt (the object table) and
// golden/L<level>_t<ticks>_script.txt (the script register file and the per-object VM state);
// the C++ twins are tick1.cpp's dumpTable and dumpScript, which must print the same bytes.
//
//   Probe <level> <ticks>           the object table: position, yaw, health, flags, the move
//                                   command, the door's connection and the path
//   Probe <level> <ticks> script    the target, the global variables, Kurt's combat state, the
//                                   mixer's live voices, the debris pieces, and per object its
//                                   script flags, variables, restart/wait, gosub stacks, level
//                                   timers and the two sounds it holds
//   Probe <level> <ticks> pose      per object (tick1's objectIdentity line) its PoseParts: per part
//                                   the vertex count, the float sum in order, the first and last
//   Probe <level> <ticks> transform per object its Transform (16 elements, %.6f)
//   Probe <level> <ticks> blend     per object PositionAt/YawAt/TransformAt at alphas 0-1 by 0.25
//   Probe <level> <ticks> layout    ObjectView's per-(model, palette) layout, in the draw order the
//                                   layout cache fills: batches, corners, UVs, Arrange under masks
//   Probe <level> <ticks> draw      ObjectView.Draw over every visible object into one frame's
//                                   stream: each queued draw's offset, corners, surface, palette
//                                   digest, frame, digest of its corners and the world it carries
//   Probe <level> <ticks> bound     the stream's capacity rule: the first three visible objects
//                                   drawn into a stream prefilled to capacity minus a chosen size
//   Probe <level> <ticks> runner x,y,z  Kurt's feet at x,y,z (yaw: the level's start), then per
//                                   tick the SW_H150's state after the tick and whether RUNNER plays
// These are the twins of tick1.cpp's dumpObjectPoses / dumpObjectTransforms / dumpObjectBlend /
// dumpObjectLayout / dumpObjectDraw / dumpStreamBound / dumpRunner (`tick1 dump <mode> ...`), and
// their goldens are golden/L<level>_t<ticks>_{pose,transform,blend,layout,draw,stream_bound}.txt
// and L7_t120_runner_{a,b}.txt (runner a: -21,-13,0.73; b: -26,-18,0.73).
//
// THE RENDERER SIDE (layout, draw, bound): ObjectView and MaterialResolver need a Renderer, which
// needs an SDL GPU device (and the shaders the scratch build drops). So Probe.csproj compiles
// upstream's own ObjectView.cs and MaterialResolver.cs, unmodified, into this probe (they shadow
// Mdk.Game's copies, warning CS0436), against the Renderer at the end of this file: the CPU half
// only -- Stream is the real Renderer.Stream (its capacity read from the real class), Draw records
// the queued draw, texture and palette creation hand out ids as the real one does (one list, in
// creation order). Nothing the dumps print depends on a GPU.
//
// The goldens were first made by a scratch driver that was lost; this probe was rebuilt
// 2026-11-26 from tick1.cpp's dumpTable/dumpScript and checked byte for byte against the
// goldens. Kurt is placed at the level's start and never updated (no input); the mixer runs
// on a muted device and is never updated, so its voice list is what the scripts started.
// Removed objects leave ScriptRuntime.Objects, so every listed object is printed.
//
// HOW IT IS BUILT AND RUN: as tests/scripts/cheats_probe -- copy src/Mdk.Formats,
// src/Mdk.Engine, src/Mdk.Game and Directory.Build.props (upstream C# 1.1.1, d5bb66ec) into a
// scratch tree with this directory beside src/; in the scratch Mdk.Engine.csproj drop the shader
// targets and the icon resource; `dotnet build -c Release -o probe-bin`, then
// `DOTNET_ROOT=<sdk> MDK_DATA_DIR=<the MDK folder> probe-bin/Probe 5 590`.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;
var flags = BindingFlags.NonPublic | BindingFlags.Instance;

var level = int.Parse(args[0]);
var ticks = int.Parse(args[1]);
var mode = args.Length > 2 ? args[2] : "table";
var script = mode == "script";
string[] modes = ["table", "script", "pose", "transform", "blend", "layout", "draw", "bound", "runner"];
if (!modes.Contains(mode))
{
    Console.Error.Write($"unknown mode {mode}\n");
    return 1;
}

var kurtAt = Vector3.Zero;
if (mode == "runner")
{
    var xyz = args.Length > 3 ? args[3].Split(',') : [];
    if (xyz.Length != 3)
    {
        Console.Error.Write("usage: Probe <level> <ticks> runner <x,y,z>\n");
        return 1;
    }

    kurtAt = new Vector3(float.Parse(xyz[0], CultureInfo.InvariantCulture), float.Parse(xyz[1], CultureInfo.InvariantCulture),
        float.Parse(xyz[2], CultureInfo.InvariantCulture));
}

// The world, as cheats_probe builds it: the reachable arenas for the space, every arena's
// triangle groups, the level's sounds preloaded, the sprites archive.
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
var kurt = new KurtT(space, mixer, _ => 0)
{
    Feet = mode == "runner" ? kurtAt : levelData.Dti.StartPosition,
    Yaw = levelData.Dti.StartAngle,
};
var cmi = Cmi.Load(data.PathOf($"TRAVERSE/LEVEL{level}/LEVEL{level}.CMI"));
var groups = new TriangleGroups();
foreach (var arena in levelData.Arenas)
{
    groups.Add(arena);
}

var runtime = new ScriptRuntime(levelData, cmi, sprites, space, groups, mixer, kurt, 1) { Option = 1 };
string F(float v) => v.ToString("F4", CultureInfo.InvariantCulture);

// RUNNER ticks inside the dump, one line per tick per SW_H150 (tick1's dumpRunner).
if (mode == "runner")
{
    Console.Write($"level={level} ticks={ticks} seed=1 kurt={F(kurtAt.X)},{F(kurtAt.Y)},{F(kurtAt.Z)}\n");
    for (var t = 1; t <= ticks; t++)
    {
        runtime.Update(1f / 30f);
        var live = runtime.Objects;
        for (var i = 0; i < live.Count; i++)
        {
            var o = live[i];
            if (o.TypeName != "SW_H150")
            {
                continue;
            }

            Console.Write($"t={t} obj={i:D3} pos={F(o.Position.X)},{F(o.Position.Y)},{F(o.Position.Z)} yaw={F(o.Yaw)} " +
                          $"anim={o.Animation?.Name ?? "-"} frame={o.AnimationFrame} push={F(o.Push.X)},{F(o.Push.Y)},{F(o.Push.Z)} " +
                          $"flags=0x{o.Flags:X8} mixer={(mixer.IsPlaying("RUNNER") ? "RUNNER" : "-")}\n");
        }
    }

    return 0;
}

for (var t = 0; t < ticks; t++)
{
    runtime.Update(1f / 30f);
}

var objects = runtime.Objects;

if (mode is "pose" or "transform" or "blend" or "layout" or "draw" or "bound")
{
    ObjectDumps.Run(mode, level, ticks, objects, levelData);
    return 0;
}

if (!script)
{
    Console.Write($"level={level} ticks={ticks} seed=1 currentArena={runtime.CurrentArena} objects={objects.Count}\n");
    for (var i = 0; i < objects.Count; i++)
    {
        var o = objects[i];
        Console.Write($"{i:D3} type={o.TypeName} arena={o.Arena} pos=({F(o.Position.X)},{F(o.Position.Y)},{F(o.Position.Z)}) " +
                      $"yaw={F(o.Yaw)} hp={o.Health} flags=0x{o.Flags:X8} dead={(o.Dead ? 1 : 0)} " +
                      $"move={o.MoveCommand}/{o.MoveParameter} connects={o.Connects} " +
                      $"path={o.Path}/{F(o.PathTime)}/{F(o.PathSpeed)}/{o.PathStop}\n");
    }

    return 0;
}

// SCRIPT: the target the scripts aim at this tick, and the aliens' target's index in the list.
var target = runtime.TargetPosition;
var alien = runtime.AlienTarget == null ? -1 : objects.IndexOf(runtime.AlienTarget);
Console.Write($"SCRIPT level={level} ticks={ticks} seed=1 currentArena={runtime.CurrentArena} objects={objects.Count} " +
              $"target_pos={F(target.X)},{F(target.Y)},{F(target.Z)} target_yaw={F(runtime.TargetYaw)} alien_target={alien}\n");
var g = runtime.GlobalVariables;
Console.Write($"GVAR {F(g[0])} {F(g[1])} {F(g[2])} {F(g[3])}\n");

// KURT: what hurt_kurt (109) writes and if_inventory (175) reads.
Console.Write($"KURT health={kurt.Health} hurt_flash={F(kurt.HurtFlash)} invulnerable={F(kurt.Invulnerable)} " +
              $"knock={F(kurt.KnockDamage)} state={(int)kurt.Current} slots={kurt.Inventory.Slots.Count}\n");

// AUDIO: the mixer's voices (SoundMixer._voices, a private list of a private class), id:name.
var voices = (IList)typeof(SoundMixer).GetField("_voices", flags)!.GetValue(mixer)!;
var list = new List<string>();
foreach (var voice in voices)
{
    var type = voice!.GetType();
    list.Add($"{type.GetField("Id")!.GetValue(voice)}:{type.GetField("Name")!.GetValue(voice)}");
}

Console.Write($"AUDIO voices={voices.Count} list={string.Join(",", list)}\n");

// DEBRIS: the shattered groups and sparks in flight.
var pieces = runtime.Debris.Pieces.Select(p =>
    $"{p.Arena}@{F(p.Center.X)},{F(p.Center.Y)},{F(p.Center.Z)}v{F(p.Velocity.X)},{F(p.Velocity.Y)},{F(p.Velocity.Z)}t{p.Ticks}");
Console.Write($"DEBRIS pieces={runtime.Debris.Pieces.Count} list={string.Join(",", pieces)}\n");

for (var i = 0; i < objects.Count; i++)
{
    var o = objects[i];
    var v = o.Variables;
    var lt = o.LevelTimers;
    Console.Write($"OBJ {i:D3} type={o.TypeName} sflags=0x{o.ScriptFlags:X8} vars={F(v[0])},{F(v[1])},{F(v[2])},{F(v[3])} " +
                  $"restart={o.Restart} wait={F(o.WaitTime)} wresume={o.WaitResume} death={o.DeathScript} cmdtgt={o.CommandTarget} " +
                  $"gret={string.Join(",", o.GosubReturns)} gres={string.Join(",", o.GosubRestarts)} " +
                  $"timers={F(lt[0])},{F(lt[1])},{F(lt[2])},{F(lt[3])},{F(lt[4])} " +
                  $"tsnd={o.TrackedSound} tvoice={o.TrackedVoice} tplay={(mixer.IsVoicePlaying(o.TrackedVoice) ? 1 : 0)} " +
                  $"lsnd={o.LoopSoundName} lvoice={o.LoopSound} lplay={(mixer.IsVoicePlaying(o.LoopSound) ? 1 : 0)}\n");
}

return 0;

/// <summary>The object-pass dumps: tick1.cpp's dumpObjectPoses, dumpObjectTransforms,
/// dumpObjectBlend, dumpObjectLayout, dumpObjectDraw and dumpStreamBound, field for field.</summary>
internal static class ObjectDumps
{
    private const BindingFlags Private = BindingFlags.NonPublic | BindingFlags.Instance;
    private const BindingFlags PrivateStatic = BindingFlags.NonPublic | BindingFlags.Static;
    private static readonly float[] Alphas = [0f, 0.25f, 0.5f, 0.75f, 1f];

    private static string F(float v) => v.ToString("F4", CultureInfo.InvariantCulture);
    private static string F6(float v) => v.ToString("F6", CultureInfo.InvariantCulture);
    private static uint Bits(float v) => BitConverter.SingleToUInt32Bits(v);

    public static void Run(string mode, int level, int ticks, IReadOnlyList<MdkObject> objects, LevelData levelData)
    {
        var o = Console.Out;
        switch (mode)
        {
            case "pose":
                o.Write($"level={level} ticks={ticks} seed=1 objects={objects.Count}\n");
                for (var i = 0; i < objects.Count; i++)
                {
                    Pose(i, objects[i]);
                }

                break;
            case "transform":
                o.Write($"level={level} ticks={ticks} seed=1 objects={objects.Count}\n");
                for (var i = 0; i < objects.Count; i++)
                {
                    o.Write(Identity(i, objects[i]));
                    var t = Elements(objects[i].Transform);
                    o.Write("  T " + string.Join(" ", t.Select(F6)) + "\n");
                }

                break;
            case "blend":
                o.Write($"BLEND level={level} ticks={ticks} seed=1 objects={objects.Count} alphas={Alphas.Length}\n");
                for (var i = 0; i < objects.Count; i++)
                {
                    var obj = objects[i];
                    o.Write(Identity(i, obj));
                    foreach (var alpha in Alphas)
                    {
                        var at = obj.PositionAt(alpha);
                        o.Write($"  A alpha={F(alpha)} pos={F6(at.X)},{F6(at.Y)},{F6(at.Z)} yaw={F6(obj.YawAt(alpha))}\n");
                        o.Write(MatrixLine("T", obj.TransformAt(alpha)));
                    }
                }

                break;
            default:
                new RenderSide(levelData).Run(mode, level, ticks, objects);
                break;
        }
    }

    /// <summary>tick1.cpp's objectIdentity (roll left out on purpose there).</summary>
    private static string Identity(int index, MdkObject o) =>
        $"OBJ {index:D3} type={o.TypeName} arena={o.Arena} model={o.Model?.Name ?? "-"} anim={o.Animation?.Name ?? "-"} " +
        $"frame={o.AnimationFrame} hidden={o.HiddenParts} visible={(o.Visible ? 1 : 0)} dead={(o.Dead ? 1 : 0)} " +
        $"scale={F(o.Scale)} pos={F(o.Position.X)},{F(o.Position.Y)},{F(o.Position.Z)} yaw={F(o.Yaw)} pitch={F(o.Pitch)} " +
        $"effect={o.EffectFrames} texframe={o.TextureFrame}\n";

    private static void Pose(int index, MdkObject obj)
    {
        var o = Console.Out;
        o.Write(Identity(index, obj));
        var pose = obj.PoseParts();
        o.Write($"  PARTS {pose.Length}\n");
        for (var p = 0; p < pose.Length; p++)
        {
            var v = pose[p];
            float sx = 0f, sy = 0f, sz = 0f;
            foreach (var x in v)
            {
                sx += x.X;
                sy += x.Y;
                sz += x.Z;
            }

            var first = v.Length == 0 ? "-" : $"{F(v[0].X)},{F(v[0].Y)},{F(v[0].Z)}";
            var last = v.Length == 0 ? "-" : $"{F(v[^1].X)},{F(v[^1].Y)},{F(v[^1].Z)}";
            o.Write($"  P {p} n={v.Length} sum={F(sx)},{F(sy)},{F(sz)} first={first} last={last}\n");
        }
    }

    private static float[] Elements(Matrix4x4 m) =>
        [m.M11, m.M12, m.M13, m.M14, m.M21, m.M22, m.M23, m.M24, m.M31, m.M32, m.M33, m.M34, m.M41, m.M42, m.M43, m.M44];

    /// <summary>tick1.cpp's matrixLine: %.6f, then the raw bits.</summary>
    private static string MatrixLine(string label, Matrix4x4 m)
    {
        var e = Elements(m);
        return $"  {label} " + string.Join(" ", e.Select(F6)) + " b=" + string.Join(" ", e.Select(v => Bits(v).ToString("X8"))) + "\n";
    }

    /// <summary>The renderer side: upstream's ObjectView and MaterialResolver (compiled into this probe)
    /// over the CPU-only Renderer below, with Viewer.ObjectLook's per-arena looks.</summary>
    private sealed class RenderSide
    {
        private readonly LevelData _level;
        private readonly Renderer _renderer = new();
        private readonly MaterialResolver _resolver;
        private readonly ObjectView _view;
        private readonly Dictionary<string, ObjectView.Look> _looks = [];

        public RenderSide(LevelData level)
        {
            _level = level;
            _resolver = new MaterialResolver(_renderer, level.Dti);
            _view = new ObjectView(_renderer, _resolver);
        }

        public void Run(string mode, int level, int ticks, IReadOnlyList<MdkObject> objects)
        {
            switch (mode)
            {
                case "layout":
                    Layout(level, ticks, objects);
                    break;
                case "draw":
                    Draw(level, ticks, objects);
                    break;
                case "bound":
                    Bound(level, ticks, objects);
                    break;
            }
        }

        /// <summary>Viewer.ObjectLook.</summary>
        private ObjectView.Look LookOf(string name)
        {
            if (_looks.TryGetValue(name, out var look))
            {
                return look;
            }

            var arena = _level.ArenaNamed(name);
            return _looks[name] = arena != null
                ? new ObjectView.Look(_level.PaletteOf(arena), _level.ArchivesOf(arena))
                : new ObjectView.Look(_level.Dti.Palette, [_level.LevelTextures]);
        }

        // ObjectView's private members.
        private object? LayoutOf(Model model, ObjectView.Look look) =>
            typeof(ObjectView).GetMethod("LayoutOf", Private)!.Invoke(_view, [model, look]);

        private static int Arrange(object layout, int hidden) =>
            (int)typeof(ObjectView).GetMethod("Arrange", PrivateStatic)!.Invoke(null, [layout, hidden])!;

        private static int Visible(object batch, int hidden) =>
            (int)typeof(ObjectView).GetMethod("Visible", PrivateStatic)!.Invoke(null, [batch, hidden])!;

        private static T Field<T>(object of, string name) => (T)of.GetType().GetField(name, BindingFlags.Public | BindingFlags.Instance)!.GetValue(of)!;

        /// <summary>A texture id's name (MaterialResolver._textures inverted); "flat" for none.</summary>
        private string TextureName(int id)
        {
            if (id == Material.None)
            {
                return "flat";
            }

            var textures = (Dictionary<Texture, int>)typeof(MaterialResolver).GetField("_textures", Private)!.GetValue(_resolver)!;
            return textures.First(t => t.Value == id).Key.Name;
        }

        private string SurfaceText(Material m) =>
            $"tex={TextureName(m.Texture)} colour={F(m.Colour.X)},{F(m.Colour.Y)},{F(m.Colour.Z)},{F(m.Colour.W)} " +
            $"frames={m.FrameCount} pass={m.Pass} shading={m.Shading} rowshift={F(m.RowShift)}";

        /// <summary>ObjectView.Draw's refusal (no model, dead, no layout).</summary>
        private object? Drawable(MdkObject obj, ObjectView.Look look) =>
            obj.Model == null || obj.Dead ? null : LayoutOf(obj.Model, look);

        private void Layout(int level, int ticks, IReadOnlyList<MdkObject> objects)
        {
            var o = Console.Out;
            var seen = new List<(Model, Palette)>();
            o.Write($"level={level} ticks={ticks} seed=1 objects={objects.Count}\n");
            for (var index = 0; index < objects.Count; index++)
            {
                var obj = objects[index];
                if (!obj.Visible || obj.Dead || obj.Model == null)
                {
                    continue;
                }

                var look = LookOf(obj.Arena);
                if (seen.Contains((obj.Model, look.Palette)))
                {
                    continue;
                }

                seen.Add((obj.Model, look.Palette));
                var layout = LayoutOf(obj.Model, look);
                if (layout == null)
                {
                    o.Write($"LAYOUT obj={index} model={obj.Model.Name} arena={obj.Arena} batches=0\n");
                    continue;
                }

                var batches = Field<IList>(layout, "Batches");
                o.Write($"LAYOUT obj={index} model={obj.Model.Name} arena={obj.Arena} batches={batches.Count}\n");
                for (var b = 0; b < batches.Count; b++)
                {
                    var batch = batches[b]!;
                    var parts = Field<List<int>>(batch, "Parts");
                    var uvs = Field<List<Vector2>>(batch, "Uvs");
                    var partCorners = Field<int[]>(batch, "PartCorners");
                    o.Write($"  B{b} {SurfaceText(Field<Material>(batch, "Material"))} corners={parts.Count}\n");
                    o.Write($"    parts={string.Join(",", parts)}\n");
                    o.Write($"    indices={string.Join(",", Field<List<int>>(batch, "Indices"))}\n");
                    for (var p = 0; p < partCorners.Length; p++)
                    {
                        if (partCorners[p] == 0)
                        {
                            continue;
                        }

                        float su = 0f, sv = 0f;
                        var n = 0;
                        string first = "-", last = "-";
                        for (var c = 0; c < parts.Count; c++)
                        {
                            if (parts[c] != p)
                            {
                                continue;
                            }

                            su += uvs[c].X;
                            sv += uvs[c].Y;
                            var text = $"{F(uvs[c].X)},{F(uvs[c].Y)}";
                            if (n == 0)
                            {
                                first = text;
                            }

                            last = text;
                            n++;
                        }

                        o.Write($"    uv p={p} n={n} sum={F(su)},{F(sv)} first={first} last={last}\n");
                    }
                }

                foreach (var hidden in new[] { 0, 1, 3, obj.HiddenParts, -1 })
                {
                    var count = Arrange(layout, hidden);
                    var order = Field<int[]>(layout, "Order");
                    var shown = Enumerable.Range(0, count).Select(i => order[i]).ToList();
                    var visible = shown.Select(i => Visible(batches[i]!, hidden));
                    o.Write($"  A hidden={hidden} count={count} order={string.Join(",", shown)} visible={string.Join(",", visible)}\n");
                }
            }
        }

        /// <summary>FNV-1a over the five floats of each vertex, in stream order.</summary>
        private static uint Hash(List<Vertex> vertices, int first, int corners)
        {
            var hash = 2166136261u;
            for (var i = 0; i < corners; i++)
            {
                var v = vertices[first + i];
                foreach (var value in (ReadOnlySpan<float>)[v.Position.X, v.Position.Y, v.Position.Z, v.Uv.X, v.Uv.Y])
                {
                    hash ^= Bits(value);
                    hash *= 16777619u;
                }
            }

            return hash;
        }

        private static uint PaletteDigest(Palette palette)
        {
            var hash = 2166136261u;
            foreach (var b in palette.Rgba)
            {
                hash ^= b;
                hash *= 16777619u;
            }

            return hash;
        }

        private string QueueLine(int index, int count, int queued, int start)
        {
            var stream = _renderer.Streamed;
            return $"  Q obj={index} count={count} queued={queued} stream={stream.Count} " +
                   $"hash={Hash(stream, start, stream.Count - start):X8}\n";
        }

        private void Draw(int level, int ticks, IReadOnlyList<MdkObject> objects)
        {
            var o = Console.Out;
            _renderer.Reset(0, default);
            o.Write($"level={level} ticks={ticks} seed=1 objects={objects.Count}\n");
            for (var index = 0; index < objects.Count; index++)
            {
                var obj = objects[index];
                if (!obj.Visible)
                {
                    continue;
                }

                var look = LookOf(obj.Arena);
                if (Drawable(obj, look) is not { } layout)
                {
                    continue;
                }

                var before = _renderer.Draws.Count;
                var start = _renderer.Streamed.Count;
                _view.Draw(obj, look, obj.Transform);
                var count = Arrange(layout, obj.HiddenParts);
                o.Write($"DRAW obj={index} model={obj.Model!.Name} arena={obj.Arena} hidden={obj.HiddenParts} " +
                        $"shines={(obj.EffectFrames > 0 ? 1 : 0)} count={count}\n");
                var stream = _renderer.Streamed;
                for (var d = before; d < _renderer.Draws.Count; d++)
                {
                    var draw = _renderer.Draws[d];
                    float sx = 0f, sy = 0f, sz = 0f, su = 0f, sv = 0f;
                    for (var i = 0; i < draw.Count; i++)
                    {
                        var v = stream[draw.First + i];
                        sx += v.Position.X;
                        sy += v.Position.Y;
                        sz += v.Position.Z;
                        su += v.Uv.X;
                        sv += v.Uv.Y;
                    }

                    var head = stream[draw.First];
                    var tail = stream[draw.First + draw.Count - 1];
                    var m = draw.Material;
                    o.Write($"  B{d - before} first={draw.First} corners={draw.Count} frame={draw.Frame} pal={PaletteDigest(look.Palette):X8} " +
                            $"{SurfaceText(m)} sum={F(sx)},{F(sy)},{F(sz)} uvsum={F(su)},{F(sv)} " +
                            $"first={F(head.Position.X)},{F(head.Position.Y)},{F(head.Position.Z)},{F(head.Uv.X)},{F(head.Uv.Y)} " +
                            $"last={F(tail.Position.X)},{F(tail.Position.Y)},{F(tail.Position.Z)},{F(tail.Uv.X)},{F(tail.Uv.Y)} " +
                            $"hash={Hash(stream, draw.First, draw.Count):X8}\n");
                    o.Write(MatrixLine("W", draw.World));
                }

                o.Write(QueueLine(index, count, _renderer.Draws.Count - before, start));
            }

            o.Write($"STREAM vertices={_renderer.Streamed.Count}\n");
        }

        private void Bound(int level, int ticks, IReadOnlyList<MdkObject> objects)
        {
            var o = Console.Out;
            var picks = new List<(int Index, MdkObject Obj)>();
            for (var index = 0; index < objects.Count && picks.Count < 3; index++)
            {
                if (objects[index].Visible)
                {
                    picks.Add((index, objects[index]));
                }
            }

            var capacity = Renderer.Capacity;
            int[] prefills = [0, capacity - 300, capacity - 299, capacity - 150, capacity - 149, capacity - 90, capacity - 60, capacity];
            o.Write($"level={level} ticks={ticks} seed=1 objects={objects.Count} picks={picks.Count}\n");
            var filler = new Vertex(new Vector3(1f, 2f, 3f), new Vector2(0.25f, 0.5f));
            foreach (var prefill in prefills)
            {
                o.Write($"BOUND prefill={prefill}\n");
                _renderer.Reset(prefill, filler);
                foreach (var (index, obj) in picks)
                {
                    var look = LookOf(obj.Arena);
                    if (Drawable(obj, look) is not { } layout)
                    {
                        continue;
                    }

                    var before = _renderer.Draws.Count;
                    var start = _renderer.Streamed.Count;
                    _view.Draw(obj, look, obj.Transform);
                    var count = Arrange(layout, obj.HiddenParts);
                    o.Write(QueueLine(index, count, _renderer.Draws.Count - before, start));
                }

                o.Write($"STREAM vertices={_renderer.Streamed.Count}\n");
            }
        }
    }
}

namespace Mdk.Engine.Render
{
    /// <summary>The CPU half of Mdk.Engine's Renderer, as much as ObjectView and MaterialResolver call:
    /// no SDL device exists here. It shadows the real class inside this probe only (CS0436).</summary>
    public sealed class Renderer
    {
        /// <summary>The real Renderer's StreamVertices (1 &lt;&lt; 18), read from the real class.</summary>
        public static readonly int Capacity = (int)typeof(Vertex).Assembly.GetType("Mdk.Engine.Render.Renderer")!
            .GetField("StreamVertices", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!.GetRawConstantValue()!;

        private const int StreamMesh = 1;
        private int _textures = 1;  // texture 0: the real constructor's 1x1 for flat colours

        public readonly record struct Queued(int Mesh, int First, int Count, Material Material, int Frame, System.Numerics.Matrix4x4 World);

        public List<Vertex> Streamed { get; } = [];
        public List<Queued> Draws { get; } = [];

        /// <summary>A frame's start, with the stream prefilled (the bound dump).</summary>
        public void Reset(int prefill, Vertex filler)
        {
            Streamed.Clear();
            Draws.Clear();
            for (var i = 0; i < prefill; i++)
            {
                Streamed.Add(filler);
            }
        }

        /// <summary>Renderer.Stream, verbatim but for the list's name.</summary>
        public Span<Vertex> Stream(int count, out int mesh, out int first)
        {
            mesh = StreamMesh;
            first = Streamed.Count;
            if (first + count > Capacity)
            {
                return [];
            }

            System.Runtime.InteropServices.CollectionsMarshal.SetCount(Streamed, first + count);
            return System.Runtime.InteropServices.CollectionsMarshal.AsSpan(Streamed).Slice(first, count);
        }

        public void Draw(int mesh, int firstVertex, int vertexCount, Material material, int frame, System.Numerics.Matrix4x4 world) =>
            Draws.Add(new Queued(mesh, firstVertex, vertexCount, material, frame, world));

        public int CreateIndexTexture(int width, int height, byte[] indices) => _textures++;

        public int CreatePalette(byte[] rgba) => _textures++;

        public void UpdateTexture(int texture, int width, int height, byte[] pixels)
        {
        }

        public void Prepare(Material material)
        {
        }

        public void Replace(Material material, int width, int height, byte[] rgba)
        {
        }
    }
}
