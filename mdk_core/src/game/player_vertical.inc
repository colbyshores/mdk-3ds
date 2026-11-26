// player_vertical.inc -- the scope's sidestep (sniperMove), jumping, the air, falling and landing.
// Part of player.cpp's single translation unit (see its header): pasted inside
// namespace mdk after its file-local constants and helpers, which it uses.
// No include guards, no includes of its own; the order is load-bearing.

void Player::sniperMove(const Input& input, bool turbo, float delta) {
    // C# Kurt.SniperMove (Kurt.Sniper.cs:178-192): Kurt sidesteps at a quarter of the
    // running speed and still falls. It lives here rather than in player_sniper.cpp
    // because it is the walk's arithmetic: the same Accelerate, speeds, slide and
    // gravity. The move is the C#'s `(Right * StrafeSpeed + (push, 0)) * delta`, taken
    // before the push drains, as the walk's; the move passes Nearby (the solid objects).
    const float max = (turbo ? kMaxSpeedTurbo : kMaxSpeed) * 0.25f;  // SniperStrafe
    forwardSpeed = 0.0f;
    strafeSpeed = clampf(accelerate(strafeSpeed, input.axis(kKeyStrafeRight, kKeyStrafeLeft), turbo,
                                    1.0f, 1.0f, delta),
                         -max, max);
    const Vec3 r = right();
    const Vec3 move{(r.x * strafeSpeed + _knockdownPush.x) * delta,
                    (r.y * strafeSpeed + _knockdownPush.y) * delta, 0.0f};
    drainKnockdownPush(delta);
    if (move.x != 0.0f || move.y != 0.0f) {
        if (_space) {
            // Kurt.Sniper.cs:187: against the objects nearby too (package #3), and with
            // no Contact, as the C#'s sidestep records none.
            feet = fromVec(_space
                               ->move(toVec(feet), toVec(move), ArenaSpace::kMotionWalk, kWalkSlide,
                                      nearby(Vec3{feet.x + move.x, feet.y + move.y,
                                                  feet.z + move.z}))
                               .feet);
        } else {
            feet.x += move.x;
            feet.y += move.y;
        }
    }
    verticalSpeed = std::max(verticalSpeed - kGravity * delta, -kMaxFallSpeed);
    fall(delta);
}

void Player::updateVertical(const Input& input, const Vec3& move, float delta) {
    const bool jumpHeld = input.isDown(kKeyJump);
    if (!jumpHeld) {
        _jumpReleased = true;
        if (_jumpTicksLeft > 0) {
            verticalSpeed -= _jumpTicksLeft * kJumpReleasePenalty;
            _jumpTicksLeft = 0;
        }
    }

    if (onFloor) {
        chuteOpen = false;
        if (jumpHeld && _jumpReleased) {
            verticalSpeed = kJumpSpeed;
            _jumpTicksLeft = kJumpHoldTicks;
            _jumpReleased = false;
        } else {
            glueDownhill(move, delta, true);
        }
    } else {
        updateAir(jumpHeld, delta);
    }

    // C# Kurt.cs:417: the fans' updraft, after the floor/air branch and before the fall
    // (Kurt.Updraft.cs). Out of every fan it caps the upward speed at 40 u/s -- except
    // while Kurt is knocked down, which is the oracle's own exception.
    updateUpdraft(delta);
    fall(delta);
    _blown = false;
    // health 0 on the floor is death (C# "health 0 --on the floor--> DEAD"). A
    // hard landing lands the same tick and fall-out forces OnFloor, so both die
    // now; a future mid-air damage source would wait for the landing, as in the C#.
    if (health() == 0 && onFloor) {
        dead = true;
    }
}

void Player::updateAir(bool jumpHeld, float delta) {
    if (_jumpTicksLeft > 0) {
        _jumpTicksLeft = std::max(0, _jumpTicksLeft - (int)std::ceil(delta * kTicks));
    }

    // A wind zone keeps the chute shut (upstream dce8d904): LEVEL6's COLYM_3 slide starts
    // only in the wind box under OLYM_3's hatch, and with the chute open Kurt glided past.
    if (!chuteOpen && jumpHeld && !_blown && (verticalSpeed < kFallStartSpeed || _pastJumpTop)) {
        chuteOpen = true;
        playSound("CHUTEOUT");
    } else if (chuteOpen && (_blown || (!jumpHeld && !_inUpdraft))) {
        // Kurt.cs:435: letting go of jump closes the chute -- unless a fan holds him,
        // which keeps it open (UpdateUpdraft opened it); the wind closes it.
        chuteOpen = false;
    }

    if (!chuteOpen) {
        verticalSpeed = std::max(verticalSpeed - kGravity * delta, -kMaxFallSpeed);
        return;
    }

    verticalSpeed -= kChuteGravity * delta;
    if (verticalSpeed < kChuteFallSpeed) {
        verticalSpeed = std::min(verticalSpeed + kChuteBrake * delta, kChuteFallSpeed);
    }
}

