// SPDX-License-Identifier: BSD-3-Clause

#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "osmem.h"
#include "block_meta.h"

#define ALIGNMENT	8
#define ALIGN(x)	(((x) + (ALIGNMENT - 1)) & ~((size_t)ALIGNMENT - 1))
#define META_SIZE	ALIGN(sizeof(struct block_meta))
#define MMAP_THRESHOLD	(128 * 1024)

/* Heap (brk) blocks, kept in address order. */
static struct block_meta *heap_head;
/* mmap-ed blocks (order does not matter). */
static struct block_meta *mmap_head;
static int heap_preallocated;

static inline void *payload(struct block_meta *b)
{
	return (char *)b + META_SIZE;
}

static inline struct block_meta *meta(void *ptr)
{
	return (struct block_meta *)((char *)ptr - META_SIZE);
}

/* ------------------------------------------------------------------ */
/* Heap list helpers                                                   */
/* ------------------------------------------------------------------ */

static struct block_meta *heap_last(void)
{
	struct block_meta *b = heap_head;

	if (!b)
		return NULL;
	while (b->next)
		b = b->next;
	return b;
}

/* Merge every pair of adjacent free blocks on the heap. */
static void coalesce_all(void)
{
	struct block_meta *b = heap_head;

	while (b && b->next) {
		struct block_meta *n = b->next;

		if (b->status == STATUS_FREE && n->status == STATUS_FREE) {
			b->size += META_SIZE + n->size;
			b->next = n->next;
			if (n->next)
				n->next->prev = b;
		} else {
			b = n;
		}
	}
}

/* Best fit: smallest free block that is large enough. */
static struct block_meta *find_best(size_t size)
{
	struct block_meta *best = NULL;

	for (struct block_meta *b = heap_head; b; b = b->next)
		if (b->status == STATUS_FREE && b->size >= size &&
		    (!best || b->size < best->size))
			best = b;
	return best;
}

/*
 * Shrink block to `size` bytes of payload and turn the rest into a new
 * free block, but only if the rest can hold a header + at least 1 byte
 * (aligned -> 8 bytes).
 */
static void split_block(struct block_meta *b, size_t size)
{
	struct block_meta *n;

	if (b->size < size + META_SIZE + ALIGN(1))
		return;

	n = (struct block_meta *)((char *)b + META_SIZE + size);
	n->size = b->size - size - META_SIZE;
	n->status = STATUS_FREE;
	n->prev = b;
	n->next = b->next;
	if (b->next)
		b->next->prev = n;
	b->next = n;
	b->size = size;
}

/* ------------------------------------------------------------------ */
/* Allocation back-ends                                                */
/* ------------------------------------------------------------------ */

static void preallocate_heap(void)
{
	struct block_meta *b = sbrk(MMAP_THRESHOLD);

	DIE(b == (void *)-1, "sbrk");

	b->size = MMAP_THRESHOLD - META_SIZE;
	b->status = STATUS_FREE;
	b->prev = NULL;
	b->next = NULL;
	heap_head = b;
	heap_preallocated = 1;
}

/* `size` is already aligned. */
static void *alloc_heap(size_t size)
{
	struct block_meta *b, *last;

	if (!heap_preallocated)
		preallocate_heap();

	coalesce_all();

	b = find_best(size);
	if (b) {
		split_block(b, size);
		b->status = STATUS_ALLOC;
		return payload(b);
	}

	/* No fit: expand the last block if it is free... */
	last = heap_last();
	if (last && last->status == STATUS_FREE) {
		DIE(sbrk(size - last->size) == (void *)-1, "sbrk");
		last->size = size;
		last->status = STATUS_ALLOC;
		return payload(last);
	}

	/* ...otherwise append a brand new block. */
	b = sbrk(META_SIZE + size);
	DIE(b == (void *)-1, "sbrk");
	b->size = size;
	b->status = STATUS_ALLOC;
	b->next = NULL;
	b->prev = last;
	if (last)
		last->next = b;
	else
		heap_head = b;
	return payload(b);
}

/* `size` is already aligned. */
static void *alloc_mmap(size_t size)
{
	struct block_meta *b = mmap(NULL, META_SIZE + size,
				    PROT_READ | PROT_WRITE,
				    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	DIE(b == MAP_FAILED, "mmap");

	b->size = size;
	b->status = STATUS_MAPPED;
	b->prev = NULL;
	b->next = mmap_head;
	if (mmap_head)
		mmap_head->prev = b;
	mmap_head = b;
	return payload(b);
}

static void free_mmap(struct block_meta *b)
{
	if (b->prev)
		b->prev->next = b->next;
	else
		mmap_head = b->next;
	if (b->next)
		b->next->prev = b->prev;

	DIE(munmap(b, META_SIZE + b->size) == -1, "munmap");
}

static void *alloc(size_t size, size_t threshold)
{
	if (size == 0)
		return NULL;

	size = ALIGN(size);
	if (META_SIZE + size < threshold)
		return alloc_heap(size);
	return alloc_mmap(size);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void *os_malloc(size_t size)
{
	return alloc(size, MMAP_THRESHOLD);
}

void os_free(void *ptr)
{
	struct block_meta *b;

	if (!ptr)
		return;

	b = meta(ptr);
	if (b->status == STATUS_MAPPED)
		free_mmap(b);
	else if (b->status == STATUS_ALLOC)
		b->status = STATUS_FREE;	/* heap memory is never returned */
}

void *os_calloc(size_t nmemb, size_t size)
{
	size_t total;
	void *ptr;

	if (nmemb == 0 || size == 0)
		return NULL;
	if (nmemb > (size_t)-1 / size)	/* overflow */
		return NULL;

	total = nmemb * size;
	ptr = alloc(total, (size_t)getpagesize());
	if (ptr)
		memset(ptr, 0, total);
	return ptr;
}

/* Allocate a new block, copy the data, free the old one. */
static void *move_block(void *ptr, size_t old_size, size_t new_size)
{
	void *new_ptr = os_malloc(new_size);

	if (!new_ptr)
		return NULL;
	memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
	os_free(ptr);
	return new_ptr;
}

void *os_realloc(void *ptr, size_t size)
{
	struct block_meta *b;
	size_t old_size;

	if (!ptr)
		return os_malloc(size);
	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	b = meta(ptr);
	if (b->status == STATUS_FREE)
		return NULL;

	size = ALIGN(size);
	old_size = b->size;

	/* mmap-ed blocks, or heap blocks that must become mmap-ed: move. */
	if (b->status == STATUS_MAPPED || META_SIZE + size >= MMAP_THRESHOLD)
		return move_block(ptr, old_size, size);

	/* Heap block, shrinking (or same size): truncate in place. */
	if (size <= b->size) {
		return ptr;
	}

	/* Heap block, growing. Last block -> just push the break. */
	if (!b->next) {
		DIE(sbrk(size - b->size) == (void *)-1, "sbrk");
		b->size = size;
		return ptr;
	}

	/* Coalesce with following free blocks one at a time. */
	while (b->next && b->next->status == STATUS_FREE && b->size < size) {
		struct block_meta *n = b->next;

		b->size += META_SIZE + n->size;
		b->next = n->next;
		if (n->next)
			n->next->prev = b;
	}

	if (b->size >= size) {
		split_block(b, size);
		return ptr;
	}

	/* Absorbed everything up to the end of the heap -> expand. */
	if (!b->next) {
		DIE(sbrk(size - b->size) == (void *)-1, "sbrk");
		b->size = size;
		return ptr;
	}

	return move_block(ptr, old_size, size);
}
