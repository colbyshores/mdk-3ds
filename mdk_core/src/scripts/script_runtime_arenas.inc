// script_runtime_arenas.inc -- the arenas: the drawn set, show/preload/load, the doors' pulls, entering, corridors, teleports, triggers, spawning.
// Part of script_runtime.cpp's single translation unit (see its header): pasted
// inside namespace mdk::scripts after its file-local constants and helpers,
// which it uses. No include guards, no includes of its own; the order is
// load-bearing.

void ScriptRuntime::updateDrawnArenas() {
    // C# ScriptRuntime.UpdateArenas (:658-664, the drawn half). The current arena goes
    // in even when it is "", exactly as :660 does: the empty name is ArenaSpace's to
    // drop (its C# Named() finds no arena called ""), not this function's. The second
    // goes in only while it is BOTH active and named (:661-664). Both rooms being in
    // the list is the whole point of it: move() sweeps element 0 as Kurt's own arena
    // and the rest as the free pass (arena_space.cpp), so the room behind the door he
    // came through keeps its floor.
    //
    // First the loaded half (:650-692, 0x419cb0): the current arena (named or not) and
    // the second (named, active or not) are loaded; when that pair changes, an arena
    // that left it is put away -- Kurt's thrown items and effects there (flags 0x1000 or
    // 0x4000, so a collected pickup too) go, the other objects' loop sounds stop -- and
    // one that came back restarts its objects' loop sounds (arena_activate 0x43f8e0).
    // The comparison is the C#'s Same: ordered, element by element.
    std::vector<std::string> loaded;
    loaded.push_back(currentArena_);
    if (!secondArena_.empty()) {
        loaded.push_back(secondArena_);
    }
    if (loaded != loadedArenas_) {
        std::vector<MdkObject*> snapshot;
        for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
            if (!it->dead) {
                snapshot.push_back(&(*it));
            }
        }
        for (size_t i = 0; i < snapshot.size(); i++) {
            MdkObject* obj = snapshot[i];
            const bool wasLoaded = std::find(loadedArenas_.begin(), loadedArenas_.end(),
                                             obj->arena) != loadedArenas_.end();
            const bool isLoaded = std::find(loaded.begin(), loaded.end(), obj->arena) != loaded.end();
            if (wasLoaded && !isLoaded) {
                if ((obj->flags & (kItemFlagThrown | kItemFlagActive)) != 0) {
                    itemsForget(obj);
                    remove(obj);
                    continue;
                }
                mixer_.stopVoice(obj->loop_sound);
                obj->loop_sound = 0;
            } else if (isLoaded && !wasLoaded && !obj->loop_sound_name.empty()) {
                obj->loop_sound = playOn(obj, obj->loop_sound_name, audio::SoundMixer::New, Vec3());
            }
        }
        loadedArenas_ = loaded;
    }

    drawnArenas_.clear();
    drawnArenas_.push_back(currentArena_);
    if (secondActive_ && !secondArena_.empty()) {
        drawnArenas_.push_back(secondArena_);
    }
    // obj.Visible (:696-699): an object is drawn only while its arena is. This is
    // the consumer the drawn list was waiting for -- until it was written every
    // object of every arena the scripts had ever shown was drawn, wherever Kurt was
    // -- and it is what lets the frontends keep only the drawn arenas' textures
    // (game/arena_views.h). The C#'s other term, `!Rides.Hides(obj)` (the bomber's
    // XE once the view is inside it), is rides_.hides(), false until the bomber
    // (package #11) lands.
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        bool drawn = false;
        for (size_t i = 0; i < drawnArenas_.size() && !drawn; i++) {
            drawn = drawnArenas_[i] == it->arena;
        }
        it->visible = drawn && !rides_.hides(&(*it));
    }
}

