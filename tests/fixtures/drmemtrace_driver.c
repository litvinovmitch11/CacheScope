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

extern void _mlir_ciface_matmul_kernel(struct memref_2d *);

alignas(64) static float data[64];

int main(void) {
  struct memref_2d descriptor = {data, data, 0, {8, 8}, {8, 1}};
  data[32] = 15.0f;
  _mlir_ciface_matmul_kernel(&descriptor);
  printf("base=%p size=%zu result=%.0f\n", (void *)data, sizeof(data),
         (double)data[1]);
  return data[1] == 16.0f ? 0 : 1;
}
