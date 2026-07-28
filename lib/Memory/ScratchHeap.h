#pragma once

#include <cstddef>
#include <cstdint>

// A first-fit allocator over a block of memory the caller lends us (in practice the framebuffer
// bytes parked in buildscratch). It exists so a consumer that insists on malloc/free semantics --
// wolfSSL -- can be served from that block while the real heap is too fragmented to hold its
// ~17 KB record buffer.
//
// owns() keeps answering after deactivate() so a late free() from the consumer is recognised and
// ignored rather than handed to the C heap, which would corrupt it.
namespace scratchheap {

bool activate(uint8_t* buffer, size_t size);
void deactivate();  // logs when allocations are still outstanding
bool isActive();

void* alloc(size_t size);  // nullptr when inactive or out of room: the caller falls back to malloc
void free(void* pointer);
bool owns(const void* pointer);
size_t sizeOf(const void* pointer);
size_t outstanding();

}  // namespace scratchheap
