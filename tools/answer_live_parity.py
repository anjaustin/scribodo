#!/usr/bin/env python3
"""Exact live-input numerical comparison, not a model-quality score.

Run prepare, llama, reference, compare SERIALly with build/venv-causal/bin/python.
The CPU reference teacher-forces the actual candidate token prefix in batches;
it is not described as an independent free-running answer after divergence.
Only cached pinned local artifacts are used.
"""
import argparse
import json
import os
from pathlib import Path
import resource
import secrets
import shutil
import subprocess
import time

from memory_causal_parity import CODE, GGUF, GGUF_SHA, ROOT, SNAPSHOT, sha, tokenizer
from memory_bedrock_probe import request

INDICES = (1, 2)  # Income and Collins, fixed before inspecting this live run.
SLOT_TOKENS = 8192  # Actual live memory-fit result, not requested 16K.


def write(path, value):
    with path.open('x') as file:
        json.dump(value, file, ensure_ascii=False, indent=2, allow_nan=False)
        file.write('\n')


def prepare(args):
    args.run.mkdir(parents=True, mode=0o700, exist_ok=False)
    tok = tokenizer()
    rows = json.loads(args.messages.read_text())
    by_seq = {row['seq']: row for row in rows}
    cases = []
    inputs = args.run / 'inputs'
    inputs.mkdir()
    for index in INDICES:
        original = args.capture / f'request-{index}.json'
        retrieval = args.capture / f'retrieval-{index}.json'
        query = json.loads(retrieval.read_text())['query']
        payload = json.loads(original.read_text())
        author, reply = by_seq[403+2*index], by_seq[404+2*index]
        if author['role'] != 'user' or author['content'] != query or reply['role'] != 'assistant':
            raise RuntimeError('Live question/reply/capture identity mismatch')
        if payload.get('chat_template_kwargs') != {'enable_thinking': False}:
            raise RuntimeError('Unexpected template settings; do not silently rematch')
        rendered = tok.apply_chat_template(payload['messages'], tokenize=False, add_generation_prompt=True,
                                          enable_thinking=False)
        ids = tok.encode(rendered, add_special_tokens=False)
        if len(ids)+args.tokens > SLOT_TOKENS:
            raise RuntimeError('Diagnostic would exceed the actual 8K slot')
        cases.append({'index': index, 'question': query, 'request': payload, 'rendered': rendered,
                      'ids': ids, 'native_reply': reply, 'max_new_tokens': args.tokens})
        for source in (original, retrieval):
            shutil.copy2(source, inputs / source.name)
    shutil.copy2(args.messages, inputs / 'messages.json')
    shutil.copy2(__file__, args.run / 'parity.py')
    write(args.run / 'cases.json', cases)
    write(args.run / 'manifest.json', {
        'method': 'Exact live messages; matched official template/token IDs; unpenalized greedy numerical diagnostic',
        'indices': list(INDICES), 'max_new_tokens': args.tokens, 'cases_sha256': sha(args.run / 'cases.json'),
        'slot_tokens': SLOT_TOKENS,
        'tool_sha256': sha(Path(__file__)), 'official_files_sha256': {name: sha(SNAPSHOT / name) for name in CODE},
        'helpers_sha256': {name: sha(ROOT / 'tools' / name) for name in ('memory_causal_parity.py', 'memory_bedrock_probe.py')},
        'inputs_sha256': {p.name: sha(p) for p in inputs.iterdir()},
        'limits': '512 diagnostic tokens by default, not the 2048-token native gate. Teacher forcing is not a reference free-running answer after divergence.',
    })


def validate(run):
    manifest = json.loads((run / 'manifest.json').read_text())
    if manifest['tool_sha256'] != sha(Path(__file__)) or manifest['cases_sha256'] != sha(run / 'cases.json'):
        raise RuntimeError('Frozen script or cases changed')
    for name, digest in manifest['official_files_sha256'].items():
        if sha(SNAPSHOT / name) != digest:
            raise RuntimeError('Official input changed: '+name)
    for name, digest in manifest['helpers_sha256'].items():
        if sha(ROOT / 'tools' / name) != digest:
            raise RuntimeError('Diagnostic helper changed: '+name)
    for name, digest in manifest['inputs_sha256'].items():
        if sha(run / 'inputs' / name) != digest:
            raise RuntimeError('Frozen input changed: '+name)
    return json.loads((run / 'cases.json').read_text())


