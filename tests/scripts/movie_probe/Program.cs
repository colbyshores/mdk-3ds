using System.Globalization;
using System.Reflection;
using Mdk.Engine.Audio;
using Mdk.Engine.Platform;
using Mdk.Engine.Render;
using Mdk.Formats;
using Mdk.Game.Flow;
using Mdk.Game.Menu;

// THE MOVIES' ORACLE (package #20c): the C#'s own Flc and Mve decoders, IntroSplash,
// VideoPlayer, MainMenu (its splash and MDK12.FLC), EndMovie and LoadingScreen, driven at the
// game's fixed step (1/60 s), their state printed. Its output is
// tests/scripts/golden/movie_<section>.txt; the C++ twin is tests/scripts/movie_tests.cpp
// (`vm_tests dump-movie <section>`), which must print the same bytes. The sections:
//   decode   every movie the game names (and the two other FLCs of MISC/FLIC), frame by frame:
//            FNV-1a of the indices and of the palette (and of an MVE's sound); a missing file
//            says so (the GOG data has no MDKEND.FLC and no MDKBZK.MVE).
//   splash   INTRO1A: its palettes and picture, IntroSplash.Blend's weights, EndMovie.Flash.
//   menu     MainMenu with the splash: no key (the splash's 10 s, then MDK12.FLC to its last
//            frame), and a key that skips the splash (the key is the splash's, not the menu's).
//   endmovie EndMovie as the data has it (no FLC, no MVE: the menu at once), Back, and its
//            per-frame extras over pie.flc standing in for MDKEND.FLC (311 frames: the sounds,
//            the loop's stop at 188, the flash 210-260 and the 1 s hold at 233).
//   loading  LoadingScreen's pieces for LEVEL3-8: the LBB, the colours, the text, the layout.
//
// The screens need a real Ui (a window, a renderer, an audio device), so this runs the C#'s
// own engine headless, as tests/scripts/menu_probe does: SDL's offscreen video driver, lavapipe
// Vulkan, a muted audio device.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as menu_probe -- copy src/,
// assets/, shaders/ and Directory.Build.props into a scratch tree with this directory beside
// src/, and the SPIR-V shaders into src/Mdk.Engine/obj/shaders; then
//   dotnet build movie_probe/Probe.csproj -c Release -p:PrecompiledShaders=spirv \
//       -o movie_probe-bin --ignore-failed-sources
//   MDK_DATA_DIR=<MDK> MDK_HIDDEN=1 SDL_VIDEODRIVER=offscreen \
//       VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
//       DOTNET_ROOT=<sdk> movie_probe-bin/Probe <section> 2>/dev/null \
//       > tests/scripts/golden/movie_<section>.txt
// No window opens (offscreen), so no display is used (AGENTS.md P7).
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;

const float Step = 1f / 60f;
var section = args.Length > 0 ? args[0] : "all";
var stdout = Console.Out;
var captured = new StringWriter();

string F(float f) => f.ToString(CultureInfo.InvariantCulture);

// FNV-1a, 32 bits.
string Fnv(ReadOnlySpan<byte> bytes)
{
    var h = 2166136261u;
    foreach (var b in bytes)
    {
        h ^= b;
        h *= 16777619u;
    }

    return h.ToString("x8", CultureInfo.InvariantCulture);
}

string FnvAudio(List<System.Numerics.Vector2> audio)
{
    var bytes = new byte[audio.Count * 8];
    for (var i = 0; i < audio.Count; i++)
    {
        BitConverter.TryWriteBytes(bytes.AsSpan(i * 8), BitConverter.SingleToInt32Bits(audio[i].X));
        BitConverter.TryWriteBytes(bytes.AsSpan(i * 8 + 4), BitConverter.SingleToInt32Bits(audio[i].Y));
    }

    return Fnv(bytes);
}

T Field<T>(object o, string name) =>
    (T)o.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(o)!;

object? FieldObj(object o, string name) =>
    o.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(o);

void SetField(object o, string name, object? value) =>
    o.GetType().GetField(name, BindingFlags.NonPublic | BindingFlags.Instance)!.SetValue(o, value);

var data = MdkData.Find() ?? throw new InvalidOperationException("no MDK data");

