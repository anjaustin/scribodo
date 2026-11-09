#!/usr/bin/env python3
"""Prove SQL and vector pre-top-k cutoffs independently using the real integration test.

Generated sources only. Requires local SQL/Qdrant, not model servers.
Build errors, crashes and timeouts are errors, not detected mutations.
"""
import json
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/answer-cutoff-mutations'


def compiler(target, changed):
    text = subprocess.check_output(['make', '--no-print-directory', '-n', '-W', changed, target], cwd=ROOT, text=True)
    commands = [shlex.split(line) for line in text.splitlines()]
    matches = [args for args in commands if '-o' in args and args[args.index('-o')+1] == target]
    if len(matches) != 1:
        raise RuntimeError(f'Expected one compiler command for {target}')
    return matches[0]


def main():
    OUT.mkdir(exist_ok=False)
    subprocess.run(['make', 'build/test_memory_chat'], cwd=ROOT, check=True)
    link = compiler('build/test_memory_chat', 'src/llm/chat.c')
    variants = [
        ('control', None, None, None),
        ('sql_cutoff_bypassed', 'src/app/memory_store.c',
         'DB_S(s->database), DB_S(s->tokenizer), DB_S(q), DB_I(before), DB_I(limit)',
         'DB_S(s->database), DB_S(s->tokenizer), DB_S(q), DB_I(INT64_MAX), DB_I(limit)'),
        ('vector_cutoff_bypassed', 'src/llm/semantic.c',
         'double bound = before > INT_MAX ? (double)INT_MAX + 1 : (double)before;',
         'double bound = (double)INT_MAX + 1;'),
    ]
    results = []
    for name, filename, old, new in variants:
        binary = OUT / name
        commands = []
        linked = list(link)
        if filename:
            source = (ROOT / filename).read_text()
            if source.count(old) != 1:
                raise RuntimeError(f'Mutation match changed: {name}')
            generated, obj = OUT / f'{name}.c', OUT / f'{name}.o'
            generated.write_text(source.replace(old, new))
            original_obj = 'build/' + filename[:-2] + '.o'
            compile_args = compiler(original_obj, filename)
            compile_args = [str(generated) if arg == filename else arg for arg in compile_args]
            compile_args[compile_args.index('-o')+1] = str(obj)
            compile_args.insert(1, '-I' + str(Path(filename).parent))
            commands.append(compile_args)
            linked = [str(obj) if arg == original_obj else arg for arg in linked]
        linked[linked.index('-o')+1] = str(binary)
        commands.append(linked)
        with (OUT / f'{name}-build.log').open('w') as log:
            for args in commands:
                subprocess.run(args, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=180, check=True)
        run = subprocess.run(['tools/watchdog.sh', '180', name, str(binary)], cwd=ROOT,
                             text=True, capture_output=True, timeout=200)
        text = run.stdout + run.stderr
        (OUT / f'{name}.log').write_text(text)
        summary = re.search(r'(\d+) checks, (\d+) failures', text)
        if not summary:
            raise RuntimeError(f'No normal summary for {name}: exit {run.returncode}')
        failures = int(summary.group(2))
        if filename is None:
            if run.returncode or failures:
                raise RuntimeError('Unchanged control failed')
        elif run.returncode != 1 or not failures:
            raise RuntimeError(f'Mutation survived or errored: {name}')
        result = {'name': name, 'checks': int(summary.group(1)), 'failures': failures,
                  'result': 'passed' if filename is None else 'killed'}
        results.append(result)
        (OUT / 'results.json').write_text(json.dumps(results, indent=2)+'\n')
        print(json.dumps(result), flush=True)


if __name__ == '__main__':
    main()
