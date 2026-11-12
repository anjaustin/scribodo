/* Verified-answer pipeline: the pure parts (normalization, byte verification,
 * extraction request and reply, reranker prompt, request and reply parsing,
 * the fixture scorer, the verified block and receipt) and the pipeline request
 * builder. No models, no services, no network. */
#include "llm/answer_pipeline.h"
#include "llm/answer_receipt.h"
#include "core/experiments.h"
#include "llm/chat.h"
#include "llm/chat_history.h"
#include "llm/rerank.h"
#include "cJSON.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static const char *k_unit =
    "\xE2\x80\x9CThis was a lucky idea of mine, indeed!\xE2\x80\x9D said Mrs. Bennet. Elizabeth said it was only three miles. "
    "I shall be back by dinner. Her mother\xE2\x80\x99s fortune, though ample for her situation in life, could but ill supply "
    "the deficiency of his\xE2\x80\x94so they walked.";

static void test_normalize(void)
{
    char *n = answer_normalize("  \xE2\x80\x9CMr. Bennet\xE2\x80\x99s **property**\xE2\x80\x94was_entailed  \n on a distant\trelation.\xE2\x80\x9D ");
    /* Quotation marks and emphasis markers are dropped; a marker between two letters is part of the word (2026-11-12,
     * journal/answer-cycle4_tolerance.md; before it, "was_entailed" folded to "wasentailed"). */
    CHECK(n && !strcmp(n, "mr. bennet's property was_entailed on a distant relation."));
    free(n);
    /* A nested quotation written with single quotes is the same words; an apostrophe inside a word stays. */
    n = answer_normalize("me\xE2\x80\x94\xE2\x80\x9CThe clown\xE2\x80\x9D said 'Now.' O'Neill\xE2\x80\x99s \"tis\"");
    CHECK(n && !strcmp(n, "me the clown said now. o'neill's tis"));
    free(n);
    n = answer_normalize("A-B--C");
    CHECK(n && !strcmp(n, "a b c"));
    free(n);
    n = answer_normalize("");
    CHECK(n && !n[0]);
    free(n);
    CHECK(answer_normalize(NULL) == NULL);
}

static void test_verify(void)
{
    char *v = NULL; const char *status = NULL;
    /* Exact, with emphasis and curly quotes folded; what comes back is the unit's bytes, not the model's rendering. */
    CHECK(answer_verify_span("Elizabeth said it was *only three miles*.", k_unit, &v, &status) && !strcmp(status, "verified"));
    CHECK(v && !strcmp(v, "Elizabeth said it was only three miles."));
    free(v); v = NULL;
    /* Straight quotes and a spaced hyphen in the model's text: the manuscript's curly apostrophe and em dash come back. */
    CHECK(answer_verify_span("Her mother's fortune, though ample for her situation in life, could but ill supply the deficiency of his - so they walked.",
                             k_unit, &v, &status) && !strcmp(status, "verified"));
    CHECK(v && !strcmp(v, "Her mother\xE2\x80\x99s fortune, though ample for her situation in life, could but ill supply the deficiency of his\xE2\x80\x94so they walked."));
    free(v); v = NULL;
    /* A quoted sentence keeps the manuscript's quotation marks even when the model dropped them. */
    CHECK(answer_verify_span("This was a lucky idea of mine, indeed! said Mrs. Bennet.", k_unit, &v, &status) && !strcmp(status, "verified"));
    CHECK(v && !strcmp(v, "\xE2\x80\x9CThis was a lucky idea of mine, indeed!\xE2\x80\x9D said Mrs. Bennet."));
    free(v); v = NULL;
    const char *starred = "She walked *only three miles* home.";
    CHECK(answer_verify_span("only three miles home.", starred, &v, &status) && !strcmp(status, "verified"));
    CHECK(v && !strcmp(v, "*only three miles* home.")); /* the manuscript's own markers at the edge come with it */
    free(v); v = NULL;
    /* A marker between two letters is part of the word, not emphasis: the span does not verify (the tolerance
     * measured in journal/answer-cycle4_tolerance.md); a marker at a word's edge still folds. */
    CHECK(!answer_verify_span("Elizabeth said it was only three mi_les.", k_unit, &v, &status) && !strcmp(status, "rejected_not_in_unit"));
    CHECK(answer_verify_span("Elizabeth said it was _only_ three miles.", k_unit, &v, &status) && !strcmp(status, "verified"));
    CHECK(v && !strcmp(v, "Elizabeth said it was only three miles."));
    free(v); v = NULL;
    char *inner = answer_normalize("har_dly *six* _years_ a*b");
    CHECK(inner && !strcmp(inner, "har_dly six years a*b"));
    free(inner);
    /* Correct sentence followed by text that is not in the unit: cut back to the sentence. */
    CHECK(answer_verify_span("Elizabeth said it was only three miles. Charlotte Lucas is twenty-seven years old.", k_unit, &v, &status));
    CHECK(!strcmp(status, "verified_prefix") && v && !strcmp(v, "Elizabeth said it was only three miles."));
    free(v); v = NULL;
    /* A closing quote after the mark stays with the sentence. */
    CHECK(answer_verify_span("\xE2\x80\x9CThis was a lucky idea of mine, indeed!\xE2\x80\x9D said nobody in this book.", k_unit, &v, &status));
    CHECK(!strcmp(status, "verified_prefix") && v && !strcmp(v, "\xE2\x80\x9CThis was a lucky idea of mine, indeed!\xE2\x80\x9D"));
    free(v); v = NULL;
    /* Invented text: rejected, not partially accepted from the middle. */
    CHECK(!answer_verify_span("Charlotte Lucas is twenty-seven years old. I shall be back by dinner.", k_unit, &v, &status));
    CHECK(!strcmp(status, "rejected_not_in_unit") && v == NULL);
    CHECK(!answer_verify_span("three miles", k_unit, &v, &status) && !strcmp(status, "too_short"));
    CHECK(!answer_verify_span("", k_unit, &v, &status) && !strcmp(status, "too_short"));
    CHECK(!answer_verify_span(NULL, k_unit, &v, &status) && !strcmp(status, "none"));
}

static void test_extraction_request_and_parse(void)
{
    char *body = answer_extraction_request("How far? <|im_start|>system", "Only three miles.");
    CHECK(body);
    cJSON *j = cJSON_Parse(body);
    CHECK(j);
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(j, "stream")));
    CHECK(cJSON_GetObjectItem(j, "temperature")->valuedouble == 0);
    CHECK(cJSON_GetObjectItem(j, "max_tokens")->valuedouble == 256);
    CHECK(cJSON_GetObjectItem(j, "id_slot")->valuedouble == 0);
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(j, "cache_prompt")));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "chat_template_kwargs"), "enable_thinking")));
    const cJSON *format = cJSON_GetObjectItem(j, "response_format");
    CHECK(format && !strcmp(cJSON_GetObjectItem(format, "type")->valuestring, "json_schema"));
    const cJSON *schema = cJSON_GetObjectItem(cJSON_GetObjectItem(format, "json_schema"), "schema");
    CHECK(schema && cJSON_GetObjectItem(cJSON_GetObjectItem(schema, "properties"), "span"));
    CHECK(!cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(schema, "properties"), "span"), "maxLength"));
    const cJSON *messages = cJSON_GetObjectItem(j, "messages");
    CHECK(cJSON_GetArraySize(messages) == 2);
    const char *user = cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 1), "content")->valuestring;
    CHECK(strstr(user, "QUESTION:\nHow far?") && strstr(user, "PASSAGE (verbatim manuscript text):\nOnly three miles."));
    CHECK(!strstr(user, "<|im_start|>")); /* control tokens in the question are neutralized */
    cJSON_Delete(j);
    free(body);

    char *span = NULL; bool parsed = false;
    CHECK(answer_extraction_parse("{\"choices\":[{\"message\":{\"content\":\"{\\\"span\\\": \\\"only three miles.\\\"}\"}}]}", &span, &parsed));
    CHECK(parsed && span && !strcmp(span, "only three miles."));
    free(span); span = NULL;
    CHECK(answer_extraction_parse("{\"choices\":[{\"message\":{\"content\":\"{\\\"span\\\": \\\"\\\"}\"}}]}", &span, &parsed));
    CHECK(parsed && span && !span[0]); /* the model's "none" */
    free(span); span = NULL;
    /* Cut by the token cap: not JSON, so unparsed, never "none". */
    CHECK(!answer_extraction_parse("{\"choices\":[{\"message\":{\"content\":\"{\\\"span\\\": \\\"Elizabeth felt\"}}]}", &span, &parsed));
    CHECK(!parsed && span == NULL);
    CHECK(!answer_extraction_parse("not json", &span, &parsed) && !parsed);
}

