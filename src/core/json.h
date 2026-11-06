/* cJSON's parser mutates process-global error state, including on success.
 * All application and test parsing goes through this serialized boundary.
 * Returned trees remain caller-owned and need no lock once parsing finishes.
 * Do not call cJSON_GetErrorPtr or change allocator hooks/locale at runtime. */
#ifndef SCRIBODO_JSON_H
#define SCRIBODO_JSON_H

#include "cJSON.h"

cJSON *json_parse(const char *text);

#endif
