#ifndef SCRIBODO_FAULT_ALLOC_H
#define SCRIBODO_FAULT_ALLOC_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void fault_alloc_after(long successful_calls);
void fault_alloc_once_after(long successful_calls);
long fault_alloc_count(void);
long fault_alloc_failures(void); /* actual injected failures, not merely an armed position */
void *fault_malloc(size_t n);
void *fault_calloc(size_t n, size_t size);
void *fault_realloc(void *p, size_t n);
char *fault_strdup(const char *s);

#ifndef SCRIBODO_FAULT_ALLOC_IMPL
#define malloc fault_malloc
#define calloc fault_calloc
#define realloc fault_realloc
#define strdup fault_strdup
#endif

#endif
