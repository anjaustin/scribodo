#!/usr/bin/env python3
"""Deterministic manuscript-scale memory fixture (MEMORY-015 S2).

Builds a long author chat archive whose prose reuses the same characters,
places, objects and colours everywhere, so that planted items sit among
realistic lexical distractors instead of unique rare-word sentences. Planted
items are labelled by category so reach can be reported per category:

  control             a V2-style unique catalogue number; the harness check
  stated_fact         a fact asserted once, with near-miss assertions elsewhere
  superseded_fact     a fact and a later correction that replaces it
  implied_decision    an assistant proposal accepted by the author turns later
  rejected_proposal   an assistant proposal the author explicitly rejects
  protected_ambiguity an author instruction not to settle something
  paraphrase          a fact whose question uses words the source does not
  multilingual        a Spanish fact asked in Spanish
  absence             a question nothing in the archive answers
  document_fact       a fact stated once inside an attached saved document

The archive, not any answer key, is what ingestion sees. No row is marked
privileged or canonical. Everything is reproducible from (seed, words,
per_category). Stdlib only.
"""
import argparse
import json
import random
import re
import sys
import unicodedata
from pathlib import Path

CHARACTERS = ["Elin", "Oren", "Mira", "Zoë", "Dvořák", "Maël", "Íñigo", "Tarn", "Lena", "Renée"]
PLACES = ["the lighthouse", "the fjord quay", "the observatory", "the ferry landing", "the archive",
          "the cedar hall", "the salt market", "the tower stair"]
PLACES_ES = ["el faro", "el muelle del fiordo", "el observatorio", "el embarcadero", "el archivo"]
OBJECTS = ["lantern", "key", "coat", "ledger", "bell", "compass", "letter", "cloak", "ring", "map"]
SYNONYMS = {"lantern": "lamp", "key": "latchkey", "coat": "jacket", "ledger": "account book", "bell": "chime",
            "compass": "bearing dial", "letter": "note", "cloak": "mantle", "ring": "band", "map": "chart"}
ATTRIBUTES = ["amber", "cobalt", "ochre", "violet", "slate", "ivory", "crimson", "copper"]
VERBS = ["lifted", "set down", "studied", "carried", "forgot", "polished", "hid", "found", "mended", "traded"]
WEATHER = ["fog", "rain", "a thin wind", "low cloud", "evening light", "frost", "sleet", "haze"]
TIDES = ["turned", "slackened", "ran high", "pulled at the moorings", "went out", "came in"]
WORDS = ["Late", "Quietly", "Again", "Nothing", "Wait", "Enough", "Listen", "Perhaps"]
RECENT = 64


def normalized(value):
    return " ".join(unicodedata.normalize("NFKC", value).casefold().split())


