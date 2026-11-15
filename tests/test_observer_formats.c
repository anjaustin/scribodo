/* The observer's C-to-Python formats from the writer's side (llm/observer_formats.h, AUDIT-004 F7).
 *
 * Every kind of judge-log line and finding line is written here by the product's own formatters and compared byte for
 * byte with tests/fixtures/observer_formats/; tests/test_observer_formats.py reads the same files with
 * tools/observer_judge_score.py and tools/observer_lines.py. SCRIBODO_WRITE_FIXTURES=1 rewrites the files (run from the
 * repository root) after a deliberate format change, with its version bumped. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "llm/observer.h"
#include "llm/observer_formats.h"
#include "test_util.h"

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    size_t cap = 0, n = 0, got;
    do {
        if (n + 4096 + 1 > cap) {
            cap = cap ? cap * 2 : 16384;
            char *grown = realloc(buf, cap);
            if (!grown) { free(buf); fclose(f); return NULL; }
            buf = grown;
        }
        got = fread(buf + n, 1, 4096, f);
        n += got;
    } while (got > 0);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static void fixture_matches(const char *name, const char *text)
{
    char path[256];
    snprintf(path, sizeof(path), "tests/fixtures/observer_formats/%s", name);
    if (getenv("SCRIBODO_WRITE_FIXTURES")) {
        FILE *f = fopen(path, "wb");
        CHECK(f && fputs(text, f) >= 0 && !fclose(f));
        return;
    }
    char *expected = read_file(path);
    if (!expected) fprintf(stderr, "missing %s (run from the repository root)\n", path);
    CHECK(expected != NULL);
    if (expected && strcmp(expected, text))
        fprintf(stderr, "%s differs from the writer:\n--- fixture ---\n%s--- writer ---\n%s", path, expected, text);
    CHECK(expected && !strcmp(expected, text));
    free(expected);
}

/* Appends line and a newline to a growing buffer. */
static void append(char **buf, size_t *n, const char *line)
{
    size_t l = strlen(line);
    char *grown = realloc(*buf, *n + l + 2);
    CHECK(grown != NULL);
    if (!grown) return;
    *buf = grown;
    memcpy(*buf + *n, line, l);
    (*buf)[*n + l] = '\n';
    *n += l + 1;
    (*buf)[*n] = '\0';
}

static void test_judge_log(void)
{
    const char *canon = "Sola's eyes are green.", *sentence = "\xE2\x80\x9CSola's eyes,\xE2\x80\x9D he said, \xE2\x80\x9C" "are brown.\xE2\x80\x9D";
    ObserverJudgeLogEntry e[3] = {
        { .is_doc = true, .name = "Kapitel \xC3\xB8.md", .message = 0, .start = 54, .len = 21, .type = "event",
          .canon_id = "c-20261115-101500-0001", .canon = "Zo\xC3\xAB's eyes are green.",
          .statement = "Zo\xC3\xAB's eyes are brown. (\xE2\x80\x9CZo\xC3\xAB's eyes are brown.\xE2\x80\x9D)",
          .replies = { "{\"check\":\"green vs brown\",\"verdict\":\"conflict\"}", "{\"check\":\"brown\",\"verdict\":\"conflict\"}" },
          .conflict = true },
        { .is_doc = true, .name = "ch1.md", .message = 0, .start = 0, .len = 38, .type = "sentence", .sentence = true,
          .attributes = obs_attributes_differ(canon, sentence), .subject = obs_same_subject(canon, sentence),
          .canon_id = "c-20261115-101500-0002", .canon = canon, .statement = sentence,
          .replies = { "{\"check\":\"no\",\"verdict\":\"consistent\"}", NULL }, .conflict = false },
        { .is_doc = false, .name = "20261112-120000-ab12", .message = 3, .start = 7, .len = 12, .type = "relationship",
          .canon_id = "c-20261115-101500-0003", .canon = "Tars Tarkas is Sola's father.",
          .statement = "Sola's father is \"Lorquas\"\\Ptomel", .replies = { "{\"check\":\"line one\\nline two\",\"verdict\":\"unrelated\"}", NULL },
          .conflict = false },
    };
    CHECK(e[1].attributes && e[1].subject); /* the sentence case carries both co-signers' answers as true */
    char *buf = NULL;
    size_t n = 0;
    for (int i = 0; i < 3; i++) {
        char *line = observer_judge_log_line(&e[i]);
        CHECK(line && !strchr(line, '\n') && !strncmp(line, "{\"version\":1,", 13));
        if (line) append(&buf, &n, line);
        free(line);
    }
    if (buf) fixture_matches("judge-log.jsonl", buf);
    free(buf);
}

