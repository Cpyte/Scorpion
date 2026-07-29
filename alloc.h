#ifndef WEW_SCORPION_ALLOC_H
#define WEW_SCORPION_ALLOC_H

#include <stddef.h>

void  alloc_init(void);
void *alloc_(size_t size);
void  free_(void *ptr);

#endif // WEW_SCORPION_ALLOC_H
