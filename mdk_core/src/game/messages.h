#pragma once

// ============================================================================
// messages.h -- the on-screen messages, a port of src/Mdk.Game/Hud/Messages.cs.
//
// Texts of MISC/MDKFONT.FTI queued by hud_message (opcode 247), the pickups
// and a few events (0x425400), shown one after the other in the big font
// (0x425474). One or two lines (split at the two-character "\n" escape),
// centred on the canvas, baseline at y 120 (two lines: 105 and 135). Flag 1
// grows the message from nothing in half a second and shrinks it back when
// its time is up; its time runs twice as fast while others wait. Flag 2
// queues it first. A line too wide for the big font is drawn in the small
// one (0x415b30).
//
//   push ──► queue (4) ──► current: grow 0.5 s ─► time (x2 while others wait) ─► shrink 0.5 s
//
// NO HEAP AFTER LOAD. The C#'s queue is a LinkedList of records holding
// List<byte[]> lines; here a message is a fixed record (two lines of at most
// 35 bytes, which is all PushText ever keeps) and the queue a fixed array, so
// a push and the per-frame update and draw allocate nothing. The texts are
// copied out of the FTI once (setTexts): every entry's text bytes, which is
// exactly what Fti.GetTextBytes would return for it, a few KB instead of
// keeping the whole 160 KB file.
// ============================================================================

#include <stdint.h>

#include <string>
#include <vector>

#include "render/render.h"

namespace mdk {

class FontView;

namespace formats {
class Fti;
} // namespace formats

class Messages {
public:
    static const int32_t kFlagZoom = 1;
    static const int32_t kFlagFront = 2;
    // The queue has 4 entries (0x57ecf0).
    static const int32_t kQueueSize = 4;
    static const int32_t kMaxLines = 2;
    static const int32_t kMaxLineLength = 35;

    struct Message {
        uint8_t text[kMaxLines][kMaxLineLength] = {};
        int32_t length[kMaxLines] = {};
        int32_t lineCount = 0;
        int32_t flags = 0;
        float time = 0.0f;
    };

    // The FTI's texts, copied once (cold: at load). Without them every push of a
    // name fails, as a name the FTI does not have does.
    void setTexts(const formats::Fti& fti);

    // Empties the queue and the current message (a respawn is a fresh Viewer, and
    // with it a fresh HudView and Messages). The texts stay.
    void reset();

    // C# Push: queues the text named `textName`; false if there is none.
    bool push(const char* textName, int32_t flags, float seconds);

    // C# PushText: queues `text` (the FTI's bytes, "\n" escapes kept); false if it
    // is empty.
    bool pushText(const uint8_t* text, int32_t length, int32_t flags, float seconds);

    // The viewer's subscriber to Inventory.PickedUp (Viewer.cs:249: pickups show
    // their names, 0x46c448): the name, zoomed, for 2 seconds. `context` is the
    // Messages; bound as Kurt::PickupHook's function.
    static const float kPickupSeconds;
    static void pickedUp(void* context, const std::string& name);

    // C# Update: advances the messages (0x425474, once per frame).
    void update(float delta);

    // C# Draw: the current message, centred on a canvas `width` wide. Call between
    // mdk_render_canvas_begin and _end.
    void draw(MdkRenderer renderer, const FontView& big, const FontView& small,
              float width) const;

    // ---- Readable state (the golden's dump) ----
    const Message* current() const { return hasCurrent_ ? &current_ : nullptr; }
    int32_t queued() const { return queueCount_; }
    const Message& queuedAt(int32_t i) const { return queue_[i]; }
    float time() const { return time_; }
    float zoom() const { return zoom_; }

private:
    struct Text {
        std::string name;
        int32_t offset = 0;
        int32_t length = 0;
    };

    // The FTI's texts: names in the FTI's order, bytes back to back.
    std::vector<Text> texts_;
    std::vector<uint8_t> textBytes_;

    // The queue, first to last; one more than its size for the moment between an
    // add and the removal of the last (the C#'s AddLast/AddFirst then RemoveLast).
    Message queue_[kQueueSize + 1];
    int32_t queueCount_ = 0;
    Message current_;
    bool hasCurrent_ = false;
    // The time the current message stays (0x57ece0) and its zoom (0x57ece4, 0-0.5).
    float time_ = 0.0f;
    float zoom_ = 0.0f;
};

} // namespace mdk
