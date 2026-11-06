#ifndef SCRIBODO_TEST_DB_INTERNAL_H
#define SCRIBODO_TEST_DB_INTERNAL_H

#include <stdbool.h>

#include "app/store.h"

extern char g_name[64];
extern char g_name2[64];
extern StoreManager *g_store;

bool admin_name(const char *sql_format, const char *name);
bool admin(const char *sql_format);
int run_db_core_tests(void);

#endif
