#pragma once

// ============================================================================
// material_resolver.h — what a triangle's material value MEANS.
//
// A 1:1 port of src/Mdk.Game/Level/MaterialResolver.cs, minus the parts that
// belong to the GPU.
//
// A MATERIAL VALUE is one of:
//
//   value < 0      a palette colour, the index being -value
//   value >= 0     an index into the arena's material names; the name is
//                  then a texture, an archive colour, PEN_n, or NONE
//   colour 256-1028  a SPECIAL: the glass (GLASS1-4), the mirrors, NONE,
//                  PEN_ENV and RIPPLE
//
// The resolver turns a value into a SURFACE: a texture, or a flat colour, or
// nothing (not drawn).
//
// WHY IT IS SHARED. Both backends need the same answer, and a difference
// between them would be a difference in the PICTURE -- which the whole
// design forbids (SPEC.md 5.3). The backend turns a surface into its own
// GPU resources; the resolution itself is identical everywhere.
//
// The GPU handles are NOT here. The C#'s resolver creates the textures and
// the palettes as it resolves them, and hands the renderer their ids. Here
// the surface carries the TEXTURE (the format's object) and the backend
// maps it to a GPU texture, caching as it goes. That keeps the GPU's
// lifetime and the format's lifetime apart.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "formats/dti.h"
#include "formats/palette.h"
#include "formats/texture_archive.h"
#include "render/render_types.h"

namespace mdk {

// A surface: what a triangle draws with.
struct Surface {
    // The texture. Null for a flat colour.
    const formats::Texture* texture = nullptr;

    // The flat colour, for a surface with no texture.
    Colour colour = {1.0f, 1.0f, 1.0f, 1.0f};

    // The texture's frame count, for an animated texture.
    int32_t frameCount = 1;

    Pass pass = Pass::Solid;
    Shading shading = Shading::Original;

    // The mirror's panorama row shift, in rows. Zero for everything else.
    float rowShift = 0.0f;
};

// The material resolver.
class MaterialResolver {
public:
    // The first special colour. Below it, a colour is a palette index.
    static constexpr int32_t kSpecialFirst = 256;

    // The mirrors' range, and the glass's.
    static constexpr int32_t kMirrorFirst = 990;
    static constexpr int32_t kMirrorLast = 1010;
    static constexpr int32_t kGlassFirst = 1024;
    static constexpr int32_t kGlassLast = 1027;

    // A mirror's panorama row is 8 rows per value from MIRRMED (1000):
    // MIRRLOW (990) is 80 lower, MIRRHIGH (1010) is 80 higher.
    static constexpr int32_t kMirrorMiddle = 1000;
    static constexpr float kMirrorRowsPerValue = 8.0f;

    // The prefix of a flat palette colour's material name.
    static const char* const kPenPrefix;

    MaterialResolver(const formats::Dti* dti, Shading shading = Shading::Original);

    // The surface of a triangle's material value. Returns false when the
    // value is not drawn.
    //
    // `names` is the arena's material list, `palette` the arena's palette,
    // and `archives` the texture archives searched for the name: the arena's
    // own, the level's, then every other arena's (some borrow, e.g. O3_*
    // in level 6).
    //
    // `pass` is the C#'s Resolve argument: the arena draws its meshes Solid and
    // the objects draw theirs DoubleSided (ObjectView.cs), and the pass reaches
    // the surface's shading. The glass and the mirrors keep their own pass in
    // both trees, exactly as the C# hardcodes them.
    bool resolve(int32_t value,
                 const std::vector<std::string>& names,
                 const formats::Palette& palette,
                 const std::vector<const formats::TextureArchive*>& archives,
                 Surface* out,
                 Pass pass = Pass::Solid) const;

    // Whether a material value is drawn, without touching a GPU.
    static bool isDrawn(int32_t value,
                        const std::vector<std::string>& names,
                        const std::vector<const formats::TextureArchive*>& archives);

    // Whether a palette colour is drawn: the ordinary ones, the glass and
    // the mirrors are; the other specials are not.
    static bool isDrawnColour(int32_t index);

private:
    bool resolveColour(int32_t index, const formats::Palette& palette, Surface* out, Pass pass) const;

    const formats::Dti* dti_ = nullptr;
    Shading shading_ = Shading::Original;
};

} // namespace mdk
