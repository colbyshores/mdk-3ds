// player_state.inc -- the state machine, animation, sounds, the mouse look, teleport, damage and the knockdown.
// Part of player.cpp's single translation unit (see its header): pasted inside
// namespace mdk after its file-local constants and helpers, which it uses.
// No include guards, no includes of its own; the order is load-bearing.

void Player::updateState(float forward, float strafe, float delta) {
    _stateTime += delta;
    _airFor = onFloor ? 0.0f : _airFor + delta;

    // C# UpdateCombatState's first arm (Kurt.Combat.cs:371): health zero on a floor is
    // the death state. The knock-down, get-up and throw arms below are the C#'s next
    // three.
    if (health() == 0 && onFloor) {
        // C# UpdateCombatState's first arm (Kurt.Combat.cs:371-377): StopFiring, the
        // flash zeroed, State.Dead. updateFiring has already refused a corpse the gun
        // (Health > 0 is in its test), so the StopFiring is the oracle's belt and braces.
        // The zeroing is not: it is what makes the
        // death beat the oracle's 255 steps whatever the red flash was doing a step
        // earlier, and it is the reason the frontend's old stand-in (a skull fade of its
        // own, started from its own clock) could not be too far out -- it was starting
        // from the same zero, for the same 255/60 s.
        stopFiring();
        combat_.setHurtFlash(0.0f);
        setState(Kurt::Dead);
        return;
    }

    // C# UpdateCombatState's knock-down arms (Kurt.Combat.cs:379-399). K_BANG runs
    // frameCount(Knocked) frames at one frame per tick; then he flips, the push is
    // cleared at the flip, and K_BFLIP runs frameCount(GetUp). The flip carries the
    // leftover of the frame K_BANG ended on -- AnimationFrame - bang, not a reset to
    // zero -- so a step that crosses the boundary does not lose the fraction of the
    // frame it overshot by.
    if (state() == Kurt::Knocked) {
        _animationFrame += kTicks * delta;
        const float bang = (float)frameCount(Kurt::Knocked);
        if (_animationFrame < bang) {
            return;
        }
        const float flip = _animationFrame - bang;
        _knockdownPush.x = 0.0f;
        _knockdownPush.y = 0.0f;
        setState(Kurt::GetUp);
        _animationFrame = flip;
        return;
    }

    if (state() == Kurt::GetUp) {
        _animationFrame += kTicks * delta;
        if (_animationFrame < (float)frameCount(Kurt::GetUp)) {
            return;
        }
        // Back on his feet -- and the C# returns FALSE here, so the ordinary state
        // selection and Animate run on the very step he stands up: a Kurt who was
        // running when he went down has his run frame again at once, and one who was
        // standing goes through the Still/Idle test like anyone else.
        setState(Kurt::Still);
    } else if (state() == Kurt::Throw && onFloor &&
               _animationFrame < (float)(frameCount(Kurt::Throw) - 1)) {
        // C# UpdateCombatState's throw arm (Kurt.Combat.cs:410-423): Kurt stands still while
        // throwing (update() zeroed his walk) and the item leaves his hand on the step
        // K_SPWEP's frame clock crosses frame 8 (ThrowFrame, damp_animate). The arm owns the
        // step until the last frame; in the air, or on the last frame, the C# returns false
        // and the ordinary selection below takes him out of THROW. The GetUp arm above
        // returns false in the C# without reaching this test, which the else keeps.
        // Truncations, not floors: AnimationFrame only grows here from 0.
        const int32_t kThrowFrame = 8;
        const int32_t previous = (int32_t)_animationFrame;
        _animationFrame += kTicks * delta;
        if (previous < kThrowFrame && (int32_t)_animationFrame >= kThrowFrame) {
            itemUsed();
        }
        return;
    }

    // The C# has no missing-animation guard: a missing animation has count 0,
    // so `done` is `AnimationFrame >= -1`. A missing animation is a broken
    // archive, not a state; the comparison is kept exactly as the oracle's.
    const bool done = _animationFrame >= (float)(frameCount(state()) - 1);
    if (!onFloor) {
        if (chuteOpen) {
            setState(Kurt::Chute);
        } else if (state() == Kurt::Chute) {
            // The closing frame is shown once, then the fall starts.
            _chuteClosing = true;
            setState(Kurt::Fall);
            return;
        } else if (verticalSpeed > 0.0f &&
                   state() != Kurt::Jump && state() != Kurt::RunJump && state() != Kurt::Fall) {
            setState(std::fabs(forwardSpeed) > kMovingSpeed ? Kurt::RunJump : Kurt::Jump);
        } else if (verticalSpeed < kFallStartSpeed &&
                   ((state() != Kurt::Jump && state() != Kurt::RunJump) || done) &&
                   (_airFor >= kFallGrace || state() == Kurt::Jump ||
                    state() == Kurt::RunJump || state() == Kurt::Fall)) {
            // Upstream 3f9315c5: a running or standing Kurt falls only after kFallGrace.
            setState(Kurt::Fall);
        }
    } else if (state() == Kurt::Jump || state() == Kurt::RunJump || state() == Kurt::Fall ||
               state() == Kurt::Chute) {
        playSound("LAND");
        setState(Kurt::Land);
    } else if (state() == Kurt::Land && !done && forward == 0.0f && strafe == 0.0f) {
        // The landing animation keeps running while he stands still.
    } else if (forward != 0.0f || std::fabs(forwardSpeed) > kMovingSpeed) {
        // Kurt.cs:663: running while the gun fires is K_RUNFIR.
        setState(combat_.firing() ? Kurt::RunFire : Kurt::Run);
    } else if (strafe != 0.0f || std::fabs(strafeSpeed) > kMovingSpeed) {
        setState(Kurt::Side);
    } else if (std::fabs(turnRate) > kMovingSpeed) {
        setState(Kurt::Turn);
    } else if (combat_.firing()) {
        // Kurt.cs:673: standing and firing is K_SHOT, ahead of the idle test, so a
        // Kurt who fires never goes idle.
        setState(Kurt::Shot);
    } else if (state() == Kurt::Idle && !done) {
        // The idle animation keeps running.
    } else if (state() == Kurt::Still && _stateTime > kIdleDelay) {
        setState(Kurt::Idle);
    } else if (state() != Kurt::Still) {
        setState(Kurt::Still);
    }

    animate(delta);
}

