using System.Globalization;
using System.Numerics;
using Mdk.Game.Fall;
using Mdk.Game.Flow;
using Mdk.Game.Kurt;
using Mdk.Game.Scripts;
using Mdk.Game.Stream;

// THE NEXT-LEVEL ORACLE (package 2): what follows the end of a level, in the C#'s own order
// (Flow/Game.cs Handle(LevelEnded) -> AfterLevel -> ShowStream -> AfterStream -> Statistics ->
// Fall -> Play), with every screen this port does not have SKIPPED the way the C# skips it:
//
//   Stream      StreamScreen's Back key: Flight.Stop(), then the flight is updated until it has
//               Ended (Flight.cs:161, :220-228). The real Flight over a real StreamTube of the
//               level's index and kind (StreamScreen.cs:57-66), seeded with WatcomRandom's
//               DefaultSeed, at the C#'s Normal difficulty; the outcome is Flight.Outcome.
//   Statistics  StatsScreen: after its Score page EndPhase moves the level on
//               (StatsScreen.cs:263-264), then the save prompt and the briefing, then
//               Event.Fall. StatsScreen cannot be built without a window (Ui), so THIS ONE
//               STEP is transliterated from :135 and :263-264 -- the only one here that is.
//   Fall        FallScreen's Back key: FallSim.Skip() on the real sim of the new level's index
//               (FallScreen.cs:51), updated once as FallScreen.Frame does; then Next():
//               Carry = (Health, Collected) and Event.Play.
//   SavePrompt  After LEVEL8: GameState.EnterLastLevel(stream health) (Game.cs:529), the
//               prompt closed (Event.Play).
//
// and then the carry the next Viewer takes (Viewer.CarryFromFall: SetHealth, the pickups).
// Its output is tests/scripts/golden/flow_next_level.txt; the C++ twin is
// `tick1 dump flow` (GameFlow::skipUnported), which must print the same bytes.
//
// HOW IT WAS BUILT AND RUN (2026-11-20, .NET SDK 10.0.404): as tests/scripts/end_probe -- copy
// src/Mdk.Formats, src/Mdk.Engine, src/Mdk.Game and Directory.Build.props into a scratch tree with
// this directory beside src/; in the scratch Mdk.Engine.csproj drop the shader targets and the
// icon resource; `dotnet build -c Release -o probe-bin`, then `DOTNET_ROOT=<sdk> probe-bin/Probe`.
// It reads no game data.
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
CultureInfo.CurrentCulture = CultureInfo.InvariantCulture;

const int LastStatisticsIndex = 3;   // StreamScreen.LastStatisticsIndex
const int LastFallIndex = 4;         // FallScreen.LastIndex

void Chain(int level, int health)
{
    var state = new GameState { Level = level };
    Console.WriteLine($"level {level} ended health {health}");
    var after = LevelFlow.AfterLevel(level, ScriptRuntime.GameOver.No);
    Console.WriteLine($"  after level: {after}");
    if (after == LevelFlow.After.Menu)
    {
        Console.WriteLine("  screen Menu");
        return;
    }

    // ShowStream(_viewer.Health): the stream, skipped.
    var index = Math.Clamp(GameState.IndexOf(level), 0, LastStatisticsIndex + 1);
    var kind = index > LastStatisticsIndex ? StreamTube.Kind.Gunter : StreamTube.Kind.Normal;
    var random = new WatcomRandom();
    var tube = new StreamTube(index, Difficulty.Normal, kind, random);
    var clip = new Clip(1, 1f);
    var flight = new Flight(tube, kind, Difficulty.Normal, random, health, clip, clip, clip);
    flight.Stop();
    var updates = 0;
    while (!flight.Ended && updates < 600)
    {
        flight.Update(1f / 60f, default);
        updates++;
    }

    Console.WriteLine($"  skipped Stream ({kind}): ended {flight.Ended} after {updates} update(s), health {flight.Health}, outcome {flight.Outcome}");
    if (flight.Outcome == Outcome.Died)
    {
        Console.WriteLine("  screen Menu");
        return;
    }

    // AfterStream.
    switch (LevelFlow.AfterLevel(state.Level, ScriptRuntime.GameOver.No))
    {
        case LevelFlow.After.Statistics:
        {
            // StatsScreen (transliterated: :135, :263-264).
            var statsIndex = Math.Max(GameState.IndexOf(state.Level), 0);
            statsIndex = Math.Min(statsIndex + 1, GameState.Order.Length - 1);
            state.Level = GameState.Order[statsIndex];
            Console.WriteLine($"  skipped Statistics: level {state.Level}");
            // Event.Fall: the fall of state.Level, skipped.
            var fallIndex = Math.Clamp(GameState.IndexOf(state.Level), 0, LastFallIndex);
            var anim = new FallSim.Animation(1f, 1);
            var sim = new FallSim(new FallSim.Setup(fallIndex, Difficulty.Normal, [], anim, anim, anim), new Random(0));
            sim.Skip();
            sim.Update(1f / 60f, Vector2.Zero);
            Console.WriteLine($"  skipped Fall (index {fallIndex}): {sim.State}, health {sim.Health}, pickups {sim.Collected.Count}");
            if (sim.State != FallSim.Outcome.Landed)
            {
                Console.WriteLine("  screen Menu");
                return;
            }

            state.Carry = new FallCarry(sim.Health, [.. sim.Collected]);
            break;
        }
        case LevelFlow.After.LastLevel:
            state.EnterLastLevel(flight.Health);
            Console.WriteLine($"  skipped SavePrompt: level {state.Level}");
            break;
        default:
            Console.WriteLine("  screen Menu");
            return;
    }

    var carry = state.Carry!;
    Console.WriteLine($"  play level {state.Level} health {carry.Health} pickups {carry.Pickups.Count}");
}

foreach (var level in new[] { 7, 6, 3, 4, 8, 5, 2 })
{
    foreach (var health in new[] { 100, 37, 1, 0 })
    {
        Chain(level, health);
    }
}
