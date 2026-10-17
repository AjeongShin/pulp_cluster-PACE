#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "data.h"
float expf_rvf(float x);
int main(void) {
  uint32_t maxulp = 0; int exact = 0;
  printf("// Host-generated reference: expf_rvf.c built natively without FMA contraction,\n");
  printf("// evaluated on the ifmap[] of the data.h in DATA_DIR. Regenerate with `make ref`.\n");
  printf("const uint32_t expc_ref[%d] = {\n", INPUTS_LEN);
  for (int i = 0; i < INPUTS_LEN; i++) {
    float x, y, z; uint32_t yb, zb;
    memcpy(&x, &ifmap[i], 4);
    y = expf_rvf(x); z = expf(x);
    memcpy(&yb, &y, 4); memcpy(&zb, &z, 4);
    uint32_t d = yb > zb ? yb - zb : zb - yb;
    if (d > maxulp) maxulp = d;
    exact += (d == 0);
    printf("  0x%08x,%s", yb, (i % 6 == 5) ? "\n" : "");
  }
  printf("\n};\n");
  fprintf(stderr, "vs host libm expf: exact %d / %d, max ulp diff %u\n", exact, INPUTS_LEN, maxulp);
  return 0;
}
