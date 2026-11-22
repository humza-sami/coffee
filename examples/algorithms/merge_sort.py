"""Bottom-up merge sort of packed int64 values: O(n log n) CPU and two n-element buffers (source + auxiliary) of array('q').

Plain Python, one thread. Memory is 16 bytes per element for the two buffers plus the interpreter's own footprint (reported as
peak_rss_bytes); at the sizes below the run is CPU-bound in the Python interpreter, so this is NOT a RAM benchmark.

Data: element i is  (p_i // 4 - n // 8) * 1000003  where p_i = (a*i + b) mod n is a scrambled permutation of 0..n-1 (a coprime to n).
Hence the sorted sequence is known without sorting anything: element j is (j // 4 - n // 8) * 1000003 (every value appears four
times, the last one n mod 4 times). The run checks (1) non-decreasing order, (2) an order-independent fingerprint of the input
(count, sum, sum of squares mod 2^64, xor) equals the output's and (3) the output equals that hand-derived sequence element by element.

Sizes (elements): light 2^18 = 262144, medium 2^20 = 1048576, heavy 2^22 = 4194304 (64 MiB of buffers); hard upper bound MAX_ELEMENTS = 2^23 (128 MiB of buffers), enforced by generate().
"""
import os
import sys
from array import array
from functools import reduce
from hashlib import sha256
from math import gcd
from operator import le, mul, xor

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import algo_common  # noqa: E402
from algo_common import VerificationError  # noqa: E402

SIZES = {"light": 1 << 18, "medium": 1 << 20, "heavy": 1 << 22}
MAX_ELEMENTS = 1 << 23
SCALE = 1_000_003
MASK64 = (1 << 64) - 1


def expected_value(n, j):
    """The j-th element of the sorted data of n elements, derived by hand (not by sorting)."""
    return ((j >> 2) - (n >> 3)) * SCALE


def fingerprint(arr):
    """Order-independent summary of a multiset of int64 values."""
    return (len(arr), sum(arr), sum(map(mul, arr, arr)) & MASK64, reduce(xor, arr, 0))


def generate(n):
    """Return (array('q') of n scrambled values, its fingerprint)."""
    if not 1 <= n <= MAX_ELEMENTS:
        raise ValueError("n must be between 1 and %d" % MAX_ELEMENTS)
    step = max(1, int(n * 0.6180339887))
    while gcd(step, n) != 1:
        step += 1
    p = (n // 3) % n
    shift = n >> 3

    def values():
        nonlocal p
        for _ in range(n):
            yield ((p >> 2) - shift) * SCALE
            p += step
            if p >= n:
                p -= n
    data = array("q", values())
    return data, fingerprint(data)


def merge_sort(a):
    """Sort array('q') a with bottom-up merging and return the sorted array. a is used as scratch: its contents are undefined afterwards."""
    n = len(a)
    if n < 2:
        return a
    src, dst = a, array("q", [0]) * n
    width = 1
    while width < n:
        sv, dv = memoryview(src), memoryview(dst)
        for lo in range(0, n, 2 * width):
            mid = min(lo + width, n)
            hi = min(lo + 2 * width, n)
            i, j, k = lo, mid, lo
            if mid < hi:
                x, y = src[i], src[j]
                while True:
                    if y < x:
                        dst[k] = y
                        k += 1
                        j += 1
                        if j == hi:
                            break
                        y = src[j]
                    else:
                        dst[k] = x
                        k += 1
                        i += 1
                        if i == mid:
                            break
                        x = src[i]
            if i < mid:
                dv[k:k + mid - i] = sv[i:mid]
            elif j < hi:
                dv[k:k + hi - j] = sv[j:hi]
        sv.release()
        dv.release()
        src, dst = dst, src
        width *= 2
    return src


def verify(arr, n, fp):
    """Order and multiset checks; raises VerificationError."""
    if len(arr) != n:
        raise VerificationError("output has %d elements, expected %d" % (len(arr), n))
    view = memoryview(arr)
    ordered = all(map(le, view[:-1], view[1:]))
    if not ordered:
        raise VerificationError("output is not in non-decreasing order")
    if fingerprint(arr) != fp:
        raise VerificationError("output is not a permutation of the input (multiset fingerprint differs)")


def verify_exact(arr, n):
    """Compare with the hand-derived sorted sequence, in bounded chunks so no third n-element buffer exists."""
    for lo in range(0, n, algo_common.CHUNK):
        hi = min(lo + algo_common.CHUNK, n)
        want = array("q", [expected_value(n, j) for j in range(lo, hi)])
        if arr[lo:hi] != want:
            raise VerificationError("output differs from the expected sorted sequence in [%d, %d)" % (lo, hi))


def solve(n):
    data, fp = generate(n)
    result = merge_sort(data)
    verify(result, n, fp)
    verify_exact(result, n)
    digest = sha256()
    algo_common.update_digest(digest, result)
    passes = (n - 1).bit_length()
    return {
        "input": {"elements": n, "element_type": "int64", "buffer_bytes": 2 * 8 * n, "merge_passes": passes,
                  "data": "(p // 4 - n // 8) * 1000003, p = (a*i + b) mod n a scrambled permutation of 0..n-1"},
        "check": {"name": "order_multiset_and_exact_sequence", "passed": True, "ordered": True, "multiset_fingerprint_equal": True,
                  "equals_hand_derived_sequence": True, "elements_checked": n},
        "result": {"sorted_sha256": digest.hexdigest(), "digest_format": "SHA-256 of the sorted array, little-endian int64", "min": result[0],
                   "max": result[-1], "distinct_values": (n + 3) // 4},
    }


def main(argv=None):
    return algo_common.main("merge-sort", "Bottom-up merge sort of packed int64 values with order, multiset and exact-sequence checks.", SIZES, solve, argv)


if __name__ == "__main__":
    sys.exit(main())
