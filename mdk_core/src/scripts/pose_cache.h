#pragma once

// ============================================================================
// pose_cache.h — dropping the shared object-pose caches.
//
// The C#'s MdkObject.RestPoses and MdkObject.Baked are static caches keyed by
// the Model/ModelAnimation OBJECT (see scripts/object.h's pose_parts()). They
// hold derived data -- rest vertices and baked animation frames -- so dropping
// them is always safe: the next pose is rebuilt from the model. They are only
// unsafe to KEEP across a level change, because a new level's model can land on
// a freed address and inherit the old one's pose.
//
// The C# drops them once per level, in the Viewer's constructor (Viewer.cs:185)
// right after the LevelData is built and before anything is laid out or drawn.
// The C++ calls this from LevelData::load, which is the same moment and covers
// every consumer, including a harness with no viewer at all.
//
// WHY A FREE FUNCTION IN ITS OWN HEADER, when MdkObject::forget_poses() is the
// C#-named API: the game layer must be able to invalidate the caches without
// including the scripts layer (scripts includes game, and a .cpp-level cycle
// would be worse than the two names). forget_poses() calls this; both are the
// same drop.
// ============================================================================

namespace mdk {
namespace scripts {

// The C#'s MdkObject.ForgetPoses(): drop every cached rest pose and baked
// animation frame.
void forgetPoseCaches();

} // namespace scripts
} // namespace mdk
