#!/usr/bin/env python3
"""Apply the unchanged native chat text transforms to diagnostic provider replies.

Requires a local shared library built from src/llm/llm_text.c. This is not a
streaming/persistence replay; it measures the final pure text transformations.
"""
import argparse
import ctypes
import json
from pathlib import Path

from memory_causal_parity import ROOT, sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--run', type=Path, action='append', required=True)
    args = parser.parse_args()
    library = ctypes.CDLL(str(args.library.resolve()))
    library.llm_split_thinking.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
    library.llm_split_thinking.restype = None
    library.llm_strip_speaker_labels.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]
    library.llm_strip_speaker_labels.restype = ctypes.c_void_p
    libc = ctypes.CDLL(None)
    libc.free.argtypes = [ctypes.c_void_p]
    libc.free.restype = None

    def visible(raw):
        split = ctypes.c_void_p()
        library.llm_split_thinking(raw.encode(), ctypes.byref(split), None)
        if not split.value:
            raise MemoryError('Native thinking split failed')
        try:
            stripped = library.llm_strip_speaker_labels(ctypes.string_at(split), None, b'Scribodo')
        finally:
            libc.free(split)
        if not stripped:
            raise MemoryError('Native speaker cleanup failed')
        try:
            return ctypes.string_at(stripped).decode()
        finally:
            libc.free(stripped)

    # Qualify the actual loaded functions before evaluating any model reply.
    assert visible('Zoë') == 'Zoë'
    assert visible('scratch</think>\nAnswer') == 'Answer'
    assert visible('<think>scratch</think>\nAnswer') == 'Answer'
    assert visible('Scribodo: Answer') == 'Answer'
    for run in args.run:
        results = json.loads((run / 'results.json').read_text())
        rows = []
        for result in results:
            raw = result['message'].get('content') or ''
            clean = visible(raw)
            rows.append({'arm': result['arm'], 'finish_reason': result['finish_reason'],
                         'raw_characters': len(raw), 'visible_characters': len(clean),
                         'unchanged': raw == clean, 'visible': clean})
        with (run / 'native-visible.json').open('x') as file:
            json.dump({'method': 'Actual native split-thinking and speaker cleanup; default Scribodo label, no author label',
                       'native_source_sha256': sha(ROOT / 'src/llm/llm_text.c'), 'library_sha256': sha(args.library),
                       'tool_sha256': sha(Path(__file__)), 'results_sha256': sha(run / 'results.json'),
                       'native_smoke_controls': 4, 'rows': rows}, file, ensure_ascii=False, indent=2)
            file.write('\n')
        print(json.dumps({'run': str(run), 'changed_arms': [r['arm'] for r in rows if not r['unchanged']]}))


if __name__ == '__main__':
    main()