// --- the decoders --------------------------------------------------------------
string[] Movies = ["MISC/FLIC/MDK12.FLC", "MISC/FLIC/MDKEND.FLC", "MISC/FLIC/pie.flc", "MISC/FLIC/shiny.flc", "MISC/FLIC/MDKBZK.MVE"];
if (section is "all" or "decode")
{
    foreach (var name in Movies)
    {
        var path = data.PathOf(name);
        if (!File.Exists(path))
        {
            stdout.WriteLine($"== {name}: missing");
            continue;
        }

        if (name.EndsWith(".MVE", StringComparison.OrdinalIgnoreCase))
        {
            var mve = Mve.Load(path);
            if (mve == null)
            {
                stdout.WriteLine($"== {name}: not an MVE");
                continue;
            }

            stdout.WriteLine($"== {name}: mve");
            while (mve.NextFrame())
            {
                stdout.WriteLine($"frame {mve.Frame} {mve.Width}x{mve.Height} time={mve.FrameTime} rate={mve.SampleRate} channels={mve.Channels} indices={Fnv(mve.Indices)} palette={Fnv(mve.Palette)} audio={mve.Audio.Count} sound={FnvAudio(mve.Audio)}");
            }

            stdout.WriteLine($"end frame={mve.Frame}");
            continue;
        }

        var flc = Flc.Load(path);
        if (flc == null)
        {
            stdout.WriteLine($"== {name}: not an FLC");
            continue;
        }

        stdout.WriteLine($"== {name}: flc {flc.Width}x{flc.Height} frames={flc.FrameCount} speed={flc.Speed}");
        while (flc.NextFrame())
        {
            stdout.WriteLine($"frame {flc.Frame} indices={Fnv(flc.Indices)} palette={Fnv(flc.Palette)}");
        }

        stdout.WriteLine($"end frame={flc.Frame}");
        flc.Rewind();
        stdout.WriteLine($"rewind frame={flc.Frame} indices={Fnv(flc.Indices)}");
        flc.NextFrame();
        stdout.WriteLine($"again frame={flc.Frame} indices={Fnv(flc.Indices)} palette={Fnv(flc.Palette)}");
    }
}

// --- the splash's pieces and the end movie's flash (static, no Ui) ---------------
if (section is "all" or "splash")
{
    var options = Bni.Load(data.PathOf("MISC/OPTIONS.BNI"));
    var bytes = Ui.BytesOf(options, "INTRO1A");
    var black = new byte[768];
    var one = bytes[..768];
    var two = bytes[768..1536];
    var picture = IntroSplash.Unpack(bytes, 1536, 600 * 360);
    stdout.WriteLine($"== INTRO1A: {bytes.Length} bytes palette1={Fnv(one)} palette2={Fnv(two)} picture={Fnv(picture)}");
    // A crafted stream: each run kind and the 0 end, whole and cut short at 4 bytes.
    byte[] crafted = [3, 7, 254, 1, 2, 2, 9, 253, 4, 5, 6, 0, 5, 5];
    stdout.WriteLine($"crafted {Fnv(IntroSplash.Unpack(crafted, 0, 16))} short {Fnv(IntroSplash.Unpack(crafted, 0, 4))} {Convert.ToHexString(IntroSplash.Unpack(crafted, 0, 16))}");
    foreach (var t in new[] { 0f, 0.001f, 0.003f, 0.25f, 0.5f, 0.5019531f, 0.6f, 0.75f, 0.998f, 1f })
    {
        stdout.WriteLine($"blend t={F(t)} black-one={Fnv(IntroSplash.Blend(black, one, t))} one-two={Fnv(IntroSplash.Blend(one, two, t))} two-black={Fnv(IntroSplash.Blend(two, black, t))}");
    }

    for (var frame = 200; frame <= 265; frame++)
    {
        stdout.WriteLine($"flash {frame} {F(EndMovie.Flash(frame))}");
    }
}

// --- a real Ui for the screens ---------------------------------------------------
if (section is "decode" or "splash")
{
    return;
}

var user = Path.Combine(Path.GetTempPath(), $"mdk-movie-probe-{Environment.ProcessId}");
Directory.CreateDirectory(user);
var window = new Window("probe", 1280, 960, Visibility.Hidden);
var renderer = new Renderer(window);
var audio = new AudioDevice(Output.Muted);
var input = new Input();
var settings = new Settings();
settings.Apply(audio, window, renderer, input);
var ui = new Ui(data, window, renderer, audio, input, settings, user);
var menuDown = (HashSet<MenuKey>)input.GetType().GetField("_menuDown", BindingFlags.NonPublic | BindingFlags.Instance)!.GetValue(input)!;

