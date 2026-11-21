using System.Collections;
using System.Drawing;
using System.Globalization;
using System.Reflection;
using System.Text;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Engine.Render;
using Mdk.Formats;
using Mdk.Game.Flow;
using Mdk.Game.Menu;

// THE MENUS' ORACLE (package #20a): the C#'s own MainMenu, MenuItems, PauseMenu,
// Settings, Typesetter and StatsScreen (StatsPages.Briefing), driven by key
// schedules at the game's fixed step (1/60 s), their state printed after every
// frame. Its output is tests/scripts/golden/menu_*.txt; the C++ twin is
// tests/scripts/menu_tests.cpp (`vm_tests dump-menu <section>`), which must print
// the same bytes.
//
// The menus need a real Ui (a window, a renderer, an audio device), so this runs
// the C#'s own engine headless: SDL's offscreen video driver, lavapipe Vulkan, a
// muted audio device.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): copy src/,
// assets/, shaders/ and Directory.Build.props into a scratch tree with this
// directory beside src/, and the SPIR-V shaders into src/Mdk.Engine/obj/shaders
// (tests/frames/capture_oracle.sh compiles them with glslang; its scratch tree
// /tmp/mdk-frame-oracle already holds them); then
//   dotnet build menu_probe/Probe.csproj -c Release -p:PrecompiledShaders=spirv \
//       -o menu_probe-bin --ignore-failed-sources
//   MDK_DATA_DIR=<MDK> MDK_HIDDEN=1 SDL_VIDEODRIVER=offscreen \
//       VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
//       DOTNET_ROOT=<sdk> menu_probe-bin/Probe <section> 2>/dev/null \
//       > tests/scripts/golden/menu_<section>.txt
// for each section: settings, typeset, main, pause, briefing. No window opens
// (offscreen), so no display is used (AGENTS.md P7).
//
// PACKAGE #20b added the statistics (sections stats-all7, stats-score4,
// stats-nogore6 -> golden/stats_<name>.txt) and the slideshow (section slideshow
// -> golden/stats_slideshow.txt), built and run the same way (2026-11-20); their
// C++ twin is tests/scripts/stats_tests.cpp (`vm_tests dump-stats <section>`).
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;

const float Step = 1f / 60f;
var section = args.Length > 0 ? args[0] : "all";
var stdout = Console.Out;
var captured = new StringWriter();

string F(float f) => f.ToString(CultureInfo.InvariantCulture);

T Field<T>(object o, string name) =>
    (T)o.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(o)!;

object FieldObj(object o, string name) =>
    o.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(o)!;

// The page's entries: (text, grow, area).
List<(string Text, float Grow, RectangleF Area)> Entries(MenuItems items)
{
    var list = (IList)FieldObj(items, "_entries");
    var result = new List<(string, float, RectangleF)>();
    foreach (var e in list)
    {
        var t = e.GetType();
        result.Add(((string)t.GetField("Text")!.GetValue(e)!, (float)t.GetField("Grow")!.GetValue(e)!, (RectangleF)t.GetField("Area")!.GetValue(e)!));
    }

    return result;
}

// The lines the menus printed this frame ("Menu: ...") come first.
void Flush()
{
    foreach (var line in captured.ToString().Split('\n'))
    {
        if (line.StartsWith("Menu: ", StringComparison.Ordinal))
        {
            stdout.WriteLine(line);
        }
    }

    captured.GetStringBuilder().Clear();
}

