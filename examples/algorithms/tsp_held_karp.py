"""Exact travelling salesman by Held-Karp dynamic programming: the optimal closed tour through n cities.

TSP optimisation is NP-hard. Held-Karp is exact and takes O(2^n * n^2) time and a table of 2^(n-1) * (n-1) cells (4 bytes each here), so
both CPU and memory grow exponentially with n; nothing here is a heuristic or a factorial search. It runs on one thread (one CPU slot).

Instance: n cities placed on a ring, labels scrambled by a multiplier coprime to n. Two cities that are neighbours on the ring are
10 apart; every other pair is 10 * ring_distance plus a small deterministic symmetric offset (0..9), so only neighbour pairs cost 10.
Independent proof of the optimum: a tour has n edges, each costs at least 10, and the cost-10 edges are exactly the n ring-neighbour
pairs, which form one Hamiltonian cycle. So n * 10 is a lower bound and the ring tour reaches it. A found tour must be a valid
permutation, its cost recomputed from the distance matrix must equal the DP value, and that value must equal n * 10; a tour cost alone
is never taken as proof of optimality.

Sizes (cities): light 15, medium 18, heavy 21 (table_bytes(21) = 80 MiB); hard upper bound MAX_CITIES = 22 (table_bytes(22) = 168 MiB), enforced by held_karp().
"""
import os
import sys
from array import array
from math import gcd

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import algo_common  # noqa: E402
from algo_common import VerificationError  # noqa: E402

SIZES = {"light": 15, "medium": 18, "heavy": 21}
MAX_CITIES = 22
MAX_DISTANCE = 1_000_000
UNIT = 10  # distance between ring neighbours, the smallest distance in a ring instance
METHOD = "Held-Karp dynamic programming (exact)"
INF = 2**31 - 1


def table_bytes(n):
    """Size of the DP table for n cities: 2^(n-1) subsets x (n-1) end cities x 4 bytes."""
    return 4 * (1 << (n - 1)) * (n - 1)


def _validate(dist):
    n = len(dist)
    if not 1 <= n <= MAX_CITIES:
        raise ValueError("cities must be between 1 and %d" % MAX_CITIES)
    for i, row in enumerate(dist):
        if len(row) != n:
            raise ValueError("distance matrix must be square")
        for j, d in enumerate(row):
            if isinstance(d, bool) or not isinstance(d, int) or not 0 <= d <= MAX_DISTANCE:
                raise ValueError("distances must be integers between 0 and %d" % MAX_DISTANCE)
            if (i == j and d != 0) or d != dist[j][i]:
                raise ValueError("distance matrix must be symmetric with a zero diagonal")


def tour_cost(dist, tour):
    return sum(dist[a][b] for a, b in zip(tour, tour[1:] + tour[:1])) if len(tour) > 1 else 0


def held_karp(dist):
    """Return (optimal cost, tour) for a symmetric non-negative integer matrix; the tour starts at city 0 and lists each city once."""
    _validate(dist)
    n = len(dist)
    if n == 1:
        return 0, [0]
    m = n - 1  # cities 1..n-1 are bits 0..m-1; city 0 is the fixed start
    d = [[dist[a + 1][b + 1] for b in range(m)] for a in range(m)]
    full = (1 << m) - 1
    dp = array("i", [INF]) * ((full + 1) * m)  # dp[mask*m + j]: cheapest path 0 -> (cities in mask) ending at j
    for j in range(m):
        dp[(1 << j) * m + j] = dist[0][j + 1]
    for prev in range(1, full):  # extend every reached subset by one more city
        members = [i for i in range(m) if prev >> i & 1]
        base = prev * m
        costs = [dp[base + i] for i in members]
        for j in range(m):
            if prev >> j & 1:
                continue
            dj = d[j]
            dp[(prev | 1 << j) * m + j] = min([c + dj[i] for c, i in zip(costs, members)])
    best, end = min((dp[full * m + j] + dist[j + 1][0], j) for j in range(m))
    tour, mask, cur = [], full, end
    while True:  # walk back, always taking the lowest-numbered predecessor that explains the cost
        tour.append(cur + 1)
        prev = mask ^ (1 << cur)
        if prev == 0:
            break
        want = dp[mask * m + cur]
        cur = next(i for i in range(m) if prev >> i & 1 and dp[prev * m + i] + d[i][cur] == want)
        mask = prev
    tour.append(0)
    tour.reverse()
    cost = tour_cost(dist, tour)
    if cost != best:
        raise VerificationError("internal inconsistency: reconstructed tour costs %d, the table says %d" % (cost, best))
    return cost, tour


def ring_instance(n):
    """Return (distance matrix, proven optimal cost) for the scrambled ring of n >= 3 cities."""
    if not 3 <= n <= MAX_CITIES:
        raise ValueError("ring instances need 3 to %d cities" % MAX_CITIES)
    a = max(2, int(n * 0.6180339887))
    while gcd(a, n) != 1:
        a += 1
    pos = [(a * c) % n for c in range(n)]
    dist = [[0] * n for _ in range(n)]
    for i in range(n):
        for j in range(i + 1, n):
            k = (pos[i] - pos[j]) % n
            r = min(k, n - k)
            dist[i][j] = dist[j][i] = UNIT if r == 1 else UNIT * r + (i * j + i + j) % UNIT
    return dist, n * UNIT


def verify(dist, cost, tour, optimum):
    """Raise VerificationError unless tour is a valid city permutation whose recomputed cost equals both the claim and the independent optimum."""
    n = len(dist)
    if sorted(tour) != list(range(n)) or tour[0] != 0:
        raise VerificationError("tour is not a permutation of the cities starting at city 0")
    actual = tour_cost(dist, tour)
    if actual != cost:
        raise VerificationError("tour costs %d but %d was claimed" % (actual, cost))
    if cost != optimum:
        raise VerificationError("cost %d is not the proven optimum %d" % (cost, optimum))


def solve(n):
    dist, optimum = ring_instance(n)
    cost, tour = held_karp(dist)
    verify(dist, cost, tour, optimum)
    return {
        "input": {"cities": n, "subsets": 1 << (n - 1), "dp_table_cells": (1 << (n - 1)) * (n - 1), "dp_table_bytes": table_bytes(n),
                  "distances": "ring of %d cities, neighbours %d apart, other pairs %d * ring_distance + offset in 0..%d" % (n, UNIT, UNIT, UNIT - 1)},
        "check": {"name": "valid_tour_equals_proven_optimum", "passed": True, "independent_optimum": optimum,
                  "optimality_proof": "lower bound: a tour has %d edges each costing at least %d, and the cost-%d edges are exactly the ring neighbours, "
                                      "one Hamiltonian cycle, so %d is reached" % (n, UNIT, UNIT, optimum)},
        "result": {"method": METHOD, "optimal_cost": cost, "tour": tour},
    }


def main(argv=None):
    return algo_common.main("tsp-exact", "Exact travelling salesman (Held-Karp) on a ring instance with a proven optimum.", SIZES, solve, argv)


if __name__ == "__main__":
    sys.exit(main())