void Keys(string key)
{
    window.PumpEvents(input);
    menuDown.Clear();
    switch (key)
    {
        case "down": input.Press(MenuKey.Down); break;
        case "accept": input.Press(MenuKey.Accept); break;
        case "back": input.Press(MenuKey.Back); break;
    }
}

// "key:frames,...": the key on the first frame, then nothing.
IEnumerable<string> Schedule(string text)
{
    foreach (var step in text.Split(','))
    {
        var parts = step.Split(':');
        var count = int.Parse(parts[1], CultureInfo.InvariantCulture);
        for (var i = 0; i < count; i++)
        {
            yield return i == 0 ? parts[0] : "-";
        }
    }
}

// The player's state: playing, its clock, the hold, the brightening, and the image (its
// indices and the palette it shows, brightened).
string Video(VideoPlayer video)
{
    var flc = (Flc?)FieldObj(video, "_flc");
    var still = video.Still;
    var image = still is { } s
        ? $"{s.Width}x{s.Height} {Fnv(s.Indices)} {Fnv(Brightened(video, s.Palette))}"
        : "none";
    return $"playing={(video.Playing ? 1 : 0)} flc={(flc == null ? "-" : flc.Frame.ToString(CultureInfo.InvariantCulture))} time={F(Field<float>(video, "_time"))} hold={F(Field<float>(video, "_hold"))} bright={F(Field<float>(video, "_brighten"))} image={image}";
}

// The palette IntroSplash.Draw sets this frame: Blend(from, to, clamp(time / seconds)) of its
// step, from its own fields and its own Blend; "-" once it is done.
string SplashPalette(IntroSplash splash)
{
    if (splash.Done)
    {
        return "-";
    }

    var steps = (Array)typeof(IntroSplash).GetField("Steps", BindingFlags.NonPublic | BindingFlags.Static)!.GetValue(null)!;
    var (seconds, from, to) = ((float, int, int))steps.GetValue(Field<int>(splash, "_step"))!;
    var palettes = Field<byte[][]>(splash, "_palettes");
    return Fnv(IntroSplash.Blend(palettes[from], palettes[to], Math.Clamp(Field<float>(splash, "_time") / seconds, 0f, 1f)));
}

byte[] Brightened(VideoPlayer video, byte[] palette) =>
    (byte[])typeof(VideoPlayer).GetMethod("Brightened", BindingFlags.NonPublic | BindingFlags.Instance)!.Invoke(video, [palette])!;

// --- the main menu with its splash and MDK12.FLC ---------------------------------
if (section is "all" or "menu")
{
    foreach (var (label, schedule) in new[] { ("no key", "-:800"), ("a key skips the splash", "-:30,accept:20,down:60,-:100") })
    {
        stdout.WriteLine($"== menu: {label}");
        var state = new GameState { Splash = true };
        Console.SetOut(captured);
        var menu = new MainMenu(ui, state, SaveGames.In(user), MenuPage.Main);
        Console.SetOut(stdout);
        captured.GetStringBuilder().Clear();
        var splash = (IntroSplash?)FieldObj(menu, "_splash");
        var video = Field<VideoPlayer>(menu, "_video");
        var items = Field<MenuItems>(menu, "_items");
        var frame = 0;
        foreach (var key in Schedule(schedule))
        {
            Keys(key);
            Console.SetOut(captured);
            var next = menu.Frame(Step, null);
            Console.SetOut(stdout);
            captured.GetStringBuilder().Clear();
            var splashText = splash == null ? "none"
                : $"{(splash.Done ? 1 : 0)} step={Field<int>(splash, "_step")} time={F(Field<float>(splash, "_time"))} pal={SplashPalette(splash)}";
            stdout.WriteLine($"f{frame} {key} status=\"{menu.Status[0]}\" next={next} splash={splashText} sel={Field<int>(items, "_selected")} {Video(video)}");
            frame++;
        }

        menu.Dispose();
    }
}

