/* The index-text experiment's table (app/memory_index_text.h): off by default, loaded from a JSON file of
 * {sha256(original text): resolved text}, resolves by hash, falls back to the text itself, and refuses malformed
 * files without touching a loaded table. No database, no model. */
#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/memory_index_text.h"
#include "core/experiments.h"
#include "test_util.h"

static char g_path[256];

static void write_file(const char *content)
{
    FILE *f = fopen(g_path, "wb");
    if (f) {
        fwrite(content, 1, strlen(content), f);
        fclose(f);
    }
}

static void hex_of(const char *text, char out[65])
{
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(text, (CC_LONG)strlen(text), digest);
    for (size_t i = 0; i < sizeof(digest); i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
}

static const char *ORIGINAL = "They were rather handsome; had a fortune of twenty thousand pounds.";
static const char *RESOLVED = "Bingley’s sisters were rather handsome; had a fortune of twenty thousand pounds.";

static void test_off_by_default(void)
{
    unsetenv("SCRIBODO_INDEX_TEXT_FILE");
    experiments_reload();
    memory_index_text_disable();
    CHECK(!memory_index_text_enabled());
    CHECK(memory_index_text_for(ORIGINAL) == NULL);
    CHECK(!strcmp(memory_index_text_match_column(), "u.text"));
}

static void test_loads_and_resolves_by_hash(void)
{
    char hash[65], body[512], err[256];
    hex_of(ORIGINAL, hash);
    snprintf(body, sizeof(body), "{\"%s\": \"%s\"}", hash, RESOLVED);
    write_file(body);
    CHECK(memory_index_text_load(g_path, err, sizeof(err)));
    CHECK(memory_index_text_enabled());
    CHECK(!strcmp(memory_index_text_match_column(), "u.index_text"));
    const char *resolved = memory_index_text_for(ORIGINAL);
    CHECK(resolved && !strcmp(resolved, RESOLVED));
    const char *other = "Mr. Bingley inherited property.";
    CHECK(memory_index_text_for(other) == other); /* no resolution: the text itself is the index text */
    CHECK(memory_index_text_for(NULL) == NULL);
    memory_index_text_disable();
    CHECK(!memory_index_text_enabled());
    CHECK(memory_index_text_for(ORIGINAL) == NULL);
}

static void test_rejects_malformed_files_and_keeps_the_loaded_table(void)
{
    char hash[65], body[9000], err[256], zeros[65];
    hex_of(ORIGINAL, hash);
    memset(zeros, '0', 64);
    zeros[64] = '\0';
    snprintf(body, sizeof(body), "{\"%s\": \"%s\"}", hash, RESOLVED);
    write_file(body);
    CHECK(memory_index_text_load(g_path, err, sizeof(err)));

    write_file("[1, 2]");
    CHECK(!memory_index_text_load(g_path, err, sizeof(err)) && strstr(err, "not a JSON object"));
    write_file("{\"abc\": \"x\"}");
    CHECK(!memory_index_text_load(g_path, err, sizeof(err)) && strstr(err, "sha256"));
    snprintf(body, sizeof(body), "{\"%s\": 1}", zeros);
    write_file(body);
    CHECK(!memory_index_text_load(g_path, err, sizeof(err)) && strstr(err, "strict UTF-8"));
    snprintf(body, sizeof(body), "{\"%s\": \"\"}", zeros);
    write_file(body);
    CHECK(!memory_index_text_load(g_path, err, sizeof(err)));
    snprintf(body, sizeof(body), "{\"%s\": \"\xff\xfe\"}", zeros);
    write_file(body);
    CHECK(!memory_index_text_load(g_path, err, sizeof(err)));
    size_t at = (size_t)snprintf(body, sizeof(body), "{\"%s\": \"", zeros);
    memset(body + at, 'a', MEMORY_INDEX_TEXT_BYTES + 1);
    at += MEMORY_INDEX_TEXT_BYTES + 1;
    memcpy(body + at, "\"}", 3);
    write_file(body);
    CHECK(!memory_index_text_load(g_path, err, sizeof(err)));
    snprintf(body, sizeof(body), "{\"%s\": \"a\", \"%s\": \"b\"}", zeros, zeros);
    write_file(body);
    CHECK(!memory_index_text_load(g_path, err, sizeof(err)) && strstr(err, "duplicate"));
    unlink(g_path);
    CHECK(!memory_index_text_load(g_path, err, sizeof(err)) && strstr(err, "cannot open"));

    /* Every refusal left the earlier table in place. */
    CHECK(memory_index_text_enabled());
    const char *resolved = memory_index_text_for(ORIGINAL);
    CHECK(resolved && !strcmp(resolved, RESOLVED));
    memory_index_text_disable();

    write_file("{}");
    CHECK(memory_index_text_load(g_path, err, sizeof(err)));
    CHECK(memory_index_text_enabled());
    CHECK(memory_index_text_for(ORIGINAL) == ORIGINAL); /* enabled with no resolutions: every unit indexes its own text */
    memory_index_text_disable();
}

static void test_valid_text_bounds(void)
{
    size_t bytes = 0;
    CHECK(!memory_index_text_valid(NULL, &bytes));
    CHECK(!memory_index_text_valid("", &bytes));
    CHECK(memory_index_text_valid("a", &bytes) && bytes == 1);
    CHECK(memory_index_text_valid(RESOLVED, &bytes) && bytes == strlen(RESOLVED));
    CHECK(!memory_index_text_valid("\xff", &bytes));
    char *big = malloc(MEMORY_INDEX_TEXT_BYTES + 2);
    memset(big, 'a', MEMORY_INDEX_TEXT_BYTES + 1);
    big[MEMORY_INDEX_TEXT_BYTES] = '\0';
    CHECK(memory_index_text_valid(big, &bytes) && bytes == MEMORY_INDEX_TEXT_BYTES);
    big[MEMORY_INDEX_TEXT_BYTES] = 'a';
    big[MEMORY_INDEX_TEXT_BYTES + 1] = '\0';
    CHECK(!memory_index_text_valid(big, &bytes));
    free(big);
}

int main(void)
{
    snprintf(g_path, sizeof(g_path), "/tmp/scribodo_index_text_%ld.json", (long)getpid());
    const TestCase tests[] = {
        { "off_by_default", test_off_by_default },
        { "loads_and_resolves_by_hash", test_loads_and_resolves_by_hash },
        { "rejects_malformed_files_and_keeps_the_loaded_table", test_rejects_malformed_files_and_keeps_the_loaded_table },
        { "valid_text_bounds", test_valid_text_bounds },
    };
    int rc = run_tests(tests, sizeof(tests) / sizeof(tests[0]));
    unlink(g_path);
    return rc;
}