// Upstream a91c0870 (C# 1.2.0 Kurt.AnimateJump, 0x4646a4): a jump's frames are timed,
// then follow the vertical speed down through its top, then the fall. Played through by
// time, the jump ended while Kurt still rose and he hung on its last frame until he fell
// fast enough, the only time the chute could open (issue #17).
void Player::animateJump(float delta) {
    const bool standing = state() == Kurt::Jump;
    const int timed = standing ? kJumpTimedFrames : kRunJumpTimedFrames;
    const int frames = standing ? kJumpFrames : kRunJumpFrames;
    if (_animationFrame < (float)timed) {
        _animationFrame = std::min(_animationFrame + kTicks * delta, (float)timed);
        return;
    }
    int frame = (int)_animationFrame;
    while (frame < frames && verticalSpeed <= kJumpSpeeds[frame]) {
        frame++;
    }
    _animationFrame = (float)frame;
    _pastJumpTop = _pastJumpTop || verticalSpeed <= 0.0f;
    if (frame >= frames) {
        setState(Kurt::Fall);
    }
}

// Upstream 3f9315c5: with `exact` (the walk's glue), the feet follow the slope's whole
// drop (the move along the slope over the normal's z), capped at kGlueMaxSpeed, not only
// its share along the normal: on a steep floor (LEVEL6's OLYM_8, 42 degrees) the feet
// left it every other step and Kurt flickered between the run and the fall.
void Player::glueDownhill(const Vec3& move, float delta, bool exact) {
    const Vec3 n = _floorNormal.z < 0.0f
                       ? Vec3{-_floorNormal.x, -_floorNormal.y, -_floorNormal.z}
                       : _floorNormal;
    const float downhill = move.x * n.x + move.y * n.y;
    if (n.z > kGlueNz && downhill > 0.0f) {
        const float speed = (exact ? downhill / n.z : downhill) / delta;
        verticalSpeed = std::min(verticalSpeed, -std::min(speed, exact ? kGlueMaxSpeed : speed));
    }
    verticalSpeed = std::max(verticalSpeed - kGravity * delta, -kMaxFallSpeed);
}

void Player::fall(float delta) {
    // C# Kurt.Fall: the fall move first, then the fall-out death (damp_gravity
    // 0x469efc). Fifty below his arena's lowest point Kurt dies and counts as
    // on a floor so the death plays. This post-move test is load-bearing in
    // UpdateDeath too: below the arena it sets OnFloor back to true after the
    // zero-length move, exactly as the oracle does. God mode (Kurt.Mortality ==
    // God, the console's cheat, gap package #21) skips the death: `|| Mortality ==
    // Mortality.God` in the C#'s return test.
    fallMove(delta);
    float bottom = 0.0f;
    if (_space && _space->bottom(toVec(feet), &bottom) && feet.z <= bottom + kFallOutDepth &&
        combat_.mortality() != Kurt::God) {
        combat_.setHealth(0);
        verticalSpeed = 0.0f;
        onFloor = true;
    }
}

void Player::fallMove(float delta) {
    const Vec3 before = feet;
    float dz = verticalSpeed * delta;

    if (!_space) {
        feet.z += dz;
        if (feet.z < before.z) {
            verticalSpeed = (feet.z - before.z) / delta;
        }
        onFloor = false;
        return;
    }

    // C# Kurt.FallMove (Kurt.cs:510-526): a platform under the feet -- the keeping
    // one first, else one the fall reaches -- stops the fall 0.05 above its top, and
    // when the BSP lets him get there he lands on it (Platform) at its top.
    float platformTop = 0.0f;
    scripts::MdkObject* platformOwner = nullptr;
    const bool onPlatform = keptPlatform(dz, &platformTop, &platformOwner) ||
                            findPlatform(dz, &platformTop, &platformOwner);
    if (onPlatform) {
        dz = platformTop + kPlatformLift - feet.z;
    }

    const Vec3 down{0.0f, 0.0f, dz};
    const ArenaSpace::Result result =
        contact(_space->move(toVec(feet), toVec(down), ArenaSpace::kMotionFall, kFallSlide));
    feet = fromVec(result.feet);
    _platform = nullptr;
    if (!result.hit() && onPlatform) {
        feet.z = platformTop;
        _platform = platformOwner;
        keepOn(platformOwner);
        land(before, Vec3{0.0f, 0.0f, 1.0f}, delta);
        return;
    }

    if (!result.hit()) {
        // Sliding down a steep face: the real fall rate.
        if (feet.z < before.z) {
            verticalSpeed = (feet.z - before.z) / delta;
        }
        onFloor = false;
        return;
    }

    // C# FallMove's Sliding branch (Kurt.cs:540-546): on his back a hit is not a
    // landing -- no hard landing, no creep snap -- the vertical speed is what the move
    // made of it, and the floor it hit is the slide's floor normal.
    if (_sliding) {
        verticalSpeed = (feet.z - before.z) / delta;
        onFloor = true;
        _floorNormal = fromVec(result.normal());
        return;
    }

    if (verticalSpeed > 0.0f) {
        // Going up, bumped the head.
        verticalSpeed = 0.0f;
        onFloor = false;
        return;
    }

    climbOut(result);
    land(before, fromVec(result.normal()), delta);
}