const std::vector<std::string>& ScriptRuntime::solidArenas() const {
    // C# ScriptRuntime.SolidArenas (ScriptRuntime.cs:265-282). Off the board it is the
    // drawn list itself -- the snapshot updateDrawnArenas() took at the tick's own
    // UpdateArenas point, NOT a fresh read of currentArena_/secondArena_ (which is what
    // a door or an arena_show that ran later in the tick would have moved). On the
    // board it is drawn[0] alone (:274-280), and drawn[0] is always the current arena
    // because :660 adds it unconditionally -- so the board case is this list without the
    // second, which is what the `!rides_.onBoard()` half below encodes. The snowboard
    // (package #10, scripts/snowboard.cpp) reaches it: golden/L4_board_*.txt pin the one
    // name from the mount on, and both names again once Kurt is off.
    //
    // Before the first tick drawnArenas_ is empty and so is the C#'s _drawnArenas:
    // ArenaSpace::setSolid then clears its pin and Kurt collides with the arenas around
    // his feet, in both trees.
    if (!rides_.onBoard()) {
        return drawnArenas_;
    }
    solidArenas_.clear();
    if (!drawnArenas_.empty()) {
        // drawn[0] is the current arena, so on the board the room behind the door is
        // NOT swept: that is the oracle's rule, not an oversight (:274-280).
        solidArenas_.push_back(drawnArenas_[0]);
    }
    return solidArenas_;
}

void ScriptRuntime::showArena(const std::string& arena) {
    if (arena.empty() || arena == "NONE") {
        secondArena_ = "";
        secondActive_ = false;
        return;
    }

    if (arena == currentArena_) {
        // The C# loads Kurt's own arena too (:582-586): it is the one arena whose doors
        // PullDoors must NOT move, but its resident-arena bookkeeping still runs.
        loadArena(arena);
    } else if (arena != secondArena_) {
        secondArena_ = arena;
        loadArena(arena);
    }

    secondActive_ = true;
    ArenaState& state = getArenaState(arena);
    if (!state.started) {
        state.started = true;
        spawnDtiAliens(arena);
    }
}

// C# ScriptRuntime.PreloadArena (ScriptRuntime.cs:602-612, arena_set_neighbour 0x41a2d0, VM
// opcode 223): the arena takes the SECOND slot and is loaded, but the second stays INACTIVE,
// so the room is loaded ahead of Kurt -- its doors pulled, its MTO resident -- without being
// drawn or live with him. "NONE" is the C#'s NoArena (:85). The `arena == SecondArena` test
// runs BEFORE the assignment (:604), so preloading the arena that is already the second does
// nothing at all: not the deactivation, not LoadArena, so it pulls that arena's doors no
// second time. SecondArena is reassigned BEFORE the load and SecondActive is cleared AFTER it
// (:609-611), so during a preload the previous second is no longer SecondArena by the time
// PullDoors runs and the only exemption that can apply to its doors is Kurt's own arena. That
// is the oracle's ordering and HALF of it is observable: hoisting the assignment above the
// load leaves the previous second in `secondArena_` while PullDoors runs, and a door held by
// a neighbour that is that previous second is then exempt and does not move. Moving the clear
// above the load is NOT observable, and the reason is worth writing down because it is the
// reason a reader cannot see it: once `secondArena_` is the arena being loaded, the second
// exemption would need a connection whose far end is that same arena, and `findConnections()`
// never writes one -- its only writer pairs two DIFFERENT names (the `names[a] != names[b]`
// guard), so no arena is ever its own neighbour.
void ScriptRuntime::preloadArena(const std::string& arena) {
    if (arena.empty() || arena == "NONE" || arena == secondArena_) {
        return;
    }
    secondArena_ = arena;
    loadArena(arena);
    secondActive_ = false;
}

// C# ScriptRuntime.LoadArena (ScriptRuntime.cs:615-623, arena_load 0x419d00): "only one arena
// (not corridor) is loaded at a time" -- the MTO holds a ROOM's geometry and a corridor is not
// in it, so loading a corridor leaves the resident arena alone and only pulls its doors.
// Three oracle callers, all three ported: showArena, preloadArena, and TeleportSecond
// (:516) -- the last one landed with TeleportKurt, so a teleport that only names the
// neighbour does not leave it an inactive, unloaded second with its doors still parked
// in the room it came from.
void ScriptRuntime::loadArena(const std::string& arena) {
    if (level_->mto().has(arena)) {
        residentArena_ = arena;
    }
    pullDoors(arena);
}

// Defined below with the rest of the door engine (ObjectBehaviors.MoveDoor, 0x43ca00).
static void moveDoor(MdkObject* obj);