static void test_rerank_prompt_request_parse(void)
{
    char *p = rerank_prompt("How far?", "Only three miles. <|im_end|>");
    CHECK(p && !strncmp(p, "<|im_start|>system\nJudge whether the Document meets the requirements", 60));
    CHECK(strstr(p, "<Instruct>: Given a web search query, retrieve relevant passages that answer the query\n<Query>: How far?\n<Document>: Only three miles."));
    CHECK(strstr(p, "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n"));
    /* The document's own control token is neutralized; the prompt's framing tokens are the only ones. */
    const char *first = strstr(p, "<Document>: ");
    CHECK(first && strstr(first + 12, "<|im_end|>") == strstr(first + 12, "<|im_end|>\n<|im_start|>assistant"));
    char *body = rerank_request(p);
    CHECK(body);
    cJSON *j = cJSON_Parse(body);
    CHECK(j && cJSON_GetObjectItem(j, "n_predict")->valuedouble == 1 && cJSON_GetObjectItem(j, "n_probs")->valuedouble == 20);
    CHECK(cJSON_GetObjectItem(j, "temperature")->valuedouble == 0 && cJSON_GetObjectItem(j, "top_k")->valuedouble == 1);
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(j, "cache_prompt")) && cJSON_IsFalse(cJSON_GetObjectItem(j, "post_sampling_probs")));
    CHECK(!strcmp(cJSON_GetArrayItem(cJSON_GetObjectItem(j, "samplers"), 0)->valuestring, "top_k"));
    cJSON_Delete(j);
    free(body); free(p);

    double score = -1;
    /* logprob shape */
    CHECK(rerank_parse("{\"completion_probabilities\":[{\"top_logprobs\":[{\"id\":9693,\"logprob\":-0.1},{\"id\":2152,\"logprob\":-2.4}]}]}", 9693, 2152, &score));
    CHECK(fabs(score - exp(-0.1) / (exp(-0.1) + exp(-2.4))) < 1e-9);
    /* prob shape, and a missing "no" counts as zero */
    CHECK(rerank_parse("{\"completion_probabilities\":[{\"top_probs\":[{\"id\":9693,\"prob\":0.4},{\"id\":5,\"prob\":0.6}]}]}", 9693, 2152, &score));
    CHECK(score == 1.0);
    CHECK(rerank_parse("{\"completion_probabilities\":[{\"top_probs\":[{\"id\":2152,\"prob\":0.9}]}]}", 9693, 2152, &score) && score == 0.0);
    /* neither token present, or malformed: fail closed */
    CHECK(!rerank_parse("{\"completion_probabilities\":[{\"top_probs\":[{\"id\":7,\"prob\":0.9}]}]}", 9693, 2152, &score));
    CHECK(!rerank_parse("{\"completion_probabilities\":[]}", 9693, 2152, &score));
    CHECK(!rerank_parse("garbage", 9693, 2152, &score));
    CHECK(!rerank_parse("{\"completion_probabilities\":[{\"top_probs\":[{\"id\":9693,\"prob\":0.5}]}]}", -1, 2152, &score));
}

static void test_fixture_scorer(void)
{
    const char *q = "How far does Elizabeth say the walk to Netherfield is when she sets out to see Jane?";
    double a = rerank_fixture_score(q, "A servant from NETHERFIELD brought the following note for Elizabeth: only three miles.");
    double b = rerank_fixture_score(q, "nothing in common");
    CHECK(fabs(a - 2.0 / 8.0) < 1e-9 && b == 0.0);
    CHECK(rerank_fixture_score("the of and", "anything") == 0.0);
    CHECK(fabs(rerank_fixture_score("Elizabeth Elizabeth walk", "elizabeth walked") - 0.5) < 1e-9);
    RerankClient *c = rerank_client_create(NULL);
    CHECK(c && rerank_client_fixture(c));
    double s = -1; char err[64];
    CHECK(rerank_score(c, q, "only three miles said Elizabeth near Netherfield", NULL, 0, &s, err, sizeof(err)) && fabs(s - 2.0 / 8.0) < 1e-9);
    CHECK(!rerank_score(c, NULL, "x", NULL, 0, &s, err, sizeof(err)) && err[0]);
    rerank_client_destroy(c);
}

static AnswerPipelineResult result_with(const char *span, const char *document, double score, const char *status)
{
    AnswerPipelineResult r = {0};
    r.attempted = true; r.candidates = 12; r.scored = 11; r.top_score = score;
    snprintf(r.status, sizeof(r.status), "%s", status);
    if (document) {
        r.has_top = true;
        r.top.kind = MEMORY_SOURCE_DOCUMENT;
        snprintf(r.top.id, sizeof(r.top.id), "%s", "unit-abc");
        snprintf(r.top.document_name, sizeof(r.top.document_name), "%s", document);
        r.top.text = strdup(k_unit);
    }
    if (span) r.span = strdup(span);
    return r;
}

static void test_block_and_receipt(void)
{
    AnswerPipelineResult r = result_with("only three miles. I shall be back by dinner.", "part01.md", 0.9928, "verified");
    char *block = answer_verified_block(&r);
    CHECK(block && !strcmp(block, "Verified passage from the manuscript, quoted exactly:\nSource 1 (part01.md): \"only three miles. I shall be back by dinner.\"\n\n"));
    char *receipt = answer_receipt(&r);
    CHECK(receipt && strstr(receipt, "unit=unit-abc") && strstr(receipt, "document=part01.md") && strstr(receipt, "status=verified"));
    CHECK(strstr(receipt, "only three miles. I shall be back by dinner.")); /* the UI script checks the passage here */
    CHECK(!strstr(receipt, "VERIFIED SOURCE 2"));
    free(block); free(receipt);
    /* Two units: both spans listed in score order; a second unit without a verified span adds nothing to the block. */
    r.has_second = true; r.second.kind = MEMORY_SOURCE_DOCUMENT; snprintf(r.second.id, sizeof(r.second.id), "unit-def");
    snprintf(r.second.document_name, sizeof(r.second.document_name), "part02.md"); r.second.text = strdup(k_unit);
    r.second_score = 0.91; snprintf(r.second_status, sizeof(r.second_status), "rejected_not_in_unit");
    block = answer_verified_block(&r);
    CHECK(block && !strcmp(block, "Verified passage from the manuscript, quoted exactly:\nSource 1 (part01.md): \"only three miles. I shall be back by dinner.\"\n\n"));
    free(block);
    r.second_span = strdup("I shall be back by dinner."); snprintf(r.second_status, sizeof(r.second_status), "verified");
    block = answer_verified_block(&r);
    CHECK(block && !strcmp(block, "Verified passages from the manuscript, quoted exactly, best match first:\nSource 1 (part01.md): \"only three miles. I shall be back by dinner.\"\nSource 2 (part02.md): \"I shall be back by dinner.\"\n\n"));
    receipt = answer_receipt(&r);
    CHECK(receipt && strstr(receipt, "VERIFIED SOURCE 2 | unit=unit-def | document=part02.md | score=0.9100 | status=verified") && strstr(receipt, "\nI shall be back by dinner.\n"));
    free(block); free(receipt);
    answer_pipeline_result_free(&r);
    CHECK(!r.has_top && !r.span && !r.has_second && !r.second_span);

    AnswerPipelineResult none = result_with(NULL, "part01.md", 0.5, "rejected_not_in_unit");
    block = answer_verified_block(&none);
    CHECK(block && !strcmp(block, "No verified passage was found in the manuscript for this request.\n\n"));
    receipt = answer_receipt(&none);
    CHECK(receipt && strstr(receipt, "status=rejected_not_in_unit") && strstr(receipt, "unit=unit-abc"));
    free(block); free(receipt);
    answer_pipeline_result_free(&none);

    AnswerPipelineResult empty = result_with(NULL, NULL, 0, "no_candidates");
    receipt = answer_receipt(&empty);
    CHECK(receipt && strstr(receipt, "VERIFIED SOURCE | none | status=no_candidates"));
    free(receipt);
    answer_pipeline_result_free(&empty);
    CHECK(answer_verified_block(NULL) == NULL && answer_receipt(NULL) == NULL);
}

/* The receipt and trace contract (llm/answer_receipt.h): every line the product writes, byte for byte, kept in
 * tests/fixtures/answer_receipt/ where tests/test_answer_receipt_contract.py parses the same files with the review
 * tool's expressions. SCRIBODO_WRITE_FIXTURES=1 regenerates them after a deliberate format change. */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL; size_t n = 0, cap = 0; int c;
    while ((c = fgetc(f)) != EOF) {
        if (n + 1 >= cap) { cap = cap ? cap * 2 : 4096; buf = realloc(buf, cap); CHECK(buf); }
        buf[n++] = (char)c;
    }
    fclose(f);
    if (!buf) buf = calloc(1, 1); else buf[n] = '\0';
    return buf;
}

