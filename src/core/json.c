#include "json.h"

#include <pthread.h>

static pthread_mutex_t parse_mutex = PTHREAD_MUTEX_INITIALIZER;

cJSON *json_parse(const char *text)
{
    if (pthread_mutex_lock(&parse_mutex) != 0) return NULL;
    cJSON *tree = cJSON_Parse(text);
    pthread_mutex_unlock(&parse_mutex);
    return tree;
}
