# Algorithm Recipes: matrix multiplication, merge sort and exact TSP

Three small university-style workloads, each runnable on its own with Python 3 or as a Recipe on a local Coffee cluster.
Every run checks its own answer against an independent oracle and writes one result file.
They use the Python standard library only: no network, no packages, and the only file written is the `--output` path.

They need `python3` on every Bean's `PATH`. They were tested here with one Python 3 release; other 3.x versions are untested.
This example is part of the source tree and is **not** in the published `beta-v0.1` binaries. It is planned for the next release,
and building from source is the way to get it today.

## The three workloads

| Workload | Script | CPU work | Memory | Uses |
| --- | --- | --- | --- | --- |
| Dense matrix multiplication, `C = A x B` for `n x n` matrices | `matrix_multiply.py` | Θ(n³) multiply-adds | Θ(n²): `A`, `B` and the transposed `B`, 24 n² bytes | one CPU |
| Bottom-up merge sort of `n` packed 64-bit integers | `merge_sort.py` | Θ(n log n) | Θ(n): a source and an auxiliary buffer, 16 n bytes | one CPU |
| Exact travelling salesman by Held–Karp dynamic programming, `n` cities | `tsp_held_karp.py` | Θ(2ⁿ n²) | Θ(2ⁿ n): `2^(n-1) (n-1)` table cells of 4 bytes | one CPU |

All three run in one Python process on one thread, so one Step uses one CPU slot. Python loops are not parallelised and this is
not a measurement of Python's multithreading, of BLAS, or of RAM speed. The merge sort in particular is limited by the Python
interpreter at these sizes: it has a predictable buffer size but it is **not** a RAM benchmark.
TSP *optimisation* is the NP-hard problem; Held–Karp is exact and exponential, which is why the sizes are bounded.
It is not a heuristic and it does not try all `n!` tours.

## Sizes

`--size light|medium|heavy` (default `light`). Anything else is rejected, and there is no way to ask for an arbitrary size:
each script refuses to build an instance above its hard bound (`MAX_N`, `MAX_ELEMENTS`, `MAX_CITIES` in the source).

| Size | Matrix `n` | Merge sort elements | TSP cities |
| --- | --- | --- | --- |
| light | 175 (5.4 million multiply-adds, 0.7 MiB data) | 2¹⁸ = 262,144 (4 MiB buffers) | 15 (0.9 MiB table) |
| medium | 350 (43 million, 2.8 MiB) | 2²⁰ = 1,048,576 (16 MiB) | 18 (8.5 MiB) |
| heavy | 700 (343 million, 11.2 MiB) | 2²² = 4,194,304 (64 MiB) | 21 (80 MiB) |
| hard bound | 800 | 2²³ (128 MiB) | 22 (168 MiB) |

Data sizes are formulas from the source, not measurements; the Python interpreter and its loaded modules add a fixed amount on top.
Each result file records the real `elapsed_seconds`, `peak_rss_bytes` (resident peak) and `peak_virtual_bytes` of that run.
Heavy is sized conservatively so that it is expected to finish within about two minutes and under 512 MiB resident on a slow
laptop-class machine. That is a design target and not a guarantee: it depends on the CPU and Python version, so check
`elapsed_seconds` on your hardware before relying on it. **Light is the default** and is quick enough for tests; **heavy is
opt-in**.

Recipe reservations, per Step (`cpus: 1` throughout):

| Recipe | matrix-multiply | merge-sort | tsp-exact | `timeout_seconds` |
| --- | --- | --- | --- | --- |
| `coffee-light.yaml` | 256 MiB | 256 MiB | 256 MiB | 60 |
| `coffee-medium.yaml` | 256 MiB | 256 MiB | 256 MiB | 120 |
| `coffee-heavy.yaml` | 256 MiB | 384 MiB | 384 MiB | 300, `max_attempts: 1` |

Each Recipe has three independent Steps (no `depends_on`) that stage their script and `algo_common.py` as inputs and return
`result.json`. They use the unchanged `coffee/v1alpha1` schema.

## Run one directly

From this directory (or its installed copy, `PREFIX/share/coffee/examples/algorithms`):

```sh
python3 matrix_multiply.py --size light --output /tmp/matrix.json
python3 merge_sort.py      --size medium --output /tmp/sort.json
python3 tsp_held_karp.py   --size light --output /tmp/tsp.json
```

`--output` must name a new or existing regular `.json` file. The script refuses a directory, a missing parent directory, a path
that is not `.json`, or one of the example's own files, before it starts any work. Exit status: `0` verified result written,
`1` the answer failed its check or memory ran out (nothing is written), `2` bad arguments or output path.

