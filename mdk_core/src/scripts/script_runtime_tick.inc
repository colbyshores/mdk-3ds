// script_runtime_tick.inc -- the tick: runTick, motionUpdate, runControllers.
// Part of script_runtime.cpp's single translation unit (see its header): pasted
// inside namespace mdk::scripts after its file-local constants and helpers,
// which it uses. No include guards, no includes of its own; the order is
// load-bearing.

void ScriptRuntime::runTick() {
    // The tick probe's laps (script_runtime.h): each adds the time since the last
    // lap to one part of the row. Nothing when no clock is set.
    uint64_t probeMark = probeClock != nullptr ? probeClock() : 0;
    const auto probeLap = [&](uint64_t* part) {
        if (probeClock != nullptr) {
            const uint64_t now = probeClock();
            *part += now - probeMark;
            probeMark = now;
        }
    };
    // The C# copies Kurt's live feet and yaw into the script runtime at the
    // start of every tick (ScriptRuntime.cs:886-889). A bound Player is the
    // live source; with no Player, the level-start state remains in place.
    if (player_ != nullptr) {
        kurtPosition_ = player_->feet;
        kurtYaw_ = wrap360(player_->yaw);
        targetPosition_ = kurtPosition_;
        targetYaw_ = kurtYaw_;
    }

    // A cutscene's tick replaces the game's (ScriptRuntime.cs:890-894): only some objects
    // run, no controller, no countdown, trigger, pickup, gun or effect -- everything below
    // waits until EndCutscene.
    if (cutscene != 0) {
        cutsceneTick();
        return;
    }

    // The alarm countdown decays once per tick, before any object runs
    // (ScriptRuntime.cs:896), after the cutscene's early return above.
    alarmTicks = alarmTicks > 0 ? alarmTicks - 1 : 0;

    // The camera-track request lasts for the current tick and the next one
    // (ScriptRuntime.cs:898).
    cameraTrackTicks = cameraTrackTicks > 0 ? cameraTrackTicks - 1 : 0;

    // The screen-shake request belongs to one tick (ScriptRuntime.cs:899).
    // Calls made later in this tick replace it only when they are stronger.
    shake = 0.0f;
    // The health bar's second runs out, or its object died (ScriptRuntime.cs:900).
    updateBar();
    // The town's timer (ScriptRuntime.cs:901-908, package #15): at 0 the minecrawler
    // has reached the town.
    if (townTicks > 0) {
        townTicks--;
        if (townTicks == 0) {
            flattenTown();
        }
    }

    // The first tick has no movement to observe (ScriptRuntime.cs:911-917,
    // after the decrements above, as in the oracle). A teleport can set the
    // previous position, and when it did BEFORE this first tick the C# keeps it
    // (`&& !_teleported`, :911): the soak tour teleported onto a doorway before the
    // first tick, the first tick overwrote the teleport's destination with the same
    // place, and Kurt stayed in the room while standing in the corridor until it went
    // (ArenaSwitchTests.FirstTickKeepsTeleport). The flag is never cleared, and cannot
    // need clearing: this rule only runs at tick 0.
    if (tickCount_ == 0 && !teleported_) {
        previousKurtPosition_ = kurtPosition_;
    }

    // Kurt changes arena only through a connection; on the first tick he is
    // placed and the arena under him is found (ScriptRuntime.cs:918-919).
    //
    // The C#'s first branch is `IsBeta ? BetaArena() : ...`. The 1996 demo is not
    // reachable from this runtime (see the IsBeta note by the animation lookup), so
    // the retail branch is the whole behaviour here; when the demo lands, BetaArena
    // (:29-34) has to be ported and NOT approximated by the geometric arena, which
    // is a different rule (smallest box around Kurt, only if the current arena
    // connects to it).
    std::string arena;
    if (currentArena_.empty()) {
        formats::Vec3f at;
        at.x = kurtPosition_.x;
        at.y = kurtPosition_.y;
        at.z = kurtPosition_.z;
        const std::string* found = space_.arenaAt(at);
        arena = found ? *found : std::string();
    } else {
        arena = crossedArena(currentArena_, previousKurtPosition_, kurtPosition_);
    }

    if (!arena.empty() && arena != currentArena_) {
        if (!currentArena_.empty()) {
            secondArena_ = currentArena_;
            // Kurt.EnterArena() (ScriptRuntime.cs:926): a slide that carries him through
            // a doorway ends 15 ticks later (Kurt.Slide.cs:97-105); it does nothing
            // unless he is sliding with no slide budget running.
            if (player_ != nullptr) {
                player_->enterArena();
            }
        }
        secondActive_ = true;
        currentArena_ = arena;
        showArena(arena);
    }

    // The collision world is snapshotted HERE, at the oracle's own position:
    // ScriptRuntime.cs:934-935 runs CheckTriggers() and then UpdateArenas(), and
    // UpdateArenas is the only writer of _drawnArenas (:694-695). Everything after it
    // in the C# -- KurtTouchesGroups, Rides.Update, CollectPickups, the object loop
    // (:962-987) and RunControllers (:991) -- can still move CurrentArena,
    // SecondArena and SecondActive (a door opening/closing, an arena_show opcode) and
    // the oracle's pin does NOT follow them until the next tick.
    //
    // CheckTriggers is the other writer, and it is the LAST one above the snapshot: the
    // oracle's own order is `CheckTriggers(); UpdateArenas();` (:934-935), so the arena a
    // trigger box shows or preloads belongs to THIS tick's pin, while a door or an arena_show
    // that runs in the object loop below belongs to the next one's.
    probeLap(&probeNow_.head);
    checkTriggers();
    updateDrawnArenas();
    probeLap(&probeNow_.triggers);
    // KurtTouchesGroups (ScriptRuntime.cs:936), after UpdateArenas and before
    // Rides.Update and CollectPickups (package #3).
    kurtTouchesGroups();
    // Rides.Update (ScriptRuntime.cs:937, package #9): getting on, riding the XD2,
    // getting off -- after Kurt moved, before the pickups and the objects.
    rides_.update();

    // ScriptRuntime.cs:945-946 records Kurt's observed feet delta before the
    // object scripts run. The C# computes it after CollectPickups; pickups and
    // the other movement writers that can precede it are later slices, so the
    // copied Player position is the complete source in this runtime.
    //
    // The cutscene early-return above (ScriptRuntime.cs:890-894) skips this write AND
    // the previous-position update during a cutscene, as the C# does. One oracle
    // detail this slice does not have yet: the C#'s save/load snapshot
    // resets the previous position (ScriptRuntime.Snapshot.cs:37-39), so the
    // first tick after a load observes no movement. The snapshot slice must
    // reset previousKurtPosition_ the same way.
    // CollectPickups (ScriptRuntime.cs:940-943): from the second tick on, after
    // KurtTouchesGroups and Rides.Update and BEFORE the velocity
    // write below, because it reads the segment from the previous position.
    if (tickCount_ > 0) {
        collectPickups();
    }
    kurtVelocity.x = (kurtPosition_.x - previousKurtPosition_.x) * kTicksPerSecond;
    kurtVelocity.y = (kurtPosition_.y - previousKurtPosition_.y) * kTicksPerSecond;
    kurtVelocity.z = (kurtPosition_.z - previousKurtPosition_.z) * kTicksPerSecond;
    previousKurtPosition_ = kurtPosition_;

    // Kurt moves and takes pickups, then fires, then the objects run (game_frame,
    // ScriptRuntime.cs:947-950): one tick of the chain gun while Kurt.Firing.
    if (kurt().firing()) {
        fireChainGun();
    }
    probeLap(&probeNow_.kurtSide);

    // Items.UpdateTwisters (ScriptRuntime.cs:952), then Effects.Update(1f),
    // Fans.Update(), Debris.Update(1f) (ScriptRuntime.cs:953-955): the twisters,
    // effects, fan sparks, then the pieces move, all before the object loop, as in
    // the C#.
    updateTwisters();
    effects.update(1.0f);
    fans.update();
    debris_.update(1.0f);
    // SniperRounds.Update(1f) (ScriptRuntime.cs:956): the rounds fly and hit, before the
    // objects run. AirStrike.Update(1f) next (ScriptRuntime.cs:957, package 14): the strike
    // flies its curve, drops its teeth or dives, and crashes. Then EndLevel?.Update(1f)
    // (ScriptRuntime.cs:958): the end of the level tears, lifts and turns, before the
    // objects run. Its Finished event is EndLevel.cs:1509's LevelEnded(GameOver.No),
    // latched here.
    sniperRounds_.update(1.0f);
    airStrike_.update(1.0f);
    if (endLevel_ && endLevel_->update(1.0f)) {
        levelEnded_ = true;
    }
    // UpdateSniperTarget (ScriptRuntime.cs:959): the scope's lock and zoom limit.
    updateSniperTarget();
    probeLap(&probeNow_.systems);

    // Only Kurt's arena and the active second one update. On the first tick
    // nothing has spawned yet, so this loop is empty; the controller below is
    // what creates the level's objects.
    std::vector<MdkObject*>& snapshot = tickObjects_;
    snapshot.clear();
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        snapshot.push_back(&(*it));
    }
    for (size_t i = 0; i < snapshot.size(); i++) {
        MdkObject* obj = snapshot[i];
        if (obj->dead) {
            continue;
        }
        bool live = obj->arena == currentArena_ || (secondActive_ && obj->arena == secondArena_);
        if (!live) {
            continue;
        }
        // A door's behaviour runs BEFORE its script (ScriptRuntime.cs:970-973), so
        // the script's if_flag_clear sees the state the door engine just wrote.
        if ((obj->flags & MdkObject::FlagDoor) != 0) {
            updateDoor(obj);
        }
        // Where the object was before its script and motion (ScriptRuntime.cs:975): the
        // platform carry below moves Kurt by the difference (package #3).
        const Vec3 carriedFrom = obj->position;
        const float carriedYaw = obj->yaw;
        uint64_t probeObject = 0;
        if (probeClock != nullptr) {
            vm_.probeReset();
            probeObject = probeClock();
        }
        vm_.run(obj);
        const uint64_t probeScript = probeClock != nullptr ? probeClock() : 0;
        // ObjectMotion.Update (ScriptRuntime.cs:976-979: `if (!obj.Dead) Motion.Update(obj)`),
        // shared with the cutscene tick (motionUpdate below).
        if (!obj->dead) {
            motionUpdate(obj);
        }
        // Kurt moves and turns with the platform he stands on (ScriptRuntime.cs:982-986,
        // after Motion.Update), for an effect too. Where motionUpdate returned early the
        // object is dead (a removed thrown item, a cow on its fuse), which the carry skips.
        carryKurt(obj, carriedFrom, carriedYaw);
        if (probeClock != nullptr) {
            const uint64_t now = probeClock();
            probeNow_.objectsRun++;
            if (now - probeObject > probeNow_.slowVm + probeNow_.slowMotion) {
                probeName(probeNow_.slowType, obj->type_name);
                probeNow_.slowVm = probeScript - probeObject;
                probeNow_.slowMotion = now - probeScript;
                probeNow_.slowOps = vm_.probeOps;
                probeNow_.slowOpcode = vm_.probeOpcode;
                probeNow_.slowOpcodeTime = vm_.probeOpcodeTime;
            }
        }
    }
    probeLap(&probeNow_.objects);

    if (probeClock != nullptr) {
        vm_.probeReset();
    }
    runControllers();
    probeLap(&probeNow_.controllers);
    // The controllers are one candidate for the slowest, typed "@ctl".
    if (probeClock != nullptr &&
        probeNow_.controllers > probeNow_.slowVm + probeNow_.slowMotion) {
        probeName(probeNow_.slowType, "@ctl");
        probeNow_.slowVm = probeNow_.controllers;
        probeNow_.slowMotion = 0;
        probeNow_.slowOps = vm_.probeOps;
        probeNow_.slowOpcode = vm_.probeOpcode;
        probeNow_.slowOpcodeTime = vm_.probeOpcodeTime;
    }
}

