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
// The triangle groups as the Viewer registers them (LevelView's constructor: every arena of
// the level, before the runtime), so the blasts find the groups' triangles (GroupCenters).
var groups = new TriangleGroups();
foreach (var arena in levelData.Arenas)
{
    groups.Add(arena);
}

var runtime = new ScriptRuntime(levelData, cmi, sprites, space, groups, mixer, kurt, 1) { Option = 1 };
var picked = new List<string>();
kurt.Inventory.PickedUp += name => picked.Add(name);
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
foreach (var token in schedule.Split(',', StringSplitOptions.RemoveEmptyEntries))
{
    var parts = token.Split(':');
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

    foreach (var key in keys.Values)
    {
        var down = holds.Any(h => h.Key == key && step >= h.From && step < h.To);
        input.Hold(key, down ? Input.State.Down : Input.State.Up);
    }

    kurt.Update(input, 1f / 60f);
    runtime.Update(1f / 60f);

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

    var messages = picked.Count == 0 ? "-" : string.Join("|", picked);
    picked.Clear();
    Console.WriteLine($"s={step} state={kurt.Current} feet64={P(kurt.Feet.X)},{P(kurt.Feet.Y)},{P(kurt.Feet.Z)} yaw={F(kurt.Yaw)} " +
        $"frame={F(kurt.AnimationFrame)} hp={kurt.Health} kd={F(kurt.KnockDamage)} inv={(slots.Length == 0 ? "-" : slots)} sel={inventory.Selected} " +
        $"sgun={inventory.SuperChainGun} ammo={ammo}/{inventory.SelectedAmmo} picked={messages} objs={runtime.Objects.Count} ohp={ohp} " +
        $"kills={runtime.Stats.Kills} thrown={(thrown.Count == 0 ? "-" : string.Join(";", thrown))} " +
        $"collected={(collected.Count == 0 ? "-" : string.Join(";", collected))} " +
        $"bomb={(runtime.Items.Bomb is { Dead: false } ? 1 : 0)} decoy={(runtime.Items.Decoy is { Dead: false } ? 1 : 0)} " +
        $"tw={runtime.Items.Twisters.Count}@{P(tw.X)},{P(tw.Y)},{P(tw.Z)} pieces={runtime.Debris.Pieces.Count} effects={runtime.Effects.All.Count} " +
        $"white={F(kurt.WhiteFlash)} shake={F(runtime.Shake)} voices={Voices()}");
}
