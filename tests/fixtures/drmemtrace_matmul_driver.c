#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>

struct memref_2d {
  float *allocated;
  float *aligned;
  int64_t offset;
  int64_t sizes[2];
  int64_t strides[2];
};

extern void _mlir_ciface_matmul_kernel(struct memref_2d *, struct memref_2d *,
                                       struct memref_2d *);

alignas(64) static float a[256], b[256], c[256];

int main(void) {
  struct memref_2d da = {a, a, 0, {16, 16}, {16, 1}};
  struct memref_2d db = {b, b, 0, {16, 16}, {16, 1}};
  struct memref_2d dc = {c, c, 0, {16, 16}, {16, 1}};
  for (int i = 0; i < 256; ++i) {
    a[i] = 1.0f;
    b[i] = 1.0f;
    c[i] = 0.0f;
  }
  _mlir_ciface_matmul_kernel(&da, &db, &dc);
  for (int i = 0; i < 256; ++i)
    if (c[i] != 16.0f)
      return 1;
  printf("a=%p b=%p c=%p size=%zu result=16\n", (void *)a, (void *)b, (void *)c,
         sizeof(a));
  return 0;
}