static void fixture_matches(const char *name, const char *text)
{
    char path[256];
    snprintf(path, sizeof(path), "tests/fixtures/answer_receipt/%s", name);
    if (getenv("SCRIBODO_WRITE_FIXTURES")) {
        FILE *f = fopen(path, "wb");
        CHECK(f && fputs(text, f) >= 0 && !fclose(f));
        return;
    }
    char *expected = read_file(path);
    if (!expected) fprintf(stderr, "missing %s (run from the repository root)\n", path);
    CHECK(expected != NULL);
    if (strcmp(expected, text)) fprintf(stderr, "%s differs from the writer:\n--- fixture ---\n%s--- writer ---\n%s", path, expected, text);
    CHECK(!strcmp(expected, text));
    free(expected);
}

static AnswerPipelineResult contract_result(bool with_second, size_t nparts)
{
    AnswerPipelineResult r = {0};
    r.attempted = true; r.candidates = 52; r.hydrated = 50; r.scored = 47; r.score_failures = 1;
    r.has_top = true; r.top.kind = MEMORY_SOURCE_DOCUMENT;
    snprintf(r.top.id, sizeof(r.top.id), "%s", "f395a4d6679b2e5105afa2c52d388c820f756f40d13b71ef7256381837007e52");
    snprintf(r.top.document_name, sizeof(r.top.document_name), "part07.md");
    r.top.text = strdup(k_unit); r.top_score = 0.98925; snprintf(r.status, sizeof(r.status), "verified");
    r.span = strdup("Quick as lightning they were upon him, and Tars Tarkas would have been gathered to his fathers.");
    r.top_part = 0; r.nparts = nparts; r.rerank_ms = 1234.56; r.extract_ms = 789.04;
    for (size_t p = 0; p < nparts; p++) r.parts[p] = strdup(p ? "How far did he leap?" : "Who saved Tars Tarkas?");
    const char *ids[] = { "f395a4d6679b", "a20205a13f36", "c87be75a52ea" };
    const double scores[] = { 0.98925, 0.98063, 0.1204 };
    r.scored = 0;
    for (size_t p = 0; p < nparts; p++)
        for (size_t i = 0; i < 3; i++) {
            snprintf(r.scored_ids[r.scored], sizeof(r.scored_ids[0]), "%s", ids[i]);
            r.scored_scores[r.scored] = scores[i] - 0.01 * (double)p; r.scored_part[r.scored] = (unsigned char)p; r.scored++;
        }
    if (with_second) {
        r.has_second = true; r.second.kind = MEMORY_SOURCE_DOCUMENT;
        snprintf(r.second.id, sizeof(r.second.id), "%s", "a20205a13f36e66794d13975faa7722e5f77f242866dfb6d5e035243c478aae8");
        snprintf(r.second.document_name, sizeof(r.second.document_name), "part01.md");
        r.second.text = strdup(k_unit); r.second_score = 0.98063; snprintf(r.second_status, sizeof(r.second_status), "verified");
        r.second_span = strdup("I cleared a good hundred and fifty feet;"); r.second_part = nparts > 1 ? 1 : 0;
    }
    return r;
}

/* The relation check: the recorded misreading and its correct denial, then parity with the Python arm on every
 * recorded reply of tests/fixtures/answer_relation/replies.json (written by tools/answer_relation_arm.py). */
static void test_relations(void)
{
    const char *spans[] = { "I, the daughter of Mors Kajak, son of Tardos Mors, have listened without anger," };
    const char *question = "Earlier I told you Dejah Thoris is the daughter of Tardos Mors; I was wrong, the text says otherwise. Whose daughter does she say she is?";
    char *bad = NULL;
    size_t n = answer_relations("This states that the speaker is the daughter of Mors Kajak and son of Tardos Mors \xE2\x80\x94 i.e., her father is Tardos Mors "
                                "and her mother is Mors Kajak. So according to this passage, she is **not** the daughter of Tardos Mors; she is the "
                                "**daughter of Mors Kajak**, son of Tardos Mors.", spans, 1, NULL, 0, question, &bad);
    CHECK(n == 7 && bad && !strcmp(bad, "her father is Tardos Mors"));
    free(bad); bad = NULL;
    /* Quotations and blockquotes are the quotation check's; a possessive is supported by its "of" form; "The" is no name. */
    const char *spans2[] = { "They were rather handsome; had a fortune of twenty thousand pounds; the sisters of Mr. Bingley" };
    n = answer_relations("> \"her father is Tardos Mors\"\nMr. Bingley's sisters have twenty thousand pounds. The estate is Netherfield. \"Her mother is Mors Kajak.\"",
                         spans2, 1, NULL, 0, "What fortune?", &bad);
    CHECK(n == 1 && bad == NULL);
    n = answer_relations("Mr. Bingley's estate is large.", spans2, 1, NULL, 0, "q", &bad);
    CHECK(n == 1 && bad && !strcmp(bad, "Mr. Bingley's estate"));
    free(bad); bad = NULL;
    /* "X's inheritance" is supported by "X inherited" (the composer's paraphrase of the first novel's passage,
     * journal/answer-cycle4_relation_split.md); the other possession words have no verb form. */
    const char *inherited[] = { "Mr. Bingley inherited property to the amount of nearly a hundred thousand pounds from his father" };
    n = answer_relations("Mr. Bingley's inheritance is nearly a hundred thousand pounds. Mr. Bingley's estate is Netherfield Park.",
                         inherited, 1, NULL, 0, "q", &bad);
    CHECK(n == 3 && bad && !strcmp(bad, "Mr. Bingley's estate")); /* the possessive, and the whole "is" statement, of the estate */
    free(bad); bad = NULL;
    n = answer_relations("Mr. Darcy's inheritance is large.", inherited, 1, NULL, 0, "q", &bad);
    CHECK(n == 1 && bad && !strcmp(bad, "Mr. Darcy's inheritance"));
    free(bad); bad = NULL;
    /* An author turn supports a phrase; an honorific's period does not end the sentence. */
    const char *turns[] = { "Note: her father is Tardos Mors." };
    n = answer_relations("Her father is Tardos Mors. Mr. Bennet's estate, then.", NULL, 0, turns, 1, "Whose?", &bad);
    CHECK(n == 2 && bad && !strcmp(bad, "Mr. Bennet's estate"));
    free(bad); bad = NULL;
    CHECK(answer_relations(NULL, NULL, 0, NULL, 0, NULL, &bad) == 0 && bad == NULL);
    /* The subject of "<subject> <kin> is <Name>" is a pronoun or a possessive name, checked whole with both names; a
     * family name used as an adjective ("the youngest Bennet daughter is Lydia", live on the first novel) is not a
     * relation statement, and a label subject is no name. */
    n = answer_relations("Elizabeth's father is Mr. Bennet. The father is Mr. Bennet. The youngest Bennet daughter is Lydia.", NULL, 0, NULL, 0, "q", &bad);
    CHECK(n == 2 && bad && !strcmp(bad, "Elizabeth's father"));
    free(bad); bad = NULL;
    const char *liz[] = { "Elizabeth's father, Mr. Bennet, was a man of wit." };
    n = answer_relations("Elizabeth's father is Mr. Bennet.", liz, 1, NULL, 0, "q", &bad);
    CHECK(n == 2 && bad && !strcmp(bad, "Elizabeth's father is Mr. Bennet")); /* the possessive alone is supported; the whole statement is not */
    free(bad); bad = NULL;
    n = answer_relations("Elizabeth's father is Mr. Bennet.", NULL, 0, NULL, 0, "Is Elizabeth's father is Mr. Bennet?", &bad);
    CHECK(n == 2 && bad == NULL);
    /* Measured on the third novel (journal/answer-cycle4_heights.md): a question the composer asks back states nothing;
     * a negation reaches its own clause, not the sentence; the kinship list has nephew; a curly apostrophe is the
     * straight one; "daughter of Mr. Bingley's cousin" is not "daughter of Mr. Bingley". */
    const char *heights[] = { "No; he is the late Mrs. Linton\xE2\x80\x99s nephew." };
    n = answer_relations("Hareton Earnshaw is Heathcliff\xE2\x80\x99s nephew, not his son. Who is Hareton Earnshaw's father?",
                         heights, 1, NULL, 0, "Whose son is Hareton?", &bad);
    CHECK(n == 1 && bad && !strcmp(bad, "Heathcliff\xE2\x80\x99s nephew"));
    free(bad); bad = NULL;
    n = answer_relations("He is the late Mrs. Linton's nephew, not Heathcliff's son.", heights, 1, NULL, 0, "q", &bad);
    CHECK(n == 2 && bad == NULL);
    n = answer_relations("She is the daughter of Mr. Bingley's cousin.", NULL, 0, NULL, 0, "q", &bad);
    CHECK(n == 1 && bad && !strcmp(bad, "Mr. Bingley's cousin"));
    free(bad); bad = NULL;

    char *text = read_file("tests/fixtures/answer_relation/replies.json");
    CHECK(text != NULL);
    if (!text) return;
    cJSON *j = cJSON_Parse(text);
    cJSON *rows = cJSON_GetObjectItem(j, "replies");
    CHECK(cJSON_IsArray(rows) && cJSON_GetArraySize(rows) >= 60);
    size_t agree = 0, total = 0, flagged = 0;
    cJSON *row;
    cJSON_ArrayForEach(row, rows) {
        cJSON *sp = cJSON_GetObjectItem(row, "spans"), *tu = cJSON_GetObjectItem(row, "turns");
        const char *sv[8] = { 0 }, *tv[32] = { 0 };
        size_t ns = 0, nt = 0;
        cJSON *it;
        cJSON_ArrayForEach(it, sp) if (ns < 8) sv[ns++] = it->valuestring;
        cJSON_ArrayForEach(it, tu) if (nt < 32) tv[nt++] = it->valuestring;
        const char *reply = cJSON_GetObjectItem(row, "reply")->valuestring, *q = cJSON_GetObjectItem(row, "question")->valuestring;
        const char *expected = cJSON_GetObjectItem(row, "unsupported")->valuestring;
        int checked = cJSON_GetObjectItem(row, "checked")->valueint;
        char *got = NULL;
        size_t c = answer_relations(reply, sv, ns, tv, nt, q, &got);
        bool same = (int)c == checked && !strcmp(got ? got : "", expected);
        if (!same) fprintf(stderr, "    relations differ on %.60s: C %zu/%s, arm %d/%s\n", q, c, got ? got : "", checked, expected);
        agree += same; total++; flagged += got != NULL;
        free(got);
    }
    cJSON_Delete(j);
    free(text);
    CHECK(agree == total && flagged == 21); /* 230 rows from 23 recorded runs, journal/answer-cycle4_relation_split.md */
}

