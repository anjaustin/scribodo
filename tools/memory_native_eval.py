#!/usr/bin/env python3
"""Freeze and run development-only native continuity gates. No automatic quality pass."""
import hashlib
import json
import os
from pathlib import Path
import secrets
import re
import shutil
import signal
import subprocess
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]
def sha(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda:f.read(1048576),b''): h.update(block)
    return h.hexdigest()

def main():
    run=Path(tempfile.mkdtemp(prefix='mn-',dir=ROOT/'build'))
    os.chmod(run,0o700)
    database=f'scribodo_test_native_{os.getpid()}_{secrets.token_hex(4)}'
    for name in ('dev.json','preregistration.json'):
        shutil.copyfile(ROOT/'tests/fixtures/memory_native'/name,run/name)
    binary=run/'native-eval'; shutil.copyfile(ROOT/'build/memory_native_eval',binary); binary.chmod(0o700)
    manifest={'protocol':'native-continuity-dev-v1','split':'development','database':database,
              'binary_sha256':sha(binary),'cases_sha256':sha(run/'dev.json'),
              'registration_sha256':sha(run/'preregistration.json'),
              'expected_answers':42,'selection':'frozen SQL episode candidate seam; native hydration/admission/inference',
              'working_tree_sources':{str(p.relative_to(ROOT)):sha(p) for p in sorted((ROOT/'src').rglob('*')) if p.suffix in ('.c','.h')}}
    (run/'manifest.json').write_text(json.dumps(manifest,indent=2))
    print(f'Frozen native DEV run: {run}',flush=True)
    subprocess.run(['tools/services.sh','testdb','create',database],cwd=ROOT,check=True)
    environment=os.environ.copy(); environment.setdefault('SCRIBODO_LLAMA_SERVER',str(ROOT/'build/llama/bin/llama-server'))
    process=None; returncode=-1; began=time.monotonic()
    try:
        with (run/'driver.log').open('w') as log:
            process=subprocess.Popen([str(binary),str(run/'dev.json'),str(run),database,str(run/'events.jsonl')],
                                     cwd=ROOT,env=environment,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
            returncode=process.wait(timeout=3600)
    finally:
        if process:
            try:os.killpg(process.pid,signal.SIGTERM)
            except ProcessLookupError:pass
            if process.poll() is None:process.wait(timeout=20)
        subprocess.run(['tools/services.sh','testdb','drop',database],cwd=ROOT,check=True)
    rows=[json.loads(s) for s in (run/'events.jsonl').read_text().splitlines()]
    answers=[r for r in rows if r.get('event')=='answer']
    cohorts={}
    for row in answers:
        cohorts.setdefault((row['task'],row['layout']),[]).append(row)
    same_sources=len(cohorts)==14 and all(len(group)==3 and
        len(set(re.findall(r'unit=([0-9a-f]{64})',group[0]['sources'])))==3 and
        all(set(re.findall(r'unit=([0-9a-f]{64})',r['sources']))==set(re.findall(r'unit=([0-9a-f]{64})',group[0]['sources'])) for r in group)
        for group in cohorts.values())
    questions={c['name']:c['question'] for c in json.loads((run/'dev.json').read_text())['cases']}
    protected=all(r.get('request') and json.loads(r['request'])['messages'][-1]['content'].endswith(questions[r['task']]) for r in answers)
    mechanics={'driver_complete':returncode==0 and len(answers)==42,
               'all_admitted_sources':all(r.get('sources') for r in answers) and bool(answers),
               'no_errors_or_truncation':all(not r.get('error') and not r.get('truncated') for r in answers),
               'registration_intact':sha(run/'preregistration.json')==manifest['registration_sha256'],
               'cases_intact':sha(run/'dev.json')==manifest['cases_sha256']}
    mechanics['same_source_ids_across_presentations']=same_sources
    mechanics['current_author_requests_preserved']=protected
    report={'result':'SEMANTIC_REVIEW_REQUIRED' if all(mechanics.values()) else 'MECHANICAL_GATE_FAILED',
            'quality_pass':False,'mechanics':mechanics,'answers':len(answers),
            'not_heldout_evidence':True,'runtime_seconds':time.monotonic()-began,
            'semantic_review':'Review complete native outputs against preregistered task clauses; never infer pass from source presence.'}
    (run/'report.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2),flush=True)
    return 0 if all(mechanics.values()) else 1

if __name__=='__main__':raise SystemExit(main())
