/*
 * golden.h — loader for the committed blob fixtures in golden/.
 *
 * Fixtures are read from disk rather than compiled in so that the hex a test
 * asserts against is the same text a human (or the configurator author) reads.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Load golden/blob_<stem>.hex. Returns the byte length, or 0 (with a message on
   stderr) if the file is missing or malformed. */
size_t golden_load(const char *stem, uint8_t *out, size_t cap);
