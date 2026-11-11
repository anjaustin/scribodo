#!/usr/bin/env python3
"""Window replay: how the composer answers when earlier refusals leave its history.

    python3 tools/answer_window_replay.py --capture LABEL=DIR [--capture ...] --output DIR

The verified pipeline's composer sees a six-message window of the conversation
(chat_build_request_pipeline). Pass 4 and cycles 3 and 4 recorded that an
earlier refusal in that window, a withheld notice or the composer's own
abstention, is imitated on later turns, including turns whose verified spans
hold the answer (journal/answer-cycle3_coref.md, journal/answer-cycle4_mars.md).
This tool measures the window construction offline, on the actual requests a
live run sent (the live-capture `request-N.json` files), by regenerating each
request whose window holds a refusal under four arms:

- `recorded`: the request as sent (also the replay-fidelity control against
  the recorded `content-N.txt`);
- `marker`: every refusal assistant turn replaced by one short neutral marker;
- `drop_turn`: every refusal assistant turn removed, its question kept;
- `drop_pair`: every refusal assistant turn removed with the question before it.

Same model, same server flags as the app, same sampling fields including the
seed; only the history differs. No production change; the arms are what a
window rule could do, and this measures them before any ships. Each reply is
scored mechanically for whether it is itself a refusal and whether it carries
the fixture's designated passages for that question; the replies are kept for
reading. Deterministic given the server.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from answer_bedrock_probe import ROOT, SHA, call, private_socket  # noqa: E402
from memory_causal_parity import GGUF  # noqa: E402

REFUSAL = [re.compile(p, re.I) for p in (
    r'^\W*the supplied (manuscript )?passages do not',
    r'an answer was withheld',
    r'an answer was not found',
    r'^\W*no verified passage',
    r"^\W*i don'?t have a verified passage",
    r'^\W*i cannot answer from the provided',
)]
MARKER = '(No answer was given.)'
FIXTURES = ['tests/fixtures/answer_boundary/questions.json', 'tests/fixtures/answer_boundary/princess-of-mars.json']


def is_refusal(text):
    head = (text or '')[:240]
    return any(p.search(head) for p in REFUSAL)


def normalize(text):
    text = (text or '').lower().replace('’', "'").replace('“', '"').replace('”', '"')
    return re.sub(r'[^a-z0-9]+', ' ', text).strip()


def variants(request, marker=MARKER):
    """The four arms for one captured request, or None when its window holds no refusal."""
    msgs = request['messages']
    refusals = [i for i, m in enumerate(msgs) if m.get('role') == 'assistant' and is_refusal(m.get('content'))]
    if not refusals:
        return None
    base = copy.deepcopy(request)
    base['stream'] = False
    arms = {'recorded': base}
    with_marker = copy.deepcopy(base)
    for i in refusals:
        with_marker['messages'][i]['content'] = marker
    arms['marker'] = with_marker
    drop_turn = copy.deepcopy(base)
    drop_turn['messages'] = [m for i, m in enumerate(msgs) if i not in refusals]
    arms['drop_turn'] = drop_turn
    gone = set(refusals)
    for i in refusals:
        if i > 0 and msgs[i - 1].get('role') == 'user':
            gone.add(i - 1)
    drop_pair = copy.deepcopy(base)
    drop_pair['messages'] = [m for i, m in enumerate(msgs) if i not in gone]
    arms['drop_pair'] = drop_pair
    return refusals, arms


def load_cases():
    cases = {}
    for f in FIXTURES:
        for c in json.loads((ROOT / f).read_text())['cases']:
            cases[c['question']] = {'id': c['id'], 'fixture': f, 'passages': [d['passage'] for d in c['designated']]}
    return cases


def score(reply, case):
    norm = normalize(reply)
    passages = case['passages'] if case else []
    return {'refusal': is_refusal(reply),
            'passages_present': sum(1 for p in passages if normalize(p) in norm),
            'passages': len(passages)}


def requests_of(capture):
    files = sorted(capture.glob('request-*.json'), key=lambda p: int(p.stem.split('-')[1]))
    for f in files:
        n = int(f.stem.split('-')[1])
        recorded = capture / f'content-{n}.txt'
        yield n, json.loads(f.read_text()), recorded.read_text() if recorded.exists() else None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--capture', action='append', required=True, help='LABEL=DIR of a live capture (request-N.json, content-N.txt)')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--marker', default=MARKER)
    parser.add_argument('--arms', default='recorded,marker,drop_turn,drop_pair')
    parser.add_argument('--model', type=Path, default=GGUF, help='the pinned BF16 GGUF (hash checked)')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    digest = hashlib.sha256()
    with args.model.open('rb') as file:
        for block in iter(lambda: file.read(1 << 20), b''):
            digest.update(block)
    if digest.hexdigest() != SHA:
        raise SystemExit('Pinned model hash mismatch')
    cases = load_cases()
    arms_wanted = args.arms.split(',')
    plan = []
    for spec in args.capture:
        label, directory = spec.split('=', 1)
        for n, request, recorded in requests_of(Path(directory)):
            v = variants(request, args.marker)
            if not v:
                continue
            refusals, arms = v
            question = request['messages'][-1]['content']
            plan.append({'label': label, 'request': n, 'question': question, 'case': cases.get(question),
                         'refusal_turns': refusals, 'recorded': recorded, 'arms': {a: arms[a] for a in arms_wanted}})
    (args.output / 'plan.json').write_text(json.dumps([{k: p[k] for k in p if k != 'arms'} for p in plan], ensure_ascii=False, indent=2) + '\n')
    print(f'{len(plan)} requests with a refusal in the window; {sum(len(p["arms"]) for p in plan)} generations', flush=True)
    results = []
    with private_socket() as sock:
        command = [str(ROOT / 'build/llama/bin/llama-server'), '-m', str(args.model), '-ngl', '99', '-fa', 'off',
                   '-ctk', 'bf16', '-ctv', 'bf16', '--jinja', '--reasoning-format', 'deepseek',
                   '-np', '2', '-c', '32768', '-b', '256', '-ub', '256', '--cache-ram', '0',
                   '--host', str(sock), '--no-webui', '--no-slots']
        manifest = {'model_sha256': SHA, 'command': command, 'marker': args.marker, 'arms': arms_wanted,
                    'server_sha256': hashlib.sha256(Path(command[0]).read_bytes()).hexdigest(),
                    'tool_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    'captures': args.capture,
                    'method': 'Captured composer requests regenerated with the history window changed; no production change'}
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        with (args.output / 'server.log').open('w') as log:
            server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 180
                while True:
                    if server.poll() is not None:
                        raise RuntimeError('Server exited during startup')
                    try:
                        call(sock, '/health')
                        break
                    except (OSError, RuntimeError):
                        if time.monotonic() > deadline:
                            raise TimeoutError('Server startup')
                        time.sleep(.2)
                for p in plan:
                    for arm, request in p['arms'].items():
                        name = f"{p['label']}-r{p['request']}-{arm}"
                        (args.output / f'{name}-request.json').write_text(json.dumps(request, ensure_ascii=False, indent=2) + '\n')
                        start = time.monotonic()
                        response = call(sock, '/v1/chat/completions', request, timeout=600)
                        (args.output / f'{name}-response.json').write_text(json.dumps(response, ensure_ascii=False, indent=2) + '\n')
                        reply = response['choices'][0]['message'].get('content') or ''
                        row = {'label': p['label'], 'request': p['request'], 'arm': arm, 'case': (p['case'] or {}).get('id'),
                               'question': p['question'][:80], 'wall_seconds': round(time.monotonic() - start, 1),
                               'finish_reason': response['choices'][0]['finish_reason'],
                               'reply': reply, **score(reply, p['case'])}
                        if arm == 'recorded' and p['recorded'] is not None:
                            row['matches_recorded'] = reply.strip() == p['recorded'].strip()
                        results.append(row)
                        (args.output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2) + '\n')
                        print(json.dumps({k: row[k] for k in row if k != 'reply'}, ensure_ascii=False), flush=True)
            finally:
                server.terminate()
                try:
                    server.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
    # Summary table: per request, per arm: refusal? passages present / total
    print('\nlabel  req case                       | ' + ' | '.join(f'{a:>20s}' for a in arms_wanted))
    for p in plan:
        cells = []
        for a in arms_wanted:
            r = next((x for x in results if x['label'] == p['label'] and x['request'] == p['request'] and x['arm'] == a), None)
            cells.append(f"{'REFUSES' if r['refusal'] else 'answers':>8s} {r['passages_present']}/{r['passages']}{' =rec' if r.get('matches_recorded') else ''}" if r else '-')
        print(f"{p['label']:6s} {p['request']:3d} {(p['case'] or {}).get('id', '?'):26s} | " + ' | '.join(f'{c:>20s}' for c in cells))


if __name__ == '__main__':
    main()