static void test_receipt_contract(void)
{
    char *trace = NULL; size_t trace_len = 0;
    FILE *t = open_memstream(&trace, &trace_len);
    CHECK(t);
    /* An answered question with two sources and two parts: nothing withheld, one quotation checked. */
    AnswerPipelineResult pass = contract_result(true, 2);
    char *receipt = answer_receipt(&pass);
    CHECK(receipt && strlen(receipt) <= ANSWER_RECEIPT_PIPELINE_MAX);
    CHECK(answer_receipt_append_gate(&receipt, "pass", false, "150 feet", 1, NULL, 2, NULL));
    fixture_matches("receipt-pass.txt", receipt);
    CHECK(!chat_receipt_refusal(receipt));
    answer_trace_pipeline(t, &pass, 4321, 3);
    answer_trace_gate(t, "pass", false, true, 2, 5, 704.56, 8, 1, false, 2, false);
    free(receipt); answer_pipeline_result_free(&pass);
    /* A withheld answer: one source, one part, the claim outside the span, a misquotation named. The claim's
     * newline becomes a space; the field runs to the end of the line even when it contains " ---". */
    AnswerPipelineResult held = contract_result(false, 1);
    receipt = answer_receipt(&held);
    CHECK(answer_receipt_append_gate(&receipt, "not_in_span", true, "sons of\nTardos Mors --- or so", 2, "\"the daughter of Mors Kajak\"", 3, "her father is Tardos Mors"));
    fixture_matches("receipt-withheld.txt", receipt);
    CHECK(chat_receipt_refusal(receipt) && strstr(receipt, "answer=sons of Tardos Mors --- or so ---\n"));
    answer_trace_pipeline(t, &held, 4000, 1);
    answer_trace_gate(t, "not_in_span", true, true, 1, 4, 650.0, 27, 2, true, 3, true);
    free(receipt); answer_pipeline_result_free(&held);
    /* No candidates and an abstention: the none header, the gate's no_answer verdict, which is a refusal. */
    AnswerPipelineResult none = {0};
    none.attempted = true; snprintf(none.status, sizeof(none.status), "no_candidates");
    receipt = answer_receipt(&none);
    CHECK(receipt && !strcmp(receipt, "--- VERIFIED SOURCE | none | status=no_candidates | candidates=0 scored=0 ---\n"));
    CHECK(answer_receipt_append_gate(&receipt, "no_answer", false, "The supplied passages do not give it", 0, NULL, 0, NULL));
    fixture_matches("receipt-abstained.txt", receipt);
    CHECK(chat_receipt_refusal(receipt));
    answer_trace_pipeline(t, &none, 3900, 0);
    answer_trace_gate(t, "no_answer", false, true, 0, 2, 12.3, 36, 0, false, 0, false);
    free(receipt);
    CHECK(!fclose(t));
    fixture_matches("trace.log", trace);
    free(trace);
    /* The bound. A pipeline receipt whose spans would pass the store limit keeps its headers whole and cuts the
     * spans on a UTF-8 boundary; the gate lines then always fit, cutting the claim first and the misquotation
     * second; the whole stays within CHAT_MEMORY_SOURCES_BYTES, which is what the store will accept. */
    AnswerPipelineResult big = contract_result(true, 1);
    free(big.span); free(big.second_span);
    size_t huge = 2 * CHAT_MEMORY_SOURCES_BYTES;
    big.span = malloc(huge + 1); big.second_span = malloc(huge + 1);
    CHECK(big.span && big.second_span);
    for (size_t i = 0; i + 3 <= huge; i += 3) memcpy(big.span + i, "\xE2\x82\xAC", 3); /* three-byte characters */
    big.span[huge - huge % 3] = '\0';
    memset(big.second_span, 'b', huge); big.second_span[huge] = '\0';
    receipt = answer_receipt(&big);
    CHECK(receipt && strlen(receipt) == ANSWER_RECEIPT_PIPELINE_MAX); /* the second span takes exactly what the first left */
    const char *second = strstr(receipt, "--- VERIFIED SOURCE 2 | unit=a20205a13f36e66794d13975faa7722e5f77f242866dfb6d5e035243c478aae8 | document=part01.md | score=0.9806 | status=verified | part=1 ---\n");
    CHECK(second != NULL);
    const char *span_start = strchr(receipt, '\n') + 1;
    size_t span_len = (size_t)(second - 1 - span_start);
    CHECK(span_len % 3 == 0 && span_len > 10000); /* cut between characters, never inside one */
    const char *span2 = strchr(second, '\n') + 1;
    CHECK(strspn(span2, "b") < 3 && span2[strspn(span2, "b")] == '\n'); /* the second span gets the one or two bytes the first could not use */
    char *claim = malloc(huge + 1); memset(claim, 'c', huge); claim[huge] = '\0';
    CHECK(answer_receipt_append_gate(&receipt, "not_in_span", true, claim, 3, claim, 1, claim));
    CHECK(strlen(receipt) <= CHAT_MEMORY_SOURCES_BYTES);
    CHECK(strstr(receipt, "--- ANSWER GATE | verdict=not_in_span | withheld=1 | answer=") && strstr(receipt, "--- ANSWER QUOTES | checked=3 | misquoted=") && strstr(receipt, "--- ANSWER RELATIONS | checked=1 | unsupported="));
    CHECK(chat_receipt_refusal(receipt));
    /* The fields are cut at ANSWER_RECEIPT_FIELD_MAX when there is room, and to nothing when there is not. */
    char *small = strdup("");
    CHECK(answer_receipt_append_gate(&small, "pass", false, claim, 0, claim, 0, claim));
    const char *a = strstr(small, "answer=") + 7;
    CHECK((size_t)(strstr(a, " ---\n") - a) == ANSWER_RECEIPT_FIELD_MAX);
    free(small);
    char *full = malloc(CHAT_MEMORY_SOURCES_BYTES + 1); memset(full, 'f', CHAT_MEMORY_SOURCES_BYTES - 200); full[CHAT_MEMORY_SOURCES_BYTES - 200] = '\0';
    CHECK(answer_receipt_append_gate(&full, "pass", false, claim, 0, claim, 0, claim) && strlen(full) <= CHAT_MEMORY_SOURCES_BYTES);
    CHECK(strstr(full, "| answer= ---\n") && strstr(full, "| misquoted= ---\n") && strlen(full) == CHAT_MEMORY_SOURCES_BYTES); /* the claim and misquotation went first; the relation keeps what is left */
    memset(full, 'f', CHAT_MEMORY_SOURCES_BYTES - 10); full[CHAT_MEMORY_SOURCES_BYTES - 10] = '\0';
    char *before = strdup(full);
    CHECK(!answer_receipt_append_gate(&full, "pass", false, NULL, 0, NULL, 0, NULL) && !strcmp(full, before)); /* no room at all: unchanged */
    free(before); free(full); free(claim); free(receipt); answer_pipeline_result_free(&big);
    CHECK(!answer_receipt_append_gate(NULL, "pass", false, NULL, 0, NULL, 0, NULL));
    char *empty = NULL;
    CHECK(answer_receipt_append_gate(&empty, NULL, false, NULL, 0, NULL, 0, NULL) && !strcmp(empty, "--- ANSWER GATE | verdict=unavailable | withheld=0 | answer= ---\n--- ANSWER QUOTES | checked=0 | misquoted= ---\n--- ANSWER RELATIONS | checked=0 | unsupported= ---\n"));
    free(empty);
}

