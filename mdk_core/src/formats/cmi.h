#pragma once

// ============================================================================
// cmi.h — LEVELn.CMI: a level's scripts and global models.
//
// A 1:1 port of src/Mdk.Formats/Cmi.cs.
//
// THE FILE. After the header, four directories, each a `u32 count` and then
// per entry a `u8 length`, a name of that length, and a `u32 offset` (from
// 4; zero means none):
//
//   0  alien instance scripts   HMO_1$XG_0
//   1  global models            (0 = look it up in the arena)
//   2  object type scripts      HMO_1$XH1_DOOR
//   3  arenas                   record: pascal NONE?, pascal music, u32 script
//
// THE ARENA'S RECORD is read at the offset the arena's directory entry
// gives, and the entry is then REPLACED by the script's offset: the
// directory says where the record is, and the record says where the script
// is.
//
// THE 1996 DEMO'S FILE is in cmi_beta.h.
//
// C++ HAS NO PARTIAL CLASSES, so the C#'s two halves (Cmi.cs and
// Cmi.Beta.cs) are one class here: the retail half is implemented in
// cmi.cpp and the demo's half in cmi_beta.cpp.
//
// THE ARCHIVE CANNOT BE COPIED. The model animations it hands out read its
// bytes when they are baked, and a copy of the file would have its own
// bytes while the animations still pointed at the original's.
// ============================================================================

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "formats/model.h"
#include "formats/model_animation.h"
#include "formats/script_decoder.h"

namespace mdk {
namespace formats {

// The absolute offset of a directory entry: from 4, and zero means none.
int32_t cmiAbsoluteOffset(uint32_t offset);

// Find a name in a name to offset map.
bool findOffset(const std::vector<std::pair<std::string, int32_t>>* map, const std::string& name,
                int32_t* outOffset);

// Set a name's offset. A name that is already there keeps its place and
// takes the new value, which is what the C#'s dictionary does.
void setOffset(std::vector<std::pair<std::string, int32_t>>* map, const std::string& name, int32_t offset);

// Find a name in a name to text map.
bool findText(const std::vector<std::pair<std::string, std::string>>* map, const std::string& name,
              std::string* outText);

// Set a name's text. A name that is already there keeps its place and
// takes the new value.
void setText(std::vector<std::pair<std::string, std::string>>* map, const std::string& name,
             const std::string& text);

// A level's CMI file.
class Cmi {
public:
    // Read a CMI. The bytes are copied. Returns false when the data runs
    // out.
    static bool parse(Cmi* out, const uint8_t* bytes, int32_t byteCount);

    // Read a CMI, taking the caller's buffer over. No copy.
    static bool parse(Cmi* out, std::vector<uint8_t>&& bytes);

    // Read a CMI of the 1996 demo. Implemented in cmi_beta.cpp.
    static bool parseBeta(Cmi* out, const uint8_t* bytes, int32_t byteCount);
    static bool parseBeta(Cmi* out, std::vector<uint8_t>&& bytes);

    // The default constructor: parse() takes the object as an out
    // parameter, so one has to be creatable.
    Cmi() = default;

    Cmi(const Cmi&) = delete;
    Cmi& operator=(const Cmi&) = delete;
    Cmi(Cmi&&) = default;
    Cmi& operator=(Cmi&&) = default;

    const std::vector<uint8_t>& bytes() const { return bytes_; }

    ScriptDialect dialect() const { return dialect_; }

    // Name to absolute file offset (0 = none), per directory. The order is
    // the order the file has them in.
    const std::vector<std::pair<std::string, int32_t>>& alienScripts() const { return alienScripts_; }
    const std::vector<std::pair<std::string, int32_t>>& modelOffsets() const { return modelOffsets_; }
    const std::vector<std::pair<std::string, int32_t>>& objectScripts() const { return objectScripts_; }
    const std::vector<std::pair<std::string, int32_t>>& arenaScripts() const { return arenaScripts_; }

    // Arena to its music (a sound of LEVELnO.SNI, or NONE).
    const std::vector<std::pair<std::string, std::string>>& arenaMusic() const { return arenaMusic_; }

    // Where scripts start: every alien, object and arena script, in
    // directory order (a name repeated in a directory counts once per
    // entry).
    const std::vector<int32_t>& entryPoints() const { return entries_; }

    // A global model, or false when it is an arena's (or unknown).
    bool model(const std::string& name, Model* out) const;

    // Arena to its ambient loop, played under the music (the 1996 demo's).
    const std::vector<std::pair<std::string, std::string>>& arenaAmbience() const { return arenaAmbience_; }

    // The demo's paths turned into retail spline records at the end of the
    // file: the old offset to the new.
    const std::vector<std::pair<int32_t, int32_t>>& betaPaths() const { return betaPaths_; }

    // An animation of the demo's file, which scripts point at directly.
    bool betaAnimation(int32_t offset, ModelAnimation* out);

private:
    // Read the four directories. `directory` is where the first count is.
    bool readDirectories(int32_t directory, bool beta);

    // The retail arena records: a name, the music, and the script.
    bool readArenas();

    // The demo's half: read the arena records and turn the paths into
    // spline records.
    bool readBetaArenas();
    void convertPaths();

    std::vector<uint8_t> bytes_;
    ScriptDialect dialect_ = ScriptDialect::Retail;

    std::vector<std::pair<std::string, int32_t>> alienScripts_;
    std::vector<std::pair<std::string, int32_t>> modelOffsets_;
    std::vector<std::pair<std::string, int32_t>> objectScripts_;
    std::vector<std::pair<std::string, int32_t>> arenaScripts_;
    std::vector<std::pair<std::string, std::string>> arenaMusic_;
    std::vector<std::pair<std::string, std::string>> arenaAmbience_;
    std::vector<std::pair<int32_t, int32_t>> betaPaths_;
    std::vector<int32_t> entries_;

    // The models parsed so far, and the demo's animations.
    mutable std::vector<std::pair<std::string, Model>> models_;
    std::vector<std::pair<int32_t, ModelAnimation>> betaAnimations_;
};

} // namespace formats
} // namespace mdk
