using System.Globalization;
using System.Numerics;
using System.Text;
using Mdk.Formats;
using Mdk.Game.Fall;
using Mdk.Game.Flow;
using Mdk.Game.Kurt;

// THE FALL ORACLE: the C# FallSim (src/Mdk.Game/Fall/FallSim*.cs, FallMissile.cs) stepped at a
// fixed delta under a scripted steering, from a seeded System.Random. It pins the whole
// simulation of the pre-level skydive: the intro in space, Kurt's steering and the pull back to
// the centre, the camera and its braking, the radars, the missiles and their hits, the
// explosions, the pickups on their chutes and the ones Kurt takes (Inventory.Collect), Bones,
// the palette effects, the end (Landed or GameOver), and what the fall carries into the level
// (FallScreen.Next's FallCarry, applied as Viewer.CarryFromFall applies it). Its output is
// tests/scripts/golden/L*_skydive_*.txt, and the C++ twin is `tick1 dump skydive ...`
// (tests/scripts/skydive_dump.cpp, skydiveDump), which must print the same bytes.
//
//   Probe <level> <difficulty> <seed> <steps> <delta> <schedule> [full]
//
// <level> is the LEVELn whose fall it is (7, 6, 3, 4, 8: GameState.Order's first five, the fall
// index 0-4 as FallScreen computes it); <difficulty> 0 easy, 1 normal, 2 hard; <delta> the
// seconds per step (FallScreen passes the frame's elapsed time). The setup is FallScreen's:
// FALLPU_n of FALL3D.BNI, and the KURTANIM, KURT_HIT and BONESANM animations of the same BNI
// (FallModels.Animation).
//
// The schedule is comma-separated: `key:from-to` holds a key over steps [from, to) (keys left,
// right, up, down, and chase: steer towards the first pickup under its chute, one axis at a
// time by the sign of the gap when it is a unit or more), and `skip:step` presses Esc
// (FallSim.Skip) before that step's update, as FallScreen.Frame does. `-` is no schedule.
//
// Each step prints one short line (the state, the ticks, the health, the events -- sounds and
// messages in the order they fired -- and an FNV-1a hash of every public float and int of the
// sim, every missile with its trail, every pickup and explosion). Every 10th step, every step
// with an event, and every step in `full` mode also print the full state, floats as their
// bits. The run stops at the end of the fall, and the last lines are the carry (health and
// pickups) and the level's inventory after it was carried in.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/fire_probe -- copy
// src/Mdk.Formats, src/Mdk.Engine, src/Mdk.Game and Directory.Build.props into a scratch tree
// with this directory beside src/; in the scratch Mdk.Engine.csproj drop the shader targets and
// the icon resource; `dotnet build -c Release -o probe-bin`, then
// `DOTNET_ROOT=<sdk> MDK_DATA_DIR=<the MDK folder> probe-bin/Probe ...`.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;

var level = int.Parse(args[0]);
var difficulty = (Difficulty)int.Parse(args[1]);
var seed = int.Parse(args[2]);
var steps = int.Parse(args[3]);
var delta = float.Parse(args[4]);
var schedule = args[5];
var fullMode = args.Length > 6 && args[6] == "full";

var data = MdkData.Find() ?? throw new Exception("no data");
// FallScreen: the falls are those of the level indices 0-4 (LEVEL5 has none: it clamps to 4).
var index = Math.Clamp(GameState.IndexOf(level), 0, 4);
var n = index + 1;
var bni = Bni.Load(data.PathOf("FALL3D/FALL3D.BNI"));
FallSim.Animation Animation(string name)
{
    var animation = ModelAnimation.Parse(name, bni.Bytes, bni.Entries[name].Offset);
    return new FallSim.Animation(animation.Speed, animation.FrameCount);
}

var setup = new FallSim.Setup(index, difficulty, Fall3d.Pickups(bni, $"FALLPU_{n}"),
    Animation("KURTANIM"), Animation("KURT_HIT"), Animation("BONESANM"));
var sim = new FallSim(setup, new Random(seed));
var events = new List<string>();
sim.Sound += name => events.Add(name);
sim.Message += name => events.Add("msg:" + name);

static string B(float v) => BitConverter.SingleToUInt32Bits(v).ToString("X8");
static string V3(Vector3 v) => $"{B(v.X)},{B(v.Y)},{B(v.Z)}";