class Builder:
    def __init__(self, seed, words, per_category):
        self.rng = random.Random(seed)
        self.words = words
        self.per_category = per_category
        self.forbidden = {}      # (char, object) -> set of attributes filler must not assert
        self.planted = []        # dicts: {"at": fraction, "turns": [(role, text)], ...}
        self.tasks = []

    # ---- filler prose ----------------------------------------------------
    def attribute_for(self, char, obj):
        banned = self.forbidden.get((char, obj), set())
        choices = [a for a in ATTRIBUTES if a not in banned]
        return self.rng.choice(choices)

    def sentence(self):
        r = self.rng
        char, other = r.sample(CHARACTERS, 2)
        obj = r.choice(OBJECTS)
        attr = self.attribute_for(char, obj)
        place, weather, verb = r.choice(PLACES), r.choice(WEATHER), r.choice(VERBS)
        kind = r.randrange(7)
        if kind == 0:
            return f"{char} {verb} the {attr} {obj} at {place} while {weather} came in off the water."
        if kind == 1:
            return f"“{r.choice(WORDS)},” said {char}, watching the {weather} settle over {place}."
        if kind == 2:
            return f"The {obj} on the table at {place} had belonged to {other}, or so {char} believed."
        if kind == 3:
            return f"Nothing at {place} explained the {attr} stain on the {obj} {char} had {verb}."
        if kind == 4 and (char, obj) not in self.forbidden:
            # Assertive near-miss about an unplanted pair: realistic distractor pressure.
            return f"{char}'s {obj} is {attr}, {other} noted, though nobody had asked."
        if kind == 5:
            return f"By the time the tide {r.choice(TIDES)}, {char} and {other} had stopped speaking of the {obj}."
        return f"{other} walked back from {place} through {weather}, the {attr} light fading behind the {obj} seller's stall."

    def paragraph(self):
        sentences = []
        target = self.rng.randint(80, 130)
        while len(" ".join(sentences).split()) < target:
            sentences.append(self.sentence())
        text = " ".join(sentences)
        assert len(text.encode()) <= 2000
        return text

    def acknowledgement(self):
        r = self.rng
        return f"Noted. The {r.choice(PLACES)} passage reads well; the {r.choice(OBJECTS)} detail is vivid. Shall I keep going?"

    def recent_turn(self, i):
        r = self.rng
        return (f"Recent manuscript turn {i:03}: {r.choice(WEATHER)} over {r.choice(PLACES)}; "
                f"the tide {r.choice(TIDES)}. Ordinary weather observations only.")

    # ---- planted items ---------------------------------------------------
    def plant(self, category, turns, question, expected, answers, abstain=False, gap=0, also=None, distract=None):
        """turns: list of (role, text) placed consecutively (gap filler turns between them when gap>0)."""
        at = self.rng.uniform(0.05, 0.78)
        self.planted.append({"category": category, "at": at, "turns": turns, "gap": gap,
                             "question": question, "expected": expected, "answers": answers,
                             "abstain": abstain, "also": also or [], "distract": distract or []})

    def reserve(self, char, obj, attrs):
        self.forbidden.setdefault((char, obj), set()).update(attrs)
        # Assertive filler about a planted pair is never generated (kind 4 checks membership).

    def plan_items(self):
        r = self.rng
        pairs = r.sample([(c, o) for c in CHARACTERS for o in OBJECTS], 60)
        pair_iter = iter(pairs)
        for _ in range(self.per_category):
            # control
            char = r.choice(CHARACTERS)
            number = f"QX-{r.randint(1000, 9999)}"
            self.plant("control", [("user", f"For the records: the {char} archive catalogue number is {number}.")],
                       f"What is the {char} archive catalogue number?", f"{number}", [number])
            # stated_fact with two near-miss assertions
            char, obj = next(pair_iter)
            attr, near1, near2 = r.sample(ATTRIBUTES, 3)
            other = r.choice([c for c in CHARACTERS if c != char])
            obj2 = r.choice([o for o in OBJECTS if o != obj])
            self.reserve(char, obj, {attr, near1, near2})
            self.reserve(other, obj, {near1})
            self.reserve(char, obj2, {near2})
            item = {"category": "stated_fact", "at": r.uniform(0.05, 0.78),
                    "turns": [("user", f"{char}'s {obj} is {attr}; that is established for this book.")], "gap": 0,
                    "question": f"What colour is {char}'s {obj}?", "expected": attr, "answers": [attr],
                    "abstain": False, "also": [], "distract": []}
            d1 = {"category": "distractor", "at": r.uniform(0.05, 0.78),
                  "turns": [("user", f"{other}'s {obj} is {near1}, which suits the {r.choice(PLACES)} scenes.")], "gap": 0}
            d2 = {"category": "distractor", "at": r.uniform(0.05, 0.78),
                  "turns": [("user", f"{char}'s {obj2} is {near2}; keep that for the later chapters.")], "gap": 0}
            item["distract_items"] = [d1, d2]
            self.planted.extend([item, d1, d2])
            # superseded_fact
            char, obj = next(pair_iter)
            a1, a2 = r.sample(ATTRIBUTES, 2)
            self.reserve(char, obj, {a1, a2})
            self.plant("superseded_fact",
                       [("user", f"{char}'s {obj} is {a1}."),
                        ("user", f"Correction, replacing the earlier note: {char}'s {obj} is {a2}, not {a1}.")],
                       f"What colour is {char}'s {obj} now?", f"{a2} (the correction replaces {a1})", [a2], gap=r.randint(3, 9))
            # implied_decision
            char = r.choice(CHARACTERS)
            place = r.choice(PLACES)
            self.plant("implied_decision",
                       [("assistant", f"One option: {char} could narrate the {place} chapter."),
                        ("user", f"Yes. {char} narrates the {place} chapter. That is settled.")],
                       f"Who narrates the {place} chapter?", f"{char}; the author accepted the proposal", [char],
                       gap=r.randint(2, 6), also=[0])
            # rejected_proposal
            char, obj = next(pair_iter)
            attr = r.choice(ATTRIBUTES)
            self.reserve(char, obj, {attr})
            self.plant("rejected_proposal",
                       [("assistant", f"Perhaps {char}'s {obj} should be {attr}."),
                        ("user", f"No. Not {attr}. Drop that; {char}'s {obj} stays undecided.")],
                       f"Did we decide that {char}'s {obj} is {attr}?",
                       f"No; the author rejected {attr} and left the {obj} undecided", [], abstain=True,
                       gap=r.randint(1, 4), also=[0])
            # protected_ambiguity
            c1, c2 = r.sample(CHARACTERS, 2)
            obj, place = r.choice(OBJECTS), r.choice(PLACES)
            self.plant("protected_ambiguity",
                       [("user", f"Keep it open whether {c1} or {c2} rang the {obj} at {place}. Do not settle it in this draft.")],
                       f"Who rang the {obj} at {place}?", "Deliberately unresolved: either; must stay open", [],
                       abstain=True)
            # paraphrase
            char, obj = next(pair_iter)
            self.plant("paraphrase",
                       [("user", f"{char} keeps the {obj} inside the cedar chest beneath the window at {r.choice(PLACES)}.")],
                       f"Where does {char} store the {SYNONYMS[obj]}?", "in the cedar chest beneath the window",
                       ["cedar chest", "in the cedar chest", "the cedar chest"])
            # multilingual
            char = r.choice(CHARACTERS)
            n = r.randint(11, 97)
            self.plant("multilingual",
                       [("user", f"En {r.choice(PLACES_ES)}, {char} guarda {n} llaves de cobre en un cajón cerrado.")],
                       f"¿Cuántas llaves de cobre guarda {char}?", str(n), [str(n)])
            # absence
            char = r.choice(CHARACTERS)
            self.plant("absence", [], f"What is {char}'s birthday?", "Not established anywhere in the archive", [],
                       abstain=True)

    # ---- assembly ----------------------------------------------------------
    def build(self):
        self.plan_items()
        paragraphs = (self.words + 99) // 100
        body = []
        for i in range(paragraphs):
            body.append(("assistant" if i % 7 == 6 else "user", self.acknowledgement() if i % 7 == 6 else self.paragraph()))
        # Insert planted turns at their fractional positions (later ones first so indices hold).
        inserts = []
        for item in self.planted:
            if not item["turns"]:
                continue
            base = int(item["at"] * len(body))
            positions = []
            for k, turn in enumerate(item["turns"]):
                positions.append(base + k * (item["gap"] + 1))
            item["_positions"] = positions
            for pos, turn in zip(positions, item["turns"]):
                inserts.append((pos, len(inserts), id(item), turn))
        # Ties on position break by planting order, never by object identity:
        # two builds with one seed must produce one archive.
        inserts.sort(key=lambda x: (x[0], x[1]))
        archive_turns = []
        pending = {}
        cursor = 0
        for pos, _, owner, turn in inserts:
            while cursor < pos and cursor < len(body):
                archive_turns.append(body[cursor]); cursor += 1
            pending.setdefault(owner, []).append(len(archive_turns))
            archive_turns.append(turn)
        archive_turns.extend(body[cursor:])
        for i in range(RECENT):
            archive_turns.append(("user", self.recent_turn(i)))
        archive = [{"id": f"turn:{seq}", "sequence": seq, "role": role, "text": text}
                   for seq, (role, text) in enumerate(archive_turns)]
        boundary = len(archive) - RECENT
        tasks = []
        for item in self.planted:
            if item["category"] == "distractor":
                continue
            seqs = pending.get(id(item), [])
            required = list(seqs)
            also = [seqs[i] for i in item["also"]] if seqs else []
            if item["category"] in ("implied_decision", "rejected_proposal"):
                required = [seqs[-1]]  # the author's turn carries the authority
            distract = []
            for d in item.get("distract_items", []):
                distract.extend(pending.get(id(d), []))
            assert all(s < boundary for s in seqs)
            tasks.append({"id": f"t{len(tasks) + 1:02}", "category": item["category"], "question": item["question"],
                          "expected": item["expected"], "answers": item["answers"], "abstain": item["abstain"],
                          "required_sequences": required, "also_relevant_sequences": also,
                          "distractor_sequences": distract,
                          "history": [{"role": role, "content": text} for role, text in item["turns"]]})
        # Attached saved documents: two chapters of filler prose, each with
        # planted document facts. The planner sees them as saved-revision units.
        documents = []
        document_tasks = []
        r = self.rng
        for d in range(2):
            paragraphs = [self.paragraph() for _ in range(max(4, self.per_category * 2))]
            planted = []
            for k in range(self.per_category if d == 0 else 0):
                place, attr, obj = r.choice(PLACES), r.choice(ATTRIBUTES), r.choice(OBJECTS)
                n = r.randint(12, 98)
                sentence = f"The {place} ledger lists {n} {attr} {obj}s in its final column."
                slot = r.randrange(len(paragraphs))
                paragraphs[slot] = paragraphs[slot] + " " + sentence
                planted.append({"sentence": sentence, "question": f"How many {attr} {obj}s does the {place} ledger list?",
                                "expected": str(n), "answers": [str(n)]})
            text = f"# Chapter {d + 1}\n\n" + "\n\n".join(paragraphs) + "\n"
            name = f"ch{d + 1:02}.md"
            documents.append({"name": name, "text": text})
            for item in planted:
                document_tasks.append({"id": f"t{len(tasks) + len(document_tasks) + 1:02}", "category": "document_fact",
                                       "question": item["question"], "expected": item["expected"], "answers": item["answers"],
                                       "abstain": False, "required_sequences": [], "also_relevant_sequences": [],
                                       "distractor_sequences": [], "required_document": name,
                                       "required_document_text": item["sentence"], "history": []})
        tasks.extend(document_tasks)
        recent_text = normalized(" ".join(row["text"] for row in archive[boundary:]))
        for task in tasks:
            for alias in task["answers"]:
                pattern = r"(?<!\w)" + re.escape(normalized(alias)) + r"(?!\w)"
                assert not re.search(pattern, normalized(task["question"])), (task["id"], "question leak")
                assert not re.search(pattern, recent_text), (task["id"], "recent leak")
        actual_words = sum(len(row["text"].split()) for row in archive)
        assert all(len(row["text"].encode()) <= 2000 for row in archive)
        return {"protocol": "memory-reach-v1", "archive": archive, "boundary": boundary, "tasks": tasks, "documents": documents,
                "words": actual_words, "recent": RECENT,
                "bible": {"characters": CHARACTERS, "places": PLACES, "objects": OBJECTS, "attributes": ATTRIBUTES}}


