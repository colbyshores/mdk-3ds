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

audio.Dispose();
renderer.Dispose();
window.Dispose();
Directory.Delete(user, true);
