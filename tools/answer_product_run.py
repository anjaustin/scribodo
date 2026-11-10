#!/usr/bin/env python3
"""Drive a real-app question script over the real manuscript and preserve the evidence.

Usage: python3 tools/answer_product_run.py <label> <app-binary>
  SCRIBODO_PRODUCT_SCRIPT       UI script (default tests/ui/store/18_manuscript_questions.scribo)
  SCRIBODO_ANSWER_LIVE_CAPTURE  private 0700 directory for a live-capture app build (optional)

Writes build/product-memory-use/<label>/ with the frozen binary, manifest, app log,
exported messages (and source units when capturing), a copy of the work directory,
then drops the disposable test database. Promoted from the untracked
build/product-memory-use-run.py used by MEMORY-022 and the cycle journals; the
only change is tolerant decoding of the UI log.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

root = Path.cwd()
label, executable = sys.argv[1:]
destination = root / 'build/product-memory-use' / label
destination.mkdir(parents=True, exist_ok=False)
executable = Path(executable).resolve()
frozen = destination / 'scribodo'
shutil.copy2(executable, frozen)
frozen.chmod(0o500)
script = root / os.environ.get('SCRIBODO_PRODUCT_SCRIPT', 'tests/ui/store/18_manuscript_questions.scribo')
manifest = {'label': label, 'executable': str(executable),
            'binary_sha256': hashlib.sha256(executable.read_bytes()).hexdigest(),
            'script_sha256': hashlib.sha256(script.read_bytes()).hexdigest(),
            'seed': 42, 'words': 30000, 'per_category': 3,
            'source_sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in (root / 'src').rglob('*') if p.suffix in ('.c', '.h')}}
if label.endswith('before'):
    for name in ('src/llm/context_planner.c', 'src/llm/context_planner.h', 'src/app/memory_store.h'):
        original = root / 'build/product-memory-baseline' / name
        manifest['source_sha256'][str(root / name)] = hashlib.sha256(original.read_bytes()).hexdigest()
    manifest['baseline'] = 'HEAD planner and plan headers; chat.c recompiled against those original headers'
    if label != 'before':
        for name in ('src/app/memory_document_store.c', 'src/app/memory_documents.h'):
            manifest['source_sha256'][str(root / name)] = hashlib.sha256((root / 'build/product-memory-baseline' / name).read_bytes()).hexdigest()
        manifest['baseline'] += '; HEAD document ingestion; trace-only chat instrumentation and scripted cancellation'
(destination / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
environment = {**os.environ, 'SCRIBODO_SOAK_APP': str(frozen), 'SCRIBODO_SEED': '42',
               'SCRIBODO_LLAMA_SERVER': str(root / 'build/llama/bin/llama-server'),
               'SCRIBODO_MEMORY_TRACE': '1', 'SCRIBODO_MEMORY_EXCERPTS': '0'}
started = time.monotonic()
with (destination / 'app.log').open('w') as log:
    result = subprocess.run(['tools/manuscript_baseline.sh', '--keep', '--real', '--words', '30000',
                             '--context', '16384',
                             '--per-category', '3', '--manuscript', 'build/manuscripts/pride-and-prejudice/parts',
                             '--script', str(script.relative_to(root))], env=environment,
                            stdout=log, stderr=subprocess.STDOUT)
output = (destination / 'app.log').read_text(errors='replace')  # the UI log may cut a multibyte status line
kept = re.search(r'kept database=(scribodo_test_manuscript_[a-z0-9_]+) work=(\S+)', output)
summary = {'exit_code': result.returncode, 'wall_seconds': time.monotonic() - started,
           'full_replies_saved': False, 'database_cleanup_exit': None}
if kept:
    database, work = kept.groups()
    query = ("SELECT HEX(JSON_OBJECT('seq',seq,'role',role,'content',content,'error',error,"
             "'truncated',truncated,'memory_sources',CONVERT(memory_sources USING utf8mb4) COLLATE utf8mb4_bin)) AS payload "
             "FROM messages WHERE seq>=402 ORDER BY seq;\n")
    export = subprocess.run(['tools/services.sh', 'testdb', 'sql', database], input=query,
                            text=True, capture_output=True)
    (destination / 'messages-export.log').write_text(export.stdout)
    if export.returncode == 0:
        messages = [json.loads(bytes.fromhex(line).decode('utf-8')) for line in export.stdout.splitlines()
                    if re.fullmatch(r'[0-9A-Fa-f]+', line)]
        (destination / 'messages.json').write_text(json.dumps(messages, ensure_ascii=False, indent=2) + '\n')
        summary['full_replies_saved'] = True
        summary['messages'] = len(messages)
    else:
        (destination / 'export-error.log').write_text(export.stderr)
    if environment.get('SCRIBODO_ANSWER_LIVE_CAPTURE'):
        fields = {
            'unit_id': 'u.id', 'text_hash': 'u.text_hash', 'source_hash': 'u.source_hash',
            'scope_id': 'u.scope_id', 'source_kind': 'u.source_kind', 'role': 'u.role',
            'document_name': 'd.name', 'revision_id': 'u.revision_id', 'text': 'u.text',
        }
        pairs = [f"'{key}',CONVERT({column} USING utf8mb4) COLLATE utf8mb4_bin" for key, column in fields.items()]
        pairs += ["'sequence',u.sequence", "'byte_start',u.byte_start", "'tokens',u.tokens", "'generation',u.generation",
                  "'source_hash_current',u.source_hash=COALESCE(r.source_hash,m.source_hash)",
                  "'source_bytes_current',CAST(u.text AS BINARY)=SUBSTRING(CAST(COALESCE(r.content,m.content) AS BINARY),u.byte_start+1,OCTET_LENGTH(u.text))"]
        query = ("SELECT HEX(JSON_OBJECT(" + ','.join(pairs) + ")) AS payload FROM memory_units u "
                 "LEFT JOIN documents d ON d.id=u.document_id LEFT JOIN revisions r ON r.id=u.revision_id "
                 "LEFT JOIN messages m ON m.id=u.message_id ORDER BY u.scope_id,u.id;\n")
        units = subprocess.run(['tools/services.sh', 'testdb', 'sql', database], input=query, text=True, capture_output=True)
        (destination / 'units-export.log').write_text(units.stdout + units.stderr)
        if units.returncode == 0:
            rows = [json.loads(bytes.fromhex(line).decode('utf-8')) for line in units.stdout.splitlines()
                    if re.fullmatch(r'[0-9A-Fa-f]+', line)]
            (destination / 'units.json').write_text(json.dumps(rows, ensure_ascii=False, indent=2)+'\n')
            summary['source_units_saved'] = True
            summary['source_units'] = len(rows)
        else:
            summary['source_units_saved'] = False
    shutil.copytree(work, destination / 'work', symlinks=True, ignore=shutil.ignore_patterns('*.sock'))
    if summary['full_replies_saved'] and summary.get('source_units_saved', True):
        cleanup = subprocess.run(['tools/services.sh', 'testdb', 'drop', database], capture_output=True, text=True)
        summary['database_cleanup_exit'] = cleanup.returncode
        (destination / 'cleanup.log').write_text(cleanup.stdout + cleanup.stderr)
(destination / 'result.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps({'directory': str(destination), **summary}, indent=2))
sys.exit(0 if summary['full_replies_saved'] and summary['database_cleanup_exit'] == 0 else 1)
