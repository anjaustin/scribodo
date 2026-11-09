#!/usr/bin/env python3
"""Build a diagnostic-only app: real preparation, frozen visible replies, no generation.

This reconstructs wire requests using the prior visible transcript. It does not
claim byte-identical retrieval ordering or capture of the original run. Production
sources are not edited. Output contains public fixture text and is kept local.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import shlex

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/answer-bedrock-capture'


def compiler(target, changed):
    dry = subprocess.check_output(['make', '--no-print-directory', '-n', '-W', changed, target], cwd=ROOT, text=True)
    commands = [shlex.split(line) for line in dry.splitlines()]
    matches = [args for args in commands if '-o' in args and args[args.index('-o')+1] == target]
    if len(matches) != 1:
        raise RuntimeError(f'Expected one compiler command for {target}')
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=OUT)
    parser.add_argument('--transcript', type=Path, default=ROOT / 'build/product-memory-use/after/messages.json')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    transcript = args.transcript.read_bytes()
    fixture = json.loads(transcript)
    replies = [row for row in fixture if row['role'] == 'assistant']
    if len(replies) != 7:
        raise ValueError('Expected seven frozen assistant replies')
    by_seq = {row['seq']: row for row in fixture}
    for reply in replies:
        question = by_seq[reply['seq']-1]
        if question['role'] != 'user':
            raise ValueError('Frozen reply lacks its author question')
        reply['question'] = question['content']
    (out / 'transcript.json').write_bytes(transcript)
    (out / 'replies.json').write_text(json.dumps(replies, ensure_ascii=False))
    source = (ROOT / 'src/llm/chat.c').read_text()
    anchor = '    StreamCtx c = { .s = s, .rc = 1 };'
    assert source.count(anchor) == 1
    diagnostic = r'''
    /* Diagnostic fixture ONLY: preserve prior visible history without generation. */
    static unsigned capture_serial;
    const unsigned serial = capture_serial++;
    const char *directory = getenv("SCRIBODO_ANSWER_CAPTURE");
    char path[4096];
    fprintf(stderr, "DIAGNOSTIC REPLAY: request=%u; frozen reply, no generation; answer assertions are not inference evidence\n", serial);
    if (!directory || serial >= 7 || s->via_remote) { finish(s, true, "Capture fixture missing or unsupported remote"); return NULL; }
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
    cJSON *question = cJSON_GetObjectItem(reply, "question");
    pthread_mutex_lock(&s->mu);
    bool same_question = cJSON_IsString(question) && s->user_index < s->count &&
        s->msgs[s->user_index].content && !strcmp(question->valuestring, s->msgs[s->user_index].content);
    pthread_mutex_unlock(&s->mu);
    if (saved && same_question && cJSON_IsString(content)) {
        publish(s, content->valuestring);
        cJSON *truncated = cJSON_GetObjectItem(reply, "truncated");
        s->last_truncated = cJSON_IsNumber(truncated) && truncated->valueint;
        finish(s, false, NULL);
    } else finish(s, true, "Capture fixture failed");
    cJSON_Delete(replies);
    return NULL;
'''
    generated = out / 'chat.c'
    generated.write_text(source.replace(anchor, diagnostic + '\n' + anchor))
    (out / 'manifest.json').write_text(json.dumps({
        'method': 'Diagnostic reconstruction: production preparation; frozen prior visible replies replace inference',
        'generation_performed': False,
        'ui_answer_assertions': 'Frozen reply playback, NOT model quality evidence',
        'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
        'generated_sha256': hashlib.sha256(generated.read_bytes()).hexdigest(),
        'replies_sha256': hashlib.sha256((out / 'replies.json').read_bytes()).hexdigest(),
        'transcript_sha256': hashlib.sha256(transcript).hexdigest(),
    }, indent=2) + '\n')
    with (out / 'build.log').open('w') as log:
        subprocess.run(['make', 'all'], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        obj = out / 'chat.o'
        build = compiler('build/src/llm/chat.o', 'src/llm/chat.c')
        build = [str(generated) if arg == 'src/llm/chat.c' else arg for arg in build]
        build[build.index('-o')+1] = str(obj)
        build.insert(1, '-Isrc/llm')
        link = compiler('build/scribodo', 'src/llm/chat.c')
        link = [str(obj) if arg == 'build/src/llm/chat.o' else arg for arg in link]
        link[link.index('-o')+1] = str(out / 'scribodo')
        for command in (build, link):
            subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)


if __name__ == '__main__':
    main()
