/* The chat model's settings. See model_settings.h. */
#define _DARWIN_C_SOURCE
#include "model_settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"

static const ModelSettingInfo k_info[MODEL_SETTING_COUNT] = {
    { "Max reply length", "tokens per reply", 256, 8192, 256, true },
    { "Temperature", "higher: more varied", 0.0, 1.5, 0.05, false },
    { "Top-p", "keep the likeliest words up to this share", 0.05, 1.0, 0.05, false },
    { "Top-k", "keep this many likeliest words (0: off)", 0, 200, 5, true },
    { "Min-p", "drop words this much less likely than the best", 0.0, 0.5, 0.01, false },
    { "Repeat penalty", "discourage repeating words", 1.0, 1.5, 0.01, false },
    { "Edit temperature", "for reviewed edits", 0.0, 1.5, 0.05, false },
    { "Context per conversation", "tokens; the model restarts to change it", 8192, 32768, 8192, true },
};

const ModelSettingInfo *model_setting_info(int i) { return i >= 0 && i < MODEL_SETTING_COUNT ? &k_info[i] : NULL; }

void model_settings_defaults(ModelSettings *m)
{
    m->max_reply = 2048;
    m->temperature = 0.7;
    m->top_p = 0.95;
    m->top_k = 40;
    m->min_p = 0.05;
    m->repeat_penalty = 1.05;
    m->edit_temperature = 0.4;
    m->context = 32768;
}

static double clampd(double v, double lo, double hi) { return isnan(v) ? lo : v < lo ? lo : v > hi ? hi : v; }

void model_settings_clamp(ModelSettings *m)
{
    for (int i = 0; i < MODEL_SETTING_COUNT; i++) model_setting_set(m, i, model_setting_get(m, i));
    if (m->max_reply > m->context / 2) m->max_reply = m->context / 2; /* room for the conversation */
}

double model_setting_get(const ModelSettings *m, int i)
{
    switch (i) {
    case 0: return m->max_reply;
    case 1: return m->temperature;
    case 2: return m->top_p;
    case 3: return m->top_k;
    case 4: return m->min_p;
    case 5: return m->repeat_penalty;
    case 6: return m->edit_temperature;
    case 7: return m->context;
    default: return 0;
    }
}

void model_setting_set(ModelSettings *m, int i, double v)
{
    const ModelSettingInfo *in = model_setting_info(i);
    if (!in) return;
    v = clampd(v, in->min, in->max);
    /* onto the step grid (no 0.7000000001 creeping in from repeated steps) */
    v = in->min + round((v - in->min) / in->step) * in->step;
    v = clampd(v, in->min, in->max);
    if (!in->integer) v = round(v * 100.0) / 100.0; /* 0.7, never 0.7000000000000001 */
    switch (i) {
    case 0: m->max_reply = (int)lround(v); break;
    case 1: m->temperature = v; break;
    case 2: m->top_p = v; break;
    case 3: m->top_k = (int)lround(v); break;
    case 4: m->min_p = v; break;
    case 5: m->repeat_penalty = v; break;
    case 6: m->edit_temperature = v; break;
    case 7: m->context = (int)lround(v); break;
    }
}

static void path_of(char *out, size_t outlen)
{
    const char *env = getenv("SCRIBODO_MODEL_SETTINGS");
    if (env && env[0]) snprintf(out, outlen, "%s", env);
    else snprintf(out, outlen, "%s/Library/Application Support/Scribodo/model-settings.json", getenv("HOME") ? getenv("HOME") : "");
}

static const char *const k_keys[MODEL_SETTING_COUNT] = { "max_reply", "temperature", "top_p", "top_k",
                                                          "min_p", "repeat_penalty", "edit_temperature", "context" };

void model_settings_load(ModelSettings *m)
{
    model_settings_defaults(m);
    char path[1024];
    path_of(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *j = cJSON_Parse(buf);
    for (int i = 0; i < MODEL_SETTING_COUNT; i++) {
        const cJSON *v = cJSON_GetObjectItem(j, k_keys[i]);
        if (cJSON_IsNumber(v)) model_setting_set(m, i, v->valuedouble);
    }
    cJSON_Delete(j);
    model_settings_clamp(m);
}

bool model_settings_save(const ModelSettings *in)
{
    ModelSettings m = *in;
    model_settings_clamp(&m);
    char path[1024], tmp[1100], dir[1024];
    path_of(path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(dir, sizeof(dir), "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl) {
        *sl = '\0';
        mkdir(dir, 0700);
    }
    cJSON *j = cJSON_CreateObject();
    for (int i = 0; i < MODEL_SETTING_COUNT; i++) cJSON_AddNumberToObject(j, k_keys[i], model_setting_get(&m, i));
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return false;
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fputs(s, f) >= 0;
    if (f) {
        ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok;
        fclose(f);
    }
    free(s);
    ok = ok && rename(tmp, path) == 0;
    if (!ok) unlink(tmp);
    return ok;
}
