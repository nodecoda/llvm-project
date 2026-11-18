#include <cstdio>
#include <cstdlib>
#include <cuda_runtime.h>
#include "prog.h"

struct Sr { int v[8]; };

__device__ __forceinline__ int exec_prog(const Inst *prog, int nprog, Sr sr,
                                         int *g, int *sh) {
  int r[32], p[8];
#pragma unroll
  for (int i = 0; i < 32; ++i) r[i] = 0;
#pragma unroll
  for (int i = 0; i < 8; ++i) p[i] = 0;
  int pc = 0;
  while (pc < nprog) {
    Inst in = prog[pc];
    if (in.pe && !p[in.pr]) { ++pc; continue; }
    switch (in.op) {
    case 0: return r[2];                       // EXIT
    case 1: r[in.rd] = in.imm; break;          // MOVI
    case 2: r[in.rd] = r[in.rs1]; break;       // MOV
    case 3: r[in.rd] = r[in.rs1] + r[in.rs2]; break;
    case 4: r[in.rd] = r[in.rs1] - r[in.rs2]; break;
    case 5: r[in.rd] = r[in.rs1] * r[in.rs2]; break;
    case 6: r[in.rd] = r[in.rs1] >> in.imm; break;
    case 7: p[in.pr] = (r[in.rs1] < r[in.rs2]); break;
    case 8: pc += in.imm; continue;            // BRA
    case 9: r[in.rd] = g[r[in.rs1] >> 2]; break;
    case 10: g[r[in.rs1] >> 2] = r[in.rs2]; break;
    case 11: __syncthreads(); break;           // SYNC
    case 12: r[in.rd] = sr.v[in.imm]; break;   // SR
    case 13: pc += in.imm; continue;           // JMP
    case 14: r[in.rd] = sh[r[in.rs1] >> 2]; break;
    case 15: sh[r[in.rs1] >> 2] = r[in.rs2]; break;
    default: return -1;
    }
    ++pc;
  }
  return r[2];
}

__global__ void gm32_run(const Inst *prog, int nprog, Sr sr, int *g) {
  extern __shared__ int sh[];
  sr.v[0] = threadIdx.x;
  sr.v[1] = blockIdx.x;
  exec_prog(prog, nprog, sr, g, sh);
}

__global__ void vecadd_ref(const int *a, const int *b, int *o, int n) {
  int i = blockIdx.x * 32 + threadIdx.x;
  if (i < n) o[i] = a[i] + b[i];
}

__global__ void reduce_ref(const int *a, int *o) {
  __shared__ int s[32];
  int t = threadIdx.x;
  s[t] = a[blockIdx.x * 32 + t];
  __syncthreads();
  for (int st = 16; st > 0; st >>= 1) {
    if (t < st) s[t] += s[t + st];
    __syncthreads();
  }
  if (t == 0) o[blockIdx.x] = s[0];
}

static float bench(void (*launch)(void *), void *arg, int iters) {
  cudaEvent_t e0, e1;
  cudaEventCreate(&e0); cudaEventCreate(&e1);
  launch(arg);  // warmup
  cudaEventRecord(e0);
  for (int i = 0; i < iters; ++i) launch(arg);
  cudaEventRecord(e1);
  cudaEventSynchronize(e1);
  float ms = 0; cudaEventElapsedTime(&ms, e0, e1);
  cudaEventDestroy(e0); cudaEventDestroy(e1);
  return ms / iters;
}

struct VecCtx { const Inst *prog; int n; int *g; Sr sr; };
static VecCtx vc, rc;
static void launch_vec_interp(void *p) {
  gm32_run<<<vc.n / 32, 32, 32 * sizeof(int)>>>(vc.prog, PROG_VECADD_N, vc.sr, vc.g);
}
static void launch_reduce_interp(void *p) {
  gm32_run<<<rc.n / 32, 32, 32 * sizeof(int)>>>(rc.prog, PROG_REDUCE_N, rc.sr, rc.g);
}
static void launch_vec_ref(void *p) {
  vecadd_ref<<<vc.n / 32, 32>>>(vc.g, vc.g + vc.n, vc.g + 2 * vc.n, vc.n);
}
static void launch_reduce_ref(void *p) {
  reduce_ref<<<rc.n / 32, 32>>>(rc.g, rc.g + rc.n);
}

