// player_slide.inc -- the wind zones' slide (C# Kurt.Slide.cs).
// Part of player.cpp's single translation unit (see its header): pasted inside
// namespace mdk after its file-local constants and helpers, which it uses.
// No include guards, no includes of its own; the order is load-bearing.

// ---- The wind zones' slide: C# Kurt.Slide.cs ----

/* @vfp-exempt R6 — the slide's heading: cosf/sinf are the oracle's MathF.Cos/Sin
 * (Kurt.Facing) and the conversion is .NET's left-associative DegreesToRadians. The
 * slide feeds its own heading back every step (Steer: yaw from atan2 of the velocity,
 * then the velocity from the yaw), so the fastSin/fastCos LUT's error would compound
 * into the path golden/L6_wind_*.txt pins. Once a step while sliding, not a loop body.
 * measured n/a (corpus empty). Verified 2026-11-20. */
Vec2 Player::slideFacing() const {
    const float radians = yaw * 3.14159265358979323846f / 180.0f;
    return Vec2{cosf(radians), sinf(radians)};
}

void Player::startSlide() {
    // C# Kurt.StartSlide (Kurt.Slide.cs:77-93).
    if (_sliding || health() == 0) {
        return;
    }
    _sliding = true;
    _slideVelocity = Vec2{0.0f, 0.0f};
    _slideCap = kSlideCap;
    _slideNormal = Vec3{0.0f, 0.0f, 1.0f};
    _slidePush = Vec2{0.0f, 0.0f};
    _slideAir = 0.0f;
    _slideLeft = 0.0f;
    stopFiring();
    chuteOpen = false;
    setState(Kurt::Slip);
}

void Player::enterArena() {
    // C# Kurt.EnterArena (Kurt.Slide.cs:97-105): a slide ends 15 ticks after an arena
    // change (LEVEL6's slide drops him into OLYM_2 against a wall); a second crossing
    // inside those ticks does not restart them.
    if (!_sliding || _slideLeft > 0.0f) {
        return;
    }
    _slideLeft = kSlideArenaTicks;
}

void Player::stopSlide() {
    // C# Kurt.StopSlide (Kurt.Slide.cs:107-114).
    _sliding = false;
    _slideVelocity = Vec2{0.0f, 0.0f};
    if (_mixer != nullptr) {
        _mixer->stopVoice(_slideVoice);
    }
    _slideVoice = 0;
    _slideSound = "";
}

void Player::slideAccel(const Vec2& push, float dt) {
    // C# Kurt.SlideAccel (Kurt.Slide.cs:116-119).
    _slideVelocity = Vec2{slideAxisAccel(_slideVelocity.x, push.x, dt),
                          slideAxisAccel(_slideVelocity.y, push.y, dt)};
}

bool Player::updateSlide(const Input& input, float delta) {
    // C# Kurt.UpdateSlide (Kurt.Slide.cs:138-198), in its order. False: it ended (or
    // never ran) and the ordinary step goes on.
    if (!_sliding) {
        return false;
    }
    if (health() == 0) {
        stopSlide();
        return false;
    }

    // 15 ticks after an arena change he falls.
    if (_slideLeft > 0.0f) {
        _slideLeft -= kTicks * delta;
        if (_slideLeft <= 0.0f) {
            stopSlide();
            setState(Kurt::Fall);
            return false;
        }
    }

    if (onFloor) {
        _slideNormal = Vec3{
            kSlideNormalKeep * _slideNormal.x + (1.0f - kSlideNormalKeep) * _floorNormal.x,
            kSlideNormalKeep * _slideNormal.y + (1.0f - kSlideNormalKeep) * _floorNormal.y,
            kSlideNormalKeepZ * _slideNormal.z + (1.0f - kSlideNormalKeepZ) * _floorNormal.z};
        _slidePush = Vec2{_slideNormal.x * kSlideSlope, _slideNormal.y * kSlideSlope};
        _slideAir = 0.0f;
        if (_slidePush.x * _slidePush.x + _slidePush.y * _slidePush.y < kSlideRest &&
            _slideVelocity.x == 0.0f && _slideVelocity.y == 0.0f) {
            // At rest he gets up (state 901 from K_BFLIP): K_BANG at its last frame, so
            // the next step's knock-down arm flips him straight into K_BFLIP.
            stopSlide();
            setState(Kurt::Knocked);
            _animationFrame = (float)frameCount(Kurt::Knocked);
            return true;
        }
    } else {
        _slideAir += kTicks * delta;
        if (_slideAir > kSlideAirTicks) {
            stopSlide();
            setState(Kurt::Fall);
            return false;
        }
    }

    slideAccel(_slidePush, delta);
    _cameraRoll.slide(_slideNormal, slideFacing(), delta);
    const float forward = input.axis(kKeyForward, kKeyBack);
    const float speed = slideSpeed(forward, delta);
    steer(input, speed, delta);
    slideMove(delta);
    verticalSpeed -= kSlideGravity * delta;
    verticalSpeed = std::max(verticalSpeed - kGravity * delta, -kMaxFallSpeed);
    fall(delta);
    updateTouched();
    updateSlideState(forward, delta);
    updateMuzzle(delta);
    return true;
}

