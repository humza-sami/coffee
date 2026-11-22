"""Dense matrix multiplication C = A x B with exact integer arithmetic: cubic CPU work in one Python process (one CPU slot).

Plain Python executes this on a single thread; it is not parallel and not a BLAS benchmark. A and B are structured so that every
element of C also has a closed form, derived below by hand, which is the independent check (C is never compared with a second
call of the multiplication):

    A[i][j] = 3*i + 5*j - 7            B[j][k] = 2*j - 4*k + 11
    with p = 3*i - 7 and q = 11 - 4*k:   C[i][k] = sum_j (p + 5j)(q + 2j) = p*q*n + (2p + 5q) * n(n-1)/2 + 10 * (n-1)n(2n-1)/6

Sizes (n x n, int64 values; |C| < 48 n^3, far below 2^63 for every allowed n):
    light 175, medium 350, heavy 700; hard upper bound MAX_N = 800, enforced by generate().
Only A, B and the transposed copy of B are kept (3 n^2 int64 values, 14.7 MiB at MAX_N); C is checked and hashed row by row.
"""
import os
import sys
from array import array
from hashlib import sha256
from operator import mul

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import algo_common  # noqa: E402
from algo_common import VerificationError  # noqa: E402,F401

SIZES = {"light": 175, "medium": 350, "heavy": 700}
MAX_N = 800


def generate(n):
    """Return (A, B) as lists of n array('q') rows."""
    if not 1 <= n <= MAX_N:
        raise ValueError("n must be between 1 and %d" % MAX_N)
    a = [array("q", [3 * i + 5 * j - 7 for j in range(n)]) for i in range(n)]
    b = [array("q", [2 * j - 4 * k + 11 for k in range(n)]) for j in range(n)]
    return a, b


def multiply_rows(a, b):
    """Yield the rows of A x B one at a time: n^3 multiply-adds, the inner dot product over a row of A and a column of B."""
    if not a or not b or len(a[0]) != len(b):
        raise ValueError("inner dimensions must match and be non-empty")
    columns = [array("q", col) for col in zip(*b)]
    for row in a:
        yield array("q", [sum(map(mul, row, col)) for col in columns])


def multiply(a, b):
    return list(multiply_rows(a, b))


def expected_element(n, i, k):
    """Closed form of C[i][k] for the structured matrices of generate(n)."""
    p, q = 3 * i - 7, 11 - 4 * k
    return p * q * n + (2 * p + 5 * q) * (n * (n - 1) // 2) + 10 * ((n - 1) * n * (2 * n - 1) // 6)


def row_matches(n, i, row):
    if len(row) != n:
        return False
    p = 3 * i - 7
    s0, s1, s2 = n, n * (n - 1) // 2, (n - 1) * n * (2 * n - 1) // 6
    return row.tolist() == [p * (11 - 4 * k) * s0 + (2 * p + 5 * (11 - 4 * k)) * s1 + 10 * s2 for k in range(n)]


def solve(n):
    a, b = generate(n)
    digest, trace, first, last = sha256(), 0, None, None
    for i, row in enumerate(multiply_rows(a, b)):
        if not row_matches(n, i, row):
            raise VerificationError("row %d of C differs from the closed form" % i)
        algo_common.update_digest(digest, row)
        trace += row[i]
        if i == 0:
            first = row[0]
        last = row[-1]
    return {
        "input": {"n": n, "element_type": "int64", "multiply_adds": n ** 3, "a": "A[i][j] = 3*i + 5*j - 7", "b": "B[j][k] = 2*j - 4*k + 11"},
        "check": {"name": "closed_form_every_element", "passed": True, "elements_checked": n * n,
                  "oracle": "C[i][k] = p*q*n + (2p+5q)*n(n-1)/2 + 10*(n-1)n(2n-1)/6 with p = 3i-7, q = 11-4k"},
        "result": {"c_sha256": digest.hexdigest(), "digest_format": "SHA-256 of C, row-major little-endian int64", "trace": trace, "c_first": first,
                   "c_last": last},
    }


def main(argv=None):
    return algo_common.main("matrix-multiply", "Exact dense matrix multiplication with a closed-form check of every element.", SIZES, solve, argv)


if __name__ == "__main__":
    sys.exit(main())