// C# ScriptRuntime.PullDoors (ScriptRuntime.cs:628-643): an arena being loaded takes the doors
// leading to it out of the neighbours that hold them and turns them around, so Kurt meets them
// in the room he is walking towards -- except from his own arena and from the active second,
// which keep theirs. Without it a door stays parked in the corridor it was spawned in, which
// is the artefact the oracle's DoorArenaTests class comment opens with ("the doorway showed the
// sky"): the door is not in the drawn list, so neither is its geometry.
//
// The pixel half of that artefact is NOT reachable here yet. What makes an undrawn door
// invisible is the drawn-list CONSUMERS -- the C#'s `obj.Visible` (ObjectBehaviors, unported;
// MdkObject::visible still defaults to true and has no writer) and the renderer's arena list --
// and neither is ported, so geometry is drawn regardless of the list. This restores the oracle's
// RULE ahead of its effect, the way residentArena_ is written ahead of its reader; the batch that
// ports obj.Visible still owes itself the capture that shows the doorway change.
//
// The C#'s Objects.Where(...) is a filter over the whole object list and the loop below is the
// same predicate per object; moveDoor changes only arena/connects/yaw, so nothing here
// disturbs the sequence the way a removal would.
void ScriptRuntime::pullDoors(const std::string& arena) {
    const std::vector<formats::Dti::Record>* records = arenaRecords(arena);
    if (records == nullptr) {
        return;  // the C#'s ArenaRecords() is an empty list for an arena the DTI does not know
    }
    for (size_t i = 0; i < records->size(); i++) {
        const formats::Dti::Record& record = (*records)[i];
        if (record.type != kRecordConnection) {
            continue;
        }
        std::map<std::pair<std::string, int32_t>, std::string>::const_iterator other =
            connections_.find(std::make_pair(arena, record.id));
        if (other == connections_.end()) {
            continue;
        }
        // The two exemptions, in the C#'s order (:633): the neighbour Kurt is standing in, and
        // the neighbour that is the ACTIVE second. Both keep their door, because a door pulled
        // out of a drawn arena would leave its doorway showing nothing.
        if (other->second == currentArena_ || (secondActive_ && other->second == secondArena_)) {
            continue;
        }
        for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
            MdkObject* door = &(*it);
            if (!door->dead && (door->flags & MdkObject::FlagDoor) != 0 &&
                door->arena == other->second && door->connects == arena) {
                moveDoor(door);
            }
        }
    }
}

// C# ScriptRuntime.EnterArena (ScriptRuntime.cs:449-456): the arena joins Kurt's
// collision space. This is how an arena no doorway leads to becomes solid -- until a
// script teleports Kurt into it, nothing puts it in the space (level.gd enter_arena),
// and ArenaSpace::add already refuses an arena with no vertices, as the C#'s Add does.
// The C#'s `ArenaEntered?.Invoke(arena)` runs first; its one subscriber is
// LevelView.Enter (Viewer.cs:255 -> Level/LevelView.cs:87), which sets `Reachable` on the
// arena's VIEW so the draw includes it. The level view is presentation and is deferred,
// so the event has no C++ twin and this is the half that has a consumer.
void ScriptRuntime::enterArena(const std::string& arena) {
    const formats::Arena* entered = level_->arenaNamed(arena);
    if (entered != nullptr) {
        space_.add(entered);
    }
}

// C# ScriptRuntime.IsCorridorLoaded (ScriptRuntime.cs:522-531): the corridors loaded with
// an arena are its own, Kurt's, and the second's -- so a teleport into one of them does
// not have to go looking for a room to load ahead. This is the ONLY reader of
// residentArena_ in the whole oracle, which is what batch 139 left "written and not yet
// read"; with TeleportKurt ported it is read.
bool ScriptRuntime::isCorridorLoaded(const std::string& corridor, const std::string& from) {
    if (corridor == secondArena_ || corridor == from) {
        return true;
    }
    const std::vector<formats::Dti::Record>* records = arenaRecords(residentArena_);
    if (records == nullptr) {
        return false;  // the C#'s `EntryNamed(...)?.Records ?? []` for a name no arena has
    }
    for (size_t i = 0; i < records->size(); i++) {
        const formats::Dti::Record& record = (*records)[i];
        if (record.type != kRecordConnection) {
            continue;
        }
        std::map<std::pair<std::string, int32_t>, std::string>::const_iterator other =
            connections_.find(std::make_pair(residentArena_, record.id));
        if (other != connections_.end() && other->second == corridor) {
            return true;
        }
    }
    return false;
}

