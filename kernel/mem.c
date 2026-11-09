/* AxisOS - kernel heap (first-fit, splitting and coalescing) */
#include "axis.h"

typedef struct block {
    uint32_t size; /* payload size */
    uint32_t magic;
    struct block *next;
    uint32_t free;
} block_t;

#define BLOCK_MAGIC 0xA715B10C
#define ALIGN8(x) (((x) + 7) & ~7u)

static block_t *heap_head;
static uint32_t heap_start, heap_end;

void heap_init(uint32_t start, uint32_t end) {
    heap_start = ALIGN8(start);
    heap_end = end;
    heap_head = (block_t *)heap_start;
    heap_head->size = heap_end - heap_start - sizeof(block_t);
    heap_head->magic = BLOCK_MAGIC;
    heap_head->next = NULL;
    heap_head->free = 1;
}

void *kmalloc(size_t n) {
    if (!n) n = 1;
    n = ALIGN8(n);
    for (block_t *b = heap_head; b; b = b->next) {
        if (!b->free || b->size < n) continue;
        if (b->size >= n + sizeof(block_t) + 16) {
            block_t *nb = (block_t *)((uint8_t *)(b + 1) + n);
            nb->size = b->size - n - sizeof(block_t);
            nb->magic = BLOCK_MAGIC;
            nb->next = b->next;
            nb->free = 1;
            b->next = nb;
            b->size = n;
        }
        b->free = 0;
        return b + 1;
    }
    return NULL;
}

void *kzalloc(size_t n) {
    void *p = kmalloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void kfree(void *p) {
    if (!p) return;
    block_t *b = (block_t *)p - 1;
    if (b->magic != BLOCK_MAGIC || b->free) return; /* ignore bad frees */
    b->free = 1;
    /* coalesce the whole list (cheap enough for our heap sizes) */
    for (block_t *c = heap_head; c && c->next;) {
        if (c->free && c->next->free) {
            c->size += sizeof(block_t) + c->next->size;
            c->next = c->next->next;
        } else
            c = c->next;
    }
}

void *krealloc(void *p, size_t n) {
    if (!p) return kmalloc(n);
    block_t *b = (block_t *)p - 1;
    if (b->size >= n) return p;
    void *q = kmalloc(n);
    if (!q) return NULL;
    memcpy(q, p, b->size);
    kfree(p);
    return q;
}

void heap_stats(uint32_t *total, uint32_t *used, uint32_t *blocks) {
    uint32_t u = 0, c = 0;
    for (block_t *b = heap_head; b; b = b->next) {
        c++;
        if (!b->free) u += b->size + sizeof(block_t);
    }
    if (total) *total = heap_end - heap_start;
    if (used) *used = u;
    if (blocks) *blocks = c;
}
