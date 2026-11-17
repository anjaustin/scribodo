/* The chat model's settings, chosen by the author (user testing
 * 2026-11-04): sampling, reply length and the context per conversation.
 * App-wide: ~/Library/Application Support/Scribodo/model-settings.json (or
 * $SCRIBODO_MODEL_SETTINGS). Every value is clamped to a safe range on load
 * and when changed. The observer is not affected: it always samples at
 * temperature 0 so its findings are reproducible. */
#ifndef SCRIBODO_MODEL_SETTINGS_H
#define SCRIBODO_MODEL_SETTINGS_H

#include <stdbool.h>

typedef struct {
    int max_reply;          /* tokens per reply */
    double temperature;     /* chat */
    double top_p;
    int top_k;              /* 0 = off */
    double min_p;
    double repeat_penalty;
    double edit_temperature; /* reviewed edits (lower: closer to the text) */
    int context;            /* tokens per conversation (the model restarts to change it) */
    int keep_in_memory;     /* 1: the chat model's weights are pinned in RAM, never swapped (the model restarts) */
} ModelSettings;

void model_settings_defaults(ModelSettings *m);
/* Missing or unreadable: the defaults. */
void model_settings_load(ModelSettings *m);
bool model_settings_save(const ModelSettings *m);
/* Into range (and max_reply at most half the context). */
void model_settings_clamp(ModelSettings *m);

/* One adjustable setting, for the panel: its label, range and step. */
typedef struct {
    const char *label, *hint;
    double min, max, step;
    bool integer;
    bool toggle; /* 0 or 1, shown as Off and On */
} ModelSettingInfo;

#define MODEL_SETTING_COUNT 9
/* The largest context per conversation the panel offers: the model's trained context (llm_model_context_length),
 * rounded down to the step. Call before model_settings_load, so a saved setting above the old ceiling survives.
 * Ignored when `tokens` is 0 (unknown). */
void model_settings_set_context_limit(int tokens);
const ModelSettingInfo *model_setting_info(int i);
double model_setting_get(const ModelSettings *m, int i);
void model_setting_set(ModelSettings *m, int i, double v); /* clamped */

#endif
