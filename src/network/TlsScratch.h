#pragma once

#include <cstddef>
#include <cstdint>

// Serves wolfSSL's large allocations from a caller-lent block instead of the heap.
//
// By the time a Lua app transfers, Wi-Fi and the Lua VM have left the heap's largest contiguous
// block around 22 KB while wolfSSL's record buffer alone wants ~17 KB, so the handshake fails with
// MEMORY_E. Lending the framebuffer's bytes for the duration of the transfer fixes that without
// freeing the framebuffer allocation, so it cannot fail to come back.
//
// Small allocations stay on the heap: the point is contiguity for the few big buffers, not
// replacing the allocator.
namespace tlsscratch {

bool activate(uint8_t* buffer, size_t size);
void deactivate();

}  // namespace tlsscratch
