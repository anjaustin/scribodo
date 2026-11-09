#!/usr/bin/env python3
"""Controls for memory red-team remediations; no services or models required.

Every mutant must compile and finish with a normal assertion-failure summary.
Crashes, timeouts and compiler errors are errors, not successful detections.
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def compiler(target, changed):
    text = subprocess.check_output(['make', '--no-print-directory', '-n', '-W', changed, target], cwd=ROOT, text=True)
    commands = [shlex.split(line) for line in text.splitlines()]
    matches = [args for args in commands if '-o' in args and args[args.index('-o')+1] == target]
    if len(matches) != 1:
        raise RuntimeError(f'Expected one compiler command for {target}')
    return matches[0]


def history_fallback(source):
    first = source.index('        char *visible = NULL;')
    last = source.index('        bool newest_user =', first)
    return source[:first] + '''        char *visible = NULL;
        if (msgs[i].role == CHAT_ASSISTANT) llm_split_thinking(msgs[i].content, &visible, NULL);
        if (msgs[i].role == CHAT_ASSISTANT && !msgs[i].is_edit) {
            char *unlabelled = llm_strip_speaker_labels(visible ? visible : msgs[i].content, names ? names->author : NULL, self);
            if (unlabelled) { free(visible); visible = unlabelled; }
        }
        char *clean = llm_neutralize(visible ? visible : msgs[i].content);
        free(visible);
''' + source[last:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/redteam-memory-mutations')
    out = parser.parse_args().output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    suites = ['test_persistence_faults', 'test_memory_vectors', 'test_llm']
    subprocess.run(['make', *['build/'+name for name in suites]], cwd=ROOT, check=True)
    variants = [(name+'_control', name, None, None) for name in suites] + [
        ('sampling_publication_bypassed', 'test_persistence_faults', 'src/llm/chat.c',
         ('char *out = ok ? cJSON_PrintUnformatted(j) : NULL;', 'char *out = cJSON_PrintUnformatted(j);')),
        ('history_fallback_restored', 'test_persistence_faults', 'src/llm/chat.c', history_fallback),
        ('invalid_sequence_accepted', 'test_memory_vectors', 'src/llm/semantic.c',
         ('         (lease->unit.kind == MEMORY_SOURCE_CHAT && (lease->unit.sequence < 0 || lease->unit.sequence > INT_MAX)) ||\n', '')),
        ('failed_user_keeps_orphan_reply', 'test_llm', 'src/llm/chat.c',
         ('while (first < n && (msgs[first].error || msgs[first].role != CHAT_USER)) first++;',
          'while (first < n && msgs[first].role != CHAT_USER) first++;')),
    ]
    results = []
    for name, suite, filename, mutation in variants:
        binary = out / name
        if filename is None:
            run_binary = ROOT / 'build' / suite
        else:
            source = (ROOT / filename).read_text()
            if callable(mutation):
                changed = mutation(source)
            else:
                old, new = mutation
                if source.count(old) != 1:
                    raise RuntimeError(f'Mutation no longer matches: {name}')
                changed = source.replace(old, new)
            generated = out / f'{name}.c'
            generated.write_text(changed)
            link = compiler('build/'+suite, filename)
            commands = []
            if suite == 'test_persistence_faults':
                link = [str(generated) if arg == filename else arg for arg in link]
                link.insert(1, '-I'+str(Path(filename).parent))
            else:
                original_obj = 'build/'+filename[:-2]+'.o'
                obj = out / f'{name}.o'
                build = compiler(original_obj, filename)
                build = [str(generated) if arg == filename else arg for arg in build]
                build[build.index('-o')+1] = str(obj)
                build.insert(1, '-I'+str(Path(filename).parent))
                commands.append(build)
                link = [str(obj) if arg == original_obj else arg for arg in link]
            link[link.index('-o')+1] = str(binary)
            commands.append(link)
            with (out / f'{name}-build.log').open('w') as log:
                for args in commands:
                    subprocess.run(args, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=180, check=True)
            run_binary = binary
        run = subprocess.run(['tools/watchdog.sh', '180', name, str(run_binary)], cwd=ROOT,
                             capture_output=True, text=True, timeout=200)
        text = run.stdout + run.stderr
        (out / f'{name}.log').write_text(text)
        summary = re.search(r'(\d+) checks, (\d+) failures', text)
        if not summary:
            raise RuntimeError(f'Missing normal summary: {name}, exit {run.returncode}')
        failures = int(summary.group(2))
        if filename is None:
            if run.returncode or failures:
                raise RuntimeError(f'Unchanged control failed: {name}')
        elif run.returncode != 1 or not failures:
            raise RuntimeError(f'Mutation survived or errored: {name}')
        row = {'name': name, 'checks': int(summary.group(1)), 'failures': failures,
               'result': 'passed' if filename is None else 'killed'}
        results.append(row)
        (out / 'results.json').write_text(json.dumps(results, indent=2)+'\n')
        print(json.dumps(row), flush=True)


if __name__ == '__main__':
    main()
