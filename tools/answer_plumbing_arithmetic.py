#!/usr/bin/env python3
"""Matched-prefix runtime/cache controls; no prompt or model substitution.

Prepare from the frozen live parity run, then run each configuration serially.
Every query is one unpenalized next-token distribution on identical token IDs.
"""
import argparse
import json
from pathlib import Path
import secrets
import subprocess
import time

from answer_live_parity import validate, write
from memory_bedrock_probe import request
from memory_causal_parity import GGUF, GGUF_SHA, ROOT, sha

CONFIGS = {'metal_bf16': (99, 'bf16'), 'cpu_bf16': (0, 'bf16'),
           'metal_f32': (99, 'f32'), 'cpu_f32': (0, 'f32')}


def completion(ids):
    return {'prompt': ids, 'n_predict': 1, 'temperature': 0, 'top_k': 1,
            'samplers': ['top_k'], 'repeat_penalty': 1.0, 'cache_prompt': False,
            'id_slot': 0, 'return_tokens': True, 'n_probs': 64, 'post_sampling_probs': False}


def backend_flags(config):
    ngl, kv = CONFIGS[config]
    flags = ['-ngl', str(ngl), '-ctk', kv, '-ctv', kv]
    if ngl == 0:
        flags += ['--device', 'none', '--no-op-offload', '--no-kv-offload']
    return flags


def prepare(args):
    cases = validate(args.source)
    points = []
    for case in cases:
        index = case['index']
        candidate = json.loads((args.source / f'llama-{index}.json').read_text())['response']
        full = json.loads((args.source / f'reference-full-{index}.json').read_text())
        compare = next(c for c in json.loads((args.source / 'comparison.json').read_text())['cases'] if c['index'] == index)
        worst = max(full['checks'], key=lambda r: r['llama_max_top10_gap'])['position']
        for position in sorted({0, worst, compare['shared_greedy_prefix']}):
            points.append({'index': index, 'position': position,
                           'ids': case['ids']+candidate['tokens'][:position],
                           'reference_top': full['top'][position],
                           'original_incremental_top': candidate['completion_probabilities'][position]['top_logprobs']})
    args.run.mkdir(parents=True, mode=0o700, exist_ok=False)
    write(args.run / 'points.json', points)
    write(args.run / 'manifest.json', {'source': str(args.source.resolve()), 'tool_sha256': sha(Path(__file__)),
          'points_sha256': sha(args.run / 'points.json'), 'gguf_sha256': GGUF_SHA,
          'slot_tokens': 8192, 'configs': CONFIGS,
          'source_sha256': {p.name: sha(p) for p in args.source.iterdir() if p.name in
              ('cases.json', 'comparison.json', 'reference-full-1.json', 'reference-full-2.json', 'llama-1.json', 'llama-2.json')},
          'limits': 'Six preselected points from known discrepant cases; cold one-token prefill vs prior incremental decode. Not an answer-quality or full-vocabulary parity test.'})
    with (args.run / 'runner.py').open('xb') as file:
        file.write(Path(__file__).read_bytes())


def frozen(args):
    m = json.loads((args.run / 'manifest.json').read_text())
    if m['tool_sha256'] != sha(Path(__file__)) or m['points_sha256'] != sha(args.run / 'points.json') or sha(GGUF) != GGUF_SHA:
        raise RuntimeError('Frozen tool, prefixes or model changed')
    for name, digest in m['source_sha256'].items():
        if sha(Path(m['source']) / name) != digest:
            raise RuntimeError('Reference evidence changed')
    return json.loads((args.run / 'points.json').read_text())