// C# ScriptRuntime.TeleportSecond (ScriptRuntime.cs:496-519, 0x41bce4): what the SECOND
// slot does when a teleport moves Kurt, and it runs BEFORE ShowArena (:468 before :469),
// which is what makes the third arm observable -- ShowArena then activates whatever this
// left in the slot. Three arms, in the C#'s order:
//   * into a ROOM (the MTO has it): the second is emptied and deactivated. A room is
//     drawn on its own; a second would be a room behind a door that does not exist.
//   * into a corridor that is already loaded (IsCorridorLoaded): NOTHING. The second
//     keeps what it had, because the room it names is still the one on the other side.
//   * into a corridor not loaded: the LAST arena in DTI order that is a room and has a
//     connection into it (CDANT_1 -> DANT_2), named as the second, INACTIVE, and LOADED
//     ahead -- the LoadArena batch 139 recorded as owed here (:516) is here, so the
//     room's doors are pulled into it before Kurt arrives.
void ScriptRuntime::teleportSecond(const std::string& arena, const std::string& from) {
    if (level_->mto().has(arena)) {
        secondArena_ = std::string();
        secondActive_ = false;
        return;
    }

    if (isCorridorLoaded(arena, from)) {
        return;
    }

    // Level.Dti.Arenas.LastOrDefault(...): the scan runs to the end and keeps the LAST
    // match, so this cannot break out early -- two rooms reaching one corridor is rare in
    // shipped data but it is a DTI that answers differently, not a bug to fix.
    const std::vector<formats::Dti::ArenaEntry>& arenas = level_->dti().arenas();
    std::string neighbour;
    bool found = false;
    for (size_t i = 0; i < arenas.size(); i++) {
        const formats::Dti::ArenaEntry& entry = arenas[i];
        if (!level_->mto().has(entry.name)) {
            continue;
        }
        for (size_t j = 0; j < entry.records.size(); j++) {
            const formats::Dti::Record& record = entry.records[j];
            if (record.type != kRecordConnection) {
                continue;
            }
            std::map<std::pair<std::string, int32_t>, std::string>::const_iterator other =
                connections_.find(std::make_pair(entry.name, record.id));
            if (other != connections_.end() && other->second == arena) {
                neighbour = entry.name;
                found = true;
                break;  // this arena matches; keep looking for a LATER one
            }
        }
    }

    secondArena_ = found ? neighbour : std::string();
    secondActive_ = false;
    if (found) {
        loadArena(neighbour);
    }
}

// C# ScriptRuntime.TeleportKurt (ScriptRuntime.cs:460-481, teleport_player 0x41bce4,
// VM opcodes 112 and 173): "Moves Kurt (teleport_player): within the arena with a white
// flash when `arena` is empty, otherwise into that arena." The order is the C#'s and two
// parts of it are load-bearing. (1) CurrentArena is written BEFORE TeleportSecond, so
// TeleportSecond's `from` is the arena Kurt LEFT while ShowArena sees the one he ENTERED
// -- which is why a teleport into a corridor leaves ShowArena taking its `arena ==
// CurrentArena` branch instead of writing the second slot a second time. (2) The
// previous position is set to the DESTINATION and teleported_ is raised: no teleport is
// ever a move across a connection, and the first-tick rule above must not overwrite it.
void ScriptRuntime::teleportKurt(const std::string& arena, const Vec3& position, float yaw) {
    if (!arena.empty()) {
        enterArena(arena);

        const std::string from = currentArena_;
        currentArena_ = arena;
        teleportSecond(arena, from);
        showArena(arena);

        // No move crosses a connection (0x41bce4 sets 0x5739cc too).
        previousKurtPosition_ = position;
        teleported_ = true;
    }

    // The C# cannot reach here without a Kurt: its constructor requires one
    // (ScriptRuntime.cs:168). With no Player bound there is no entity to move, and
    // writing only the runtime's own snapshot would invent a Kurt whose feet the next
    // tick overwrites with the place he was NOT teleported to -- the same guard and the
    // same reason pushKurt records.
    if (player_ != nullptr) {
        player_->teleport(position, yaw);
    }

    if (arena.empty()) {
        // MathF.Max, so a flash already standing stronger than the teleport's is kept --
        // and mathMax propagates a NaN through it exactly as MathF does.
        kurt().setWhiteFlash(mathMax(kurt().whiteFlash(), kTeleportFlash));
    }
}