static void test_finding_lines(void)
{
    char marker[64];
    snprintf(marker, sizeof(marker), "# observer-findings format %d", OBSERVER_FINDING_LINES_FORMAT);
    CHECK(!strcmp(marker, OBSERVER_FINDING_LINES_MARKER));
    SourceRef doc = { .kind = SRC_DOC, .doc = "part|07.md", .start = 120, .len = 31,
                      .quote = "Her eyes, he said,\nwere \"green\" | not brown." };
    SourceRef chat = { .kind = SRC_CHAT, .chat = "20261112-120000-ab12", .message = 2, .start = 0, .len = 14,
                       .quote = "Ingrid\tmet Dvo\xC5\x99\xC3\xA1k" };
    RecordVersion v[3] = {
        { .status = "new", .content = "Canon says green;\r\nthe text says brown \\ grey.", .sources = &doc, .nsources = 1,
          .finding = { .type = "conflict", .subject = "Zo\xC3\xAB", .related_text = "Zo\xC3\xAB's eyes are green." } },
        { .status = "saved", .content = "Dvo\xC5\x99\xC3\xA1k is mentioned.", .sources = &chat, .nsources = 1,
          .finding = { .type = "entity", .subject = "Dvo\xC5\x99\xC3\xA1k", .relation = "", .object = "" } },
        { .status = "dismissed", .content = "No source at all.", .nsources = 0,
          .finding = { .type = "relationship", .subject = "Sola", .relation = "daughter of", .object = "Tars Tarkas" } },
    };
    char *buf = NULL, *json = NULL;
    size_t n = 0;
    cJSON *rows = cJSON_CreateArray();
    for (int i = 0; i < 3; i++) {
        char *line = observer_finding_line(&v[i]);
        CHECK(line && !strchr(line, '\n') && !strchr(line, '\r') && !strchr(line, '\t'));
        if (line) append(&buf, &n, line);
        free(line);
        /* What a reader must get back: the twelve fields unescaped, start and len as numbers. */
        const SourceRef *s = v[i].nsources ? &v[i].sources[0] : NULL;
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "type", v[i].finding.type);
        cJSON_AddStringToObject(r, "subject", v[i].finding.subject);
        cJSON_AddStringToObject(r, "relation", v[i].finding.relation);
        cJSON_AddStringToObject(r, "object", v[i].finding.object);
        cJSON_AddStringToObject(r, "status", v[i].status);
        cJSON_AddStringToObject(r, "content", v[i].content);
        cJSON_AddStringToObject(r, "kind", !s ? "" : s->kind == SRC_DOC ? "doc" : "chat");
        cJSON_AddStringToObject(r, "doc", !s ? "" : s->kind == SRC_DOC ? s->doc : s->chat);
        cJSON_AddNumberToObject(r, "start", s ? (double)s->start : 0);
        cJSON_AddNumberToObject(r, "len", s ? (double)s->len : 0);
        cJSON_AddStringToObject(r, "quote", s && s->quote ? s->quote : "");
        cJSON_AddStringToObject(r, "related", v[i].finding.related_text ? v[i].finding.related_text : "");
        cJSON_AddItemToArray(rows, r);
    }
    json = cJSON_Print(rows);
    cJSON_Delete(rows);
    if (buf) fixture_matches("findings.txt", buf);
    char err[256];
    snprintf(err, sizeof(err), "%s\n# 3 findings, 1 jobs, status: Up to date, 0 s\n", OBSERVER_FINDING_LINES_MARKER);
    fixture_matches("findings.err", err);
    if (json) {
        size_t jl = strlen(json);
        char *withnl = malloc(jl + 2);
        if (withnl) { memcpy(withnl, json, jl); withnl[jl] = '\n'; withnl[jl + 1] = '\0'; fixture_matches("findings.json", withnl); }
        free(withnl);
    }
    free(json);
    free(buf);
}

int main(void)
{
    const TestCase tests[] = {
        { "judge_log", test_judge_log },
        { "finding_lines", test_finding_lines },
    };
    return run_tests(tests, sizeof(tests) / sizeof(tests[0]));
}