var plan = new List<(string Key, int From, int To)>();
if (schedule != "-")
{
    foreach (var part in schedule.Split(','))
    {
        var f = part.Split(':');
        if (f[0] == "skip")
        {
            var at = int.Parse(f[1]);
            plan.Add(("skip", at, at + 1));
            continue;
        }

        var range = f[1].Split('-');
        plan.Add((f[0], int.Parse(range[0]), int.Parse(range[1])));
    }
}

bool Held(string key, int s) => plan.Any(p => p.Key == key && s >= p.From && s < p.To);

Console.WriteLine($"fall level={level} index={index} difficulty={(int)difficulty} seed={seed} steps={steps} delta={B(delta)} schedule={schedule}");
Console.WriteLine($"setup beam={B(sim.BeamSpeed)} missiles={sim.MissilesPerDetection} spread={B(sim.Spread)} mgap={sim.MissileGap} rgap={sim.RadarGap} pickups={string.Join(",", setup.Pickups)} kurt={B(setup.Kurt.Speed)}:{setup.Kurt.FrameCount} hit={B(setup.KurtHit.Speed)}:{setup.KurtHit.FrameCount} bones={B(setup.Bones.Speed)}:{setup.Bones.FrameCount}");

static string Inv(Inventory inv) =>
    (inv.Slots.Count == 0 ? "-" : string.Join(";", inv.Slots.Select(s => $"{(int)s.Item}:{s.Count}")))
    + $" sel={inv.Selected} ammo={string.Join(":", inv.Ammo)}/{inv.SelectedAmmo} sgun={inv.SuperChainGun}";

uint hash = 0;
void H(uint v)
{
    for (var i = 0; i < 4; i++)
    {
        hash ^= (v >> (8 * i)) & 0xFF;
        hash *= 16777619u;
    }
}

void HF(float v) => H(BitConverter.SingleToUInt32Bits(v));
void HV(Vector3 v)
{
    HF(v.X);
    HF(v.Y);
    HF(v.Z);
}

void HS(string s)
{
    foreach (var c in s)
    {
        H(c);
    }

    H(0xFFFFFFFFu);
}

