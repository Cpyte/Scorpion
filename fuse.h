#ifndef SCORPION_FUSE_H
#define SCORPION_FUSE_H

#include <stddef.h>
#include <stdint.h>

#define FUSE_MAX_FILES 16
#define FUSE_NAME_LEN  24
#define FUSE_MAX_FD    16

#define FUSE_M_READ  1u
#define FUSE_M_WRITE 2u

int fuse_init(void);
int fuse_format(void);

int fuse_open(const char *name, unsigned mode);
int fuse_close(int fd);
int fuse_read(int fd, void *buf, size_t size);
int fuse_write(int fd, const void *buf, size_t size);

int fuse_list(void);

#endif