def llama(args):
    cases = validate(args.run)
    if sha(GGUF) != GGUF_SHA:
        raise RuntimeError('Pinned model hash mismatch')
    sock = args.run / 'llama.sock'
    if len(os.fsencode(sock)) >= 104:
        raise RuntimeError('Output path too long for a Unix socket')
    keyfile = args.run / 'api.key'
    key = secrets.token_hex(32)
    with keyfile.open('x') as file:
        keyfile.chmod(0o600)
        file.write(key)
    binary = ROOT / 'build/llama/bin/llama-server'
    command = [str(binary), '-m', str(GGUF), '--host', str(sock), '--api-key-file', str(keyfile),
               '-ngl', '99', '-fa', 'off', '-ctk', 'bf16', '-ctv', 'bf16', '--jinja',
               '--reasoning-format', 'deepseek', '-np', '2', '-c', str(2*SLOT_TOKENS), '-b', '256', '-ub', '256',
               '--cache-ram', '0', '--no-webui', '--no-slots']
    write(args.run / 'llama-runtime.json', {'command': command, 'binary_sha256': sha(binary), 'gguf_sha256': GGUF_SHA})
    with (args.run / 'llama-server.log').open('x') as log:
        process = subprocess.Popen(command, stdout=log, stderr=log)
        try:
            deadline = time.monotonic()+120
            while True:
                if process.poll() is not None:
                    raise RuntimeError('Owned server exited')
                try:
                    request(sock, key, '/health')
                    break
                except (OSError, RuntimeError):
                    if time.monotonic() > deadline:
                        raise TimeoutError('Owned server startup')
                    time.sleep(.1)
            props = request(sock, key, '/props')
            write(args.run / 'llama-props.json', props)
            if props['default_generation_settings']['n_ctx'] != SLOT_TOKENS:
                raise RuntimeError('Owned server does not match actual live slot size')
            for case in cases:
                rendered = request(sock, key, '/apply-template', case['request'])['prompt']
                ids = request(sock, key, '/tokenize', {'content': rendered, 'add_special': False, 'parse_special': True})['tokens']
                parity = {'bytes_match': rendered == case['rendered'], 'tokens_match': ids == case['ids'],
                          'rendered': rendered, 'ids': ids}
                write(args.run / f"template-{case['index']}.json", parity)
                if not parity['bytes_match'] or not parity['tokens_match']:
                    raise RuntimeError('Template/token parity failed; stop inference comparison')
                start = time.monotonic()
                response = request(sock, key, '/completion', {
                    'prompt': ids, 'n_predict': case['max_new_tokens'], 'temperature': 0,
                    'top_k': 1, 'samplers': ['top_k'], 'repeat_penalty': 1.0,
                    'cache_prompt': False, 'id_slot': 0, 'return_tokens': True,
                    'n_probs': 64, 'post_sampling_probs': False})
                write(args.run / f"llama-{case['index']}.json", {'seconds': time.monotonic()-start, 'response': response})
                if not response.get('tokens') or len(response['tokens']) != len(response.get('completion_probabilities', [])):
                    raise RuntimeError('Missing token-level distributions')
                print(json.dumps({'stage': 'llama', 'index': case['index'], 'prompt_tokens': len(ids),
                                  'generated_tokens': len(response['tokens'])}), flush=True)
        finally:
            process.terminate()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
            keyfile.unlink(missing_ok=True)


def reference(args):
    cases = validate(args.run)
    import torch
    import transformers
    from transformers import AutoModelForCausalLM
    torch.set_num_threads(4)
    os.environ['HF_HUB_OFFLINE'] = '1'
    os.environ['TRANSFORMERS_OFFLINE'] = '1'
    model = AutoModelForCausalLM.from_pretrained(str(SNAPSHOT), local_files_only=True, trust_remote_code=True,
                                               dtype=torch.float32, attn_implementation='sdpa')
    model.eval()
    if {str(p.dtype) for p in model.parameters()} != {'torch.float32'} or next(model.parameters()).device.type != 'cpu':
        raise RuntimeError('Reference must be CPU F32 widened from pinned BF16 weights')
    write(args.run / 'reference-runtime.json', {'torch': torch.__version__, 'transformers': transformers.__version__,
          'device': 'CPU', 'dtype': 'float32', 'weights_sha256': sha(SNAPSHOT / 'model.safetensors'),
          'method': 'Cached official implementation, SDPA, 128-token chunked prefill; candidate-prefix teacher forcing'})

    def forward(ids, past=None):
        output = model.model(torch.tensor([ids]), past_key_values=past, use_cache=True)
        return output.last_hidden_state, output.past_key_values

    def top_rows(hidden):
        logprobs = torch.log_softmax(model.lm_head(hidden).float(), -1)[0]
        values, ids = torch.topk(logprobs, 64, dim=-1)
        return [[{'id': token, 'logprob': value} for token, value in zip(tokens, probs)]
                for tokens, probs in zip(ids.tolist(), values.tolist())]

    with torch.inference_mode():
        # Validate the chunked reference against its own full prefill before
        # attributing any later discrepancy to the independent llama runtime.
        prefix = cases[0]['ids'][:512]
        whole, _ = forward(prefix)
        full_top = top_rows(whole[:, -1:])[0]
        past = None
        for start in range(0, len(prefix), 128):
            hidden, past = forward(prefix[start:start+128], past)
        chunk_top = top_rows(hidden[:, -1:])[0]
        cmap = {row['id']: row['logprob'] for row in chunk_top}
        covered = all(row['id'] in cmap for row in full_top[:10])
        gap = max((abs(row['logprob']-cmap[row['id']]) for row in full_top[:10] if row['id'] in cmap), default=None)
        write(args.run / 'reference-chunk-check.json', {'tokens': len(prefix), 'top10_covered': covered,
                                                        'max_top10_gap': gap, 'full_top': full_top, 'chunk_top': chunk_top})
        if not covered or gap is None or gap >= .12:
            raise RuntimeError('Chunked reference failed the existing numerical bound; do not interpret comparisons')
        del whole, hidden, past
        # Check every next-token position through a chunk boundary against a
        # full prefill, not just the last prompt position.
        sample = cases[0]['ids'][:270]
        hidden, _ = forward(sample)
        expected = top_rows(hidden[:, 255:269])
        hidden, past = forward(sample[:256])
        actual = top_rows(hidden[:, -1:])
        hidden, past = forward(sample[256:269], past)
        actual.extend(top_rows(hidden))
        alignment = []
        for left, right in zip(expected, actual):
            lookup = {row['id']: row['logprob'] for row in right}
            alignment.append(all(row['id'] in lookup and abs(row['logprob']-lookup[row['id']]) < .12
                                 for row in left[:10]))
        write(args.run / 'reference-alignment-check.json', {'positions': len(alignment), 'matches': alignment})
        if len(alignment) != 14 or not all(alignment):
            raise RuntimeError('Teacher-forced position alignment failed')
        del hidden, past
        for case in cases:
            candidate = json.loads((args.run / f"llama-{case['index']}.json").read_text())['response']
            tokens = candidate['tokens']
            start_time = time.monotonic()
            past = None
            for start in range(0, len(case['ids']), 128):
                hidden, past = forward(case['ids'][start:start+128], past)
            distributions = top_rows(hidden[:, -1:])
            for start in range(0, len(tokens)-1, 128):
                hidden, past = forward(tokens[start:min(start+128, len(tokens)-1)], past)
                distributions.extend(top_rows(hidden))
            if len(distributions) != len(tokens):
                raise RuntimeError('Reference/token position alignment mismatch')
            write(args.run / f"reference-{case['index']}.json", {'seconds': time.monotonic()-start_time,
                  'teacher_forced_candidate_tokens': tokens, 'top': distributions,
                  'peak_process_rss_bytes': resource.getrusage(resource.RUSAGE_SELF).ru_maxrss})
            print(json.dumps({'stage': 'reference', 'index': case['index'], 'positions': len(distributions),
                              'seconds': time.monotonic()-start_time}), flush=True)
            del hidden, past


