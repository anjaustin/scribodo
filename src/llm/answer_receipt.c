/* The answer receipt and trace writers; the format is the header's comment. */
#include "llm/answer_receipt.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Longest prefix of s, at most max bytes, that ends on a UTF-8 character boundary. */
static size_t utf8_prefix(const char *s, size_t max)
{
    size_t n = strlen(s);
    if (n <= max) return n;
    while (max && ((unsigned char)s[max] & 0xC0) == 0x80) max--;
    return max;
}

static size_t header_length(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static size_t header_length(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    return n < 0 ? 0 : (size_t)n;
}

static size_t field_copy(char *dst, size_t max, const char *src);

char *answer_receipt(const AnswerPipelineResult *r)
{
    if (!r) return NULL;
    const char *span = r->span ? r->span : "";
    const char *span2 = r->second_span ? r->second_span : "";
    /* Document names: "|" becomes a broken bar so no field holds the separator (the readers split on " | ";
     * answer_receipt.h promises it; red-team 2026-11-14). */
    char name1[300], name2[300];
    field_copy(name1, sizeof(name1) - 1, r->has_top ? answer_source_name(&r->top) : "");
    field_copy(name2, sizeof(name2) - 1, r->has_second ? answer_source_name(&r->second) : "");
    const char *fmt_top = "--- VERIFIED SOURCE | unit=%s | document=%s | score=%.4f | status=%s | part=%zu | candidates=%zu scored=%zu ---\n";
    const char *fmt_none = "--- VERIFIED SOURCE | none | status=%s | candidates=%zu scored=%zu ---\n";
    const char *fmt_second = "--- VERIFIED SOURCE 2 | unit=%s | document=%s | score=%.4f | status=%s | part=%zu ---\n";
    size_t fixed = r->has_top
        ? header_length(fmt_top, r->top.id, name1, r->top_score, r->status, r->top_part + 1, r->candidates, r->scored) + 1
        : header_length(fmt_none, r->status, r->candidates, r->scored);
    if (r->has_second)
        fixed += header_length(fmt_second, r->second.id, name2, r->second_score, r->second_status, r->second_part + 1) + 1;
    /* The headers are always complete; the span lines share what is left of the bound, first span first. */
    size_t room = fixed < ANSWER_RECEIPT_PIPELINE_MAX ? ANSWER_RECEIPT_PIPELINE_MAX - fixed : 0;
    size_t len1 = r->has_top ? utf8_prefix(span, room) : 0;
    size_t len2 = r->has_second ? utf8_prefix(span2, room - len1) : 0;
    size_t cap = fixed + len1 + len2 + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t k = 0;
    if (r->has_top) {
        k += (size_t)snprintf(out + k, cap - k, fmt_top, r->top.id, name1, r->top_score, r->status,
                              r->top_part + 1, r->candidates, r->scored);
        memcpy(out + k, span, len1); k += len1;
        out[k++] = '\n';
    } else {
        k += (size_t)snprintf(out + k, cap - k, fmt_none, r->status, r->candidates, r->scored);
    }
    if (r->has_second) {
        k += (size_t)snprintf(out + k, cap - k, fmt_second, r->second.id, name2, r->second_score,
                              r->second_status, r->second_part + 1);
        memcpy(out + k, span2, len2); k += len2;
        out[k++] = '\n';
    }
    out[k] = '\0';
    return out;
}

/* A free-text field: newlines become spaces, cut to max bytes on a UTF-8 boundary. Returns the length written. */
static size_t field_copy(char *dst, size_t max, const char *src)
{
    if (!src) { dst[0] = '\0'; return 0; }
    size_t n = utf8_prefix(src, max);
    for (size_t i = 0; i < n; i++) dst[i] = src[i] == '\n' || src[i] == '\r' ? ' ' : src[i] == '|' ? '/' : src[i];
    dst[n] = '\0';
    return n;
}

bool answer_receipt_append_gate(char **memory_sources, const char *verdict, bool withheld, const char *answer,
                                size_t quotations, const char *misquoted, size_t relations, const char *unsupported)
{
    if (!memory_sources) return false;
    static const char fmt_gate[] = "--- ANSWER GATE | verdict=%s | withheld=%d | answer=%s ---\n";
    static const char fmt_quotes[] = "--- ANSWER QUOTES | checked=%zu | misquoted=%s ---\n";
    static const char fmt_relations[] = "--- ANSWER RELATIONS | checked=%zu | unsupported=%s ---\n";
    const char *v = verdict ? verdict : "unavailable";
    char a[ANSWER_RECEIPT_FIELD_MAX + 1], m[ANSWER_RECEIPT_FIELD_MAX + 1], u[ANSWER_RECEIPT_FIELD_MAX + 1];
    field_copy(a, ANSWER_RECEIPT_FIELD_MAX, answer);
    field_copy(m, ANSWER_RECEIPT_FIELD_MAX, misquoted);
    field_copy(u, ANSWER_RECEIPT_FIELD_MAX, unsupported);
    size_t have = *memory_sources ? strlen(*memory_sources) : 0;
    size_t room = have < CHAT_MEMORY_SOURCES_BYTES ? CHAT_MEMORY_SOURCES_BYTES - have : 0;
#define GATE_NEED() (header_length(fmt_gate, v, withheld, a) + header_length(fmt_quotes, quotations, m) + header_length(fmt_relations, relations, u))
    size_t need = GATE_NEED();
    /* Cut the claim first, the misquotation second, the relation third, each down to empty. */
    char *fields[3] = { a, m, u };
    for (int f = 0; f < 3 && need > room; f++) {
        size_t len = strlen(fields[f]);
        if (!len) continue;
        size_t cut = need - room < len ? len - (need - room) : 0;
        fields[f][utf8_prefix(fields[f], cut)] = '\0';
        need = GATE_NEED();
    }
#undef GATE_NEED
    if (need > room) return false;
    char *grown = realloc(*memory_sources, have + need + 1);
    if (!grown) return false;
    size_t w = (size_t)snprintf(grown + have, need + 1, fmt_gate, v, withheld, a);
    w += (size_t)snprintf(grown + have + w, need + 1 - w, fmt_quotes, quotations, m);
    snprintf(grown + have + w, need + 1 - w, fmt_relations, relations, u);
    *memory_sources = grown;
    return true;
}

void answer_trace_pipeline(FILE *out, const AnswerPipelineResult *r, size_t request_bytes, size_t history_first)
{
    if (!out || !r) return;
    fprintf(out, "ANSWER_PIPELINE candidates=%zu hydrated=%zu scored=%zu failures=%zu top=%s score=%.4f status=%s second=%s second_score=%.4f second_status=%s parts=%zu top_part=%zu second_part=%zu rerank_ms=%.1f extract_ms=%.1f request_bytes=%zu history_first=%zu\n",
            r->candidates, r->hydrated, r->scored, r->score_failures,
            r->has_top ? r->top.id : "-", r->top_score, r->status,
            r->has_second ? r->second.id : "-", r->second_score, r->has_second ? r->second_status : "-",
            r->nparts, r->top_part + 1, r->has_second ? r->second_part + 1 : 0,
            r->rerank_ms, r->extract_ms, request_bytes, history_first);
    for (size_t p = 0; p < r->nparts; p++) {
        fprintf(out, "ANSWER_PIPELINE_SCORES part=%zu", p + 1);
        for (size_t i = 0; i < r->scored && i < ANSWER_SCORED_MAX; i++)
            if (r->scored_part[i] == p) fprintf(out, " %s:%.4f", r->scored_ids[i], r->scored_scores[i]);
        fprintf(out, "\n");
    }
}

void answer_trace_gate(FILE *out, const char *verdict, bool withheld, bool parsed, size_t spans, size_t author_turns,
                       double ms, size_t answer_bytes, size_t quotations, bool misquoted, size_t relations, bool unsupported)
{
    if (!out) return;
    fprintf(out, "ANSWER_GATE verdict=%s withheld=%d parsed=%d spans=%zu author_turns=%zu ms=%.1f answer_bytes=%zu quotations=%zu misquoted=%d relations=%zu unsupported=%d\n",
            verdict ? verdict : "unavailable", withheld, parsed, spans, author_turns, ms, answer_bytes, quotations, misquoted, relations, unsupported);
}