void Player::updateSpecial(float delta) {
    // C# Kurt.UpdateSpecial's first arm (Kurt.cs:598-601): climbing a ledge.
    if (state() == Kurt::Hang) {
        updateClimb(delta);
        return;
    }
    // C# Kurt.UpdateSpecial for K_CRASHL: gravity and the fall continue, and
    // the animation advances at two original ticks per frame. It is held at
    // tick eight while Kurt is still falling.
    _stateTime += delta;
    verticalSpeed = std::max(verticalSpeed - kGravity * delta, -kMaxFallSpeed);
    fall(delta);
    // C# UpdateSpecial (Kurt.cs:607): the objects his box touches after the fall.
    updateTouched();
    const float ticks = _animationFrame * (float)kCrashTicksPerFrame;
    float next = ticks + kTicks * delta;
    if (ticks <= (float)kCrashHoldTick && next >= (float)kCrashHoldTick && !onFloor &&
        verticalSpeed <= 0.0f && !chuteOpen) {
        next = (float)kCrashHoldTick;
    }
    _animationFrame = next / (float)kCrashTicksPerFrame;
    // The C# compares against the frame count with no guard: a missing
    // K_CRASHL (count 0) leaves the hard landing at once.
    if (_animationFrame >= (float)frameCount(Kurt::HardLand)) {
        setState(Kurt::Still);
    }
}

