// ============================================================================
// side_core_3ds.cpp -- see side_core_3ds.h.
// ============================================================================

#include "platform/side_core_3ds.h"

namespace mdk {

namespace {
bool g_checked = false;
bool g_new3ds = false;
bool g_syscoreAsked = false;
bool g_syscoreGranted = false;
} // namespace

int32_t offMainCore3ds(bool allowSyscore) {
    if (!g_checked) {
        g_checked = true;
        bool isNew = false;
        g_new3ds = R_SUCCEEDED(APT_CheckNew3DS(&isNew)) && isNew;
    }
    if (g_new3ds) {
        return 2;
    }
    if (allowSyscore) {
        // The system core's share is asked for once, before the first thread
        // there; refused (an older system, a launcher that forbids it), the
        // thread goes to the default core like any other.
        if (!g_syscoreAsked) {
            g_syscoreAsked = true;
            g_syscoreGranted = R_SUCCEEDED(APT_SetAppCpuTimeLimit(kSyscorePercent));
        }
        if (g_syscoreGranted) {
            return 1;
        }
    }
    return -2;
}

} // namespace mdk