float Player::slideSpeed(float forward, float delta) {
    // C# Kurt.SlideSpeed (Kurt.Slide.cs:202-229): the speed within the cap; forward
    // speeds up and raises the cap, back brakes and lowers it, without input it goes
    // back to 50.
    float speed = mathMin(length2(_slideVelocity), _slideCap);
    if (forward > 0.0f) {
        if (speed >= kSlideMinSpeed) {
            speed += kSlideAcceleration * forward * delta;
        }
        _slideCap = mathMin(_slideCap + kSlideCapRise * delta, kSlideCapMax);
    } else if (forward < 0.0f) {
        if (speed > kSlideMinSpeed) {
            speed = mathMax(speed + kSlideBrake * forward * delta, kSlideMinSpeed);
        }
        _slideCap = mathMax(_slideCap - kSlideCapFall * delta, kSlideCapMin);
    } else {
        _slideCap = moveToward(_slideCap, kSlideCap, kSlideCapRelax * delta);
    }
    return mathMin(speed, _slideCap);
}

/* @vfp-exempt R6 — the slide's heading from its velocity: atan2f is the oracle's
 * MathF.Atan2 and the conversion .NET's left-associative RadiansToDegrees; see
 * slideFacing for why the LUT would move the pinned path. measured n/a (corpus empty).
 * Verified 2026-11-20. */
void Player::steer(const Input& input, float speed, float delta) {
    // C# Kurt.Steer (Kurt.Slide.cs:232-241): Kurt faces where the slide goes, the turn
    // keys (or the frame's mouse, clamped) turn it, and the velocity is the new facing
    // at the speed.
    if (std::fabs(_slideVelocity.x) + std::fabs(_slideVelocity.y) > kSlideHeading) {
        yaw = atan2f(_slideVelocity.y, _slideVelocity.x) * 180.0f / 3.14159265358979323846f;
    }
    const float mouse = kSlideMouseTurn * delta;
    yaw += input.axis(kKeyTurnLeft, kKeyTurnRight) * kSlideTurn * delta -
           clampf(_slideMouseX * kMouseDegrees, -mouse, mouse);
    const Vec2 f = slideFacing();
    _slideVelocity = Vec2{f.x * speed, f.y * speed};
}

void Player::slideMove(float delta) {
    // C# Kurt.SlideMove (Kurt.Slide.cs:244-262): the slide's walk move; a wall that
    // stops it on an axis turns it along what Kurt moved, at the mean of the two speeds.
    const Vec3 planned{_slideVelocity.x * delta, _slideVelocity.y * delta, 0.0f};
    if (planned.x == 0.0f && planned.y == 0.0f) {
        return;
    }

    // Feet = Contact(space.Move(Feet, planned, Walk, WalkSlide, Nearby(Feet + planned))).Feet:
    // the arena and the solid objects nearby, as the walk move (Kurt.cs:225).
    const Vec3 start = feet;
    if (_space) {
        feet = fromVec(contact(_space->move(toVec(feet), toVec(planned), ArenaSpace::kMotionWalk,
                                            kWalkSlide,
                                            nearby(Vec3{feet.x + planned.x, feet.y + planned.y,
                                                        feet.z + planned.z})))
                           .feet);
    } else {
        feet.x += planned.x;
        feet.y += planned.y;
    }
    const Vec3 moved{feet.x - start.x, feet.y - start.y, feet.z - start.z};
    if (!slideStopped(planned.x, moved.x) && !slideStopped(planned.y, moved.y)) {
        return;
    }

    const Vec2 actual{moved.x, moved.y};
    const bool still = actual.x == 0.0f && actual.y == 0.0f;
    const Vec2 direction = still ? normalize2(_slideVelocity) : normalize2(actual);
    const float speed = length2(_slideVelocity) * kSlideWallShare +
                        length2(actual) / delta * (1.0f - kSlideWallShare);
    _slideVelocity = Vec2{direction.x * speed, direction.y * speed};
}

void Player::updateSlideState(float forward, float delta) {
    // C# Kurt.UpdateSlideState (Kurt.Slide.cs:265-300): K_SLIP once, then K_SLIDE,
    // K_FSLIDE or K_BSLIDE by the forward key; BUTBRAKE while braking, BUTSLIDE
    // otherwise, pitched up going forward.
    _stateTime += delta;
    _animationFrame += kTicks * delta;
    if (state() != Kurt::Slip || _animationFrame >= (float)frameCount(Kurt::Slip)) {
        const Kurt::State wanted = forward > 0.0f   ? Kurt::SlideFast
                                   : forward < 0.0f ? Kurt::SlideBrake
                                                    : Kurt::Slide;
        if (wanted != state()) {
            setState(wanted);
        }
    }

    const char* sound = state() == Kurt::SlideBrake ? kBrakeSound : kSlideSound;
    if (strcmp(sound, _slideSound) != 0) {
        if (_mixer != nullptr) {
            _mixer->stopVoice(_slideVoice);
            _slideVoice = _mixer->playLooped(sound);
        } else {
            _slideVoice = 0;
        }
        _slideSound = sound;
    }
    if (_mixer != nullptr) {
        _mixer->setPitch(_slideVoice, state() == Kurt::SlideFast ? kSlideFastPitch : 1.0f);
    }
}