void Player::updateDeath(float delta) {
    // C# UpdateDeath's first statement (Kurt.Combat.cs:238): once the skull has faded
    // in, the corpse is finished -- no gravity, no fall, no state time, no frame. It is
    // not a cosmetic freeze: a corpse below the arena stops falling here, and the one on
    // a pad stops alternating OnFloor the way Fall makes it while the skull is still
    // rising. golden/L3_kurt_flashes.txt prints the step it starts on.
    if (combat_.skullFinished()) {
        return;
    }
    forwardSpeed = 0.0f;
    strafeSpeed = 0.0f;
    verticalSpeed = onFloor ? 0.0f : std::max(verticalSpeed - kGravity * delta, -kMaxFallSpeed);
    // Kurt.Combat.cs:246: the updraft runs for a corpse too. Its Health > 0 test makes
    // it the NaN arm here: the FAN loop stops, the flag clears and an upward speed over
    // 40 u/s is capped -- a Kurt who dies in a fan's box falls out of it.
    updateUpdraft(delta);
    // The C# calls Fall() on either branch. It is the complete fall operation,
    // not only FallMove: a zero-length move hits nothing (Bsp.TestPlane bails
    // when the sweep is degenerate), but the fall-out test still runs after it.
    // On a real floor this clears OnFloor for the next step; the next gravity
    // move re-lands Kurt and the creep snap restores his position. Below the
    // arena the fall-out test sets OnFloor back to true, so the oracle holds
    // the corpse at the death point. Do not omit the post-move test.
    fall(delta);
    _stateTime += delta;
    _animationFrame = std::min(_animationFrame + kTicks * delta,
                               (float)(frameCount(Kurt::Dead) - 1));
    // The skull's rise and the one step it completes (Kurt.Combat.cs:250-253). The C#
    // raises its Died event there; see Player::died for the latch this port uses instead.
    if (combat_.fadeSkullIn(delta)) {
        _died = true;
    }
}

void Player::animate(float delta) {
    if (state() == Kurt::Run || state() == Kurt::RunFire) {
        const int32_t count = frameCount(state());
        if (count <= 0) {
            return;
        }
        // C# Kurt.Animate: the wrapped frames the step starts and ends on decide
        // which foot stamped. The floor is load-bearing when backing up: the frame
        // runs negative and Wrap maps it back into the animation, so truncating
        // here would drop a step every time the frame crosses zero backwards.
        const int32_t previous = wrapAnimationFrame((int32_t)std::floor(_animationFrame), count);
        _animationFrame += runRate() * kTicks * delta;
        const int32_t current = wrapAnimationFrame((int32_t)std::floor(_animationFrame), count);
        footstep(previous, current);
    } else if (state() == Kurt::Turn) {
        // No guard: a missing K_TRN45 leaves the frame at zero, as the C#'s
        // multiply by frameCount(Turn) == 0 does.
        _animationFrame = yaw / kTurnFrameDegrees * (float)frameCount(Kurt::Turn);
    } else {
        _animationFrame += kTicks * delta;
    }
}

void Player::footstep(int32_t previous, int32_t current) {
    // C# Kurt.Footstep: one stamp per frame the animation crossed, the left foot on
    // the left frame and the right foot alternating its pair. The pair flips only
    // on the right foot, so the pairs alternate FOOT3/FOOT4 then FOOT1/FOOT2.
    if (current == previous) {
        return;
    }

    const bool firing = state() == Kurt::RunFire;
    const int32_t left = firing ? kFireLeftStep : kLeftStep;
    const int32_t right = firing ? kFireRightStep : kRightStep;
    if (current == left) {
        playSound(_footstepPair ? "FOOT3" : "FOOT1");
    } else if (current == right) {
        playSound(_footstepPair ? "FOOT4" : "FOOT2");
        _footstepPair = !_footstepPair;
    }
}

void Player::updateChuteSound() {
    // C# Kurt.UpdateChuteSound: CHUTEON flaps after the opening frames, and closing
    // the chute stops it with CHUTEIN. CHUTEON is played Once, so the flap does not
    // stack voices; the headless device never ends a voice, so the Stop is what
    // takes it out of the mixer's list.
    if (chuteOpen && state() == Kurt::Chute && _animationFrame >= (float)kChuteOpenFrames) {
        playSound(kChuteOnSound, audio::SoundMixer::Once);
        return;
    }

    if (chuteOpen || !soundPlaying(kChuteOnSound)) {
        return;
    }

    stopSound(kChuteOnSound);
    playSound("CHUTEIN");
}

void Player::stopChuteSound() {
    // C# Kurt.StopChuteSound: CHUTEON stops without CHUTEIN when Kurt leaves the
    // chute otherwise (here: death). The ride, the cutscene and the ledge paths
    // that also call it are not ported.
    stopSound(kChuteOnSound);
}

void Player::playSound(const char* name, int32_t start) {
    if (_mixer) {
        _mixer->play(name, start);
    }
}

void Player::stopSound(const char* name) {
    if (_mixer) {
        _mixer->stop(name);
    }
}

bool Player::soundPlaying(const char* name) const {
    return _mixer != nullptr && _mixer->isPlaying(name);
}

