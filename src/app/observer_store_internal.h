#ifndef SCRIBODO_OBSERVER_STORE_INTERNAL_H
#define SCRIBODO_OBSERVER_STORE_INTERNAL_H

#include <stdbool.h>

#include "app/db.h"

typedef struct cJSON cJSON;

cJSON *observer_state_load_files(const char *project_dir);
bool observer_state_import_files(const char *project_dir, cJSON **state);
bool observer_state_save_files(const char *project_dir, cJSON *state);
bool observer_state_export_files(const char *project_dir, cJSON *state);
cJSON *observer_state_load_db(Db *db, const char *project_id);
bool observer_state_save_db(Db *db, const char *project_id, cJSON *state);
bool observer_state_import_version_db(Db *db, const char *project_id, cJSON *state);

bool observer_rules_get_files(const char *project_dir, char out[17]);
bool observer_rules_import_files(const char *project_dir, char out[17]);
bool observer_rules_set_files(const char *project_dir, const char *signature);
bool observer_rules_get_db(Db *db, const char *project_id, char out[17]);
bool observer_rules_set_db(Db *db, const char *project_id, const char *signature);

#endif