// C# ScriptRuntime.CheckTriggers (ScriptRuntime.cs:707-740, 0x41bf1c): the DTI trigger boxes
// of KURT'S arena. The move it tests is THIS tick's, and in XY ONLY: `from` is the previous
// position the tick has not published yet (the C# writes _previousKurtPosition at :946, below
// both this call and the crossing, and so does runTick) and `to` is Kurt's feet now. Z is not
// part of a trigger box at all -- the reader fills the record's third coordinate and :718
// ignores it -- so a box catches Kurt at any height above it, which is what the shipped boxes
// assume: level 7's CDANT_1 pair sits at z = -42..-36 under a tunnel whose floor is -3, and a
// Z test would switch both of them off.
void ScriptRuntime::checkTriggers() {
    const formats::Vec2f from{previousKurtPosition_.x, previousKurtPosition_.y};
    const formats::Vec2f to{kurtPosition_.x, kurtPosition_.y};
    const std::vector<formats::Dti::Record>* records = arenaRecords(currentArena_);
    if (records == nullptr) {
        return;  // the C#'s `EntryNamed(CurrentArena)?.Records ?? []` (:711)
    }
    const std::vector<formats::Dti::ArenaEntry>& arenas = level_->dti().arenas();
    for (size_t i = 0; i < records->size(); i++) {
        const formats::Dti::Record& record = (*records)[i];
        if (record.type != kRecordTriggerShow && record.type != kRecordTriggerLoad) {
            continue;
        }
        // mathMin/mathMax, not fminf/fmaxf: the C# writes MathF.Min/MathF.Max (:718-719),
        // which propagates a NaN corner instead of hiding it. Same reason as spans().
        const formats::Vec2f min{mathMin(record.position[0], record.boxEnd[0]),
                                 mathMin(record.position[1], record.boxEnd[1])};
        const formats::Vec2f max{mathMax(record.position[0], record.boxEnd[0]),
                                 mathMax(record.position[1], record.boxEnd[1])};
        // Closed near side, open far side, and on the END of the move only (:720). A move that
        // starts inside and walks out is caught by crossesBox, not by this.
        const bool inside = to.x >= min.x && to.y >= min.y && to.x < max.x && to.y < max.y;
        if (!inside && !crossesBox(min, max, from, to)) {
            continue;
        }
        // The id is an INDEX INTO THE ARENA LIST (:726) -- not a name, and not a connection id.
        // -1, the "clear it" box every shipped level ships, and an id past the end both name no
        // arena; the two types then split, and the LOAD of no arena goes through the
        // ShowArena("") arm rather than through PreloadArena (:731-738).
        const std::string arena =
            (record.id >= 0 && (size_t)record.id < arenas.size())
                ? arenas[(size_t)record.id].name
                : std::string();
        if (record.type == kRecordTriggerShow) {
            showArena(arena);
        } else if (arena.empty()) {
            showArena(std::string());
        } else {
            preloadArena(arena);
        }
    }
}

ArenaState& ScriptRuntime::getArenaState(const std::string& arena) {
    std::map<std::string, ArenaState>::iterator it = arenas_.find(arena);
    if (it != arenas_.end()) {
        return it->second;
    }
    int32_t script = 0;
    formats::findOffset(&cmi_->arenaScripts(), arena, &script);
    std::pair<std::map<std::string, ArenaState>::iterator, bool> inserted =
        arenas_.emplace(arena, ArenaState(arena, script));
    // The C#'s Dictionary keeps its insertion order, and a save lists the arenas
    // in it (package #19).
    arenaOrder_.push_back(arena);
    return inserted.first->second;
}

