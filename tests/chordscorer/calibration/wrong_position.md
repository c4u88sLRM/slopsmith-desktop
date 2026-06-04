# Bass calibration — wrong-position takes (precision / false-accept test)

The first calibration round measured **recall** (do we detect correctly-played
notes?). This round measures **precision** — specifically, do we correctly
*reject* notes played in the **wrong position**? Testers reported ~65% "hits"
while playing 2 frets off, where the honest answer is ~0%.

Record these **three** passes of the **same etude** in `score.md`, all locked to
the **same `click.wav`**, clean DI (before any amp sim/NAM/pedals), 16-bit PCM
WAV, 48 kHz preferred:

| File                  | How to play it                                              | Expected score |
|-----------------------|------------------------------------------------------------|----------------|
| `bass_correct.wav`    | Exactly as written (the right frets)                        | **high** (recall) |
| `bass_2up.wav`        | Every note **+2 frets** (whole step **up**), same rhythm    | **~0%** (reject)  |
| `bass_2down.wav`      | Every note **−2 frets** (whole step **down**), same rhythm  | **~0%** (reject)  |

Notes:
- Keep the **rhythm/timing identical** to the click — only the fretting hand
  moves +2 / −2. (On a 4-string, −2 from the open low E isn't playable; just
  hold/skip those few open-E notes in the `bass_2down` take — the rest is what
  matters.)
- A "correct" take here is the precision baseline (high recall); the two
  wrong-position takes are the false-accept test (should score near zero).
- Same recording rules as `score.md` (DI only, play to the click in headphones,
  let notes ring, 4-string stop after Section D / 5-string continue to E).

## What it's for

The detector is tuned to chase the result of `cs_bench` on all three:

```bash
./build/cs_bench bass_correct.wav calibration/chart.txt bass   # want HIGH recall
./build/cs_bench bass_2up.wav      calibration/chart.txt bass   # want ~0% (reject)
./build/cs_bench bass_2down.wav    calibration/chart.txt bass   # want ~0% (reject)
```

The goal is to keep the recall we gained while driving the wrong-position
"recall" toward zero — i.e. a real pitch-discrimination gate, not just a looser
SNR. (The original torture etude only ever played *correct* notes, which is the
blind spot that let the false-accepts ship.)