// Upstream 3bb211b6 (C# 1.2.4 Kurt.ClimbOut), a deliberate deviation from the original
// (issue #30 upstream): a floor without a side wall at its edge lets the walking box in
// from the side (the BSP has faces, no volume), and every fall move then stops at once on
// its top (bsp_sweep_node 0x409680, damp_collide_move 0x465e34): Kurt walked 2 units
// inside level 8's GUNT_7 walkway until a jump. Sunk deeper than a step, up to his waist,
// he climbs onto the top when there is room above. Not on a ride (the board, the bomber).
void Player::climbOut(const ArenaSpace::Result& result) {
    const float sunk = ArenaSpace::sunk(result, toVec(feet));
    if (sunk <= kSunkMin || sunk > kSunkMax || _rideHook.step != nullptr) {
        return;
    }
    const ArenaSpace::Result up =
        _space->move(toVec(feet), Vec{0.0f, 0.0f, sunk}, ArenaSpace::kMotionFall, kFallSlide);
    if (up.hit()) {
        return;
    }
    feet = fromVec(up.feet);
}

void Player::land(const Vec3& before, const Vec3& normal, float delta) {
    // The C# is `if (hard) HardLanding(); else if (creep) Feet = before;`. The
    // hard landing's damage and animation land with health, but the ELSE is
    // load-bearing for position: a hard landing must NOT snap to `before`, or
    // Kurt is left floating just above the floor with onFloor set. A ride's step lands
    // softly (`&& Ride == null`, Kurt.cs:560: the board skips the hard landing).
    if (verticalSpeed < kHardLandingSpeed && _rideHook.step == nullptr) {
        // C# HardLanding(): 10 damage, then K_CRASHL unless the damage killed
        // Kurt. The hard-landing state owns the next steps, so the ordinary
        // walk state machine cannot replace it on this tick.
        //
        // The C#'s own test is `Health == 0 || Current is Dead|Knocked|GetUp`
        // (Kurt.cs:584), and the `Health == 0` half is the observable one: a Kurt
        // already at 0 who lands at -194 u/s does not play K_CRASHL and does not
        // have his chute closed for him. golden/L3_kurt_lethal.txt pins it. The
        // guard around the damage call is the C#'s own `if (Health > 0)` and is not
        // observable here -- Player::damage floors at 0 exactly like Kurt.Damage's
        // Math.Max(Health - damage, 0) -- but the call is the C#'s Damage whole, so the
        // slam raises the red flash to its 180 clamp and adds to KnockDamage as the
        // oracle's does. Of those two the KnockDamage is provably inert for a hard
        // landing: Kurt.cs:583 zeroes that counter two lines after the damage call,
        // which is the line below. The flash is NOT inert and is no longer left out:
        // Kurt::fadeFlashes runs now, so a slam that raised no flash would leave the HUD
        // red for 90 steps fewer than the oracle leaves it -- golden/L3_kurt_flashes.txt
        // ends its fall in exactly this landing. What is still left out of the C#'s
        // Damage is Scaled(damage, Difficulty): on Easy the slam would be 6 and on Hard
        // 20, not the 10 the port deals. It is inert only because difficulty_ has no
        // writer yet -- the same deferral Player::damage records -- and it moves with
        // that, not with this.
        //
        // Kurt.cs:589 also calls StopFiring() (leave sniper mode, stop the chain-gun
        // voice, drop the muzzle flash) inside the same guard, below.
        if (health() > 0) {
            damage(kHardLandingDamage);
        }
        // C# HardLanding zeroes the recent-damage counter HERE (Kurt.cs:583):
        // unconditionally, between the damage and the dead/knocked guard, so a hard
        // landing always starts the knock-down counter fresh. A counter a script
        // raised (set_hurt_flash, opcode 240) or a hurt taken in the air therefore
        // cannot survive the landing; without this line the port carries a nonzero
        // counter through a landing the oracle clears, which is exactly the shape that
        // would knock Kurt down on the step after every hard landing the moment the
        // knock-down machine lands. Pinned by golden/L3_kurt_knock.txt.
        combat_.setKnockDamage(0.0f);
        if (health() > 0 && state() != Kurt::Dead && state() != Kurt::Knocked &&
            state() != Kurt::GetUp) {
            stopFiring();
            chuteOpen = false;
            setState(Kurt::HardLand);
        }
    } else {
        const float creep = kCreepSpeed * delta;
        const float dx = feet.x - before.x;
        const float dy = feet.y - before.y;
        const float dz = feet.z - before.z;
        if (dx * dx + dy * dy + dz * dz < creep * creep) {
            feet = before;  // no creeping on a landing
        }
    }
    verticalSpeed = 0.0f;
    onFloor = true;
    _floorNormal = normal;
}
