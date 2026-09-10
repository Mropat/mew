// Verifies the emitter against the independently assembled reference bytes.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "patch.inc"
int main() {
    BYTE out[72];
    // Relative encodings only depend on differences, so the real RVAs work here.
    BYTE* blk = (BYTE*)0x276811;
    size_t n = build_patch(blk, (BYTE*)0x2767EB, (BYTE*)0x2767F9,
                           (BYTE*)0x276940, (BYTE*)0x2769F0, (BYTE*)0x276AB0,
                           out, sizeof(out));
    if (!n) { printf("BUILD FAILED\n"); return 1; }
    for (size_t i = 0; i < n; ++i) printf("%02x", out[i]);
    printf("\n");
    return 0;
}