void Player::setState(Kurt::State state) {
    // The one state, and it lives in the combat half: C# Kurt is one object whose
    // Hurt reads its own Current (Kurt.Combat.cs:125), so a state field here would
    // be a second copy for hurt() to miss. Same shape as the one health.
    if (state == combat_.state() && _stateTime > 0.0f) {
        return;
    }
    combat_.setState(state);
    _stateTime = 0.0f;
    _animationFrame = 0.0f;
    // The Died latch belongs to one death, and a death is over when Kurt leaves the
    // Dead state (the C#'s Revive, Kurt.Combat.cs:190-197, is the writer that would do
    // it -- SetState(Still) -- and it is not ported yet). Clearing here is what lets a
    // later death raise the event a second time instead of silently swallowing it.
    if (state != Kurt::Dead) {
        _died = false;
    }
}

int32_t Player::frameCount(Kurt::State state) const {
    const int index = (int)state;
    if (index < 0 || index >= kStateCount) {
        return 0;
    }
    return _frameCounts.values[index];
}

int32_t Player::wrapAnimationFrame(int32_t frame, int32_t count) {
    // The C#'s Wrap: ((frame % count) + count) % count. For a positive count
    // the first remainder is already in (-count, count), so adding the count
    // once when it is negative is the same value with ONE remainder instead of
    // two. This is straight-line frame selection, not a loop body.
    if (count <= 0) {
        return 0;
    }
    const int32_t remainder = frame % count;
    return remainder < 0 ? remainder + count : remainder;
}

float Player::runRate() const {
    const float u = std::fabs(forwardSpeed) / kTicks * 1.5f;
    const float rate = u <= 1.0f ? 0.75f * u + 0.25f : 0.25f * u + 0.75f;
    return forwardSpeed < 0.0f ? -rate : rate;
}

void Player::mouseLook(Input& input) {
    // Kurt's mouse look is the yaw only (C# Kurt.MouseLook); the camera's look
    // offset (its pitch) is the FollowCamera's, applied in FollowCamera::mouseLook.
    // A dead Kurt does not turn (C# MouseLook returns no turn on State.Dead), but
    // the accumulated mouse is still drained so it cannot build up.
    // A frozen Kurt does not turn either: the C#'s Frozen step returns before
    // UpdateTurning, so its _mouseTurn stays None and MouseLook turns nothing
    // (Kurt.cs:137-150, :340-352).
    if (dead || frozen) {
        input.takeMouseX();
        return;
    }
    if (_mouseTurnScope) {
        // C# Kurt.MouseLook's scope arm (Kurt.cs:349-357): the last step looked through
        // the scope, so the frame's mouse turns and tilts it -- the mouse's share of
        // Scope.Look, with no keys and no time -- and both axes are taken (the follow
        // camera's MouseLook leaves the tilt alone in sniper mode).
        const float mouseX = input.takeMouseX() * kMouseDegrees;
        const float mouseY = input.takeMouseY() * kMouseDegrees;
        yaw += _scope.look(0.0f, 0.0f, mouseX, mouseY, Scope::TurboOff, 0.0f);
        return;
    }
    // Sliding, the C#'s MouseLook turns nothing (the slide step left _mouseTurn at
    // None) and the step's Steer reads the frame's motion instead; see _slideMouseX.
    if (_sliding) {
        _slideMouseX = input.takeMouseX();
        return;
    }
    _slideMouseX = 0.0f;
    yaw += -input.takeMouseX() * kMouseDegrees;
}

Vec3 Player::shove(Vec3 offset) {
    // C# Kurt.Shove (Kurt.cs:265): a horizontal Walk move against the arena and the
    // objects nearby, its contact recorded, returning how far the feet actually went.
    const Vec3 start = feet;
    const Vec3 move{offset.x, offset.y, 0.0f};
    if (_space && (move.x != 0.0f || move.y != 0.0f)) {
        feet = fromVec(contact(_space->move(toVec(feet), toVec(move), ArenaSpace::kMotionWalk,
                                            kWalkSlide,
                                            nearby(Vec3{feet.x + move.x, feet.y + move.y,
                                                        feet.z + move.z})))
                           .feet);
    }
    return Vec3{feet.x - start.x, feet.y - start.y, feet.z - start.z};
}

