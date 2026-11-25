// player_platforms.inc -- platforms, solid objects and touches, rides, and the fans' updrafts.
// Part of player.cpp's single translation unit (see its header): pasted inside
// namespace mdk after its file-local constants and helpers, which it uses.
// No include guards, no includes of its own; the order is load-bearing.

// ---- Platforms, solid objects and touches (package #3) ----------------------

const std::vector<solids::Solid>& Player::nearby(const Vec3& to) {
    // C# Kurt.Nearby (Kurt.cs:290-300): the region around the feet and `to`, grown by
    // the margin, and Kurt's height above it. The C#'s evaluation order is kept:
    // (max + margin) + (0, 0, KurtHeight).
    static const std::vector<solids::Solid> kNone;
    if (_solidHooks.solidsWithin == nullptr) {
        return kNone;
    }
    formats::Box region;
    region.min = Vec{std::min(feet.x, to.x) - kNearbyMargin.x,
                     std::min(feet.y, to.y) - kNearbyMargin.y,
                     std::min(feet.z, to.z) - kNearbyMargin.z};
    region.max = Vec{std::max(feet.x, to.x) + kNearbyMargin.x,
                     std::max(feet.y, to.y) + kNearbyMargin.y,
                     std::max(feet.z, to.z) + kNearbyMargin.z + kKurtHeight};
    const std::vector<solids::Solid>* list = _solidHooks.solidsWithin(_solidHooks.context, region);
    return list != nullptr ? *list : kNone;
}

bool Player::findPlatform(float dz, float* top, scripts::MdkObject** owner) {
    // C# Kurt.FindPlatform (Kurt.cs:303-313): not while rising; the platform the ray
    // to the fall's bottom meets, if the fall gets down to its top.
    if (verticalSpeed > 0.0f) {
        return false;
    }
    const float bottom = feet.z + dz;
    float found = 0.0f;
    scripts::MdkObject* by = nullptr;
    if (!solids::platform(toVec(feet), bottom, nearby(Vec3{feet.x, feet.y, bottom}), &found,
                          &by) ||
        !(bottom <= found)) {
        return false;
    }
    *top = found;
    *owner = by;
    return true;
}

ArenaSpace::Result Player::contact(const ArenaSpace::Result& result) {
    // C# Kurt.Contact (Kurt.Ride.cs:123-131).
    if (result.hit() && result.bsp != nullptr) {
        _contacts.push_back(Contact{result.bsp->arena(), result.triangle});
    }
    return result;
}

const std::vector<Player::Contact>& Player::takeContacts() {
    // C# Kurt.TakeContacts (Kurt.Ride.cs:112-118).
    _taken.clear();
    _taken.insert(_taken.end(), _contacts.begin(), _contacts.end());
    _contacts.clear();
    return _taken;
}

void Player::updateTouched() {
    // C# Kurt.UpdateTouched (Kurt.Ride.cs:133-143): what his walking box touches, and
    // his platform if it is not among them.
    _touched.clear();
    ArenaSpace::touching(toVec(feet), nearby(feet), &_touched);
    if (_platform != nullptr &&
        std::find(_touched.begin(), _touched.end(), _platform) == _touched.end()) {
        _touched.push_back(_platform);
    }
}

// ---- Rides (C# Kurt.Ride.cs, package #9) ----

void Player::glide(const Vec3& velocity, float delta) {
    // C# Kurt.Glide (Kurt.Ride.cs:50-67): a ride's move -- the walk move against the
    // arena and the objects, then sticking to downhill floors on a floor or gravity in
    // the air, the fall, and the touches. The snowboard's step (package #10) calls it.
    const Vec3 move{velocity.x * delta, velocity.y * delta, velocity.z * delta};
    if (move.x != 0.0f || move.y != 0.0f || move.z != 0.0f) {
        if (_space) {
            feet = fromVec(contact(_space->move(toVec(feet), toVec(move), ArenaSpace::kMotionWalk,
                                                kWalkSlide,
                                                nearby(Vec3{feet.x + move.x, feet.y + move.y,
                                                            feet.z + move.z})))
                               .feet);
        } else {
            feet = Vec3{feet.x + move.x, feet.y + move.y, feet.z + move.z};
        }
    }

    if (onFloor) {
        glueDownhill(move, delta);
    } else {
        verticalSpeed = std::max(verticalSpeed - kGravity * delta, -kMaxFallSpeed);
    }

    fall(delta);
    updateTouched();
}

void Player::updateGun(const Input& input, float delta) {
    // C# Kurt.UpdateGun (Kurt.Ride.cs:71-75): the chain gun and its flash on the board.
    updateFiring(input);
    updateMuzzle(delta);
}

void Player::jumpOff() {
    // C# Kurt.JumpOff (Kurt.Ride.cs:78-86): out of the XD2, a running jump forward.
    walkMode = WalkNormal;
    visible = true;
    verticalSpeed = kJumpSpeed;
    forwardSpeed = kMaxSpeed;
    onFloor = false;
    setState(Kurt::RunJump);
}

void Player::getOffBoard(float forward, float strafe) {
    // C# Kurt.GetOffBoard (Kurt.Ride.cs:89-99): off the board or the bomber, keeping
    // the ride's speeds (u/s) as his walking ones; the bomber's view goes
    // (TopDownHeight = null, package #11).
    _rideHook = RideHook();
    hasPose = false;
    poseName = nullptr;
    poseFrame = 0;
    hasTopDownHeight = false;
    topDownHeight = 0.0f;
    cameraPivot = 4.5f;  // C# Kurt.DefaultPivot
    visible = true;
    forwardSpeed = forward;
    strafeSpeed = strafe;
    setState(Kurt::Still);
}