// --- the end movie -----------------------------------------------------------------
if (section is "all" or "endmovie")
{
    // As the data has it: neither file, so the menu at once; then Back.
    stdout.WriteLine("== end movie: the data's");
    {
        var movie = new EndMovie(ui);
        var video = Field<VideoPlayer>(movie, "_video");
        foreach (var key in new[] { "-", "-", "back" })
        {
            Keys(key);
            var next = movie.Frame(Step, null);
            stdout.WriteLine($"{key} status=\"{movie.Status[0]}\" next={next} {Video(video)}");
        }

        movie.Dispose();
    }

    // pie.flc in MDKEND.FLC's place: the constructor has already fallen through to the (missing)
    // MVE, whose Finished handler is the menu's; the FLC is then played on the same player, so
    // every frame runs OnFrame.
    foreach (var (label, schedule) in new[] { ("pie.flc as MDKEND.FLC", "-:740"), ("Back gives up", "-:100,back:3") })
    {
        stdout.WriteLine($"== end movie: {label}");
        var movie = new EndMovie(ui);
        var video = Field<VideoPlayer>(movie, "_video");
        var voices = Field<Dictionary<string, int>>(movie, "_voices");
        SetField(movie, "_next", Event.None);
        video.Play("MISC/FLIC/pie.flc");
        var known = new Dictionary<string, int>(voices);
        stdout.WriteLine($"start sounds=[{string.Join(" ", voices.Keys.Order(StringComparer.Ordinal))}] {Video(video)}");
        var frame = 0;
        foreach (var key in Schedule(schedule))
        {
            Keys(key);
            var flc = (Flc?)FieldObj(video, "_flc");
            var before = flc?.Frame ?? -1;
            var next = movie.Frame(Step, null);
            var played = voices.Where(v => !known.TryGetValue(v.Key, out var id) || id != v.Value).Select(v => v.Key).Order(StringComparer.Ordinal).ToList();
            known = new Dictionary<string, int>(voices);
            var after = ((Flc?)FieldObj(video, "_flc"))?.Frame ?? -1;
            var stops = before < 188 && after >= 188 && voices.ContainsKey("DOGSHIP") ? " stop=DOGSHIP" : "";
            stdout.WriteLine($"f{frame} {key} next={next} status=\"{movie.Status[0]}\" sounds=[{string.Join(" ", played)}]{stops} {Video(video)}");
            frame++;
            if (next != Event.None && label == "Back gives up")
            {
                break;
            }
        }

        movie.Dispose();
    }
}

// --- the loading screen ----------------------------------------------------------
if (section is "all" or "loading")
{
    var view = ui.View;
    view.Layout(ScreenView.Fit.Height);
    stdout.WriteLine($"== loading: canvas {F(renderer.CanvasWidth)}x{F(Renderer.CanvasHeight)} scale={F(view.Scale)} origin={F(view.Origin.X)},{F(view.Origin.Y)}");
    var fonts = new Fonts(renderer, ui.Fti);
    var text = ui.Fti.GetTextBytes("LOAD_MSG");
    var width = fonts.Big.Width(text);
    stdout.WriteLine($"text \"{System.Text.Encoding.Latin1.GetString(text)}\" width={width} x={F((ScreenView.Size.X - width) / 2f)} y=260");
    var bar = new System.Drawing.RectangleF(10f, 290f, 580f, 20f);
    var canvasBar = view.ToCanvas(bar);
    stdout.WriteLine($"bar {F(canvasBar.X)},{F(canvasBar.Y)} {F(canvasBar.Width)}x{F(canvasBar.Height)} frame={F(view.Scale)}");
    var image = view.ToCanvas(new System.Drawing.RectangleF(200f, 25f, 200f, 200f));
    stdout.WriteLine($"image {F(image.X)},{F(image.Y)} {F(image.Width)}x{F(image.Height)}");
    for (var level = 1; level <= 8; level++)
    {
        var path = data.PathOf($"MISC/LOAD_{level}.LBB");
        if (!File.Exists(path))
        {
            stdout.WriteLine($"LOAD_{level}: missing");
            continue;
        }

        var bytes = File.ReadAllBytes(path);
        var texture = Texture.Parse($"LOAD_{level}", bytes, 768);
        var palette = Palette.FromRgb(bytes[..768]);
        string Colour(int i) => $"{palette.Rgba[i * 4]},{palette.Rgba[i * 4 + 1]},{palette.Rgba[i * 4 + 2]}";
        stdout.WriteLine($"LOAD_{level}: {bytes.Length} bytes {texture.Width}x{texture.Height} indices={Fnv(texture.Indices)} palette={Fnv(palette.Rgba)} fill={Colour(3)} frame={Colour(4)}");
    }
}

Directory.Delete(user, true);