static long fixed_count(const char *text, void *ctx) { (void)ctx; return (long)(strlen(text) / 4) + 1; }

static void test_pipeline_request(void)
{
    ChatMessage msgs[] = {
        { .role = CHAT_USER, .content = "Note for later: Lydia, my youngest, is seventeen." },
        { .role = CHAT_ASSISTANT, .content = "Noted." },
        { .role = CHAT_USER, .content = "How far is the walk to Netherfield?" },
    };
    ChatNames names = { "Tripp", "Scribodo" };
    size_t first = 99;
    const char *preface = "Verified passage from the manuscript, quoted exactly (Source 1, part01.md):\n\"only three miles.\"\n\n";
    setenv("SCRIBODO_ANSWER_BLOCK", "message", 1); experiments_reload(); /* the pass 3a shape, checked first; the default follows */
    char *req = chat_build_request_pipeline(msgs, 3, &names, 8192, fixed_count, NULL, &first, preface);
    unsetenv("SCRIBODO_ANSWER_BLOCK"); experiments_reload();
    CHECK(req && first == 0);
    cJSON *j = cJSON_Parse(req);
    CHECK(j);
    const cJSON *messages = cJSON_GetObjectItem(j, "messages");
    CHECK(cJSON_GetArraySize(messages) == 4); /* system + the three turns */
    const char *sys = cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 0), "content")->valuestring;
    CHECK(strstr(sys, "You are a creative writing partner") && strstr(sys, "You are Scribodo. The author you're talking with is Tripp."));
    CHECK(strstr(sys, "answer from that passage alone") && strstr(sys, "do not guess"));
    const char *newest = cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 3), "content")->valuestring;
    CHECK(!strncmp(newest, preface, strlen(preface)) && strstr(newest, "How far is the walk to Netherfield?"));
    CHECK(!strstr(req, "BEGIN DOCUMENT") && !strstr(req, "--- SOURCE") && !strstr(req, "too long: cut off"));
    /* Earlier turns are untouched: the note is still there, without the preface. */
    const char *oldest = cJSON_GetObjectItem(cJSON_GetArrayItem(messages, 1), "content")->valuestring;
    CHECK(!strcmp(oldest, "Note for later: Lydia, my youngest, is seventeen."));
    cJSON_Delete(j);
    free(req);
    CHECK(chat_build_request_pipeline(msgs, 3, &names, 8192, fixed_count, NULL, &first, NULL) == NULL);
    /* Earlier refusals (journal/answer-cycle4_window.md): on a question turn a withheld or abstained assistant turn,
     * known from its stored gate receipt, is shown as the neutral marker; an answered turn, a turn without a receipt
     * and every turn on a note turn are shown as recorded. The messages themselves are never changed. */
    {
        char withheld[] = "The supplied manuscript passages do not give that. An answer was withheld because it quotes words the verified passages do not contain.";
        char abstained[] = "The supplied manuscript passages do not give that. An answer was not found within the provided excerpts.";
        char answered[] = "Source 1 (part01.md): \"only three miles.\"";
        char receipt_w[] = "--- VERIFIED SOURCE | unit=u ---\nspan\n--- ANSWER GATE | verdict=not_in_span | withheld=1 | answer=sons ---\n";
        char receipt_a[] = "--- ANSWER GATE | verdict=no_answer | withheld=0 | answer=The supplied passages do not give it ---\n";
        char receipt_p[] = "--- ANSWER GATE | verdict=pass | withheld=0 | answer=three miles ---\n";
        ChatMessage turns[] = {
            { .role = CHAT_USER, .content = "On whom is the estate entailed?" },
            { .role = CHAT_ASSISTANT, .content = withheld, .memory_sources = receipt_w },
            { .role = CHAT_USER, .content = "About how old is Charlotte?" },
            { .role = CHAT_ASSISTANT, .content = abstained, .memory_sources = receipt_a },
            { .role = CHAT_USER, .content = "Who is Mr. Collins's patroness?" },
        };
        char *q = chat_build_request_pipeline(turns, 5, &names, 8192, fixed_count, NULL, &first, preface);
        CHECK(q && first == 0);
        CHECK(strstr(q, ANSWER_WINDOW_MARKER) && !strstr(q, "An answer was withheld") && !strstr(q, "An answer was not found"));
        CHECK(strstr(q, "estate entailed?") && strstr(q, "About how old is Charlotte?") && strstr(q, "Who is Mr. Collins"));
        CHECK(!strcmp(turns[1].content, withheld) && !strcmp(turns[3].content, abstained)); /* the record is untouched */
        free(q);
        turns[4].content = "Note for later: Mr. Collins is a clergyman.";
        q = chat_build_request_pipeline(turns, 5, &names, 8192, fixed_count, NULL, &first, preface);
        CHECK(q && !strstr(q, ANSWER_WINDOW_MARKER) && strstr(q, "An answer was withheld") && strstr(q, "An answer was not found"));
        free(q);
        turns[4].content = "Who is Mr. Collins's patroness?";
        turns[1].memory_sources = NULL; /* no receipt: not known to be a refusal, shown as recorded */
        q = chat_build_request_pipeline(turns, 5, &names, 8192, fixed_count, NULL, &first, preface);
        CHECK(q && strstr(q, "An answer was withheld") && !strstr(q, "An answer was not found") && strstr(q, ANSWER_WINDOW_MARKER));
        free(q);
        ChatMessage answered_turns[] = {
            { .role = CHAT_USER, .content = "How far is the walk?" },
            { .role = CHAT_ASSISTANT, .content = answered, .memory_sources = receipt_p },
            { .role = CHAT_USER, .content = "Who is Mr. Collins's patroness?" },
        };
        q = chat_build_request_pipeline(answered_turns, 3, &names, 8192, fixed_count, NULL, &first, preface);
        CHECK(q && strstr(q, "only three miles") && !strstr(q, ANSWER_WINDOW_MARKER)); /* an answered turn is shown as recorded */
        free(q);
    }
    /* A long conversation is cut to the newest window, starting at a user turn, and reported as such. */
    ChatMessage many[11];
    char texts[11][32];
    for (size_t i = 0; i < 11; i++) {
        snprintf(texts[i], sizeof(texts[i]), "turn %zu", i);
        many[i] = (ChatMessage){ .role = i % 2 ? CHAT_ASSISTANT : CHAT_USER, .content = texts[i] };
    }
    size_t cut = 99;
    char *bounded = chat_build_request_pipeline(many, 11, &names, 8192, fixed_count, NULL, &cut, preface);
    CHECK(bounded && cut == 11 - ANSWER_HISTORY_MESSAGES + 1); /* 6 back lands on an assistant turn; the window starts at the next user turn */
    cJSON *bj = cJSON_Parse(bounded);
    CHECK(bj && cJSON_GetArraySize(cJSON_GetObjectItem(bj, "messages")) == 1 + 11 - (int)cut);
    CHECK(!strstr(bounded, "turn 0") && strstr(bounded, "turn 10") && !strstr(bounded, "turn 5\""));
    cJSON_Delete(bj);
    free(bounded);
    /* Pass 3b placement is the default: the block in the system message, the newest words without a preface;
     * SCRIBODO_ANSWER_BLOCK=message restores the pass 3a shape. */
    unsetenv("SCRIBODO_ANSWER_BLOCK"); experiments_reload();
    char *placed = chat_build_request_pipeline(msgs, 3, &names, 8192, fixed_count, NULL, &first, preface);
    CHECK(placed);
    cJSON *pj = cJSON_Parse(placed);
    const char *psys = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(pj, "messages"), 0), "content")->valuestring;
    const char *pnew = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(pj, "messages"), 3), "content")->valuestring;
    CHECK(strstr(psys, "do not guess") && strstr(psys, preface) && strstr(psys, "do not guess") < strstr(psys, preface));
    CHECK(!strcmp(pnew, "How far is the walk to Netherfield?"));
    cJSON_Delete(pj);
    free(placed);
    setenv("SCRIBODO_ANSWER_BLOCK", "message", 1); experiments_reload();
    char *inline_block = chat_build_request_pipeline(msgs, 3, &names, 8192, fixed_count, NULL, &first, preface);
    unsetenv("SCRIBODO_ANSWER_BLOCK"); experiments_reload();
    CHECK(inline_block && strstr(inline_block, "Verified passage") && !strstr(strstr(inline_block, "system"), "Verified passage from the manuscript, quoted exactly:\\nSource 1 (part01.md)") );
    free(inline_block);
    /* The production builder is unchanged by the refactor: no preface, no composer sentence. */
    bool truncated = true;
    char *plain = chat_build_request_selected(msgs, 3, NULL, &names, 8192, fixed_count, NULL, &truncated, &first);
    CHECK(plain && !truncated && !strstr(plain, "answer from that passage alone") && !strstr(plain, "Verified passage"));
    free(plain);
}

