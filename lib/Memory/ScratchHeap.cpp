#include "ScratchHeap.h"

#include <Logging.h>

namespace scratchheap {
namespace {

struct BlockHeader {
  uint32_t size;  // payload bytes
  uint32_t used;
};

constexpr size_t ALIGNMENT = 8;
constexpr size_t HEADER_SIZE = sizeof(BlockHeader);

uint8_t* blockBase = nullptr;
size_t blockSize = 0;
bool blockActive = false;
size_t liveAllocations = 0;

size_t alignUp(size_t value) { return (value + ALIGNMENT - 1) & ~(ALIGNMENT - 1); }

BlockHeader* headerOf(const void* pointer) {
  return reinterpret_cast<BlockHeader*>(const_cast<uint8_t*>(static_cast<const uint8_t*>(pointer)) - HEADER_SIZE);
}

// Merge Forward - free() only marks a block, so neighbouring free blocks are joined here. The
// block count stays in the low tens, so a full walk is cheaper than maintaining links.
void coalesce() {
  uint8_t* cursor = blockBase;
  uint8_t* end = blockBase + blockSize;
  while (cursor + HEADER_SIZE <= end) {
    auto* header = reinterpret_cast<BlockHeader*>(cursor);
    uint8_t* next = cursor + HEADER_SIZE + header->size;
    if (header->used || next + HEADER_SIZE > end) {
      cursor = next;
      continue;
    }
    auto* following = reinterpret_cast<BlockHeader*>(next);
    if (following->used) {
      cursor = next;
      continue;
    }
    header->size += HEADER_SIZE + following->size;
  }
}

}  // namespace

bool activate(uint8_t* buffer, size_t size) {
  if (blockActive || !buffer) return false;

  auto* aligned = reinterpret_cast<uint8_t*>(alignUp(reinterpret_cast<uintptr_t>(buffer)));
  const size_t adjustment = static_cast<size_t>(aligned - buffer);
  if (size <= adjustment + HEADER_SIZE + ALIGNMENT) return false;

  blockBase = aligned;
  blockSize = size - adjustment;
  blockActive = true;
  liveAllocations = 0;

  auto* first = reinterpret_cast<BlockHeader*>(blockBase);
  first->size = blockSize - HEADER_SIZE;
  first->used = 0;
  return true;
}

void deactivate() {
  if (liveAllocations != 0) {
    LOG_ERR("SCRATCH", "deactivated with %u allocation(s) outstanding", (unsigned)liveAllocations);
  }
  blockActive = false;
  liveAllocations = 0;
}

bool isActive() { return blockActive; }

void* alloc(size_t size) {
  if (!blockActive || size == 0) return nullptr;

  const size_t needed = alignUp(size);
  uint8_t* cursor = blockBase;
  uint8_t* end = blockBase + blockSize;
  while (cursor + HEADER_SIZE <= end) {
    auto* header = reinterpret_cast<BlockHeader*>(cursor);
    uint8_t* payload = cursor + HEADER_SIZE;
    if (!header->used && header->size >= needed) {
      const size_t remainder = header->size - needed;
      if (remainder >= HEADER_SIZE + ALIGNMENT) {
        auto* split = reinterpret_cast<BlockHeader*>(payload + needed);
        split->size = remainder - HEADER_SIZE;
        split->used = 0;
        header->size = needed;
      }
      header->used = 1;
      ++liveAllocations;
      return payload;
    }
    cursor = payload + header->size;
  }
  return nullptr;
}

void free(void* pointer) {
  if (!pointer || !owns(pointer) || !blockActive) return;

  auto* header = headerOf(pointer);
  if (!header->used) return;  // double free: the block is already back in the pool
  header->used = 0;
  if (liveAllocations > 0) --liveAllocations;
  coalesce();
}

bool owns(const void* pointer) {
  if (!blockBase || !pointer) return false;
  const auto* address = static_cast<const uint8_t*>(pointer);
  return address >= blockBase + HEADER_SIZE && address < blockBase + blockSize;
}

size_t sizeOf(const void* pointer) { return owns(pointer) ? headerOf(pointer)->size : 0; }

size_t outstanding() { return liveAllocations; }

}  // namespace scratchheap