// C# ObjectMotion.Update, as RunTick's object loop and CutsceneTick (ScriptRuntime.cs:1398-1402)
// both call it after Vm.Run: one copy, so the cutscene's objects move exactly as the game's.
void ScriptRuntime::motionUpdate(MdkObject* obj) {
    // ObjectMotion.Update's first branch (ObjectMotion.cs:54-68): an effect (an
    // explosion) only plays its texture animation and returns, so nothing below --
    // the path, the velocity, the arena crossing, the pickup, the animation, the
    // roll -- runs for it; every gate below tests the same `effect_frames <= 0`.
    if (!obj->dead && obj->effect_frames > 0) {
        motionEffect(obj);
        return;
    }
    // The path-following half of Motion.Update, gated on liveness exactly as
    // the C# (Vm.Run then `if (!Dead) Motion.Update`): a script that kills the
    // object this tick must not have it path-moved afterwards.
    if (!obj->dead) {
        motionPath(obj);
        motionVelocity(obj);
        // An object that changes arena follows its own move through a doorway
        // (ObjectMotion.cs:104-107, after ApplyVelocity and before the pickup
        // dispatch). motionVelocity returns early on a zero-length move, and so
        // does the C#'s ApplyVelocity -- the C#'s Update keeps going to
        // FollowArenas either way, so the hook is HERE, not inside
        // motionVelocity, even though a move that did not happen crosses nothing.
        //
        // The effect gate is NOT decoration: ObjectMotion.Update returns at
        // :54-68 for EffectFrames > 0, BEFORE ApplyVelocity and therefore before
        // FollowArenas at :106, so an effect object never evaluates a crossing in
        // the oracle. It matters here for the same reason it does there:
        // previous_position is written only for a non-effect object (below, with
        // Roll and Bank), so an effect's "move" would be read off a stale segment
        // from before the effect started and could relocate it across a doorway.
        //
        // The gate is the INVERSE of the oracle's predicate, so it is `<= 0` and
        // not `== 0`: the C# returns for a POSITIVE count and runs the normal path
        // for anything else, and the three gates in this loop must agree with
        // motionPath() and motionVelocity(), which test `> 0` to return.
        // No writer in either tree makes it negative today (the oracle's two are
        // ScriptRuntime.cs:1780 and :1784, both positive counts), so this is
        // exactness against the oracle rather than a live behaviour change, and no
        // golden can distinguish the two spellings until an effects port writes a
        // writer.
        if (!obj->dead && obj->effect_frames <= 0 &&
            (obj->flags & MdkObject::FlagChangesArena) != 0) {
            followArenas(obj);
        }
        // Kurt's items (ObjectMotion.cs:110-118): a thrown item (ThrownKind > 0) goes
        // to Items.UpdateThrown, and the C# returns when that removed it -- no
        // animation, roll, bank or previous position for it this tick; otherwise
        // UpdatePickup for a pickup. Both only if the object survived the velocity
        // step (the C# returns on Dead before them), and never for an effect object.
        if (!obj->dead && obj->effect_frames <= 0 && obj->thrown_kind > 0) {
            itemsUpdateThrown(obj);
            if (obj->dead) {
                return;
            }
        } else if (!obj->dead && obj->effect_frames <= 0 &&
                   (obj->flags & MdkObject::FlagPickup) != 0) {
            motionPickup(obj);
        } else if (!obj->dead && obj->effect_frames <= 0 &&
                   (obj->flags & MdkObject::FlagCow) != 0) {
            // SW_EWJ's holy cow (ObjectMotion.cs:120-127): it may be killed by its fuse.
            updateCow(obj);
            if (obj->dead) {
                return;
            }
        }
        // AdvanceAnimation (ObjectMotion.cs:132), after ApplyVelocity and the
        // pickup dispatch. An effect object returned from Update before this
        // point, so it never advances an animation here. Same inverse predicate
        // as the two gates above: `<= 0`, because the oracle returns on `> 0`.
        if (!obj->dead && obj->effect_frames <= 0) {
            float time = obj->animation_time;
            advanceAnimation(obj, kTick);
            // The frame sound (ObjectMotion.cs:133): fired once when the frame
            // clock crosses its frame, at the object's position.
            if (!obj->frame_sound.empty() && obj->animation != nullptr &&
                time < (float)obj->frame_sound_frame &&
                (float)obj->frame_sound_frame <= time + kTick * obj->animation_rate *
                                                       obj->animation->speed) {
                playSound(obj, obj->frame_sound, 0, formats::ScriptValue());
                obj->frame_sound.clear();
            }
            // Roll (ObjectMotion.cs:139-143), then PreviousPosition
            // (ObjectMotion.cs:146) -- IN THAT ORDER, and both here rather than
            // at the end of motionVelocity: Roll reads the PREVIOUS frame's
            // position, so writing it any earlier makes every roll read a zero
            // movement and the basis never turns.
            if ((obj->flags & MdkObject::FlagRolling) != 0) {
                obj->update_rolling();
            }
            // Bank (ObjectMotion.cs:145), between Roll and PreviousPosition.
            motionBank(obj);
            obj->previous_position = obj->position;
        }
    }
}

void ScriptRuntime::runControllers() {
    if (!currentArena_.empty()) {
        vm_.run(&getArenaState(currentArena_).controller);
    }
    if (secondActive_ && !secondArena_.empty()) {
        vm_.run(&getArenaState(secondArena_).controller);
    }
}
