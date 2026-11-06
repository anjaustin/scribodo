/* cJSON's parser mutates process-global error state, including on success.
 * All application and test parsing goes through this serialized boundary.
 * Returned trees remain caller-owned and need no lock once parsing finishes.
 * Do not call cJSON_GetErrorPtr or change allocator hooks/locale at runtime. */
#ifndef SCRIBODO_JSON_H
#define SCRIBODO_JSON_H

#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>

cJSON *json_parse(const char *text);
/* Distinguish malformed crash-torn input from allocation failure. */
cJSON *json_parse_checked(const char *text, bool *allocation_error);
bool json_has_nul_escape(const char *text, size_t n);

#endif
