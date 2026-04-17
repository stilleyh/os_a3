#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
// Don't include stdlb since the names will conflict?

// TODO: align

// sbrk some extra space every time we need it.
// This does no bookkeeping and therefore has no ability to free, realloc, etc.
void *nofree_malloc(size_t size) {
  void *p = sbrk(0);
  void *request = sbrk(size);
  if (request == (void*) -1) { 
    return NULL; // sbrk failed
  } else {
    assert(p == request); // Not thread safe.
    return p;
  }
}

struct block_meta {
  size_t size;
  struct block_meta *next;
  struct block_meta *prev;
  int free;
  int magic;    // For debugging only. TODO: remove this in non-debug mode.
};

#define META_SIZE sizeof(struct block_meta)

void *global_base = NULL;

// First-fit allocation (find any usable block)
// TODO: split block up if it's larger than necessary
struct block_meta *find_free_block(struct block_meta **last, size_t size) {
  struct block_meta *current = global_base;
  while (current && !(current->free && current->size >= size)) {
    *last = current;
    current = current->next;
  }
  return current;
}

// Best-fit allocation (find smallest usable block)
struct block_meta *best_fit(size_t size) {
    struct block_meta *curr = global_base;
    struct block_meta *best = NULL;

    while (curr) {
        if (curr->free && curr->size >= size) {
            if (!best || curr->size < best->size) {
                best = curr;
            }
        }
        curr = curr->next;
    }

    return best;
}

// Worst-fit allocation (find largest usable block)
struct block_meta *worst_fit(size_t size) {
    struct block_meta *curr = global_base;
    struct block_meta *worst = NULL;

    while (curr) {
        if (curr->free && curr->size >= size) {
            if (!worst || curr->size > worst->size) {
                worst = curr;
            }
        }
        curr = curr->next;
    }

    return worst;
}

struct block_meta *request_space(struct block_meta* last, size_t size) {
  struct block_meta *block;
  block = sbrk(0);
  void *request = sbrk(size + META_SIZE);
  assert((void*)block == request); // Not thread safe.
  if (request == (void*) -1) {
    return NULL; // sbrk failed.
  }
  
  if (last) { // NULL on first request.
    last->next = block;
  }
  block->size = size;
  block->next = NULL;
  block->prev = last;
  block->free = 0;
  block->magic = 0x12345678;
  return block;
}

// Block splitting function
void split_block(struct block_meta *block, size_t size) {
    // pointer to memory right after this block
    struct block_meta *new_block =
        (struct block_meta *)((char *)(block + 1) + size);

    new_block->size = block->size - size - META_SIZE;
    new_block->next = block->next;
    new_block->prev = block;
    new_block->free = 1;

    if (new_block->next) {
        new_block->next->prev = new_block;
    }

    block->size = size;
    block->next = new_block;
}

// If it's the first ever call, i.e., global_base == NULL, request_space and set global_base.
// Otherwise, if we can find a free block, use it.
// If not, request_space.
void *malloc(size_t size) {
  struct block_meta *block;
  // TODO: align size?

  if (size <= 0) {
    return NULL;
  }

  if (!global_base) { // First call.
    block = request_space(NULL, size);
    if (!block) {
      return NULL;
    }
    global_base = block;
  } else {
    struct block_meta *last = global_base;
    block = find_free_block(&last, size);
    if (!block) { // Failed to find free block.
      block = request_space(last, size);
      if (!block) {
	return NULL;
      }
    } else {      // Found free block
      // TODO: consider splitting block here.
      if (block->size >= size + META_SIZE + 1) {
        split_block(block, size);
      }
      block->free = 0;
      block->magic = 0x77777777;
    }
  }
  
  return(block+1);
}

void *calloc(size_t nelem, size_t elsize) {
  size_t size = nelem * elsize;
  void *ptr = malloc(size);
  memset(ptr, 0, size);
  return ptr;
}

// TODO: maybe do some validation here.
struct block_meta *get_block_ptr(void *ptr) {
  return (struct block_meta*)ptr - 1;
}

// Merge / coalesce blocks
void merge_blocks(struct block_meta *block) {
    struct block_meta *next = block->next;

    if (!next || !next->free) return;

    block->size += META_SIZE + next->size;
    block->next = next->next;

    if (next->next) {
        next->next->prev = block;
    }
}

// Merge blocks, then free finalized block
void free(void *ptr) {
  if (!ptr) return;

  struct block_meta *block_ptr = get_block_ptr(ptr);
  assert(block_ptr->free == 0);

  block_ptr->free = 1;

  // merge backward
  if (block_ptr->prev && block_ptr->prev->free) {
    merge_blocks(block_ptr->prev);
  }

  // merge forward
  if (block_ptr->next && block_ptr->next->free) {
    merge_blocks(block_ptr);
  }
}

void *realloc(void *ptr, size_t size) {
  if (!ptr) { 
    // NULL ptr. realloc should act like malloc.
    return malloc(size);
  }

  struct block_meta* block_ptr = get_block_ptr(ptr);
  if (block_ptr->size >= size) {
    // We have enough space. Could free some once we implement split.
    return ptr;
  }

  // Need to really realloc. Malloc new space and free old space.
  // Then copy old data to new space.
  void *new_ptr;
  new_ptr = malloc(size);
  if (!new_ptr) {
    return NULL; // TODO: set errno on failure.
  }
  memcpy(new_ptr, ptr, block_ptr->size);
  free(ptr);  
  return new_ptr;
}

// Leak counter (bytes)
size_t get_leaks() {
  struct block_meta *current = global_base;
  size_t total = 0;

  while (current) {
    if (!current->free) {
      total += current->size;
    }
    current = current->next;
  }

  return total;
}

// Print statements for debug
void print_heap() {
    struct block_meta *curr = global_base;
    while (curr) {
        printf("Block %p | size=%zu | free=%d\n", curr, curr->size, curr->free);
        curr = curr->next;
    }
}


int main() {
  void *mal[10], *cal[10], *rea[10];

  void *heap_start = sbrk(0);
  printf("Heap start: %p\n", heap_start);
  print_heap();

  // Malloc / Calloc block
  printf("Testing allocators...")
  for (int i = 0; i < 10; i++) {
    mal[i] = malloc(1 + i);      // offset to avoid malloc(0)
    cal[i] = calloc(1, 1 + i);
  }

  print_heap();

  // Realloc block
  printf("Testing reallocator...")
  for (int i = 0; i < 10; i++) {
    rea[i] = realloc(mal[i], 1 + i);
  }

  print_heap();

  // Free all calloc allocations
  printf("Freeing memory...")
  for (int i = 0; i < 10; i++) {
    free(cal[i]);
  }

  print_heap();

  // Free realloc results
  // Last item left unfreed to demonstrate leak
  printf("Freeing reallocated memory...")
  for (int i = 0; i <= 9; i++) {
    free(rea[i]);
  }

  print_heap();

  void *test1 = malloc(5);
  void *test2 = malloc(5);

  printf("Test allocations: %p %p\n", test1, test2);
  
  void *heap_end = sbrk(0);
  printf("Heap end: %p\n", heap_end);

  size_t leaks = get_leaks();
  printf("Total memory leaked: %zu bytes\n", leaks);
  return 0;
}