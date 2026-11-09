#!/usr/bin/env python3
"""Build a diagnostic-only app: real preparation, frozen visible replies, no generation.

This reconstructs wire requests using the prior visible transcript. It does not
claim byte-identical retrieval ordering or capture of the original run. Production
sources are not edited. Output contains public fixture text and is kept local.
"""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/answer-bedrock-capture'


def main():
    OUT.mkdir(exist_ok=False)
    fixture = json.loads((ROOT / 'build/product-memory-use/after/messages.json').read_text())
    replies = [row for row in fixture if row['role'] == 'assistant']
    assert len(replies) == 7
    (OUT / 'replies.json').write_text(json.dumps(replies, ensure_ascii=False))
    source = (ROOT / 'src/llm/chat.c').read_text()
    anchor = '    StreamCtx c = { .s = s, .rc = 1 };'
    assert source.count(anchor) == 1
    diagnostic = r'''
    /* Diagnostic fixture ONLY: preserve prior visible history without generation. */
    static unsigned capture_serial;
    const unsigned serial = capture_serial++;
    const char *directory = getenv("SCRIBODO_ANSWER_CAPTURE");
    char path[4096];
    if (!directory || serial >= 7) { finish(s, true, "Capture fixture missing"); return NULL; }
    snprintf(path, sizeof(path), "%s/request-%u.json", directory, serial);
    FILE *file = fopen(path, "wx");
    bool saved = file && fputs(s->request, file) >= 0;
    if (file && fclose(file)) saved = false;
    snprintf(path, sizeof(path), "%s/replies.json", directory);
    file = fopen(path, "rb");
    char *data = NULL;
    if (file && !fseek(file, 0, SEEK_END)) {
        long bytes = ftell(file);
        if (bytes > 0 && bytes < 1024*1024 && !fseek(file, 0, SEEK_SET)) {
            data = malloc((size_t)bytes + 1);
            if (data) {
                if (fread(data, 1, (size_t)bytes, file) == (size_t)bytes) data[bytes] = 0;
                else { free(data); data = NULL; }
            }
        }
    }
    if (file) fclose(file);
    cJSON *replies = data ? json_parse(data) : NULL;
    free(data);
    cJSON *reply = cJSON_GetArrayItem(replies, (int)serial);
    cJSON *content = cJSON_GetObjectItem(reply, "content");
    if (saved && cJSON_IsString(content)) {
        publish(s, content->valuestring);
        cJSON *truncated = cJSON_GetObjectItem(reply, "truncated");
        s->last_truncated = cJSON_IsNumber(truncated) && truncated->valueint;
        finish(s, false, NULL);
    } else finish(s, true, "Capture fixture failed");
    cJSON_Delete(replies);
    return NULL;
'''
    generated = OUT / 'chat.c'
    generated.write_text(source.replace(anchor, diagnostic + '\n' + anchor))
    recipe = '''
.PHONY: answer-capture
answer-capture: $(CORE_OBJS) $(LLM_OBJS) $(APP_OBJS) $(RAYLIB_LIB)
\t$(CC) $(CFLAGS) -Isrc/llm $(INCLUDES) -c build/answer-bedrock-capture/chat.c -o build/answer-bedrock-capture/chat.o
\t$(CC) $(CFLAGS) $(filter-out $(BUILD)/src/llm/chat.o,$(CORE_OBJS) $(LLM_OBJS) $(APP_OBJS)) build/answer-bedrock-capture/chat.o $(RAYLIB_LIB) -o build/answer-bedrock-capture/scribodo $(LDLIBS) $(MARIADB_LIBS) -lm -lpthread
'''
    (OUT / 'manifest.json').write_text(json.dumps({
        'method': 'Diagnostic reconstruction: production preparation; frozen prior visible replies replace inference',
        'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
        'generated_sha256': hashlib.sha256(generated.read_bytes()).hexdigest(),
        'replies_sha256': hashlib.sha256((OUT / 'replies.json').read_bytes()).hexdigest(),
    }, indent=2) + '\n')
    with (OUT / 'build.log').open('w') as log:
        subprocess.run(['make', '--no-print-directory', '-f', 'Makefile', '-f', '-', 'answer-capture'],
                       cwd=ROOT, input=recipe, text=True, stdout=log, stderr=subprocess.STDOUT, check=True)


if __name__ == '__main__':
    main()
