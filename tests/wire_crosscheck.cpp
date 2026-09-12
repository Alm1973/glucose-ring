// Feeds the Android formatter's exact bytes through the firmware's real
// rp_parse(), using a copy of pumpUart()'s framing (glucose-ring.ino:680-702),
// in 20-byte chunks as at the default 23-byte MTU.
//
// Run via tests/run_wire_crosscheck.sh, which produces the inputs from the
// app's WireExportTest and builds this against reading_parser.cpp.
//
// usage: wire_crosscheck lines.bin expected.tsv
#include "../reading_parser.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static const size_t RX_LINE_MAX = 96;   // glucose-ring.ino:136

int main(int argc, char **argv)
{
  if (argc != 3) { fprintf(stderr, "usage: %s lines.bin expected.tsv\n", argv[0]); return 2; }
  std::ifstream fb(argv[1], std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(fb)), std::istreambuf_iterator<char>());
  std::ifstream fe(argv[2]);
  std::vector<std::string> expected;
  for (std::string l; std::getline(fe, l);) expected.push_back(l);

  char line[RX_LINE_MAX];
  size_t len = 0, idx = 0, ok = 0, bad = 0;
  bool overflow = false;

  for (size_t off = 0; off < bytes.size(); off += 20) {          // one BLE write per chunk
    size_t end = off + 20 < bytes.size() ? off + 20 : bytes.size();
    for (size_t i = off; i < end; i++) {
      char c = bytes[i];
      if (c == '\n') {
        bool good = false;
        char got[200] = "";
        const char *why = "overflow";
        if (!overflow) {
          line[len] = '\0';
          RpReading r;
          RpResult res = rp_parse(line, &RP_DEFAULT_LIMITS, &r);
          why = rp_result_name(res);
          if (res == RP_OK) {
            snprintf(got, sizeof got, "%d\t%s\t%llu", (int)r.sgv, r.direction, (unsigned long long)r.epoch_ms);
            good = idx < expected.size() && expected[idx] == got;
            if (!good) why = "decoded differently";
          }
        }
        if (good) ok++;
        else if (++bad <= 10)
          fprintf(stderr, "line %zu: %s got=[%s] expected=[%s]\n", idx, why, got,
                  idx < expected.size() ? expected[idx].c_str() : "(none)");
        idx++; len = 0; overflow = false;
        continue;
      }
      if (c == '\r') continue;
      if (len >= RX_LINE_MAX - 1) { overflow = true; continue; }
      line[len++] = c;
    }
  }
  printf("bytes=%zu lines=%zu expected=%zu ok=%zu bad=%zu unterminated_tail=%zu\n",
         bytes.size(), idx, expected.size(), ok, bad, len);
  return (bad == 0 && idx == expected.size() && len == 0 && idx > 0) ? 0 : 1;
}
