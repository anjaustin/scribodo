#!/usr/bin/env python3
"""Full-prefill control for the frozen chunked live reference, serial execution."""
import argparse
import json
import os
from pathlib import Path
import time

from answer_live_parity import validate, write
from memory_causal_parity import SNAPSHOT, sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    args = parser.parse_args()
    cases = validate(args.run)
    os.environ['HF_HUB_OFFLINE'] = '1'
    os.environ['TRANSFORMERS_OFFLINE'] = '1'
    import torch
    from transformers import AutoModelForCausalLM
    torch.set_num_threads(4)
    model = AutoModelForCausalLM.from_pretrained(str(SNAPSHOT), local_files_only=True, trust_remote_code=True,
                                               dtype=torch.float32, attn_implementation='sdpa')
    model.eval()
    results = []
    with torch.inference_mode():
        for case in cases:
            candidate = json.loads((args.run / f"llama-{case['index']}.json").read_text())['response']
            chunked = json.loads((args.run / f"reference-{case['index']}.json").read_text())
            ids = case['ids'] + candidate['tokens'][:-1]
            start = time.monotonic()
            output = model.model(torch.tensor([ids]), use_cache=False)
            hidden = output.last_hidden_state[:, len(case['ids'])-1:]
            logprobs = torch.log_softmax(model.lm_head(hidden).float(), -1)[0]
            values, tokens = torch.topk(logprobs, 64, dim=-1)
            tops = [[{'id': token, 'logprob': value} for token, value in zip(t, v)]
                    for t, v in zip(tokens.tolist(), values.tolist())]
            if len(tops) != len(chunked['top']) or len(tops) != len(candidate['tokens']):
                raise RuntimeError('Full-prefill position alignment mismatch')
            checks = []
            for position, (full, chunk, llama) in enumerate(zip(tops, chunked['top'], candidate['completion_probabilities'])):
                cmap = {r['id']: r['logprob'] for r in chunk}
                lmap = {r['id']: r['logprob'] for r in llama['top_logprobs']}
                covered = all(r['id'] in cmap for r in full[:10])
                llama_covered = all(r['id'] in lmap for r in full[:10])
                checks.append({'position': position, 'chunk_top10_covered': covered,
                               'chunk_max_top10_gap': max((abs(r['logprob']-cmap[r['id']]) for r in full[:10] if r['id'] in cmap), default=None),
                               'llama_top10_covered': llama_covered,
                               'llama_max_top10_gap': max((abs(r['logprob']-lmap[r['id']]) for r in full[:10] if r['id'] in lmap), default=None),
                               'chunk_argmax_matches': full[0]['id'] == chunk[0]['id'],
                               'llama_argmax_matches': full[0]['id'] == candidate['tokens'][position]})
            row = {'index': case['index'], 'seconds': time.monotonic()-start, 'positions': len(checks),
                   'all_chunk_top10_covered': all(r['chunk_top10_covered'] for r in checks),
                   'chunk_maximum_top10_gap': max(r['chunk_max_top10_gap'] for r in checks),
                   'chunk_argmax_disagreements': sum(not r['chunk_argmax_matches'] for r in checks),
                   'llama_maximum_top10_gap': max(r['llama_max_top10_gap'] for r in checks),
                   'llama_argmax_disagreements': sum(not r['llama_argmax_matches'] for r in checks)}
            write(args.run / f"reference-full-{case['index']}.json", {'summary': row, 'top': tops, 'checks': checks,
                  'tool_sha256': sha(Path(__file__)), 'method': 'CPU F32 SDPA, entire prompt plus candidate prefix in one forward, no cache'})
            results.append(row)
            print(json.dumps(row), flush=True)
            del output, hidden, logprobs, values, tokens
    write(args.run / 'reference-full-summary.json', results)


if __name__ == '__main__':
    main()
