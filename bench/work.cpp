// Deterministic CPU + file workload used by bench/run_bench.py (not installed, not part of the coffee runtime).
//   coffee_bench_work IN OUT ROUNDS
// Reads IN (any size), runs ROUNDS integer mixing passes over its 64-bit little-endian words in memory, and writes OUT:
// one hex line "<state-xor> <rounds> <bytes>" followed by 64 KiB of derived words. Output depends only on IN and ROUNDS.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: coffee_bench_work IN OUT ROUNDS\n");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const long rounds = std::strtol(argv[3], nullptr, 10);
  std::vector<uint64_t> w(bytes.size() / 8);
  for (size_t i = 0; i < w.size(); i++) {
    uint64_t v = 0;
    for (int b = 7; b >= 0; b--) v = (v << 8) | bytes[i * 8 + static_cast<size_t>(b)];
    w[i] = v;
  }
  uint64_t acc = 0x9e3779b97f4a7c15ULL;
  for (long r = 0; r < rounds; r++)
    for (size_t i = 0; i < w.size(); i++) {
      uint64_t x = w[i] ^ acc;
      x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
      w[i] = x;
      acc += x + static_cast<uint64_t>(i);
    }
  uint64_t x = 0;
  for (uint64_t v : w) x ^= v;
  std::ofstream out(argv[2], std::ios::binary);
  char head[96];
  const int n = std::snprintf(head, sizeof head, "%016llx %ld %zu\n", static_cast<unsigned long long>(x ^ acc), rounds, bytes.size());
  out.write(head, n);
  const size_t emit = w.size() < 8192 ? w.size() : 8192;
  for (size_t i = 0; i < emit; i++) out.write(reinterpret_cast<const char*>(&w[i]), 8);  // x86-64/little-endian host
  return out ? 0 : 1;
}