void ScriptRuntime::spawnDtiAliens(const std::string& arena) {
    const formats::Dti::ArenaEntry* entry = level_->entryNamed(arena);
    if (entry == nullptr) {
        return;
    }
    MdkObject& controller = getArenaState(arena).controller;
    for (size_t i = 0; i < entry->records.size(); i++) {
        const formats::Dti::Record& record = entry->records[i];
        if (record.type == kDtiAlien) {
            std::string key = arena + "$" + record.name + "_" + std::to_string(record.id);
            int32_t script = 0;
            formats::findOffset(&cmi_->alienScripts(), key, &script);
            Vec3 position;
            position.x = record.position[0];
            position.y = record.position[1];
            position.z = record.position[2];
            spawn(&controller, record.name, position, record.angle, record.id, script, false);
            continue;
        }
        if (record.type != kDtiStatic) {
            continue;
        }
        std::string key = arena + "$" + record.name;
        int32_t script = 0;
        formats::findOffset(&cmi_->alienScripts(), key, &script);
        Vec3 position;
        position.x = record.position[0];
        position.y = record.position[1];
        position.z = record.position[2];
        MdkObject* obj = spawn(&controller, record.name, position, record.angle, -1, script, false);
        if (obj != nullptr) {
            obj->flags |= kDtiStaticFlags;
            obj->health = 1;
        }
    }
}

MdkObject* ScriptRuntime::spawn(MdkObject* parent, const std::string& type, const Vec3& position,
                                float yaw, int32_t instance, int32_t script, bool flagged) {
    const uint64_t probeStart = probeClock != nullptr ? probeClock() : 0;
    const Model* model = findModel(parent->arena, type);
    if (model == nullptr) {
        return nullptr;
    }
    const uint64_t probeModel = probeClock != nullptr ? probeClock() - probeStart : 0;

    MdkObject obj;
    obj.arena = parent->arena;
    obj.type_name = type;
    obj.model = model;
    obj.instance_id = instance >= 0 ? instance : nextInstance_;
    obj.position = position;
    obj.spawn_position = position;
    obj.previous_position = position;
    obj.yaw = wrap360(yaw);
    if (instance < 0) {
        nextInstance_++;
    }
    if (flagged) {
        obj.flags = kSpawnFlaggedFlags;
    }

    objects_.push_back(obj);
    MdkObject* created = &objects_.back();
    // The Score-o-matic's enemies (ScriptRuntime.cs:1576), counted at creation; the
    // chain gun's kills are the other half of the same pair (chainGunHit).
    stats.countEnemy(type, GameStats::Created);

    // The object type's init script runs once at creation.
    std::string key = parent->arena + "$" + type;
    int32_t initScript = 0;
    formats::findOffset(&cmi_->objectScripts(), key, &initScript);
    if (initScript != 0) {
        created->restart = initScript;
        vm_.run(created);
    }

    created->restart = script;
    // The tick probe: spawns counted and timed (a spawn inside a spawn's init
    // script counts in both).
    if (probeClock != nullptr) {
        const uint64_t spent = probeClock() - probeStart;
        probeNow_.spawns++;
        probeNow_.spawnTime += spent;
        if (spent > probeNow_.spawnSlow) {
            probeName(probeNow_.spawnType, type);
            probeNow_.spawnSlow = spent;
            probeNow_.spawnModel = probeModel;
        }
    }
    return created;
}

void ScriptRuntime::spawnConnector(MdkObject* obj, const std::string& type, const Vec3& position,
                                   float yaw, int32_t instance, const std::string& otherArena,
                                   int32_t script) {
    static const int32_t kDoorFlags = 0x1108000;
    if (otherArena == "NONE") {
        return;
    }

    // A door already linking these two arenas is reused (moved here if out of sight),
    // not spawned again.
    for (std::deque<MdkObject>::iterator it = objects_.begin(); it != objects_.end(); ++it) {
        MdkObject& other = *it;
        if (other.dead || other.type_name != type ||
            !((other.arena == obj->arena && other.connects == otherArena) ||
              (other.arena == otherArena && other.connects == obj->arena))) {
            continue;
        }
        if (other.arena != currentArena_ && other.arena != secondArena_) {
            other.arena = obj->arena;
            other.connects = otherArena;
        }
        return;
    }

    MdkObject* door = spawn(obj, type, position, yaw, instance, script, false);
    if (door == nullptr) {
        return;
    }
    door->flags |= kDoorFlags;
    door->connects = otherArena;
    setupDoor(door);
}
