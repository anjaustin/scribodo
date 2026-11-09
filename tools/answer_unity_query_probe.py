#!/usr/bin/env python3
"""Compare retrieval intent across one SQL corpus and one vector index.

Generated diagnostic sources only. Index embeddings, eligibility, fusion,
hydration, admission and budgets are unchanged. No generation is performed.
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import subprocess

import memory_reach_eval as reach

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/answer-unity-query'
BASE = ROOT / 'build/memory-reach/runs/scribodo_test_reach_74605_b87de8bb89e09cce'
INSTRUCTIONS = {
    'topic': 'Instruct: Given a statement from a work of fiction, retrieve established facts about the same people, places or things\nQuery: ',
    'answer': 'Instruct: Given a question about a work of fiction, retrieve passages that answer the question\nQuery: ',
    'search': 'Instruct: Given a web search query, retrieve relevant passages that answer the query\nQuery: ',
}


def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def main():
    OUT.mkdir(mode=0o700, exist_ok=False)
    semantic = (ROOT / 'src/llm/semantic.c').read_text()
    anchor = '    for (size_t i = 0; i < n; i++) {\n        cJSON *item = NULL;'
    assert semantic.count(anchor) == 1
    prefix = '    const char *probe_instruction = k_query_instruction;\n'
    prefix += '    const char *probe_arm = getenv("SCRIBODO_QUERY_INTENT_PROBE");\n'
    for arm in ('answer', 'search'):
        prefix += f'    if (probe_arm && !strcmp(probe_arm, "{arm}")) probe_instruction = {json.dumps(INSTRUCTIONS[arm])};\n'
    semantic = semantic.replace(anchor, prefix + anchor)
    semantic = semantic.replace('strlen(k_query_instruction) + strlen(clip)', 'strlen(probe_instruction) + strlen(clip)')
    semantic = semantic.replace('"%s%s", k_query_instruction, clip', '"%s%s", probe_instruction, clip')
    # Leave the document index and its namespace fingerprint unchanged: only
    # query vectors vary in this paired diagnostic, never indexed vectors.
    (OUT / 'semantic.c').write_text(semantic)
    driver = (ROOT / 'tools/memory_reach_eval.c').read_text()
    anchor = '    memset(&trace, 0, sizeof(trace));'
    assert driver.count(anchor) == 1
    driver = driver.replace(anchor, '    NEED(setenv("SCRIBODO_QUERY_INTENT_PROBE", str(task, "probe_arm"), 1) == 0);\n' + anchor)
    anchor = '    cJSON_AddStringToObject(j, "event", "selection");'
    assert driver.count(anchor) == 1
    driver = driver.replace(anchor, anchor + '\n    cJSON_AddStringToObject(j, "probe_arm", str(task, "probe_arm"));')
    (OUT / 'driver.c').write_text(driver)
    recipe = '''
.PHONY: answer-unity
answer-unity: $(CORE_OBJS) $(LLM_OBJS) $(LLAMA_BIN)
\t$(CC) $(CFLAGS) -Isrc/llm $(INCLUDES) -c build/answer-unity-query/semantic.c -o build/answer-unity-query/semantic.o
\t$(CC) $(CFLAGS) $(INCLUDES) build/answer-unity-query/driver.c $(filter-out $(BUILD)/src/llm/semantic.o,$(sort $(CORE_OBJS) $(LLM_OBJS))) build/answer-unity-query/semantic.o -o build/answer-unity-query/driver $(MARIADB_LIBS) -lm -lpthread
'''
    with (OUT / 'build.log').open('w') as log:
        subprocess.run(['make', '--no-print-directory', '-f', 'Makefile', '-f', '-', 'answer-unity'],
                       input=recipe, text=True, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    original = json.loads((BASE / 'input.json').read_text())
    fixture = json.loads((BASE / 'answer-key.json').read_text())
    data = copy.deepcopy(original)
    data['tasks'] = []
    # Counterbalance arm order while keeping all questions unchanged.
    arms = list(INSTRUCTIONS)
    for i, task in enumerate(original['tasks']):
        for arm in arms[i % 3:] + arms[:i % 3]:
            data['tasks'].append({**task, 'id': task['id'] + '__' + arm, 'probe_arm': arm})
    save(OUT / 'input.json', data)
    save(OUT / 'answer-key.json', fixture)  # never passed to native code
    name = f'scribodo_test_reach_{os.getpid()}_{secrets.token_hex(8)}'
    (OUT / 'owned-database').write_text(name)
    save(OUT / 'registration.json', {
        'protocol': 'retrieval-intent-paired-dev-v1', 'arms': INSTRUCTIONS,
        'same_index': True, 'same_sql_sources': True, 'question_count_per_arm': len(original['tasks']),
        'targets': {'exact_document_key_passages_admitted': '14/14', 'chat_tasks_fully_sourced': '24/24',
                    'source_range_violations': 0, 'reference_budget_violations': 0},
        'limits': 'Development source-reach experiment, not faithful-answer acceptance. Only query input instructions change. Topic fingerprint retained to hold indexed vectors fixed; not a production migration.',
    })
    frozen = OUT / 'frozen-sources'
    for source in [*sorted((ROOT / 'src').rglob('*.c')), *sorted((ROOT / 'src').rglob('*.h')),
                   ROOT / 'tools/answer_unity_query_probe.py', ROOT / 'tools/memory_reach_eval.py']:
        dest = frozen / source.relative_to(ROOT)
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, dest)
    save(OUT / 'manifest.json', {'database': name, 'base': str(BASE),
        'sha256': {str(p.relative_to(OUT)): hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in OUT.rglob('*') if p.is_file()}})
    env = dict(os.environ, SCRIBODO_LLAMA_SERVER=str(ROOT / 'build/llama/bin/llama-server'))
    for key in ('SCRIBODO_MEMORY_EXCERPTS', 'SCRIBODO_MEMORY_SMALL_UNITS', 'SCRIBODO_QUERY_INTENT_PROBE'):
        env.pop(key, None)
    rc, cleanup = 1, None
    try:
        subprocess.run(['tools/services.sh', 'testdb', 'create', name], cwd=ROOT, env=env, check=True)
        with (OUT / 'driver.stdout.log').open('w') as stdout, (OUT / 'driver.stderr.log').open('w') as stderr:
            rc = subprocess.run(['/usr/bin/caffeinate', '-i', str(OUT / 'driver'), name, str(OUT),
                                 str(OUT / 'input.json'), str(OUT / 'events.jsonl')], cwd=ROOT, env=env,
                                stdout=stdout, stderr=stderr).returncode
    finally:
        cleanup = subprocess.run(['tools/services.sh', 'testdb', 'drop', name], cwd=ROOT, env=env).returncode
    events = [json.loads(line) for line in (OUT / 'events.jsonl').read_text().splitlines()]
    results = {}
    for arm in arms:
        selected = []
        for event in events:
            if event.get('event') != 'selection':
                selected.append(event)
            elif event['probe_arm'] == arm:
                selected.append({**event, 'task': event['task'].rsplit('__', 1)[0]})
        report = reach.summarize(fixture, selected, rc, reach.sha(OUT / 'manifest.json'))
        report['database_cleanup_returncode'] = cleanup
        save(OUT / f'{arm}-report.json', report)
        docs = [r for r in report['per_task'] if r['category'] == 'document_fact']
        chats = [r for r in report['per_task'] if r['category'] != 'document_fact' and r['reach_admitted'] is not None]
        results[arm] = {'gates': report['gates'], 'database_cleanup_returncode': cleanup,
                        'exact_document_discovered': sum(r['whole_sentence_candidates'] == 1 for r in docs),
                        'exact_document_admitted': sum(r['whole_sentence_admitted'] == 1 for r in docs),
                        'document_total': len(docs), 'aliases_admitted': sum(r['reach_admitted'] == 1 for r in docs),
                        'chat_fully_sourced': sum(r['reach_admitted'] == 1 for r in chats), 'chat_total': len(chats)}
    save(OUT / 'results.json', results)
    print(json.dumps(results, indent=2), flush=True)
    if rc or cleanup or not all(all(row['gates'].values()) for row in results.values()):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
