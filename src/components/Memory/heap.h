#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>

void heap_init(uintptr_t addr, size_t size);

void* kmalloc(size_t size);
void kfree(void* ptr);
void* krealloc(void* ptr, size_t size);

size_t heap_get_used(void);
size_t heap_get_total(void);

#endif
