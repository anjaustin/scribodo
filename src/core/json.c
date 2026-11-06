#include "json.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t parse_mutex = PTHREAD_MUTEX_INITIALIZER;

void json_vendor_reset_allocation_error(void);
bool json_vendor_allocation_error(void);

cJSON *json_parse_checked(const char *text, bool *allocation_error)
{
    *allocation_error = false;
    if (pthread_mutex_lock(&parse_mutex) != 0) { *allocation_error = true; return NULL; }
    json_vendor_reset_allocation_error();
    cJSON *tree = cJSON_Parse(text);
    *allocation_error = json_vendor_allocation_error();
    pthread_mutex_unlock(&parse_mutex);
    return tree;
}

cJSON *json_parse(const char *text)
{
    bool allocation_error;
    return json_parse_checked(text, &allocation_error);
}

bool json_has_nul_escape(const char *text, size_t n)
{
    for (size_t i = 0; i + 1 < n; i++) {
        if (text[i] != '\\') continue;
        if (n - i >= 6 && !memcmp(text + i + 1, "u0000", 5)) return true;
        i++;
    }
    return false;
}