def compare(args):
    rows = []
    for case in validate(args.run):
        candidate = json.loads((args.run / f"llama-{case['index']}.json").read_text())['response']
        reference = json.loads((args.run / f"reference-{case['index']}.json").read_text())
        if not (candidate['tokens'] == reference['teacher_forced_candidate_tokens'] and
                len(candidate['tokens']) == len(reference['top']) == len(candidate['completion_probabilities'])):
            raise RuntimeError('Comparison prefixes or distribution lengths differ')
        comparisons = []
        common = 0
        for position, (token, rtop, cprob) in enumerate(zip(candidate['tokens'], reference['top'], candidate['completion_probabilities'])):
            cmap = {row['id']: row['logprob'] for row in cprob['top_logprobs']}
            covered = all(row['id'] in cmap for row in rtop[:10])
            gap = max((abs(row['logprob']-cmap[row['id']]) for row in rtop[:10] if row['id'] in cmap), default=None)
            agrees = rtop[0]['id'] == token
            if agrees and common == position:
                common += 1
            comparisons.append({'position': position, 'reference_top10_covered': covered, 'max_top10_gap': gap,
                                'argmax_agrees': agrees, 'reference_top1_margin': rtop[0]['logprob']-rtop[1]['logprob']})
        rows.append({'index': case['index'], 'question': case['question'], 'prompt_tokens': len(case['ids']),
                     'candidate_tokens': len(candidate['tokens']), 'shared_greedy_prefix': common,
                     'all_top10_covered': all(r['reference_top10_covered'] for r in comparisons),
                     'maximum_top10_gap': max(r['max_top10_gap'] for r in comparisons if r['max_top10_gap'] is not None),
                     'native_reply': case['native_reply'], 'controlled_llama_reply': candidate.get('content'),
                     'positions': comparisons})
    write(args.run / 'comparison.json', {'quality_pass': False, 'cases': rows,
          'limits': 'Numerical comparisons on identical token prefixes; teacher forcing is not a free-running reference answer after the first divergence. No long-context quality certification.'})
    print(json.dumps([{key: row[key] for key in ('index', 'prompt_tokens', 'candidate_tokens', 'shared_greedy_prefix', 'all_top10_covered', 'maximum_top10_gap')} for row in rows], indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['prepare', 'llama', 'reference', 'compare'])
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--capture', type=Path)
    parser.add_argument('--messages', type=Path)
    parser.add_argument('--tokens', type=int, default=512)
    args = parser.parse_args()
    args.run = args.run.resolve()
    if not 1 <= args.tokens <= 1024:
        parser.error('--tokens must be 1..1024')
    if args.stage == 'prepare' and (args.capture is None or args.messages is None):
        parser.error('prepare requires --capture and --messages')
    globals()[args.stage](args)


if __name__ == '__main__':
    main()
