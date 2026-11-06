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

cJSON *json_parse_complete(const char *text, size_t n)
{
    /* cJSON strings cannot represent embedded NULs without truncating values
     * or aliasing field names. Reject them before interpreting any fields. */
    if (!text || !n || memchr(text, 0, n) || json_has_nul_escape(text, n)) return NULL;
    if (pthread_mutex_lock(&parse_mutex) != 0) return NULL;
    json_vendor_reset_allocation_error();
    const char *end = NULL;
    cJSON *tree = cJSON_ParseWithLengthOpts(text, n, &end, false);
    pthread_mutex_unlock(&parse_mutex);
    if (tree) {
        while (end < text + n && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) end++;
        if (end != text + n) {
            cJSON_Delete(tree);
            tree = NULL;
        }
    }
    return tree;
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
