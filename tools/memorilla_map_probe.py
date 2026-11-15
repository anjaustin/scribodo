"""Memorilla as a map: does the memory module's attention over documents rank the answering passage higher than
plain embedding similarity does? Offline, on the reach fixtures' hand-written questions over three novels.

Setup (outside the product; nothing here ships): a folder MEMORILLA_DIR (default build/memorilla) holding
  repo/          git clone https://github.com/snap-stanford/memorilla (read at d3e3f8f; only memorilla/memory.py
                 and memorilla/utils.py are imported: torch, numpy, transformers, huggingface_hub, yaml; no vLLM)
  checkpoints/   hf download memorilla/Memorilla-Qwen3-8B stage3/memory.pt stage3/config.json
  encoder-4b/    hf download Qwen/Qwen3-Embedding-4B        (7.5 GB)
  encoder-0.6b/  hf download Qwen/Qwen3-Embedding-0.6B      (the product's embedder, as safetensors)
  .venv/         uv venv --python 3.11; uv pip install torch transformers safetensors huggingface_hub numpy pyyaml
usage: MEMORILLA_DIR=build/memorilla build/memorilla/.venv/bin/python -I tools/memorilla_map_probe.py [--novels ...]
Results: journal/memory-map-memorilla.md (2026-11-15).

Units: each part file split at blank lines, paragraphs packed up to 2,000 bytes (the product's MEMORY_UNIT_BYTES),
an oversize paragraph cut at sentence ends. A task's gold units hold its required_document_text (or overlap it, when
it straddles two units). Every arm ranks every unit of the novel for every question.

Arms:
  e06-product  Qwen3-Embedding-0.6B, the product's query instruction (src/llm/semantic.c), cosine
  e06-plain    the same, no instruction
  e4b-plain    Qwen3-Embedding-4B, no instruction (Memorilla's own vectors), cosine
  e4b-instr    Qwen3-Embedding-4B, the product's instruction, cosine
  mem-all      Memorilla stage3: softmax attention over documents, summed over 2 layers x 8 heads x 16 slots
               (PRIMARY, fixed before any run)
  mem-l1, mem-l2  one layer each (exploratory)
Embeddings are cached under cache/ so reruns are cheap."""
import argparse
import json
import os
import re
import sys
import time

import numpy as np
import torch
import torch.nn.functional as F

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.abspath(os.environ.get("MEMORILLA_DIR", os.path.join(ROOT, "build/memorilla")))
sys.path.insert(0, os.path.join(HERE, "repo"))
from memorilla.memory import MemoryModule  # noqa: E402

UNIT_BYTES = 2000
PRODUCT_INSTRUCTION = ("Instruct: Given a statement from a work of fiction, retrieve established facts about the same "
                       "people, places or things\nQuery: ")
NOVELS = {"a-princess-of-mars": "build/manuscripts/a-princess-of-mars/parts",
          "pride-and-prejudice": "build/manuscripts/pride-and-prejudice/parts",
          "wuthering-heights": "build/manuscripts/wuthering-heights/parts"}
DEV = "mps" if torch.backends.mps.is_available() else "cpu"


def sentences(p):
    out, start = [], 0
    for m in re.finditer(r"[.!?][\"'”’]?\s+(?=[\"“‘']?[A-Z])", p):
        out.append(p[start:m.end()].strip())
        start = m.end()
    out.append(p[start:].strip())
    return [s for s in out if s]


def pack(pieces, sep):
    out, cur = [], ""
    for x in pieces:
        cand = cur + sep + x if cur else x
        if len(cand.encode()) <= UNIT_BYTES:
            cur = cand
        else:
            if cur:
                out.append(cur)
            cur = x
    if cur:
        out.append(cur)
    return out


def units(folder):
    res = []
    for fn in sorted(os.listdir(os.path.join(ROOT, folder))):
        if not fn.endswith(".md"):
            continue
        text = open(os.path.join(ROOT, folder, fn), encoding="utf-8").read()
        paras = []
        for p in re.split(r"\n\s*\n", text):
            p = p.strip()
            if not p:
                continue
            if len(p.encode()) <= UNIT_BYTES:
                paras.append(p)
            else:  # a long paragraph: its sentences packed, a giant sentence hard-cut
                for s in pack(sentences(p), " "):
                    while len(s.encode()) > UNIT_BYTES:
                        cut = s.encode()[:UNIT_BYTES].decode("utf-8", "ignore")
                        paras.append(cut)
                        s = s[len(cut):]
                    paras.append(s)
        for u in pack(paras, "\n\n"):
            res.append({"file": fn, "text": u})
    return res


