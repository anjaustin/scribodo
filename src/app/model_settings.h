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
} ModelSettingInfo;

#define MODEL_SETTING_COUNT 8
const ModelSettingInfo *model_setting_info(int i);
double model_setting_get(const ModelSettings *m, int i);
void model_setting_set(ModelSettings *m, int i, double v); /* clamped */

#endif