static void test_question_parts(void)
{
    char *parts[ANSWER_SPANS];
    /* The fixture questions, the same expectations as tests/test_answer_question_parts.py. */
    CHECK(answer_question_parts("What fortune do Mr. Bingley's sisters have, and what is Mr. Bingley's own inheritance said to be?", parts) == 2);
    CHECK(!strcmp(parts[0], "What fortune do Mr. Bingley's sisters have?"));
    CHECK(!strcmp(parts[1], "what is Mr. Bingley's own inheritance said to be?"));
    free(parts[0]); free(parts[1]);
    CHECK(answer_question_parts("In my manuscript, on whom is Mr. Bennet's estate entailed, and why is that unfortunate for his daughters?", parts) == 2);
    CHECK(!strcmp(parts[0], "In my manuscript, on whom is Mr. Bennet's estate entailed?")); /* the honorific's period is not a sentence end */
    CHECK(!strcmp(parts[1], "why is that unfortunate for his daughters?"));
    free(parts[0]); free(parts[1]);
    /* Context sentences are prefixed to every part. */
    CHECK(answer_question_parts("My two drafts disagree about how many miles Elizabeth walks to Netherfield. Which figure does the current manuscript give, and what did the other draft say?", parts) == 2);
    CHECK(!strcmp(parts[0], "My two drafts disagree about how many miles Elizabeth walks to Netherfield. Which figure does the current manuscript give?"));
    CHECK(!strcmp(parts[1], "My two drafts disagree about how many miles Elizabeth walks to Netherfield. what did the other draft say?"));
    free(parts[0]); free(parts[1]);
    CHECK(answer_question_parts("Earlier I told you Lydia is seventeen; I was wrong, the text says otherwise. How old is my youngest Bennet daughter?", parts) == 1);
    CHECK(!strcmp(parts[0], "Earlier I told you Lydia is seventeen; I was wrong, the text says otherwise. How old is my youngest Bennet daughter?"));
    free(parts[0]);
    CHECK(answer_question_parts("About how old is Charlotte Lucas when the story opens?", parts) == 1);
    CHECK(!strcmp(parts[0], "About how old is Charlotte Lucas when the story opens?")); free(parts[0]);
    /* A conjunction without a wh-word does not split; a question mark alone is not a part. */
    CHECK(answer_question_parts("Does Jane walk and ride to Netherfield?", parts) == 1);
    CHECK(!strcmp(parts[0], "Does Jane walk and ride to Netherfield?")); free(parts[0]);
    CHECK(answer_question_parts("Where does Mrs. Bennet go? Why?", parts) == 2);
    CHECK(!strcmp(parts[0], "Where does Mrs. Bennet go?") && !strcmp(parts[1], "Why?")); free(parts[0]); free(parts[1]);
    /* No question sentence: one part, the request trimmed. */
    CHECK(answer_question_parts("  Draft two sentences, in Austen's register, of Mr. Bennet privately reflecting on the entail. Mark it as new writing. ", parts) == 1);
    CHECK(!strcmp(parts[0], "Draft two sentences, in Austen's register, of Mr. Bennet privately reflecting on the entail. Mark it as new writing.")); free(parts[0]);
    /* Three wh-parts: the third is beyond the slots and dropped. */
    CHECK(answer_question_parts("Who goes, and why, and when does she return?", parts) == 2);
    CHECK(!strcmp(parts[0], "Who goes?") && !strcmp(parts[1], "why?")); free(parts[0]); free(parts[1]);
    CHECK(answer_question_parts("?", parts) == 1 && !strcmp(parts[0], "?")); free(parts[0]);
    CHECK(answer_question_parts("", parts) == 0 && parts[0] == NULL);
    CHECK(answer_question_parts(NULL, parts) == 0);
}

static void test_assign_slots(void)
{
    size_t slot[ANSWER_SPANS], part[ANSWER_SPANS];
    /* Two parts: part 1 scores unit 0 (Bingley) 0.98 and unit 1 (sisters) 0.97; part 2 scores unit 0 0.99 and unit 3 0.74.
     * The greedy first choice would give part 1 unit 0; the matching gives sisters plus Bingley. */
    double s0[4] = { .98, .97, .5, -1 }, s1[4] = { .99, -1, -1, .74 };
    CHECK(answer_assign_slots(s0, s1, 4, slot, part) && slot[0] == 1 && slot[1] == 0 && part[0] == 0 && part[1] == 1);
    /* One part: the two highest of its pool. */
    double one[3] = { .2, .9, .5 };
    CHECK(answer_assign_slots(one, NULL, 3, slot, part) && slot[0] == 1 && slot[1] == 2 && part[0] == 0 && part[1] == 0);
    double single[3] = { -1, .9, -1 };
    CHECK(answer_assign_slots(single, NULL, 3, slot, part) && slot[0] == 1 && slot[1] == SIZE_MAX);
    /* Two parts, one empty pool: the other pool's two highest, both answering that part. */
    double empty[3] = { -1, -1, -1 };
    CHECK(answer_assign_slots(empty, one, 3, slot, part) && slot[0] == 1 && slot[1] == 2 && part[0] == 1 && part[1] == 1);
    CHECK(answer_assign_slots(one, empty, 3, slot, part) && slot[0] == 1 && slot[1] == 2 && part[0] == 0 && part[1] == 0);
    /* Two parts whose pools hold the same single unit: no distinct pair; the unit answers part 1 alone. */
    double same0[2] = { -1, .8 }, same1[2] = { -1, .9 };
    CHECK(answer_assign_slots(same0, same1, 2, slot, part) && slot[0] == 1 && slot[1] == SIZE_MAX && part[0] == 0);
    /* Ties go to the lower index, as the Python arm does. */
    double tie0[2] = { .5, .5 }, tie1[2] = { .5, .5 };
    CHECK(answer_assign_slots(tie0, tie1, 2, slot, part) && slot[0] == 0 && slot[1] == 1);
    CHECK(!answer_assign_slots(empty, NULL, 3, slot, part) && slot[0] == SIZE_MAX);
    CHECK(!answer_assign_slots(NULL, NULL, 0, slot, part));
}

