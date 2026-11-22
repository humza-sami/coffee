"""Tiny shared helpers for the algorithm examples: the --size/--output command line, the result file and the resource readings.

Standard library only. A run reads nothing, uses no network and writes exactly one file: the path given to --output.
"""
import argparse
import hashlib
import json
import os
import sys
import time
from array import array

sys.dont_write_bytecode = True  # never leave a __pycache__ in an installed or staged tree

SIZE_NAMES = ("light", "medium", "heavy")
CHUNK = 1 << 16  # elements per hashing step, so digesting never copies a whole buffer


class VerificationError(Exception):
    """The computed answer failed its independent check; the run exits 1 and writes no result file."""


def _proc_status_bytes(field):
    try:
        with open("/proc/self/status") as f:
            for line in f:
                if line.startswith(field + ":"):
                    return int(line.split()[1]) * 1024
    except (OSError, ValueError, IndexError):
        pass
    return None


def peak_rss_bytes():
    """Peak resident set of this process (VmHWM), or None when /proc is unavailable. ru_maxrss is avoided: it can carry the parent's peak across exec."""
    return _proc_status_bytes("VmHWM")


def peak_virtual_bytes():
    """Peak address space (VmPeak). A Coffee Bean limits each Step with RLIMIT_AS = memory_mb, which counts this, not resident memory."""
    return _proc_status_bytes("VmPeak")


def update_digest(h, arr):
    """Feed an array('q') into hash h as little-endian int64 values, whatever the platform byte order, without copying all of it."""
    view = memoryview(arr)
    for lo in range(0, len(arr), CHUNK):
        part = view[lo:lo + CHUNK]
        if sys.byteorder == "big":
            swapped = array("q", part.tolist())
            swapped.byteswap()
            h.update(swapped.tobytes())
        else:
            h.update(part.cast("B"))


def sha256_of(arr):
    h = hashlib.sha256()
    update_digest(h, arr)
    return h.hexdigest()


def _output_error(parser, message):
    parser.error("output " + message)


def _check_output(parser, path):
    """Refuse a bad output path before any work is done, and never allow the example's own files to be overwritten."""
    if not path:
        _output_error(parser, "path is empty")
    if not path.endswith(".json"):
        _output_error(parser, "path must end in .json (refusing to write '%s')" % path)
    if os.path.isdir(path):
        _output_error(parser, "path '%s' is a directory" % path)
    parent = os.path.dirname(os.path.abspath(path))
    if not os.path.isdir(parent):
        _output_error(parser, "directory '%s' does not exist" % parent)
    here = os.path.dirname(os.path.abspath(__file__))
    for name in os.listdir(here):
        other = os.path.join(here, name)
        if os.path.exists(path) and os.path.exists(other) and os.path.samefile(path, other):
            _output_error(parser, "path '%s' is one of the example's own files" % path)
    if os.path.exists(path) and not os.path.isfile(path):
        _output_error(parser, "path '%s' is not a regular file" % path)
    if not os.access(path if os.path.exists(path) else parent, os.W_OK):
        _output_error(parser, "path '%s' is not writable" % path)


def main(algorithm, description, sizes, solve, argv=None):
    """Parse the command line, run solve(size_parameter) once and write the result JSON.

    solve returns {"input": ..., "check": {"passed": True, ...}, "result": ...} or raises VerificationError.
    Exit status: 0 verified result written, 1 verification failed or the work could not finish, 2 bad arguments or output path.
    """
    parser = argparse.ArgumentParser(prog=os.path.basename(sys.argv[0]), description=description, allow_abbrev=False)
    parser.add_argument("--size", choices=SIZE_NAMES, default="light", help="light (default), medium or heavy; heavy is opt-in")
    parser.add_argument("--output", required=True, metavar="FILE.json", help="the only file written")
    args = parser.parse_args(argv)
    _check_output(parser, args.output)
    started = time.perf_counter()
    try:
        body = solve(sizes[args.size])
        if body.get("check", {}).get("passed") is not True:
            raise VerificationError("the check did not pass")
    except VerificationError as e:
        print("%s: verification failed: %s" % (algorithm, e), file=sys.stderr)
        return 1
    except MemoryError:
        print("%s: out of memory at size %s (address-space limit too small?)" % (algorithm, args.size), file=sys.stderr)
        return 1
    elapsed = time.perf_counter() - started
    result = {"algorithm": algorithm, "size": args.size}
    result.update(body)
    result["elapsed_seconds"] = round(elapsed, 6)
    result["peak_rss_bytes"] = peak_rss_bytes()
    result["peak_virtual_bytes"] = peak_virtual_bytes()
    try:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(json.dumps(result, indent=2, sort_keys=True) + "\n")
    except OSError as e:
        print("%s: cannot write output: %s" % (algorithm, e), file=sys.stderr)
        return 2
    print("%s %s: verified in %.2f s, peak RSS %s MiB -> %s" % (algorithm, args.size, elapsed,
          "%.1f" % (result["peak_rss_bytes"] / 2**20) if result["peak_rss_bytes"] else "unknown", args.output))
    return 0
