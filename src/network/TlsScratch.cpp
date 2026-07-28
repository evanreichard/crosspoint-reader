#include "TlsScratch.h"

#include <Logging.h>
#include <ScratchHeap.h>
#include <wolfssl/ssl.h>

#include <cstdlib>
#include <cstring>

namespace tlsscratch {
namespace {

// Only the buffers that the fragmented heap cannot serve are worth diverting; everything smaller
// keeps using malloc so the scratch block stays available for the record buffers.
constexpr size_t MIN_SCRATCH_ALLOC = 4096;

bool allocatorsInstalled = false;

void* scratchMalloc(size_t size) {
  if (size >= MIN_SCRATCH_ALLOC) {
    if (void* pointer = scratchheap::alloc(size)) return pointer;
  }
  return malloc(size);
}

void scratchFree(void* pointer) {
  // owns() still answers after deactivate(), which is what keeps a late free out of the C heap.
  if (scratchheap::owns(pointer)) {
    scratchheap::free(pointer);
    return;
  }
  free(pointer);
}

void* scratchRealloc(void* pointer, size_t size) {
  if (!pointer) return scratchMalloc(size);
  if (!scratchheap::owns(pointer)) return realloc(pointer, size);

  void* moved = scratchMalloc(size);
  if (!moved) return nullptr;
  memcpy(moved, pointer, std::min(size, scratchheap::sizeOf(pointer)));
  scratchheap::free(pointer);
  return moved;
}

}  // namespace

bool activate(uint8_t* buffer, size_t size) {
  if (!scratchheap::activate(buffer, size)) return false;

  if (!allocatorsInstalled) {
    if (wolfSSL_SetAllocators(scratchMalloc, scratchFree, scratchRealloc) != 0) {
      LOG_ERR("TLS", "wolfSSL_SetAllocators rejected");
      scratchheap::deactivate();
      return false;
    }
    allocatorsInstalled = true;
  }
  return true;
}

void deactivate() { scratchheap::deactivate(); }

}  // namespace tlsscratch