def gold(us, needle):
    hit = [i for i, u in enumerate(us) if needle in u["text"]]
    if hit:
        return hit
    head, tail = needle[: len(needle) // 2], needle[len(needle) // 2:]  # straddling two units
    return [i for i, u in enumerate(us) if head[:60] in u["text"] or tail[-60:] in u["text"]]


class Embedder:
    def __init__(self, path):
        from transformers import AutoModel, AutoTokenizer
        self.tok = AutoTokenizer.from_pretrained(path, padding_side="left")
        self.model = AutoModel.from_pretrained(path, torch_dtype=torch.bfloat16).to(DEV).eval()

    @torch.no_grad()
    def __call__(self, texts, bs=8):
        out = []
        for i in range(0, len(texts), bs):
            b = self.tok(texts[i:i + bs], padding=True, truncation=True, max_length=1024, return_tensors="pt").to(DEV)
            h = self.model(**b).last_hidden_state[:, -1]  # left padding: the last position is each text's last token
            out.append(F.normalize(h.float(), dim=-1).cpu())
        return torch.cat(out).numpy()


def cached(name, fn):
    path = os.path.join(HERE, "cache", name + ".npy")
    if os.path.exists(path):
        return np.load(path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    v = fn()
    np.save(path, v)
    return v


@torch.no_grad()
def memorilla_attention(module, docs, questions):
    """Per layer, the slots' softmax over documents [questions, heads, slots, docs], as read_cache computes it."""
    d = torch.from_numpy(docs).to(DEV, torch.float32).unsqueeze(0)
    q = torch.from_numpy(questions).to(DEV, torch.float32)
    cache = module.write_cache(d)
    batch, heads = q.size(0), module.config.num_heads
    slots = module.memory_queries.expand(batch, -1, -1)
    cond, _ = module.query_cond_attn(slots, q.unsqueeze(1), q.unsqueeze(1))
    slots = module.query_cond_norm(slots + cond)
    attns = []
    for (keys, values), norm, attn, ffn_norm, ffn in zip(cache.layers, module.cross_attn_norms, module.cross_attn_layers,
                                                        module.ffn_norms, module.ffn_layers):
        s = norm(slots)
        dim = s.size(-1)
        qq = F.linear(s, attn.in_proj_weight[:dim], attn.in_proj_bias[:dim]).view(batch, -1, heads, dim // heads).transpose(1, 2)
        w = torch.softmax(torch.matmul(qq, keys) * (dim // heads) ** -0.5, dim=-1)  # [b, heads, slots, docs]
        attns.append(w.cpu().numpy())
        att = torch.matmul(w, values)
        slots = slots + attn.out_proj(att.transpose(1, 2).reshape(batch, -1, dim))
        slots = slots + ffn(ffn_norm(slots))
    return attns


def ranks(scores, golds):
    out = []
    for i, g in enumerate(golds):
        order = np.argsort(-scores[i], kind="stable")
        pos = {u: r for r, u in enumerate(order)}
        out.append(min(pos[x] for x in g) + 1)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--novels", nargs="*", default=list(NOVELS))
    a = ap.parse_args()
    module = MemoryModule.from_pretrained(os.path.join(HERE, "checkpoints/stage3")).to(DEV, torch.float32).eval()
    data = {}
    for n in a.novels:
        us = units(NOVELS[n])
        tasks = json.load(open(os.path.join(ROOT, f"tests/fixtures/memory_reach/{n}-tasks.json")))["tasks"]
        gs = [gold(us, t["required_document_text"]) for t in tasks]
        assert all(gs), [t["question"] for t, g in zip(tasks, gs) if not g]
        data[n] = (us, tasks, gs)
        print(f"# {n}: {len(us)} units, {len(tasks)} tasks, gold units per task {[len(g) for g in gs]}", flush=True)
    results = {}
    for enc, path in (("e06", "encoder-0.6b"), ("e4b", "encoder-4b")):
        emb = None
        for n, (us, tasks, gs) in data.items():
            def lazy(texts):
                nonlocal emb
                if emb is None:
                    t0 = time.time()
                    emb = Embedder(os.path.join(HERE, path))
                    print(f"# loaded {path} in {time.time() - t0:.0f} s", flush=True)
                return emb(texts)
            t0 = time.time()
            docs = cached(f"{enc}-{n}-docs", lambda: lazy([u["text"] for u in us]))
            qp = cached(f"{enc}-{n}-q-plain", lambda: lazy([t["question"] for t in tasks]))
            qi = cached(f"{enc}-{n}-q-instr", lambda: lazy([PRODUCT_INSTRUCTION + t["question"] for t in tasks]))
            print(f"# {enc} {n} embedded in {time.time() - t0:.0f} s", flush=True)
            results.setdefault(n, {})
            results[n][f"{enc}-{'product' if enc == 'e06' else 'instr'}"] = ranks(qi @ docs.T, gs)
            results[n][f"{enc}-plain"] = ranks(qp @ docs.T, gs)
            if enc == "e4b":
                att = memorilla_attention(module, docs, qp)
                l1, l2 = (x.sum(axis=(1, 2)) for x in att)
                results[n]["mem-all"] = ranks(l1 + l2, gs)
                results[n]["mem-l1"] = ranks(l1, gs)
                results[n]["mem-l2"] = ranks(l2, gs)
                ent = [float((-(x * np.log(x + 1e-12)).sum(-1)).mean()) for x in att]
                print(f"# {n}: mean attention entropy per layer {[round(e, 2) for e in ent]} "
                      f"(uniform over {docs.shape[0]} docs = {np.log(docs.shape[0]):.2f})", flush=True)
        del emb
        if DEV == "mps":
            torch.mps.empty_cache()
    json.dump(results, open(os.path.join(HERE, "results.json"), "w"), indent=1)
    arms = ["e06-product", "e06-plain", "e4b-plain", "e4b-instr", "mem-all", "mem-l1", "mem-l2"]
    print(f"\n{'arm':12} " + " ".join(f"{n[:12]:>26}" for n in data) + f" {'all':>26}")
    print(f"{'':12} " + " ".join(f"{'@1  @8 @32  MRR':>26}" for _ in list(data) + ['all']))
    for arm in arms:
        cells, every = [], []
        for n in data:
            r = results[n][arm]
            every += r
            cells.append(r)
        cells.append(every)
        print(f"{arm:12} " + " ".join(
            f"{sum(x <= 1 for x in r):>6}{sum(x <= 8 for x in r):>4}{sum(x <= 32 for x in r):>4}{np.mean([1 / x for x in r]):>6.2f}/{len(r):<3}"
            for r in cells))
    for n in data:
        print(f"\n# {n} ranks per task: " + "; ".join(f"{a}={results[n][a]}" for a in ("e06-product", "e4b-plain", "mem-all")))


if __name__ == "__main__":
    main()
