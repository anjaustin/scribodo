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
    setenv("SCRIBODO_ANSWER_BLOCK", "message", 1); /* the pass 3a shape, checked first; the default follows */
    char *req = chat_build_request_pipeline(msgs, 3, &names, 8192, fixed_count, NULL, &first, preface);
    unsetenv("SCRIBODO_ANSWER_BLOCK");
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
    /* Pass 3b placement is the default: the block in the system message, the newest words without a preface;
     * SCRIBODO_ANSWER_BLOCK=message restores the pass 3a shape. */
    unsetenv("SCRIBODO_ANSWER_BLOCK");
    char *placed = chat_build_request_pipeline(msgs, 3, &names, 8192, fixed_count, NULL, &first, preface);
    CHECK(placed);
    cJSON *pj = cJSON_Parse(placed);
    const char *psys = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(pj, "messages"), 0), "content")->valuestring;
    const char *pnew = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(pj, "messages"), 3), "content")->valuestring;
    CHECK(strstr(psys, "do not guess") && strstr(psys, preface) && strstr(psys, "do not guess") < strstr(psys, preface));
    CHECK(!strcmp(pnew, "How far is the walk to Netherfield?"));
    cJSON_Delete(pj);
    free(placed);
    setenv("SCRIBODO_ANSWER_BLOCK", "message", 1);
    char *inline_block = chat_build_request_pipeline(msgs, 3, &names, 8192, fixed_count, NULL, &first, preface);
    unsetenv("SCRIBODO_ANSWER_BLOCK");
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
    test_pipeline_request();
    printf("answer_pipeline: %d checks, 0 failures (pure helpers and the request builder; no model, no reranker claim)\n", checks);
    return 0;
}