int main() {
  int n = 1 << 20, nb = n / 32;
  cudaDeviceProp prop; cudaGetDeviceProperties(&prop, 0);
  printf("gpu=%s sm_%d%d warpSize=%d shared/block=%zu\n", prop.name, prop.major,
         prop.minor, prop.warpSize, prop.sharedMemPerBlock);

  int *g; cudaMalloc(&g, sizeof(int) * 3 * n);
  Inst *d_vec, *d_red;
  cudaMalloc(&d_vec, sizeof(Inst) * PROG_VECADD_N);
  cudaMalloc(&d_red, sizeof(Inst) * PROG_REDUCE_N);
  cudaMemcpy(d_vec, PROG_VECADD, sizeof(Inst) * PROG_VECADD_N, cudaMemcpyHostToDevice);
  cudaMemcpy(d_red, PROG_REDUCE, sizeof(Inst) * PROG_REDUCE_N, cudaMemcpyHostToDevice);
  int *h = (int *)malloc(sizeof(int) * 3 * n);
  for (int i = 0; i < n; ++i) { h[i] = i % 17; h[n + i] = (i * 3) % 19; }
  cudaMemcpy(g, h, sizeof(int) * 2 * n, cudaMemcpyHostToDevice);

  // ---- vecadd: GM32-style interpreter on GPU ----
  vc = {d_vec, n, g, {0, 0, 0, 0, n, 0, n * 4, 2 * n * 4}};
  launch_vec_interp(nullptr);
  cudaError_t e = cudaDeviceSynchronize();
  if (e != cudaSuccess) { printf("kernel error: %s\n", cudaGetErrorString(e)); return 1; }
  cudaMemcpy(h + 2 * n, g + 2 * n, sizeof(int) * n, cudaMemcpyDeviceToHost);
  int bad = 0;
  for (int i = 0; i < n; ++i)
    if (h[2 * n + i] != ((i % 17) + (i * 3) % 19)) ++bad;
  printf("vecadd  interp mismatches=%d\n", bad);

  cudaMemset(g + 2 * n, 0, sizeof(int) * n);
  launch_vec_ref(nullptr); cudaDeviceSynchronize();
  cudaMemcpy(h + 2 * n, g + 2 * n, sizeof(int) * n, cudaMemcpyDeviceToHost);
  bad = 0;
  for (int i = 0; i < n; ++i)
    if (h[2 * n + i] != ((i % 17) + (i * 3) % 19)) ++bad;
  printf("vecadd  cuda_ref mismatches=%d\n", bad);

  float t_interp = bench(launch_vec_interp, nullptr, 20);
  float t_ref = bench(launch_vec_ref, nullptr, 20);

  // ---- block reduction: interpreter vs CUDA ref ----
  int *out_ref = (int *)malloc(sizeof(int) * nb);
  rc = {d_red, n, g, {0, 0, 0, 0, n, 0, n * 4, (int)(sizeof(int) * n)}};
  cudaMemset(g + n, 0, sizeof(int) * nb);
  launch_reduce_interp(nullptr); cudaDeviceSynchronize();
  cudaMemcpy(out_ref, g + n, sizeof(int) * nb, cudaMemcpyDeviceToHost);
  int *sum_ref = (int *)malloc(sizeof(int) * nb);
  for (int b = 0; b < nb; ++b) {
    int s = 0;
    for (int i = 0; i < 32; ++i) s += (b * 32 + i) % 17;
    sum_ref[b] = s;
  }
  bad = 0;
  for (int b = 0; b < nb; ++b) if (out_ref[b] != sum_ref[b]) ++bad;
  printf("reduce  interp mismatches=%d (of %d blocks)\n", bad, nb);

  float t_red_interp = bench(launch_reduce_interp, nullptr, 20);
  float t_red_ref = bench(launch_reduce_ref, nullptr, 20);

  double lanes = (double)nb * 32;
  double ins_i = lanes * PROG_VECADD_N;
  printf("\n%-22s %10s %12s %14s\n", "path", "ms/launch", "Gelem/s", "Ginstr/s");
  printf("%-22s %10.4f %12.2f %14.2f\n", "GM32 interp on GPU", t_interp, n / t_interp / 1e6,
         ins_i / (t_interp * 1e-3) / 1e9);
  printf("%-22s %10.4f %12.2f %14s\n", "native CUDA vecadd", t_ref, n / t_ref / 1e6, "-");
  printf("%-22s %10.4f %12s %14s\n", "GM32 interp reduce", t_red_interp, "-", "-");
  printf("%-22s %10.4f %12s %14s\n", "native CUDA reduce", t_red_ref, "-", "-");
  printf("interp/native overhead vecadd: %.0fx\n", t_interp / t_ref);
  return 0;
}
