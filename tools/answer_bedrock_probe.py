#!/usr/bin/env python3
"""Serial, local inference ablations on reconstructed real-app requests.

Preserves every request, rendered template and raw response. No production
configuration is changed. The shorter diagnostic generation cap is explicit.
"""
import argparse
import copy
import hashlib
import http.client
import json
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
REV = 'cd0c74c575b5c416f3011b5b4c298077f26ca8c2'
SHA = '39158a79f56edb85e0c4252ce6447b8d7883ef82ffedd02f0cda7c84da5e9f68'


class UnixHTTP(http.client.HTTPConnection):
    def __init__(self, path):
        super().__init__('localhost', timeout=180)
        self.path = str(path)

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)


def call(sock, endpoint, data=None):
    connection = UnixHTTP(sock)
    try:
        connection.request('GET' if data is None else 'POST', endpoint,
                           body=None if data is None else json.dumps(data).encode(),
                           headers={'Content-Type': 'application/json'})
        response = connection.getresponse()
        body = response.read()
        if response.status != 200:
            raise RuntimeError(f'{endpoint}: HTTP {response.status}: {body[:1000]!r}')
        return json.loads(body)
    finally:
        connection.close()


def cases(capture, index, phase='components'):
    original = json.loads((capture / f'request-{index}.json').read_text())
    question = json.loads((ROOT / 'build/product-memory-use/after/messages.json').read_text())[1 + 2*index]['content']
    # Separate using the exact production boundary, not a guessed source suffix.
    boundary = "\nAuthor's current request (not part of the retrieved sources):\n"
    content = original['messages'][-1]['content']
    reference, foreground = content.split(boundary, 1)
    assert foreground.endswith(question)
    heads = foreground[:-len(question)]
    system = original['messages'][0]
    history = original['messages'][1:-1]
    def variant(name, tail, prior=()):
        request = copy.deepcopy(original)
        request.update(stream=False, max_tokens=256, temperature=0, cache_prompt=False)
        request['messages'] = [copy.deepcopy(system), *copy.deepcopy(list(prior)), {'role': 'user', 'content': tail}]
        return name, request
    if phase in ('evidence', 'attribution'):
        if index != 1:
            raise ValueError('Evidence isolation currently registered for income only')
        blocks = list(re.finditer(r'--- SOURCE (\d+) \|([^\n]*)---\n(.*?)\n--- END SOURCE ---', reference, re.S))
        assert len(blocks) == 7
        target = [block for block in blocks if 'four or five thousand' in block.group(3)]
        assert len(target) == 1
        opening = (ROOT / 'build/manuscripts/pride-and-prejudice/parts/part01.md').read_text().encode()[:1856].decode()
        assert 'A single man of large fortune; four or five thousand a year.' in opening
        # Oracle controls use known answer-bearing passages only for diagnosis.
        # This is not a production selector and is not scored as retrieval reach.
        plain = '\n\n'.join(block.group(3) for block in blocks)
        late = target[0].group(3)
        reordered = reference[:blocks[0].start()] + '\n\n'.join(
            block.group(0) for block in blocks if block != target[0]) + '\n\n' + target[0].group(0)
        replaced = reference[:target[0].start(3)] + opening + reference[target[0].end(3):]
        if phase == 'attribution':
            first_request = json.loads((capture / 'request-0.json').read_text())
            opening_ref = first_request['messages'][-1]['content'].split(boundary, 1)[0]
            actual = [block for block in re.finditer(r'--- SOURCE (\d+) \|([^\n]*)---\n(.*?)\n--- END SOURCE ---', opening_ref, re.S)
                      if 'A single man of large fortune; four or five thousand a year.' in block.group(3)]
            assert len(actual) == 1 and actual[0].group(3).strip() == opening.strip()
            replacement = re.sub(r'^--- SOURCE \d+', '--- SOURCE ' + target[0].group(1), actual[0].group(0))
            bound = reference[:target[0].start()] + replacement + reference[target[0].end():]
            darcy = [block for block in blocks if 'ten thousand a year' in block.group(3)]
            assert len(darcy) == 1
            unrelated = blocks[0]
            assert 'thousand' not in unrelated.group(3)
            def removed(block):
                return reference[:block.start()] + reference[block.end():] + boundary + question
            return original, [variant('packet_correct_binding', bound + boundary + question),
                              variant('full_correct_binding', bound + boundary + foreground, history),
                              variant('remove_darcy', removed(darcy[0])),
                              variant('remove_unrelated', removed(unrelated)),
                              variant('target_and_darcy', target[0].group(3) + '\n\n' + darcy[0].group(3) + '\n\n' + question),
                              variant('target_and_unrelated', target[0].group(3) + '\n\n' + unrelated.group(3) + '\n\n' + question),
                              variant('heads_with_history', foreground, history)]
        return original, [variant('oracle_opening_unit', opening + '\n\n' + question),
                          variant('oracle_later_unit', late + '\n\n' + question),
                          variant('packet_plain', plain + '\n\n' + question),
                          variant('packet_target_last', reordered + boundary + question),
                          variant('packet_opening_instead', replaced + boundary + question),
                          variant('heads_only', foreground),
                          variant('sources_only_longer', reference + boundary + question)]
    sentence = {
        0: '“My dear Mr. Bennet,” said his lady to him one day, “have you heard that Netherfield Park is let at last?”',
        1: '“Is he married or single?” “Oh, single, my dear, to be sure! A single man of large fortune; four or five thousand a year. What a fine thing for our girls!”',
        2: 'Lady Catherine de Bourgh had very lately given Mr. Collins a living.',
        3: 'Colonel Fitzwilliam, who led the way, was about thirty, not handsome, but in person and address most truly the gentleman.',
    }.get(index)
    variants = []
    if sentence:
        # Minimal control is an explicit diagnostic quotation, not a substituted
        # production retrieval result or evidence of whole-book coverage.
        variants.append(variant('minimal', sentence + '\n\n' + question))
    if index == 2:
        blocks = re.findall(r'--- SOURCE \d+ \|[^\n]*---\n(.*?)\n--- END SOURCE ---', reference, re.S)
        relevant = [body for body in blocks if 'has very lately given him a living' in body]
        assert len(relevant) == 1
        variants.append(variant('oracle_actual_unit', relevant[0] + '\n\n' + question))
    variants += [variant('sources_only', reference + boundary + question),
                 variant('full_cold', content, history),
                 variant('no_history', content),
                 variant('no_document_heads', reference + boundary + question, history),
                 variant('sources_after_heads', heads + reference + boundary + question, history)]
    cached = copy.deepcopy(next(req for name, req in variants if name == 'full_cold'))
    cached['cache_prompt'] = True
    cold_index = next(i for i, (name, _) in enumerate(variants) if name == 'full_cold')
    variants.insert(cold_index + 1, ('full_cached', cached))
    # A single sampling-only comparison, otherwise identical to the cold arm.
    sampled = copy.deepcopy(next(req for name, req in variants if name == 'full_cold'))
    sampled['temperature'] = original['temperature']
    variants.append(('full_sampled', sampled))
    return original, variants


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--index', type=int, default=1)
    parser.add_argument('--capture', type=Path, default=ROOT / 'build/answer-bedrock-capture')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--arms', nargs='*')
    parser.add_argument('--phase', choices=['components', 'evidence', 'attribution'], default='components')
    parser.add_argument('--max-tokens', type=int, default=256)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    original, variants = cases(args.capture, args.index, args.phase)
    for _, request in variants:
        request['max_tokens'] = args.max_tokens
    if args.arms:
        variants = [(name, req) for name, req in variants if name in args.arms]
        if len(variants) != len(args.arms):
            raise ValueError('Unknown/duplicate arm')
    model = Path.home() / f'.cache/huggingface/hub/models--LiquidAI--LFM3-1.7B-GGUF/snapshots/{REV}/LFM3-1.7B-BF16.gguf'
    if not model.exists():
        model = Path.home() / 'Library/Application Support/Scribodo/models/LFM3-1.7B-BF16.gguf'
    digest = hashlib.sha256()
    with model.open('rb') as file:
        for block in iter(lambda: file.read(1024*1024), b''):
            digest.update(block)
    if digest.hexdigest() != SHA:
        raise RuntimeError('Pinned model hash mismatch')
    temp = Path(tempfile.mkdtemp(prefix='ab-', dir='/private/var/folders/zq/1xg6mmv1627btc7ghn90_hlh0000gn/T/opencode'))
    sock = temp / 's.sock'
    command = [str(ROOT / 'build/llama/bin/llama-server'), '-m', str(model), '-ngl', '99', '-fa', 'off',
               '-ctk', 'bf16', '-ctv', 'bf16', '--jinja', '--reasoning-format', 'deepseek',
               '-np', '2', '-c', '32768', '-b', '256', '-ub', '256', '--cache-ram', '0',
               '--host', str(sock), '--no-webui', '--no-slots']
    manifest = {'model_sha256': SHA, 'command': command, 'case_index': args.index,
                'server_sha256': hashlib.sha256(Path(command[0]).read_bytes()).hexdigest(),
                'probe_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'capture_request_sha256': hashlib.sha256(json.dumps(original, sort_keys=True).encode()).hexdigest(),
                'diagnostic_cap': args.max_tokens, 'default_temperature': 0, 'cache_prompt': False,
                'phase': args.phase,
                'method': 'Reconstructed application request; component ablations, no production changes'}
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    results = []
    with (args.output / 'server.log').open('w') as log:
        server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 120
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
            (args.output / 'props.json').write_text(json.dumps(call(sock, '/props'), indent=2)+'\n')
            for name, request in variants:
                (args.output / f'{name}-request.json').write_text(json.dumps(request, ensure_ascii=False, indent=2)+'\n')
                rendered = call(sock, '/apply-template', request)
                (args.output / f'{name}-template.json').write_text(json.dumps(rendered, ensure_ascii=False, indent=2)+'\n')
                start = time.monotonic()
                response = call(sock, '/v1/chat/completions', request)
                (args.output / f'{name}-response.json').write_text(json.dumps(response, ensure_ascii=False, indent=2)+'\n')
                choice = response['choices'][0]
                row = {'arm': name, 'wall_seconds': time.monotonic()-start,
                       'finish_reason': choice['finish_reason'], 'usage': response.get('usage'),
                       'message': choice['message']}
                results.append(row)
                (args.output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2)+'\n')
                print(json.dumps(row, ensure_ascii=False), flush=True)
        finally:
            server.terminate()
            try:
                server.wait(timeout=20)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


if __name__ == '__main__':
    main()