for (var s = 1; s <= steps && sim.State == FallSim.Outcome.Running; s++)
{
    events.Clear();
    if (Held("skip", s))
    {
        sim.Skip();
    }

    var x = (Held("right", s) ? 1 : 0) - (Held("left", s) ? 1 : 0);
    var y = (Held("up", s) ? 1 : 0) - (Held("down", s) ? 1 : 0);
    if (Held("chase", s))
    {
        var k = sim.KurtPosition;
        foreach (var pickup in sim.Pickups)
        {
            if (!pickup.Chute)
            {
                continue;
            }

            var dx = pickup.Position.X - k.X;
            var dy = pickup.Position.Y - k.Y;
            x = MathF.Abs(dx) < 1f ? 0 : MathF.Sign(dx);
            y = MathF.Abs(dy) < 1f ? 0 : MathF.Sign(dy);
            break;
        }
    }

    sim.Update(delta, new Vector2(x, y));

    hash = 2166136261u;
    H((uint)sim.State);
    H((uint)sim.Ticks);
    H((uint)sim.IntroLeft);
    HF(sim.Time);
    H(sim.Dying ? 1u : 0u);
    H((uint)sim.Health);
    HV(sim.KurtPosition);
    H(sim.KurtVisible ? 1u : 0u);
    H((uint)sim.KurtPose);
    HF(sim.KurtFrame);
    HV(sim.CameraPosition);
    H(sim.BonesPosition.HasValue ? 1u : 0u);
    if (sim.BonesPosition is { } bp)
    {
        HV(bp);
    }

    HF(sim.BonesFrame);
    H((uint)sim.Wind);
    H((uint)sim.HazeFrame);
    HF(sim.CrawlerFrame);
    HF(sim.Whiten);
    HF(sim.Dark);
    HF(sim.Red);
    H(sim.RadarActive ? 1u : 0u);
    H((uint)sim.Radar);
    HV(sim.RadarSpot);
    HF(sim.RadarSpotRadius);
    H((uint)sim.Missiles.Count);
    foreach (var m in sim.Missiles)
    {
        HV(m.Position);
        HV(m.Velocity);
        H((uint)m.Trail.Count);
        foreach (var t in m.Trail)
        {
            HV(t);
        }
    }

    H((uint)sim.Pickups.Count);
    foreach (var p in sim.Pickups)
    {
        HS(p.Name);
        HV(p.Position);
        HV(p.Velocity);
        HF(p.Yaw);
        H(p.Chute ? 1u : 0u);
        H((uint)p.FallTicks);
        H((uint)p.Ticks);
    }

    H((uint)sim.Explosions.Count);
    foreach (var e in sim.Explosions)
    {
        HV(e.Position);
        HF(e.Yaw);
        HF(e.Ticks);
    }

    H((uint)sim.Collected.Count);
    foreach (var c in sim.Collected)
    {
        HS(c);
    }

    var inv = sim.Inventory;
    H((uint)inv.Slots.Count);
    foreach (var slot in inv.Slots)
    {
        H((uint)slot.Item);
        H((uint)slot.Count);
    }

    H((uint)inv.Selected);
    foreach (var a in inv.Ammo)
    {
        H((uint)a);
    }

    H((uint)inv.SelectedAmmo);
    H((uint)inv.SuperChainGun);

    var sb = new StringBuilder();
    sb.Append($"s={s} st={(int)sim.State} tk={sim.Ticks} hp={sim.Health} m={sim.Missiles.Count} p={sim.Pickups.Count} e={sim.Explosions.Count} c={sim.Collected.Count} ev={(events.Count == 0 ? "-" : string.Join(";", events))} h={hash:X8}");
    Console.WriteLine(sb.ToString());
    if (fullMode || s % 10 == 0 || events.Count != 0 || sim.State != FallSim.Outcome.Running)
    {
        var bones = sim.BonesPosition is { } b ? $"{V3(b)}:{B(sim.BonesFrame)}" : "-";
        Console.WriteLine($"  intro={sim.IntroLeft} t={B(sim.Time)} dying={(sim.Dying ? 1 : 0)} kurt={V3(sim.KurtPosition)} vis={(sim.KurtVisible ? 1 : 0)} pose={(int)sim.KurtPose}:{B(sim.KurtFrame)} cam={V3(sim.CameraPosition)} bones={bones}");
        Console.WriteLine($"  wind={sim.Wind} haze={sim.HazeFrame} crawler={B(sim.CrawlerFrame)} screen={B(sim.Whiten)},{B(sim.Dark)},{B(sim.Red)} radar={(sim.RadarActive ? 1 : 0)}:{(int)sim.Radar}:{V3(sim.RadarSpot)}:{B(sim.RadarSpotRadius)} wv={B(sim.WindVolume)} gv={B(sim.GrindVolume)}");
        foreach (var m in sim.Missiles)
        {
            var last = m.Trail.Count == 0 ? "-" : V3(m.Trail[^1]);
            Console.WriteLine($"  missile {V3(m.Position)} v={V3(m.Velocity)} trail={m.Trail.Count}:{last}");
        }

        foreach (var p in sim.Pickups)
        {
            Console.WriteLine($"  pickup {p.Name} {V3(p.Position)} v={V3(p.Velocity)} yaw={B(p.Yaw)} chute={(p.Chute ? 1 : 0)} ticks={p.Ticks}/{p.FallTicks}");
        }

        foreach (var e in sim.Explosions)
        {
            Console.WriteLine($"  explosion {V3(e.Position)} yaw={B(e.Yaw)} ticks={B(e.Ticks)}");
        }

        Console.WriteLine($"  collected={(sim.Collected.Count == 0 ? "-" : string.Join(",", sim.Collected))} inv={Inv(inv)}");
    }
}

// FallScreen.Next: Landed carries the health and the pickups into the level; GameOver goes to
// the menu with nothing. Viewer.CarryFromFall: Kurt takes the pickups again, then the health.
Console.WriteLine($"end state={(int)sim.State} time={B(sim.Time)} health={sim.Health}");
if (sim.State == FallSim.Outcome.Landed)
{
    var carry = new FallCarry(sim.Health, [.. sim.Collected]);
    var levelInventory = new Inventory { Difficulty = difficulty };
    var health = Inventory.MaxHealth;
    foreach (var pickup in carry.Pickups)
    {
        levelInventory.Collect(pickup, ref health);
    }

    health = Math.Max(carry.Health, 0);
    Console.WriteLine($"carry health={carry.Health} pickups={(carry.Pickups.Count == 0 ? "-" : string.Join(",", carry.Pickups))}");
    Console.WriteLine($"level health={health} inv={Inv(levelInventory)}");
}
else
{
    Console.WriteLine("carry none");
}
