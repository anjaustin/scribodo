#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app/db.h"
#include "test_util.h"

static void test_shape_and_text(void)
{
    char *cells[] = { "name", NULL, "42", "-7" };
    DbRows rows = { .nrows = 2, .ncols = 2, .cells = cells };
    const char *value = "unchanged";

    CHECK(db_rows_has_columns(&rows, 2));
    CHECK(!db_rows_has_columns(&rows, 3));
    CHECK(db_cell_text(&rows, 0, 0, &value) && !strcmp(value, "name"));
    CHECK(db_cell_nullable(&rows, 0, 1, &value) && value == NULL);
    CHECK(!db_cell_text(&rows, 0, 1, &value));
    CHECK(!db_cell_nullable(&rows, 2, 0, &value));
    CHECK(!db_cell_nullable(&rows, 0, 2, &value));
    CHECK(!db_cell_nullable(NULL, 0, 0, &value));

    DbRows overflow = { .nrows = SIZE_MAX, .ncols = 2, .cells = malloc(sizeof(char *)) };
    CHECK(overflow.cells != NULL && !db_rows_has_columns(&overflow, 2));
    db_rows_free(&overflow);
    CHECK(overflow.cells == NULL && overflow.nrows == 0 && overflow.ncols == 0);
    DbRows missing = { .nrows = 1, .ncols = 1, .cells = NULL };
    CHECK(!db_rows_has_columns(&missing, 1));
    db_rows_free(&missing);
    CHECK(missing.nrows == 0 && missing.ncols == 0);
}

static void test_integers(void)
{
    char *cells[] = { "0", "-9223372036854775808", "9223372036854775807", "18446744073709551615",
                      "9223372036854775808", "18446744073709551616", "-1", "12x", "", NULL };
    DbRows rows = { .nrows = 1, .ncols = sizeof(cells) / sizeof(cells[0]), .cells = cells };
    int64_t signed_value = 99;
    uint64_t unsigned_value = 99;

    CHECK(db_cell_i64(&rows, 0, 0, &signed_value) && signed_value == 0);
    CHECK(db_cell_i64(&rows, 0, 1, &signed_value) && signed_value == INT64_MIN);
    CHECK(db_cell_i64(&rows, 0, 2, &signed_value) && signed_value == INT64_MAX);
    CHECK(db_cell_u64(&rows, 0, 3, &unsigned_value) && unsigned_value == UINT64_MAX);
    CHECK(!db_cell_i64(&rows, 0, 4, &signed_value));
    CHECK(!db_cell_u64(&rows, 0, 5, &unsigned_value));
    CHECK(!db_cell_u64(&rows, 0, 6, &unsigned_value));
    CHECK(!db_cell_i64(&rows, 0, 7, &signed_value));
    CHECK(!db_cell_i64(&rows, 0, 8, &signed_value));
    CHECK(!db_cell_i64(&rows, 0, 9, &signed_value));
}

static void test_floating_point(void)
{
    char *cells[] = { "0", "-12.5", "1e6", "nan", "inf", "1.2x", "1e9999", NULL };
    DbRows rows = { .nrows = 1, .ncols = sizeof(cells) / sizeof(cells[0]), .cells = cells };
    double value = 99;

    CHECK(db_cell_double(&rows, 0, 0, &value) && value == 0);
    CHECK(db_cell_double(&rows, 0, 1, &value) && value == -12.5);
    CHECK(db_cell_double(&rows, 0, 2, &value) && value == 1000000);
    CHECK(!db_cell_double(&rows, 0, 3, &value));
    CHECK(!db_cell_double(&rows, 0, 4, &value));
    CHECK(!db_cell_double(&rows, 0, 5, &value));
    CHECK(!db_cell_double(&rows, 0, 6, &value));
    CHECK(!db_cell_double(&rows, 0, 7, &value));
}

int main(void)
{
    const TestCase tests[] = {
        { "shape_and_text", test_shape_and_text },
        { "integers", test_integers },
        { "floating_point", test_floating_point },
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