void Player::rideWalker(const Input& input, float forward, float delta) {
    // C# Kurt.RideWalker (Kurt.Ride.cs:101-108): in the XD2, gravity and the fall only;
    // the keys the walker reads (forward is the step's, already zeroed when he can't
    // walk); he stands, hidden.
    verticalSpeed = std::max(verticalSpeed - kGravity * delta, -kMaxFallSpeed);
    fall(delta);
    _rideKeys.steer = forward != 0.0f || input.axis(kKeyTurnLeft, kKeyTurnRight) != 0.0f;
    _rideKeys.fire = input.isDown(kKeyFire);
    setState(Kurt::Still);
}

// ---- end of rides ----

void Player::carry(const Vec3& from, const Vec3& to, float turn) {
    // C# Kurt.Carry (Kurt.cs:282-287): Vector3.Transform(Feet - from,
    // Matrix4x4.CreateRotationZ(DegreesToRadians(turn))). Measured on .NET 10
    // (200000 random cases, 0 misses): the transform is x*M11 rounded, then ONE fused
    // multiply-add of y*M21 (likewise Y); the unfused sum misses a third of them. Z
    // passes through. CreateRotationZ's sin/cos are MathF.SinCos's, which equal
    // MathF.Sin/Cos on every one of those cases.
    /* @vfp-exempt R6 — Kurt's carried position on a turning platform is gameplay
     * state the oracle computes with exact sin/cos; the LUT's error would move his
     * feet. Once per tick, only while he stands on a platform. measured n/a. Verified
     * 2026-11-20. */
    const float pi = 3.14159265358979323846f;
    const float radians = turn * pi / 180.0f;
    const float c = cosf(radians);
    const float s = sinf(radians);
    const float x = feet.x - from.x;
    const float y = feet.y - from.y;
    const float z = feet.z - from.z;
    const float ox = fmaf(y, -s, x * c);
    const float oy = fmaf(y, c, x * s);
    feet = Vec3{to.x + ox, to.y + oy, to.z + z};
    yaw += turn;
}

void Player::releaseSolids() {
    _platform = nullptr;
    _keeper = nullptr;
    _touched.clear();
    _contacts.clear();
    _taken.clear();
    _solidHooks = SolidHooks();
}

bool Player::keeperAt(scripts::MdkObject* owner, Vec3* at) const {
    // The C#'s `Keeper?.Invoke(owner)`: null without the hook.
    return _solidHooks.keeperAt != nullptr &&
           _solidHooks.keeperAt(_solidHooks.context, owner, at);
}

void Player::checkKeeper() {
    // C# Kurt.CheckKeeper (Kurt.Keeper.cs:167-173).
    Vec3 at;
    if (_keeper != nullptr && !keeperAt(_keeper, &at)) {
        _keeper = nullptr;
    }
}

bool Player::keptPlatform(float dz, float* top, scripts::MdkObject** owner) const {
    // C# Kurt.KeptPlatform (Kurt.Keeper.cs:176-185): the keeping platform as the fall's
    // floor, no scan.
    Vec3 at;
    if (_keeper == nullptr || verticalSpeed > 0.0f || !keeperAt(_keeper, &at)) {
        return false;
    }
    const float height = at.z + _keptHeight;
    if (!(feet.z + dz <= height)) {
        return false;
    }
    *top = height;
    *owner = _keeper;
    return true;
}

void Player::keepOn(scripts::MdkObject* owner) {
    // C# Kurt.KeepOn (Kurt.Keeper.cs:188-197).
    Vec3 at;
    if (_keeper != nullptr || !keeperAt(owner, &at)) {
        return;
    }
    _keeper = owner;
    _keptHeight = feet.z - at.z;
}

void Player::stayOnKeeper() {
    // C# Kurt.StayOnKeeper (Kurt.Keeper.cs:200-217).
    Vec3 at;
    if (_keeper == nullptr || !keeperAt(_keeper, &at)) {
        return;
    }
    _platform = _keeper;
    const float top = at.z + _keptHeight;
    feet = Vec3{at.x, at.y, std::max(feet.z, top)};
    if (feet.z > top) {
        return;
    }
    onFloor = true;
    verticalSpeed = std::max(verticalSpeed, 0.0f);
}

// ---- end of platforms, solid objects and touches ---------------------------
// ---- The fans' updrafts: C# Kurt.Updraft.cs ----

void Player::updateUpdraft(float delta) {
    // C# Kurt.UpdateUpdraft (Kurt.Updraft.cs:21-49), in its order. The hook is the
    // scripts runtime's KurtUpdraft: his arena's fans, else the second arena's.
    const float vz = _updraftHook.updraft != nullptr && health() > 0
                         ? _updraftHook.updraft(_updraftHook.context, verticalSpeed, delta)
                         : NAN;
    if (std::isnan(vz)) {
        if (_mixer != nullptr) {
            _mixer->stopVoice(_fanVoice);
        }
        _fanVoice = 0;
        _inUpdraft = false;
        if (state() != Kurt::Knocked && verticalSpeed > kUpdraftExitSpeed) {
            verticalSpeed = kUpdraftExitSpeed;
        }
        return;
    }

    verticalSpeed = vz;
    if (!chuteOpen) {
        chuteOpen = true;
        playSound("CHUTEOUT");
    }
    if (!_inUpdraft) {
        _inUpdraft = true;
        _fanVoice = _mixer != nullptr ? _mixer->playLooped(kFanSound) : 0;
    }
}
