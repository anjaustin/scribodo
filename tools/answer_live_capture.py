#!/usr/bin/env python3
"""Build a local diagnostic app that records REAL requests and generation.

No frozen replies or alternative prompts. Generated instrumentation records
candidate locators, final JSON, observed SSE lines before chat parsing, accumulated
content and outcomes. File I/O can affect timings; this is not a latency benchmark.
Requires an explicitly selected, private owned capture directory at runtime.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from answer_request_capture import compiler

ROOT = Path(__file__).resolve().parents[1]


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(f'Instrumentation anchor no longer unique: {old[:80]}')
    return source.replace(old, new)


HELPERS = r'''
/* Diagnostic-only live capture; never compiled into production. */
static _Thread_local const char *live_directory;
static _Thread_local unsigned live_serial;
static _Thread_local bool live_capture_ok;
static atomic_uint live_next;

FILE *scribodo_live_open(const char *kind, const char *extension)
{
    if (!live_directory) return NULL;
    char path[4096];
    int size = snprintf(path, sizeof(path), "%s/%s-%u.%s", live_directory, kind, live_serial, extension);
    if (size < 0 || (size_t)size >= sizeof(path)) { live_capture_ok = false; return NULL; }
    int fd = open(path, O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW, 0600);
    FILE *file = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!file) { if (fd >= 0) close(fd); live_capture_ok = false; }
    return file;
}

void scribodo_live_json(const char *kind, cJSON *value, bool complete)
{
    char *encoded = complete ? cJSON_PrintUnformatted(value) : NULL;
    FILE *file = encoded ? scribodo_live_open(kind, "json") : NULL;
    if (!file || fputs(encoded, file) < 0) live_capture_ok = false;
    if (file && fclose(file)) live_capture_ok = false;
    free(encoded); cJSON_Delete(value);
}

static bool live_begin(void)
{
    live_directory = getenv("SCRIBODO_ANSWER_LIVE_CAPTURE");
    live_serial = atomic_fetch_add(&live_next, 1);
    live_capture_ok = true;
    struct stat status;
    return live_directory && !lstat(live_directory, &status) && S_ISDIR(status.st_mode) &&
           status.st_uid == getuid() && !(status.st_mode & 077);
}

static void live_text(const char *kind, const char *extension, const char *text)
{
    FILE *file = scribodo_live_open(kind, extension);
    if (!file || fputs(text ? text : "", file) < 0) live_capture_ok = false;
    if (file && fclose(file)) live_capture_ok = false;
}
'''


def instrument_chat(source):
    source = replace_once(source, '#define _DARWIN_C_SOURCE\n',
                          '#define _DARWIN_C_SOURCE\n#include <fcntl.h>\n#include <sys/stat.h>\n')
    source = replace_once(source, 'static const char *k_persona =', HELPERS + '\nstatic const char *k_persona =')
    source = replace_once(source, '    char err[256];\n} StreamCtx;',
                          '    char err[256];\n    FILE *live_stream;\n} StreamCtx;')
    source = replace_once(source, '    StreamCtx *c = ctx;\n    int rc = chat_parse_sse_line', r'''    StreamCtx *c = ctx;
    if (c->live_stream && (fwrite(line, 1, len, c->live_stream) != len || fputc('\n', c->live_stream) == EOF)) {
        snprintf(c->err, sizeof(c->err), "Live capture write failed");
        c->rc = -1; return false;
    }
    int rc = chat_parse_sse_line''')
    source = replace_once(source, 'static void *server_worker(void *arg)\n{\n    ChatSession *s = arg;', r'''static void *server_worker(void *arg)
{
    ChatSession *s = arg;
    if (!live_begin() || s->via_remote) { finish(s, true, "Live capture requires a private owned directory and local inference"); return NULL; }''')
    source = replace_once(source, '    StreamCtx c = { .s = s, .rc = 1 };', r'''    live_text("request", "json", s->request);
    StreamCtx c = { .s = s, .rc = 1, .live_stream = scribodo_live_open("stream", "sse") };
    if (!live_capture_ok || !c.live_stream) {
        if (c.live_stream) fclose(c.live_stream);
        finish(s, true, "Live capture incomplete before inference"); return NULL;
    }
    fprintf(stderr, "LIVE_CAPTURE request=%u; real local generation, unchanged request\n", live_serial);''')
    # This anchor is in server_worker only: preserve content before free/finish,
    # i.e. before final speaker-label cleanup and persistence callbacks.
    anchor = '    http_result_free(&r);\n    free(c.raw);\n    s->last_truncated = c.truncated;'
    source = replace_once(source, anchor, r'''    if (fclose(c.live_stream)) live_capture_ok = false;
    live_text("content", "txt", c.raw);
    cJSON *outcome = cJSON_CreateObject();
    bool outcome_ok = outcome && cJSON_AddNumberToObject(outcome, "request", live_serial) &&
        cJSON_AddNumberToObject(outcome, "http_status", r.status) &&
        cJSON_AddNumberToObject(outcome, "parse_rc", c.rc) &&
        cJSON_AddBoolToObject(outcome, "transport_ok", ok) &&
        cJSON_AddBoolToObject(outcome, "cancelled", cancelled) &&
        cJSON_AddBoolToObject(outcome, "truncated", c.truncated) &&
        cJSON_AddBoolToObject(outcome, "capture_complete", live_capture_ok) &&
        cJSON_AddStringToObject(outcome, "error", msg);
    scribodo_live_json("outcome", outcome, outcome_ok);
    if (!live_capture_ok) { error = true; snprintf(msg, sizeof(msg), "Live capture incomplete"); }
    http_result_free(&r);
    free(c.raw);
    s->last_truncated = c.truncated;''')
    return source


def instrument_planner(source):
    declaration = r'''
/* Linked only with the generated live-capture chat object. */
extern FILE *scribodo_live_open(const char *kind, const char *extension);
extern void scribodo_live_json(const char *kind, cJSON *value, bool complete);
static cJSON *live_candidates(const MemoryCandidate *list, size_t count)
{
    cJSON *array = cJSON_CreateArray();
    for (size_t i = 0; array && i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        bool ok = item && cJSON_AddStringToObject(item, "unit_id", list[i].unit_id) &&
            cJSON_AddStringToObject(item, "text_hash", list[i].text_hash) &&
            cJSON_AddNumberToObject(item, "score", list[i].score) && cJSON_AddItemToArray(array, item);
        if (!ok) { cJSON_Delete(item); cJSON_Delete(array); return NULL; }
    }
    return array;
}
'''
    source = replace_once(source, 'void memory_plan_free(MemoryPlan *plan)', declaration + '\nvoid memory_plan_free(MemoryPlan *plan)')
    anchor = '    Ranked ranked[MEMORY_CANDIDATES] = {0};'
    trace = r'''    cJSON *trace = cJSON_CreateObject();
    cJSON *lex = live_candidates(lexical, nl), *sem = live_candidates(semantic, ns);
    bool trace_ok = trace && cJSON_AddStringToObject(trace, "query", query) &&
        cJSON_AddNumberToObject(trace, "before", (double)older_than) &&
        cJSON_AddStringToObject(trace, "scope_id", plan.scope.id) &&
        cJSON_AddStringToObject(trace, "database", plan.scope.database) &&
        cJSON_AddNumberToObject(trace, "generation", (double)plan.scope.generation) &&
        cJSON_AddBoolToObject(trace, "lexical_ok", plan.lexical_ok) &&
        cJSON_AddBoolToObject(trace, "semantic_ok", plan.semantic_ok);
    if (trace_ok && lex && cJSON_AddItemToObject(trace, "lexical", lex)) lex = NULL; else trace_ok = false;
    if (trace_ok && sem && cJSON_AddItemToObject(trace, "semantic", sem)) sem = NULL; else trace_ok = false;
    cJSON_Delete(lex); cJSON_Delete(sem);
    scribodo_live_json("retrieval", trace, trace_ok);
'''
    return replace_once(source, anchor, trace + '\n' + anchor)


QUERY_INSTRUCTION_ANCHOR = '''static const char *k_query_instruction =
    "Instruct: Given a statement from a work of fiction, retrieve established facts about the same people, places "
    "or things\\nQuery: ";
'''


def instrument_semantic(source, instruction):
    """A query-instruction variant of the embedding client for an old-versus-new live retrieval measurement.
    'production' leaves the file untouched; 'none' empties the query prefix. The vector index fingerprint
    includes the instruction, so a variant build indexes its own collection."""
    if instruction == 'production':
        return source
    if instruction != 'none':
        raise ValueError(f'Unknown query instruction variant: {instruction}')
    return replace_once(source, QUERY_INSTRUCTION_ANCHOR, 'static const char *k_query_instruction = "";  /* diagnostic variant: no query prefix */\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--query-instruction', choices=['production', 'none'], default='production',
                        help="embedding query prefix: the production string, or none (diagnostic variant)")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, mode=0o700, exist_ok=False)
    chat = (ROOT / 'src/llm/chat.c').read_text()
    planner = (ROOT / 'src/llm/context_planner.c').read_text()
    semantic = (ROOT / 'src/llm/semantic.c').read_text()
    (out / 'chat.c').write_text(instrument_chat(chat))
    (out / 'context_planner.c').write_text(instrument_planner(planner))
    generated = ['chat.c', 'context_planner.c']
    if args.query_instruction != 'production':
        (out / 'semantic.c').write_text(instrument_semantic(semantic, args.query_instruction))
        generated.append('semantic.c')
    manifest = {'method': 'Live production-path diagnostic; real generation, no replay or alternative prompt',
                'git_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                'query_instruction': args.query_instruction,
                'source_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                  for p in (ROOT / 'src').rglob('*') if p.suffix in ('.c', '.h', '.m')},
                'generated_sha256': {name: hashlib.sha256((out / name).read_bytes()).hexdigest() for name in generated},
                'tool_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'limits': 'Observed SSE lines, not HTTP framing; trace I/O may affect timings; public fixture only'}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    with (out / 'build.log').open('w') as log:
        subprocess.run(['make', 'all'], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        objects = {}
        for filename in generated:
            original = 'src/llm/'+filename
            target = 'build/src/llm/'+filename[:-2]+'.o'
            obj = out / (filename[:-2]+'.o')
            command = compiler(target, original)
            command = [str(out / filename) if arg == original else arg for arg in command]
            command[command.index('-o')+1] = str(obj)
            command.insert(1, '-Isrc/llm')
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
            objects[target] = str(obj)
        link = compiler('build/scribodo', 'src/llm/chat.c')
        link = [objects.get(arg, arg) for arg in link]
        link[link.index('-o')+1] = str(out / 'scribodo')
        subprocess.run(link, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    manifest['binary_sha256'] = hashlib.sha256((out / 'scribodo').read_bytes()).hexdigest()
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')


if __name__ == '__main__':
    main()