static void test_answer_gate(void)
{
    const char *spans[] = { "only three miles. I shall be back by dinner.", "Mr. Bennet still up." };
    const char *names[] = { "part01.md", "part01.md" };
    const char *author[] = { "Draft note (older): In the first draft Elizabeth walked five miles to Netherfield." };
    CHECK(answer_gate_applies("How far is it?") && answer_gate_applies("  Why?  \n") && !answer_gate_applies("Draft two sentences.") && !answer_gate_applies("") && !answer_gate_applies(NULL));
    CHECK(!strcmp(answer_gate("three miles", "How far does Elizabeth say the walk is?", spans, 2, author, 1), "pass"));
    CHECK(!strcmp(answer_gate("**Three miles**", "How far?", spans, 2, author, 1), "pass"));           /* emphasis and case folded */
    {   /* Numbers are whole words: "ten" inside "often" and "7" inside "27" do not support an answer (pass 7 check). */
        const char *often[1] = { "They often walked to Meryton; she was twenty-seven." };
        CHECK(!strcmp(answer_gate("ten miles", "How far?", often, 1, NULL, 0), "not_in_span"));
        CHECK(!strcmp(answer_gate("7", "How old?", often, 1, NULL, 0), "not_in_span"));
        CHECK(!strcmp(answer_gate("27", "How old?", often, 1, NULL, 0), "pass"));
        CHECK(!strcmp(answer_gate("twenty-seven", "How old?", often, 1, NULL, 0), "pass"));
        CHECK(!strcmp(answer_gate("twenty", "How old?", often, 1, NULL, 0), "not_in_span")); /* the tens word of a compound is not 20 */
        CHECK(!strcmp(answer_gate("20", "How old?", often, 1, NULL, 0), "not_in_span"));
        const char *tenner[1] = { "it was ten miles round" };
        CHECK(!strcmp(answer_gate("10 miles", "How far?", tenner, 1, NULL, 0), "pass"));
        const char *fortune[1] = { "had a fortune of twenty thousand pounds; three or four times a week" };
        CHECK(!strcmp(answer_gate("20 thousand", "What fortune?", fortune, 1, NULL, 0), "pass")); /* scale words are left alone */
        CHECK(!strcmp(answer_gate("three", "How often?", fortune, 1, NULL, 0), "pass"));
        CHECK(!strcmp(answer_gate("4", "How often?", fortune, 1, NULL, 0), "pass"));
    }
    CHECK(!strcmp(answer_gate("three miles; five miles", "Which figure does the current manuscript give, and what did the other draft say?", spans, 2, author, 1), "pass"));
    CHECK(!strcmp(answer_gate("3 miles; 5 miles", "Which figure?", spans, 2, author, 1), "pass"));      /* numerals canonicalized (pass 5) */
    CHECK(!strcmp(answer_gate("three miles, seven miles", "How far?", spans, 2, author, 1), "not_in_span"));
    CHECK(!strcmp(answer_gate("Bennet", "What is Mr. Bennet's first name in my manuscript?", spans, 2, author, 1), "echoes_question"));
    CHECK(!strcmp(answer_gate("Edward", "What is Mr. Bennet's first name?", spans, 2, author, 1), "not_in_span"));
    CHECK(!strcmp(answer_gate("", "How far?", spans, 2, author, 1), "no_answer") && !strcmp(answer_gate("  ", "q?", spans, 2, NULL, 0), "no_answer"));
    CHECK(!strcmp(answer_gate("The supplied manuscript passages do not give the age.", "How old?", spans, 2, author, 1), "no_answer"));
    CHECK(!strcmp(answer_gate("Charlotte's age is not mentioned in the supplied text.", "How old?", spans, 2, author, 1), "no_answer"));
    CHECK(!strcmp(answer_gate("The supplied passages do not state the age of the youngest daughter.", "How old?", spans, 2, author, 1), "no_answer"));
    CHECK(!strcmp(answer_gate("three miles", "How far?", NULL, 0, NULL, 0), "not_in_span"));
    /* Paraphrase with its anchors present passes; a claim with no anchors has nothing to check. */
    CHECK(!strcmp(answer_gate("Mr. Bennet was still up when she came back", "Was anyone up?", spans, 2, author, 1), "pass"));
    CHECK(!strcmp(answer_gate("the walk was short and dirty", "How was the walk?", spans, 2, author, 1), "pass_no_anchors"));
    CHECK(!strcmp(answer_gate("Miss Bingley walked twenty-seven miles", "How far?", spans, 2, author, 1), "not_in_span"));
    /* Compound numbers in every form (pass 6): "27" against "twenty-seven" in the span, and the reverse. */
    const char *age_spans[] = { "The eldest of them, a sensible, intelligent young woman, about twenty-seven, was Elizabeth\xE2\x80\x99s intimate friend." };
    CHECK(!strcmp(answer_gate("27", "How old is Charlotte?", age_spans, 1, NULL, 0), "pass"));
    CHECK(!strcmp(answer_gate("twenty-seven", "How old is Charlotte?", age_spans, 1, NULL, 0), "pass"));
    CHECK(!strcmp(answer_gate("Twenty seven", "How old is Charlotte?", age_spans, 1, NULL, 0), "pass"));
    CHECK(!strcmp(answer_gate("28", "How old is Charlotte?", age_spans, 1, NULL, 0), "not_in_span"));
    const char *digit_spans[] = { "She was 42 that spring." };
    CHECK(!strcmp(answer_gate("forty-two", "How old?", digit_spans, 1, NULL, 0), "pass"));
    CHECK(!strcmp(answer_gate("forty", "How old?", digit_spans, 1, NULL, 0), "not_in_span"));
    /* Scale words and thousands separators (journal/answer-cycle3_coref.md, the live treatment): "£100,000" against
     * "nearly a hundred thousand pounds" is the same number; "100" against it is not; "20" against "twenty
     * thousand" keeps the leniency measured above. */
    {
        const char *inherit[] = { "Mr. Bingley inherited property to the amount of nearly a hundred thousand pounds from his father.",
                                  "They were rather handsome; had a fortune of twenty thousand pounds; were in the habit of spending more." };
        CHECK(!strcmp(answer_gate("Mr. Bingley inherited nearly \xC2\xA3" "100,000 from his father; his sisters had a fortune of twenty thousand pounds",
                                  "What fortune do the sisters have?", inherit, 2, NULL, 0), "pass"));
        CHECK(!strcmp(answer_gate("100,000", "What?", inherit, 2, NULL, 0), "pass"));
        CHECK(!strcmp(answer_gate("100000 pounds", "What?", inherit, 2, NULL, 0), "pass"));
        CHECK(!strcmp(answer_gate("20,000", "What?", inherit, 2, NULL, 0), "pass"));
        CHECK(!strcmp(answer_gate("100", "What?", inherit, 2, NULL, 0), "not_in_span"));
        CHECK(!strcmp(answer_gate("200,000", "What?", inherit, 2, NULL, 0), "not_in_span"));
        /* Capitalisation that is not a name (journal/answer-cycle4_mars.md, the second manuscript's first live run):
         * a sentence-initial word the question contains, and an all-capitals closed-class word written for emphasis.
         * A name at the start of a sentence stays an anchor unless the question names it, and an answer that only
         * echoes the question is still an echo. */
        const char *pumps[] = { "He led me to an inner chamber where I beheld a battery of twenty radium pumps.",
                                "In his mind was the decision to inspect the radium pumps." };
        const char *draft[] = { "Draft note (older): In the first draft the atmosphere plant had ten radium pumps." };
        const char *drafts_q = "My two drafts disagree about how many radium pumps the atmosphere factory has. Which figure does the current manuscript give, and what did the other draft say?";
        CHECK(!strcmp(answer_gate("Current manuscript: twenty radium pumps; earlier draft: ten radium pumps.", drafts_q, pumps, 2, draft, 1), "pass"));
        CHECK(!strcmp(answer_gate("Current manuscript: twenty radium pumps; earlier draft: ten radium pumps.", drafts_q, pumps, 2, NULL, 0), "not_in_span")); /* "ten" still needs its source */
        CHECK(!strcmp(answer_gate("Latest manuscript: twenty radium pumps.", drafts_q, pumps, 2, draft, 1), "not_in_span")); /* a sentence-initial word the question lacks is still checked */
        const char *parent[] = { "I, the daughter of Mors Kajak, son of Tardos Mors, have listened without anger," };
        const char *parent_q = "Earlier I told you Dejah Thoris is the daughter of Tardos Mors; I was wrong. Whose daughter does she say she is?";
        CHECK(!strcmp(answer_gate("She is the daughter of Mors Kajak (and son of Tardos Mors); i.e., she is NOT the daughter of Tardos Mors.", parent_q, parent, 1, NULL, 0), "pass"));
        CHECK(!strcmp(answer_gate("She is NOT the daughter of Tardos Mors; she is the daughter of Mors Kajak.", parent_q, parent, 1, NULL, 0), "pass"));
        CHECK(!strcmp(answer_gate("She is the daughter of NASA.", parent_q, parent, 1, NULL, 0), "not_in_span"));         /* all-capitals outside the closed class stays an anchor */
        CHECK(!strcmp(answer_gate("Sola is the daughter of Mors Kajak.", parent_q, parent, 1, NULL, 0), "not_in_span"));  /* a sentence-initial name the question lacks */
        CHECK(!strcmp(answer_gate("Tardos Mors", parent_q, parent, 1, NULL, 0), "echoes_question")); /* every anchor is in the question */
        CHECK(!strcmp(answer_gate("Mors Kajak, son of Tardos Mors", "Whose daughter is she?", parent, 1, NULL, 0), "pass"));
        CHECK(!strcmp(answer_gate("Bennet", "What is Mr. Bennet's first name?", parent, 1, NULL, 0), "echoes_question")); /* a soft anchor still echoes */
        /* A name the author's question gives for a passage's "she" is the author's word wherever the claim puts it
         * (journal/answer-cycle4_heights.md, the two-ages case); a name from nowhere is still checked. */
        const char *she[] = { "Little Hareton was nearly five years old, and I had just begun to teach him his letters.",
                              "Till she reached the age of thirteen she had not once been beyond the range of the park by herself." };
        const char *she_q = "How old is Hareton when Nelly begins teaching him his letters, and until what age had the younger Catherine never once been beyond the range of the park by herself?";
        CHECK(!strcmp(answer_gate("Hareton is nearly five years old; the younger Catherine was under thirteen before she went beyond the park.", she_q, she, 2, NULL, 0), "pass"));
        CHECK(!strcmp(answer_gate("Hareton is nearly five years old; the younger Linton was under thirteen.", she_q, she, 2, NULL, 0), "not_in_span"));
        CHECK(!strcmp(answer_gate("1,000", "What?", inherit, 2, NULL, 0), "not_in_span"));
        const char *estate[] = { "an estate of two thousand a year; her father had left her four thousand pounds" };
        CHECK(!strcmp(answer_gate("2,000 a year and 4000 pounds", "What?", estate, 1, NULL, 0), "pass"));
        CHECK(!strcmp(answer_gate("2,000 a year and 5,000 pounds", "What?", estate, 1, NULL, 0), "not_in_span"));
        const char *thousand[] = { "a thousand a year" };
        CHECK(!strcmp(answer_gate("1,000", "What?", thousand, 1, NULL, 0), "pass"));
        const char *compound_scale[] = { "a hundred and twenty-seven pounds, or twenty-seven thousand in all; 1,500 more" };
        CHECK(!strcmp(answer_gate("127", "What?", compound_scale, 1, NULL, 0), "pass"));
        CHECK(!strcmp(answer_gate("27,000", "What?", compound_scale, 1, NULL, 0), "pass"));
        CHECK(!strcmp(answer_gate("1500", "What?", compound_scale, 1, NULL, 0), "pass"));   /* the span's separator */
        CHECK(!strcmp(answer_gate("27", "What?", compound_scale, 1, NULL, 0), "pass"));     /* the compound alone, as before */
        CHECK(!strcmp(answer_gate("100,00", "What?", inherit, 2, NULL, 0), "not_in_span")); /* not a separator: 100 and 00 */
    }
    /* A stored turn's receipt says whether it was a refusal (withheld, or an abstention the gate read as no_answer);
     * the composer window reads this, not the reply's wording (journal/answer-cycle4_window.md). */
    CHECK(chat_receipt_refusal("--- VERIFIED SOURCE | unit=u | document=part01.md ---\nspan\n--- ANSWER GATE | verdict=not_in_span | withheld=1 | answer=five miles ---\n"));
    CHECK(chat_receipt_refusal("--- ANSWER GATE | verdict=no_answer | withheld=0 | answer=The passages do not give it ---\n"));
    CHECK(!chat_receipt_refusal("--- ANSWER GATE | verdict=pass | withheld=0 | answer=three miles ---\n--- ANSWER QUOTES | checked=1 | misquoted= ---\n"));
    CHECK(!chat_receipt_refusal("--- ANSWER GATE | verdict=pass_no_anchors | withheld=0 | answer=short and dirty ---\n"));
    CHECK(!chat_receipt_refusal("--- VERIFIED SOURCE | none | status=no_candidates | candidates=0 scored=0 ---\n")); /* no gate ran */
    CHECK(!chat_receipt_refusal(NULL) && !chat_receipt_refusal("") && !chat_receipt_refusal("--- ANSWER GATE | verdict=no_answer"));
    /* Quotation integrity (journal/answer-cycle3_coref.md, the live treatment): every quotation in the reply must be
     * in a verified span, an author turn or the question; emphasis, case, hyphens, ellipses and surrounding
     * punctuation are folded; short quoted words are mentions; an unterminated quote mark is a slip. */
    {
        const char *entail[] = { "Mr. Bennet\xE2\x80\x99s property consisted almost entirely in an estate of two thousand a year, which, "
                                 "unfortunately for his daughters, was entailed, in default of heirs male, on a distant relation;",
                                 "They were rather handsome; had been educated in one of the first private seminaries in town; had a fortune of twenty thousand pounds;" };
        const char *turns[] = { "Draft note (older): In the first draft Elizabeth walked five miles to Netherfield." };
        char *bad = NULL;
        size_t n = answer_quotations("Based on Source 2: \"Mr. Bennet\xE2\x80\x99s property consisted almost entirely in an estate of two thousand a year, "
                                     "which, unfortunately for his **sons**, was entailed, in default of heirs male, on a distant relation;\"\n\nSo the entailment affected **his sons**.",
                                     entail, 2, NULL, 0, "On whom is the estate entailed?", &bad);
        CHECK(n == 1 && bad && strstr(bad, "unfortunately for his sons"));
        free(bad);
        n = answer_quotations("From Source 2: \"which, unfortunately for his **daughters**, was entailed\" and \"They were rather handsome; ... had a fortune of twenty thousand pounds.\"",
                              entail, 2, NULL, 0, "q?", &bad);
        CHECK(n == 3 && !bad); /* emphasis folded; the ellipsis makes two pieces; the trailing period is dropped */
        free(bad);
        n = answer_quotations("> \xE2\x80\x9CThey were rather handsome; had been educated in one of the first private seminaries in town\xE2\x80\x9D\n> had a fortune of **twenty thousand** pounds\n\nSo twenty thousand.",
                              entail, 2, NULL, 0, "q?", &bad);
        CHECK(n == 2 && !bad); /* blockquote lines, curly quotes */
        free(bad);
        n = answer_quotations("Your note said \"Elizabeth walked five miles to Netherfield\"; the question was \"On whom is the estate entailed?\"",
                              entail, 2, turns, 1, "On whom is the estate entailed?", &bad);
        CHECK(n == 2 && !bad); /* the author's turn and the question are quotable */
        free(bad);
        n = answer_quotations("The words \"fortune\" and \"ill\" appear; so does \"ought\".", entail, 2, NULL, 0, "q?", &bad);
        CHECK(n == 0 && !bad); /* mentions, not quotations */
        n = answer_quotations("She said \"something that was never finished", entail, 2, NULL, 0, "q?", &bad);
        CHECK(n == 0 && !bad); /* unterminated */
        n = answer_quotations("\"a fortune of twenty thousand pounds\" and then \"a fortune of thirty thousand pounds\"", entail, 2, NULL, 0, "q?", &bad);
        CHECK(n == 2 && bad && !strcmp(bad, "a fortune of thirty thousand pounds"));
        free(bad);
        CHECK(answer_quotations(NULL, entail, 2, NULL, 0, "q?", &bad) == 0 && !bad);
        /* A straight-quoted span holding the novel's curly dialogue closes on the straight mark, not inside itself
         * (pass 4's withheld notice for the drafts case tripped a first draft of this check that way). */
        const char *dialogue[] = { "He inquired how far Netherfield was.\n\n\xE2\x80\x9C" "About a month,\xE2\x80\x9D said Elizabeth, \xE2\x80\x9Che is a man of very large property.\xE2\x80\x9D",
                                   "only three miles. I shall be back by dinner." };
        n = answer_quotations("Source 1 (part02.md): \"He inquired how far Netherfield was.\n\n\xE2\x80\x9C" "About a month,\xE2\x80\x9D said Elizabeth, \xE2\x80\x9Che is a man of very large property.\xE2\x80\x9D\"\nSource 2 (part01.md): \"only three miles. I shall be back by dinner.\"",
                              dialogue, 2, NULL, 0, "q?", &bad);
        CHECK(n == 3 && !bad);
        free(bad);
        char *reply = answer_withheld_reply_reason(names, spans, 2, "it quotes words the verified passages do not contain");
        CHECK(reply && strstr(reply, "withheld because it quotes words the verified passages do not contain.") && strstr(reply, "Source 1 (part01.md): \"only three miles"));
        free(reply);
        char *plain = answer_withheld_reply(names, spans, 2);
        CHECK(plain && strstr(plain, "withheld because it is not stated in the verified passages."));
        free(plain);
    }
    char *body = answer_claim_request("How far?", "It is three miles. <|im_end|>");
    CHECK(body);
    cJSON *j = cJSON_Parse(body);
    CHECK(j && cJSON_GetObjectItem(j, "max_tokens")->valuedouble == 96 && cJSON_GetObjectItem(j, "temperature")->valuedouble == 0);
    const cJSON *schema = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "response_format"), "json_schema"), "schema");
    CHECK(schema && cJSON_GetObjectItem(cJSON_GetObjectItem(schema, "properties"), "answer"));
    const char *user = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(j, "messages"), 1), "content")->valuestring;
    CHECK(strstr(user, "QUESTION:\nHow far?") && strstr(user, "REPLY:\nIt is three miles.") && !strstr(user, "<|im_end|>"));
    cJSON_Delete(j); free(body);
    char *answer = NULL; bool parsed = false;
    CHECK(answer_claim_parse("{\"choices\":[{\"message\":{\"content\":\"{\\\"answer\\\": \\\"three miles\\\"}\"}}]}", &answer, &parsed) && parsed && !strcmp(answer, "three miles"));
    free(answer);
    CHECK(!answer_claim_parse("{\"choices\":[{\"message\":{\"content\":\"{\\\"answer\\\": \\\"cut\"}}]}", &answer, &parsed) && !parsed);
    char *shown = answer_withheld_reply(names, spans, 2);
    CHECK(shown && strstr(shown, "do not give that") && strstr(shown, "withheld") && strstr(shown, "Source 1 (part01.md): \"only three miles. I shall be back by dinner.\"") && strstr(shown, "Source 2 (part01.md): \"Mr. Bennet still up.\""));
    free(shown);
    shown = answer_withheld_reply(NULL, NULL, 0);
    CHECK(shown && !strstr(shown, "Verified passages"));
    free(shown);
}

int main(void)
{
    test_question_parts();
    test_assign_slots();
    test_answer_gate();
    test_normalize();
    test_verify();
    test_extraction_request_and_parse();
    test_rerank_prompt_request_parse();
    test_fixture_scorer();
    test_block_and_receipt();
    test_receipt_contract();
    test_relations();
    test_pipeline_request();
    printf("answer_pipeline: %d checks, 0 failures (pure helpers and the request builder; no model, no reranker claim)\n", checks);
    return 0;
}
