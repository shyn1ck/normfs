#ifndef NORMFS_QUEUE_NAME_H
#define NORMFS_QUEUE_NAME_H

#include <stddef.h>
#include <stdint.h>

/* Returns 1 when a queue path joined to the data directory stays inside it. */
int normfs_queue_name_ok(const uint8_t *name, size_t len, int windows);

#endif /* NORMFS_QUEUE_NAME_H */