void Player::teleport(const Vec3& at, float newYaw) {
    // C# Kurt.Teleport (Kurt.cs:249-261), in the C#'s order. "The chute closes
    // silently": ChuteOpen = false and StopChuteSound(), which is CHUTEON stopped
    // WITHOUT CHUTEIN (the same call death makes, and for the same reason: Kurt left
    // the chute otherwise than by closing it). Then the three speeds go, which is the
    // "he stops" of the summary: a teleport does not carry the walk, the strafe or the
    // fall into the new place. A keeping platform lets him go (`_keeper = null`,
    // Kurt.cs:254, package #3).
    chuteOpen = false;
    stopChuteSound();
    _keeper = nullptr;
    feet = at;
    yaw = newYaw;
    forwardSpeed = 0.0f;
    strafeSpeed = 0.0f;
    verticalSpeed = 0.0f;
    teleports++;
}

void Player::setHealth(int health) {
    // C# SetHealth: max(health, 0) -- on the one health, which lives on the
    // combat half this Player owns.
    combat_.setHealth(health);
}

void Player::damage(int damage) {
    // C# Damage with Normal difficulty (Easy/Hard scaling lands with difficulty): the
    // scaling, the red flash, the health and the knock counter, all of it -- the C#'s
    // HardLanding calls Damage, not the Health line out of it.
    combat_.damage(damage);
}

void Player::knockDown(const Vec2& push) {
    // C# Kurt.KnockDown (Kurt.Combat.cs:173-180), in its order: the counter is spent,
    // the invulnerability opens, the push is added, the chute closes, K_BANG starts.
    // The order is not cosmetic -- the counter going to zero is what stops the next
    // step's trigger from knocking him down again while he is already down, and the
    // invulnerability is what makes hurt() refuse him for the whole animation.
    combat_.setKnockDamage(0.0f);
    combat_.setInvulnerable(Kurt::kKnockdownInvulnerability);
    addKnockdownPush(push);
    // Kurt.Combat.cs:178: a knocked-down Kurt stops firing (and updateFiring refuses
    // the gun while he is down, so it stays stopped until he is up).
    stopFiring();
    chuteOpen = false;
    setState(Kurt::Knocked);
}

void Player::updateKnockDamage(float delta) {
    // C# Kurt.UpdateKnockDamage (Kurt.Combat.cs:214) whole, in the C#'s order: the
    // trigger, and the decline as its tail on every step the trigger does not fire on.
    // The three guards are the C#'s three: a corpse is not knocked down, the counter
    // has to reach KnockdownDamage, and a Kurt already down does not get knocked down
    // again (which is what re-opens the 3 s of invulnerability every step otherwise).
    if (health() > 0 && combat_.knockDamage() >= Kurt::kKnockdownDamage &&
        !combat_.knocked()) {
        bool down = onFloor;
        if (!down) {
            // In the air he goes down only if a floor is within KnockdownFloorDistance
            // below his feet, and then he is slammed to it at KnockdownSlamSpeed. The
            // probe is the C#'s ArenaSpace.Floor, not its FloorCrossing: the arenas
            // around BOTH ends and the nearest crossing, which is the one that answers
            // across an arena boundary. A Player with no space has no floor to find --
            // the C# would have no space to ask, so this is the same answer an empty
            // arena list gives, not a substitute for the query.
            Vec hit;
            const Vec probe = vSub(toVec(feet), Vec{0.0f, 0.0f, Kurt::kKnockdownFloorDistance});
            if (_space != nullptr && _space->floor(toVec(feet), probe, &hit)) {
                down = true;
                verticalSpeed = std::min(verticalSpeed, Kurt::kKnockdownSlamSpeed);
            }
        }
        if (down) {
            // The C#'s own KnockDown(Vector2.Zero) (Kurt.Combat.cs:225): a knock-down the
            // counter triggered carries no shove. The only writer of a non-zero push is
            // ScriptRuntime::pushKurt (opcode 248), which calls knockDown with its own
            // vector, so this call site stays zero and is not a placeholder for it.
            knockDown(Vec2{0.0f, 0.0f});
            return;
        }
    }

    combat_.declineKnockDamage(delta);
}

void Player::drainKnockdownPush(float delta) {
    // C# DrainPush (Kurt.Combat.cs:208-209).
    const float step = kPushDrain * delta;
    _knockdownPush.x = moveToward(_knockdownPush.x, 0.0f, step);
    _knockdownPush.y = moveToward(_knockdownPush.y, 0.0f, step);
}
