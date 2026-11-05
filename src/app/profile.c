/* Who is who: stored names. See profile.h. */
#define _DARWIN_C_SOURCE
#include "profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "core/utf8.h"

static void path_of(char *out, size_t outlen)
{
    const char *env = getenv("SCRIBODO_PROFILE");
    if (env && env[0]) snprintf(out, outlen, "%s", env);
    else snprintf(out, outlen, "%s/Library/Application Support/Scribodo/profile.json", getenv("HOME") ? getenv("HOME") : "");
}

void profile_clean_name(const char *in, char *out, size_t outlen)
{
    char buf[256];
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && k + 1 < sizeof(buf); p++) {
        if (*p < 0x20 || *p == 0x7F || *p == '"' || *p == '`') continue; /* no control characters or quoting */
        buf[k++] = (char)*p;
    }
    buf[k] = '\0';
    char *s = buf;
    while (*s == ' ' || *s == '\'') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '.' || s[n - 1] == '!' || s[n - 1] == '?' || s[n - 1] == ',' || s[n - 1] == '\''))
        s[--n] = '\0';
    utf8_clip(out, outlen, s, 60);
}

void profile_load(Profile *p)
{
    memset(p, 0, sizeof(*p));
    char path[1024];
    path_of(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *j = cJSON_Parse(buf);
    const cJSON *a = cJSON_GetObjectItem(j, "author"), *s = cJSON_GetObjectItem(j, "assistant");
    if (cJSON_IsString(a)) profile_clean_name(a->valuestring, p->author, sizeof(p->author));
    if (cJSON_IsString(s)) profile_clean_name(s->valuestring, p->assistant, sizeof(p->assistant));
    p->asked = cJSON_IsTrue(cJSON_GetObjectItem(j, "asked")) || p->author[0] || p->assistant[0];
    cJSON_Delete(j);
}

bool profile_save(const Profile *p)
{
    char path[1024], tmp[1100];
    path_of(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl) {
        *sl = '\0';
        mkdir(dir, 0700);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "author", p->author);
    cJSON_AddStringToObject(j, "assistant", p->assistant);
    cJSON_AddBoolToObject(j, "asked", true);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return false;
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fputs(s, f) >= 0;
    if (f) ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok, fclose(f);
    free(s);
    ok = ok && rename(tmp, path) == 0; /* atomic */
    if (!ok) unlink(tmp);
    return ok;
}

const char *profile_assistant_name(const Profile *p) { return p->assistant[0] ? p->assistant : "Scribodo"; }
