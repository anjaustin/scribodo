/* Verified-answer pipeline: the pure parts (normalization, byte verification,
 * extraction request and reply, reranker prompt, request and reply parsing,
 * the fixture scorer, the verified block and receipt) and the pipeline request
 * builder. No models, no services, no network. */
#include "llm/answer_pipeline.h"
#include "llm/chat.h"
#include "llm/rerank.h"
#include "cJSON.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
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
    CHECK(n && !strcmp(n, "\"mr. bennet's property wasentailed on a distant relation.\"")); /* underscores are dropped, as the classifier drops emphasis */
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
    /* Exact, with emphasis and curly quotes folded. */
    CHECK(answer_verify_span("Elizabeth said it was *only three miles*.", k_unit, &v, &status) && !strcmp(status, "verified"));
    CHECK(v && !strcmp(v, "Elizabeth said it was *only three miles*."));
    free(v); v = NULL;
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
    CHECK(block && !strcmp(block, "Verified passage from the manuscript, quoted exactly (Source 1, part01.md):\n\"only three miles. I shall be back by dinner.\"\n\n"));
    char *receipt = answer_receipt(&r);
    CHECK(receipt && strstr(receipt, "unit=unit-abc") && strstr(receipt, "document=part01.md") && strstr(receipt, "status=verified"));
    CHECK(strstr(receipt, "only three miles. I shall be back by dinner.")); /* the UI script checks the passage here */
    free(block); free(receipt);
    answer_pipeline_result_free(&r);
    CHECK(!r.has_top && !r.span);

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
    char *req = chat_build_request_pipeline(msgs, 3, &names, 8192, fixed_count, NULL, &first, preface);
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
    /* The production builder is unchanged by the refactor: no preface, no composer sentence. */
    bool truncated = true;
    char *plain = chat_build_request_selected(msgs, 3, NULL, &names, 8192, fixed_count, NULL, &truncated, &first);
    CHECK(plain && !truncated && !strstr(plain, "answer from that passage alone") && !strstr(plain, "Verified passage"));
    free(plain);
}

int main(void)
{
    test_normalize();
    test_verify();
    test_extraction_request_and_parse();
    test_rerank_prompt_request_parse();
    test_fixture_scorer();
    test_block_and_receipt();
    test_pipeline_request();
    printf("answer_pipeline: %d checks, 0 failures (pure helpers and the request builder; no model, no reranker claim)\n", checks);
    return 0;
}