## Run as a Brew on a local Manager and Bean

Build Coffee from source first ([build details](../../docs/native-build.md)); the commands below use `build/coffee` from the
repository root. `coffee resources` shows what this machine can offer.

<!-- brew:start -->
```sh
cd examples/algorithms
export PATH="$PWD/../../build:$PATH"
coffee validate coffee-light.yaml
coffee plan coffee-light.yaml

coffee init --state /tmp/algo-state
coffee serve --state /tmp/algo-state --listen 127.0.0.1:7077 --ready-file /tmp/algo-state/ready.json > /tmp/algo-state/serve.log 2>&1 &
until [ -s /tmp/algo-state/ready.json ]; do sleep 0.1; done
export COFFEE_SERVER=http://127.0.0.1:7077 COFFEE_TOKEN_FILE=/tmp/algo-state/token
coffee join --work-dir /tmp/algo-bean --id bean-a --cpus 3 --memory-mb 3072 > /tmp/algo-state/bean-a.log 2>&1 &
until [ "$(coffee beans | grep -c online)" -ge 1 ]; do sleep 0.2; done
coffee plan coffee-light.yaml --server "$COFFEE_SERVER"
coffee brew coffee-light.yaml --job-id algo-light --wait --out /tmp/algo-results
cat /tmp/algo-results/tsp-exact/result.json
kill %1 %2
```
<!-- brew:end -->

Use `coffee-medium.yaml` or `coffee-heavy.yaml` the same way. A Bean should have at least `cpus: 3` to run the three Steps at the
same time. More Beans or CPU slots let more Steps run at once; fewer run them one after another.

## Reading a result

Each Step returns `result.json` with sorted keys:

| Field | Meaning | Same on every run? |
| --- | --- | --- |
| `algorithm`, `size` | Which workload and size | yes |
| `input` | The instance: dimensions, generator, estimated data bytes | yes |
| `check` | The verification, with `passed: true`. A failed check exits non-zero and writes no file | yes |
| `result` | The exact answer or its digest: matrix SHA-256, sorted SHA-256, or the optimal tour cost and tour | yes |
| `elapsed_seconds` | Wall time of the computation | no |
| `peak_rss_bytes`, `peak_virtual_bytes` | Process peaks read from `/proc` (`null` if unavailable) | no |

What the checks mean:

- **Matrix:** `A[i][j] = 3i + 5j - 7` and `B[j][k] = 2j - 4k + 11`, so every `C[i][k]` also has a closed form by Faulhaber's sums
  (`sum j` and `sum j²`). All `n²` elements are compared with it; `C` is never compared with a second call of the multiplication.
- **Merge sort:** the data are a scrambled permutation scaled by 1,000,003 with every value repeated four times, so the sorted
  sequence is known by hand. The output must be non-decreasing, have the same count, sum, sum of squares and XOR as the input,
  and equal that sequence element by element.
- **TSP:** a ring of cities with scrambled labels whose neighbour distances are the unique smallest distance, so `n` times that distance
  is a proven lower bound that the tour reaches. The tour must be a valid permutation, its cost recomputed from the matrix
  must equal the Held–Karp value, and that must equal the proven optimum. A tour cost alone is never treated as proof of optimality.

## What more Beans do, and what they cannot do

The three Steps are independent, so with more Beans (or more CPU slots) the Manager can place them at the same time and they can
overlap instead of running one after another. Overlap is what is permitted, not a promised speedup, and this example does not
measure one: staging inputs, scheduling, a slower Bean, or contention with other work on the same machine can all take the gain
back, and a Brew cannot finish before its slowest single Step does. A single Step still runs on one Bean in one process. Coffee
cannot combine the RAM or CPU of several Beans for one Step today, so a larger matrix or more cities cannot borrow memory from
another machine. Pooled CPU, RAM and storage are future work ([limitations](../../docs/limitations.md)).

## What `memory_mb` does and does not do

`memory_mb` is the Step's scheduling reservation against the Bean's advertised memory. In the current runtime the Bean also applies
it as `RLIMIT_AS` (address space) to each command process. That is not a resident-memory or cgroup limit, and it does not bound the
Bean's total across Steps, so it is not a stress-test safety limit. These Recipes therefore reserve well above the peak address space
Python needs. If you experiment with larger values, add an outside limit of your own, for example
`systemd-run --user --scope -p MemoryMax=512M -p CPUQuota=100% python3 ...`, or run one process at a time under `taskset` and
`prlimit`. Do not extend these scripts into an unbounded stress tool; their size limits are deliberate.
