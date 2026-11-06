#ifndef SCRIBODO_TEST_STATE_FIXTURE_H
#define SCRIBODO_TEST_STATE_FIXTURE_H

#include <stdbool.h>
#include <stddef.h>

#include "app/store.h"

bool test_state_fixture_init(char *directory, size_t length, StoreManager **store);
void test_state_fixture_destroy(const char *directory, StoreManager *store);

#endif
