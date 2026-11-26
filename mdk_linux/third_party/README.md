# mdk_linux/third_party — vendored dependencies of the Linux build

Pinned copies, checked in, so a fresh clone builds without fetching anything:
the build never searches the system for these.

| dir       | what                                   | upstream                                   | licence      |
|-----------|----------------------------------------|--------------------------------------------|--------------|
| `vulkan/` | Vulkan C headers (`vulkan/*.h`, `vk_video/*.h`) — VK_HEADER_VERSION 360 | github.com/KhronosGroup/Vulkan-Headers | Apache-2.0 / MIT |
| `volk/`   | Vulkan meta-loader, VOLK_HEADER_VERSION 360 (dlopen + function pointer table; no link to libvulkan) | github.com/zeux/volk | MIT |

Each directory carries its upstream `LICENSE.md`. Only the C headers of
Vulkan-Headers are vendored; the C++ bindings (`vulkan.hpp`, 20 MB) are
deliberately left out — the renderer is "C with classes" and uses the flat C
API through volk.