var data = MdkData.Find() ?? throw new InvalidOperationException("no MDK data");
var user = Path.Combine(Path.GetTempPath(), $"mdk-menu-probe-{Environment.ProcessId}");
Directory.CreateDirectory(user);
var window = new Window("probe", 1280, 960, Visibility.Hidden);
var renderer = new Renderer(window);
var audio = new AudioDevice(Output.Muted);
var input = new Input();
var settings = new Settings();
settings.Apply(audio, window, renderer, input);
var ui = new Ui(data, window, renderer, audio, input, settings, user);
var menuDown = (HashSet<MenuKey>)input.GetType().GetField("_menuDown", BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(input)!;

// One frame's keys from a schedule word: up down left right accept back
// (pressed), rheld lheld (held only), fire (Fire held), - (nothing).
void Keys(string key)
{
    window.PumpEvents(input);
    menuDown.Clear();
    input.Hold(Key.Fire, Input.State.Up);
    switch (key)
    {
        case "up": input.Press(MenuKey.Up); break;
        case "down": input.Press(MenuKey.Down); break;
        case "left": input.Press(MenuKey.Left); menuDown.Add(MenuKey.Left); break;
        case "right": input.Press(MenuKey.Right); menuDown.Add(MenuKey.Right); break;
        case "accept": input.Press(MenuKey.Accept); break;
        case "back": input.Press(MenuKey.Back); break;
        case "rheld": menuDown.Add(MenuKey.Right); break;
        case "lheld": menuDown.Add(MenuKey.Left); break;
        case "fire": input.Hold(Key.Fire, Input.State.Down); break;
    }
}

// "key:frames,key:frames": the key on the first frame (held ones on each), then
// nothing for the rest.
IEnumerable<string> Schedule(string text)
{
    foreach (var step in text.Split(','))
    {
        var parts = step.Split(':');
        var count = int.Parse(parts[1], CultureInfo.InvariantCulture);
        var held = parts[0] is "rheld" or "lheld" or "fire";
        for (var i = 0; i < count; i++)
        {
            yield return i == 0 || held ? parts[0] : "-";
        }
    }
}

void PrintPage(MenuItems items, ref string last)
{
    var describe = items.Describe();
    var entries = Entries(items);
    if (describe == last || entries.Count == 0)
    {
        return;
    }

    last = describe;
    var row = entries[0].Area.Height;
    stdout.WriteLine($"  rows x={F(entries[0].Area.X)} w={F(entries[0].Area.Width)} h={F(row)} y0={F(entries[0].Area.Y)} big={(MenuItems.UsesBigFont(row) ? 1 : 0)}");
}

void PrintGrow(MenuItems items) =>
    stdout.WriteLine("  grow " + string.Join(" ", Entries(items).Select(e => F(e.Grow))));

// --- the settings file --------------------------------------------------------
if (section is "all" or "settings")
{
    stdout.WriteLine("== settings: defaults");
    stdout.Write(new Settings().Format());
    const string Crafted = "master_volume= 55 \nmusic_volume=150\neffects_volume=-3\nmusic_filter=false\r\n" +
        "mouse_sensitivity=1.5\ninvert_mouse=TRUE\npad_sensitivity=abc\nfullscreen=True\nwindow_size=800x600\n" +
        "exclusive_size=0x5\nrender_scale=150\nvsync=2\nframe_limit=61\ngpu_backend=Vulkan\ngamma=3\n" +
        "difficulty=Hard\ngore=False\ngraphics=Enhanced\nantialiasing=X4\nstereo=TabReversed\n" +
        "stereo_separation=0.3\nstereo_convergence=12.5\nhead_tracking=On\ntouch_controls=1\n" +
        "upscaler_url=http://x/y=z\nbind.Forward=Up\nbind.3=D\nbind.Nope=Q\nbind.Forward=W\npad.Jump=A\n" +
        "mod.Zeta=Off\nmod.alpha=On\nmod.ZETA=On\nmod.beta=Maybe\ntextures=Hd\ngarbage line\n=novalue\n";
    stdout.WriteLine("== settings: crafted");
    stdout.Write(Settings.Parse(Crafted).Format());
    stdout.WriteLine("== settings: older");
    stdout.Write(Settings.Parse("fullscreen=False\ntextures=Original\nmod.hd-textures=On\ndifficulty=0\ngamma=0.25\nstereo_convergence=99999\n").Format());
}

// --- the briefings' layout ----------------------------------------------------
if (section is "all" or "typeset")
{
    for (var n = 1; n <= 6; n++)
    {
        var bytes = ui.Fti.GetTextBytes($"BRIEF{n}");
        var (lines, count) = Typesetter.Layout(bytes, 32f);
        stdout.WriteLine($"== typeset BRIEF{n}: {bytes.Length} bytes, {lines.Count} lines, {count} characters");
        foreach (var line in lines)
        {
            stdout.WriteLine($"  x={F(line.X)} y={F(line.Y)} centred={(line.Centred ? 1 : 0)} start={line.Start} \"{Encoding.Latin1.GetString(line.Text)}\"");
        }
    }
}

// --- the main menu ------------------------------------------------------------
const string MainSchedule =
    "-:10,down:4,accept:4,accept:3,accept:3,accept:3,accept:3,accept:3," +
    "down:3,accept:4,back:3,accept:4," +
    "down:2,down:2,down:2,accept:4," +
    "down:3,accept:4,right:3,down:3,right:2,right:2,left:2,down:3,accept:3,down:2,back:4," +
    "down:2,down:2,accept:4,left:3,right:2,right:2,right:2,right:2,rheld:40,down:2,lheld:8,down:2,down:2,accept:3,down:3,accept:4," +
    "down:2,down:2,down:2,accept:4,right:2,right:2,left:2,back:4," +
    "back:4,back:4,down:3,accept:4,back:4,back:4," +
    "down:3,accept:4,up:3,accept:6";
if (section is "all" or "main")
{
    stdout.WriteLine($"== main menu (beta: {(ui.Beta != null ? 1 : 0)})");
    var state = new GameState { Splash = false };
    Console.SetOut(captured);
    var menu = new MainMenu(ui, state, SaveGames.In(user), MenuPage.Main);
    Flush();
    var items = Field<MenuItems>(menu, "_items");
    var last = "";
    var frame = 0;
    foreach (var key in Schedule(MainSchedule))
    {
        Keys(key);
        Console.SetOut(captured);
        var next = menu.Frame(Step, null);
        Console.SetOut(stdout);
        Flush();
        PrintPage(items, ref last);
        stdout.WriteLine($"f{frame} {key} page={FieldObj(menu, "_page")} next={next} level={state.Level} index={Field<int>(menu, "_levelIndex")} sel={Field<int>(items, "_selected")}");
        PrintGrow(items);
        frame++;
    }

    menu.Dispose();
    stdout.WriteLine("== main menu: settings after");
    stdout.Write(settings.Format());
}

// --- the pause menu -----------------------------------------------------------
const string PauseSchedule =
    "-:3,down:3,accept:4,down:3,accept:4,back:3,back:3,down:2,down:2,accept:3," +
    "new:3,back:3,new:3,up:2,down:2,down:2,down:2,accept:3";
if (section is "all" or "pause")
{
    stdout.WriteLine("== pause menu");
    var last0 = "";
    Console.SetOut(captured);
    var pause = new PauseMenu(ui);
    pause.Show();
    Console.SetOut(stdout);
    Flush();
    var items = Field<MenuItems>(pause, "_items");
    var last = last0;
    var frame = 0;
    foreach (var key in Schedule(PauseSchedule))
    {
        Console.SetOut(captured);
        if (key == "new")
        {
            // A new PauseMenu comes with each Viewer (Viewer.cs:271).
            pause = new PauseMenu(ui);
            items = Field<MenuItems>(pause, "_items");
            pause.Show();
            Keys("-");
        }
        else
        {
            Keys(key);
        }

        var next = pause.Open ? pause.Update(Step) : Event.None;
        Console.SetOut(stdout);
        Flush();
        PrintPage(items, ref last);
        stdout.WriteLine($"f{frame} {key} open={(pause.Open ? 1 : 0)} next={next} sel={Field<int>(items, "_selected")}");
        PrintGrow(items);
        frame++;
    }
}

// --- the briefing -------------------------------------------------------------
const string BriefingSchedule = "-:150,accept:2,fire:60,-:20,back:3,-:4,accept:40";
if (section is "all" or "briefing")
{
    foreach (var level in new[] { 7, 5 })
    {
        stdout.WriteLine($"== briefing LEVEL{level}");
        var state = new GameState { Level = level };
        Console.SetOut(captured);
        var screen = new StatsScreen(ui, state, SaveGames.In(user), StatsPages.Briefing, null);
        Console.SetOut(stdout);
        captured.GetStringBuilder().Clear();
        var frame = 0;
        foreach (var key in Schedule(BriefingSchedule))
        {
            Keys(key);
            Console.SetOut(captured);
            var next = screen.Frame(Step, null);
            Console.SetOut(stdout);
            captured.GetStringBuilder().Clear();
            stdout.WriteLine($"f{frame} {key} in={F(Field<float>(screen, "_fadeIn"))} out={F(Field<float>(screen, "_fadeOut"))} current={Field<int>(screen, "_current")} count={F(Field<float>(screen, "_count"))} shown={Field<int>(screen, "_shown")} ticks={F(Field<float>(screen, "_ticks"))} next={next}");
            frame++;
            if (next != Event.None)
            {
                break;
            }
        }

        screen.Dispose();
    }
}

// --- the saves on the main menu (package #19) -----------------------------------
// A user folder with LASTGAME (LEVEL6, one death), A (LEVEL3, a level's start), B (LEVEL4,
// before a level) and BAD (no valid level): Continue heads the page, Saved Game lists the
// four (BAD disabled, "Invalid"), B loads into its briefing; a second menu's Continue loads
// LASTGAME into the level. The C++ twin is menu_tests' dumpSaves over flow::SaveGames and
// flow::menuListSaves/menuPeekSave/menuLoadSave.
const string SavesSchedule = "-:4,down:3,down:3,down:3,accept:4,down:3,accept:4";
const string ContinueSchedule = "-:2,accept:4";
if (section is "saves")
{
    stdout.WriteLine("== saves");
    var saves = SaveGames.In(user);
    const string Time = "2026-11-20T12:00:00";
    saves.Write(SaveGames.LastGame, new SaveGame(SaveKind.LevelStart, 6, 100, 1, false, Time));
    saves.Write("A", new SaveGame(SaveKind.LevelStart, 3, 100, 0, false, Time));
    saves.Write("B", new SaveGame(SaveKind.BeforeLevel, 4, 100, 2, true, Time));
    File.WriteAllText(Path.Combine(saves.Directory, "BAD.sav"), "{\"level\":1}");
    foreach (var (schedule, label) in new[] { (SavesSchedule, "saved game"), (ContinueSchedule, "continue") })
    {
        stdout.WriteLine($"== saves: {label}");
        var state = new GameState { Splash = false };
        Console.SetOut(captured);
        var menu = new MainMenu(ui, state, saves, MenuPage.Main);
        Flush();
        var items = Field<MenuItems>(menu, "_items");
        var last = "";
        var frame = 0;
        foreach (var key in Schedule(schedule))
        {
            Keys(key);
            Console.SetOut(captured);
            var next = menu.Frame(Step, null);
            Console.SetOut(stdout);
            Flush();
            PrintPage(items, ref last);
            stdout.WriteLine($"f{frame} {key} page={FieldObj(menu, "_page")} next={next} level={state.Level} deaths={state.Deaths} strike={(state.StrikeUsed ? 1 : 0)} sel={Field<int>(items, "_selected")}");
            frame++;
        }

        menu.Dispose();
    }
}

// --- the slideshow (package #20b) ----------------------------------------------
// The main menu's Slideshow, started as VideoPlayer.Finished starts it (no video
// here: image 0 has no still), updated each frame and Reset on a key the way
// MainMenu.Frame does (Update, then Reset on AnyPressed). Prints the slides it
// loaded (size and an FNV-1a hash of the indices and the palette), then a line on
// every key and every change of image. The C++ twin is stats_tests.cpp
// (`vm_tests dump-stats slideshow`).
const string SlideshowSchedule = "-:200,key:1,-:400,key:1,-:300,key:1,-:1500";
if (section is "slideshow")
{
    var video = new VideoPlayer(ui);
    var show = new Slideshow(ui, video);
    show.Start();
    var images = Field<List<Gif>>(show, "_images");
    stdout.WriteLine($"== slideshow: {images.Count} slides");
    ulong Fnv(byte[] bytes)
    {
        var hash = 14695981039346656037UL;
        foreach (var b in bytes)
        {
            hash = (hash ^ b) * 1099511628211UL;
        }

        return hash;
    }

    for (var i = 0; i < images.Count; i++)
    {
        stdout.WriteLine($"  slide {i + 1}: {images[i].Width}x{images[i].Height} indices {Fnv(images[i].Indices):x16} palette {Fnv(images[i].Palette):x16}");
    }

    var frame = 0;
    var last = -1;
    foreach (var key in Schedule(SlideshowSchedule))
    {
        show.Update(Step);
        if (key != "-")
        {
            show.Reset();
        }

        if (key != "-" || show.Image != last)
        {
            stdout.WriteLine($"f{frame} {key} image={show.Image} items={(show.ItemsShown ? 1 : 0)} time={F(Field<float>(show, "_time"))}");
            last = show.Image;
        }

        frame++;
    }

    video.Dispose();
}

// --- the statistics (package #20b) ---------------------------------------------
// StatsScreen's pages after a level -- the intermission, the debriefing, the
// Score-O-matic and its heads, the save prompt, the briefing -- driven by a key
// schedule at 1/60 s with given counts, as Game.Run drives it: the screen's
// Frame, then the audio device's Update (muted: voices end in step with the
// frames). Each frame prints the page's state, the sounds started (+NAME#id),
// stopped by the screen (-#id) and ended in the mix (~#id), and the canvas the
// frame drew, captured in the renderer's Overlay hook (which Present calls before
// it consumes the canvas): its quad count and an FNV-1a hash of the quads, each
// "<kind> left,top,right,bottom r,g,b,a" with the kind B (FONTBIG), S (FONTSML),
// I (an image) or F (a fill); the quads themselves on the first frame of a page
// and every 120th. The C#'s `new Random()` (which picks only sounds) is replaced
// by a scripted one, (5n + 1) % max. The C++ twin is stats_tests.cpp
// (`vm_tests dump-stats <section>`).
//
// Sections: stats-all7 (LEVEL7 at normal speed, gore), stats-score4 (LEVEL4 from
// the Score-O-matic, Esc through it, 20 heads, saved "5"), stats-nogore6 (LEVEL6
// from the debriefing, fast, no gore, the prompt refused).
if (section.StartsWith("stats-", StringComparison.Ordinal))
{
    var scenarios = new (string Name, int Level, StatsScreen.Phase? First, int[] Counts, int Towns, bool Gore, string Schedule)[]
    {
        ("stats-all7", 7, null, [57, 31, 9, 4, 12, 20, 3], 0, true,
            "-:59,accept:2,-:150,back:2,-:60,fire:60,-:200,fire:100,-:450,accept:2,-:45,down:3,accept:3,-:60,back:2,-:5,accept:40"),
        ("stats-score4", 4, StatsScreen.Phase.Score, [0, 0, 5, 5, 7, 7, 20], 5, true,
            "-:40,back:2,-:30,back:2,-:30,back:2,-:30,back:2,-:20,back:2,-:20,back:2,-:20,back:2,-:20,fire:200,back:2,-:35,accept:3,-:5,accept:3,-:10,fire:200"),
        ("stats-nogore6", 6, StatsScreen.Phase.Debriefing, [10, 10, 0, 0, 0, 0, 2], 6, false,
            "fire:500,-:10,back:3,-:4,fire:200"),
    };
    var (name, level, first, counts, towns, gore, scheduleText) = scenarios.First(s => s.Name == section);
    stdout.WriteLine($"== {name}: level {level}, first {(first?.ToString() ?? "-")}, counts {string.Join(",", counts)}, towns {towns}, gore {(gore ? 1 : 0)}, canvas {F(renderer.CanvasWidth)}");
    settings.Gore = gore;
    var state = new GameState { Level = level };
    var stats = state.Stats;
    (stats.Shots, stats.ShotHits, stats.SniperShots, stats.SniperHits) = (counts[0], counts[1], counts[2], counts[3]);
    (stats.Kills, stats.Enemies, stats.HeadShots) = (counts[4], counts[5], counts[6]);
    stats.TownFlags = towns << 29;
    var saves = SaveGames.In(user);
    Console.SetOut(captured);
    var screen = new StatsScreen(ui, state, saves, StatsPages.All, first);
    Console.SetOut(stdout);
    void FlushStats()
    {
        foreach (var line in captured.ToString().Split('\n'))
        {
            if (line.StartsWith("Statistics: ", StringComparison.Ordinal) || line.StartsWith("Score-O-matic: ", StringComparison.Ordinal)
                || line.StartsWith("Saved game ", StringComparison.Ordinal))
            {
                stdout.WriteLine(line);
            }
        }

        captured.GetStringBuilder().Clear();
    }

    FlushStats();
    typeof(StatsScreen).GetField("_random", BindingFlags.NonPublic | BindingFlags.Instance)!.SetValue(screen, new ScriptedRandom());
    var fonts = Field<Fonts>(screen, "_fonts");
    var bigTexture = Field<int>(fonts.Big, "_texture");
    var smallTexture = Field<int>(fonts.Small, "_texture");
    var sounds = Field<Dictionary<string, Sound?>>(screen, "_sounds");
    var voices = (IDictionary)FieldObj(audio, "_voices");
    var canvasVertices = (List<Vertex>)FieldObj(renderer, "_canvasVertices");
    var canvasCommands = (List<(int First, int Count, Material Material)>)FieldObj(renderer, "_canvasCommands");
    var quads = new List<string>();
    renderer.Overlay = () =>
    {
        quads.Clear();
        foreach (var (firstVertex, count, material) in canvasCommands)
        {
            var kind = material.Texture == bigTexture ? "B" : material.Texture == smallTexture ? "S" : material.Texture == Material.None ? "F" : "I";
            for (var v = firstVertex; v < firstVertex + count; v += 6)
            {
                var a = canvasVertices[v].Position;
                var b = canvasVertices[v + 2].Position;
                var c = material.Colour;
                quads.Add($"{kind} {F(a.X)},{F(a.Y)},{F(b.X)},{F(b.Y)} {F(c.X)},{F(c.Y)},{F(c.Z)},{F(c.W)}");
            }
        }
    };

    List<int> Ids() => voices.Keys.Cast<int>().OrderBy(i => i).ToList();
    string NameOf(int id)
    {
        var sound = voices[id]!.GetType().GetField("Sound")!.GetValue(voices[id]);
        return sounds.FirstOrDefault(p => ReferenceEquals(p.Value, sound)).Key ?? "?";
    }

    ulong Hash(List<string> lines)
    {
        var hash = 14695981039346656037UL;
        foreach (var line in lines)
        {
            foreach (var b in Encoding.ASCII.GetBytes(line + "\n"))
            {
                hash = (hash ^ b) * 1099511628211UL;
            }
        }

        return hash;
    }

    var frame = 0;
    var lastStatus = "";
    foreach (var key in Schedule(scheduleText))
    {
        Keys(key);
        var before = Ids();
        Console.SetOut(captured);
        var next = screen.Frame(Step, null);
        Console.SetOut(stdout);
        var after = Ids();
        audio.Update(Step);
        var mixed = Ids();
        var sound = new StringBuilder();
        foreach (var id in after.Except(before))
        {
            sound.Append($" +{NameOf(id)}#{id}");
        }

        foreach (var id in before.Except(after))
        {
            sound.Append($" -#{id}");
        }

        foreach (var id in after.Except(mixed))
        {
            sound.Append($" ~#{id}");
        }

        FlushStats();
        var status = screen.Status[0];
        var prompt = FieldObj(screen, "_prompt");
        var promptText = prompt == null ? ""
            : $" prompt={FieldObj(prompt, "_stage")}/{Field<int>(prompt, "_selected")}/{Field<string>(prompt, "_name")}/{Field<int>(prompt, "_cursor")}";
        string Floats(string field) => string.Join(",", Field<float[]>(screen, field).Select(F));
        stdout.WriteLine($"f{frame} {key} {status}{promptText} in={F(Field<float>(screen, "_fadeIn"))} out={F(Field<float>(screen, "_fadeOut"))} cur={Field<int>(screen, "_current")} count={F(Field<float>(screen, "_count"))} shown={Field<int>(screen, "_shown")} ticks={F(Field<float>(screen, "_ticks"))} row={Field<int>(screen, "_row")}/{Field<int>(screen, "_rowStage")}/{F(Field<float>(screen, "_rowTime"))} v={Floats("_values")} s={Floats("_slides")} heads={Field<int>(screen, "_heads")}/{F(Field<float>(screen, "_headTime"))} next={next}");
        if (sound.Length > 0)
        {
            stdout.WriteLine($"  snd{sound}");
        }

        var heads = Field<int>(screen, "_heads");
        if (heads > 0)
        {
            var angles = Field<float[]>(Field<HeadsView>(screen, "_headsView"), "_angles");
            var places = HeadsView.Places(heads, state.Stats.HeadShots).ToList();
            stdout.WriteLine($"  heads a={string.Join(",", angles.Take(Math.Min(heads, HeadsView.MaxHeads)).Select(F))} p={string.Join(",", places.Select(p => $"{F(p.X)}:{F(p.Z)}"))}");
        }

        stdout.WriteLine($"  quads {quads.Count} {Hash(quads):x16}");
        if (status != lastStatus || frame % 120 == 0)
        {
            foreach (var quad in quads)
            {
                stdout.WriteLine($"    {quad}");
            }
        }

        lastStatus = status;
        frame++;
        if (next != Event.None)
        {
            break;
        }
    }

    stdout.WriteLine($"== end: level {state.Level}, saves {string.Join(",", saves.List())}");
    screen.Dispose();
}

audio.Dispose();
renderer.Dispose();
window.Dispose();
Directory.Delete(user, true);

/// <summary>The statistics' scripted Random (only sounds draw on it): (5n + 1) % max.</summary>
sealed class ScriptedRandom : Random
{
    private int _n;

    public override int Next(int maxValue) => (_n++ * 5 + 1) % maxValue;
}