def runtime(args):
    points = frozen(args)
    out = args.run / args.stage
    out.mkdir(mode=0o700, exist_ok=False)
    sock = out / 's.sock'
    if len(str(sock).encode()) >= 104:
        raise ValueError('Socket path too long')
    key = secrets.token_hex(32)
    keyfile = out / 'api.key'
    with keyfile.open('x') as file:
        keyfile.chmod(0o600); file.write(key)
    ngl, kv = CONFIGS[args.stage]
    binary = ROOT / 'build/llama/bin/llama-server'
    command = [str(binary), '-m', str(GGUF), '--host', str(sock), '--api-key-file', str(keyfile),
               *backend_flags(args.stage), '-fa', 'off', '--jinja', '--reasoning-format', 'deepseek', '-v',
               '-np', '2', '-c', '16384', '-b', '256', '-ub', '256', '--cache-ram', '0', '--no-webui', '--no-slots']
    write(out / 'runtime.json', {'command': command, 'binary_sha256': sha(binary), 'kv_type': kv, 'ngl': ngl})
    rows = []
    with (out / 'server.log').open('x') as log:
        process = subprocess.Popen(command, stdout=log, stderr=log)
        try:
            deadline = time.monotonic()+180
            while True:
                if process.poll() is not None:
                    raise RuntimeError('Owned runtime exited')
                try:
                    request(sock, key, '/health'); break
                except (OSError, RuntimeError):
                    if time.monotonic() > deadline:
                        raise TimeoutError('Owned runtime startup')
                    time.sleep(.1)
            props = request(sock, key, '/props')
            write(out / 'props.json', props)
            if props['default_generation_settings']['n_ctx'] != 8192:
                raise RuntimeError('Slot mismatch')
            for point in points:
                payload = completion(point['ids'])
                result = request(sock, key, '/completion', payload)
                name = f"{point['index']}-{point['position']}"
                write(out / f'{name}.json', {'request': payload, 'response': result})
                probs = result.get('completion_probabilities', [])
                if len(probs) != 1 or result.get('tokens') != [probs[0]['id']]:
                    raise RuntimeError('Returned token/distribution alignment mismatch')
                if result.get('timings', {}).get('cache_n', 0) != 0 or result['timings']['prompt_n'] != len(point['ids']):
                    raise RuntimeError('Prefix was not fully cold-prefilled')
                top = probs[0]['top_logprobs']; cmap = {r['id']: r['logprob'] for r in top}
                refs = point['reference_top'][:10]
                covered = all(r['id'] in cmap for r in refs)
                gap = max((abs(r['logprob']-cmap[r['id']]) for r in refs if r['id'] in cmap), default=None)
                rows.append({'index': point['index'], 'position': point['position'], 'prompt_tokens': len(point['ids']),
                             'reference_top10_covered': covered, 'max_top10_gap': gap,
                             'argmax_matches_reference': top[0]['id'] == refs[0]['id'], 'top': top})
                print(json.dumps({k:v for k,v in rows[-1].items() if k != 'top'}), flush=True)
        finally:
            process.terminate()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
            keyfile.unlink(missing_ok=True)
    write(out / 'summary.json', rows)


def compare(args):
    points = frozen(args)
    summaries = {config: json.loads((args.run / config / 'summary.json').read_text()) for config in CONFIGS}
    result = []
    for point in points:
        rows = {config: next(r for r in values if (r['index'], r['position']) == (point['index'], point['position']))
                for config, values in summaries.items()}
        baseline = {r['id']: r['logprob'] for r in point['original_incremental_top']}
        refs = point['reference_top'][:10]
        result.append({'index': point['index'], 'position': point['position'],
                       'original_incremental_max_top10_gap': max(abs(r['logprob']-baseline[r['id']]) for r in refs),
                       'cold_prefix_configs': {config: {k:v for k,v in row.items() if k != 'top'} for config, row in rows.items()}})
    write(args.run / 'comparison.json', result)
    print(json.dumps(result, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['prepare', *CONFIGS, 'compare'])
    parser.add_argument('--source', type=Path, default=ROOT / 'build/alp1')
    parser.add_argument('--run', type=Path, required=True)
    args = parser.parse_args(); args.run = args.run.resolve()
    if args.stage == 'prepare':
        prepare(args)
    elif args.stage == 'compare':
        compare(args)
    else:
        runtime(args)


if __name__ == '__main__':
    main()