def build(seed, words, per_category):
    return Builder(seed, words, per_category).build()


def export_native(fixture, path):
    """Episodes for the native 'faithful use' harness: planted turns as history."""
    cases = []
    for task in fixture["tasks"]:
        if not task["history"]:
            continue
        cases.append({"name": task["id"] + "_" + task["category"], "history": task["history"],
                      "question": task["question"], "expected": task["expected"]})
    Path(path).write_text(json.dumps({"cases": cases}, ensure_ascii=False, indent=2) + "\n")
    return len(cases)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seed", type=int, default=20261107)
    parser.add_argument("--words", type=int, default=30000)
    parser.add_argument("--per-category", type=int, default=3)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--export-native", type=Path)
    args = parser.parse_args()
    fixture = build(args.seed, args.words, args.per_category)
    if args.out:
        args.out.write_text(json.dumps(fixture, ensure_ascii=False) + "\n")
    if args.export_native:
        print(f"native cases: {export_native(fixture, args.export_native)}")
    by = {}
    for task in fixture["tasks"]:
        by[task["category"]] = by.get(task["category"], 0) + 1
    print(json.dumps({"turns": len(fixture["archive"]), "words": fixture["words"], "boundary": fixture["boundary"],
                      "tasks": len(fixture["tasks"]), "by_category": by}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
