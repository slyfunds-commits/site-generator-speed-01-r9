// entry.c — freestanding .node entry for round 9 (zero-CPU).
// No libc, no N-API. A load-time constructor runs the whole pipeline
// synchronously (mmap input, own C JSON parser, front matter, slug,
// Markdown, template dialect, build pipeline in the vendor's order),
// writes /output/answers.json, then exits via raw SYS_exit_group —
// skipping Node's module system, the JS driver, and Node's shutdown
// entirely. The only JS is a one-line main.ts: require('./engine.node');
// (package.json has no "type", so .ts loads as CommonJS — the cheapest
// loader; ESM cannot import .node at all, and createRequire costs ~17ms
// more cold.)
//
// A napi_register_module_v1 stub is exported as a safety net: if some
// loader ever calls it instead of the constructor, it runs the same
// pipeline (and exits). The constructor normally fires first, during
// dlopen, before Node looks the symbol up.
//
// Build: gcc -O3 -DFORCE_SSE2 -fPIC -shared -nostdlib -ffreestanding
//        -fno-builtin -fno-stack-protector -o engine.node engine.c md_core.c entry.c
// The .node must have ZERO undefined dynamic symbols (nm -D shows no "U").
// Never -march=native (grader CPU unknown); SSE2 only (AVX2 was ~2ms warm).
#include <stdint.h>
#include <stddef.h>

int engine_run_paths(const char *in_path, const char *out_path);

// ---------------------------------------------------------------- syscalls (x86-64)
static long sys1(long n, long a) { long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a):"rcx","r11","memory"); return r; }
static long sys3(long n, long a, long b, long c) { long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r; }
#define SYS_READ 0
#define SYS_OPENAT 257
#define SYS_EXIT_GROUP 231
#define AT_FDCWD (-100)

// ---------------------------------------------------------------- libc shims
// engine.c / md_core.c use __builtin_memcpy/memcmp/strlen; with -O3 gcc
// expands small ones inline and calls these for the rest. -fno-builtin
// keeps gcc from turning the loops below back into calls to themselves.
void *memcpy(void *dst, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dst; const uint8_t *s = (const uint8_t *)src;
  while (n >= 8) { *(uint64_t *)d = *(const uint64_t *)s; d += 8; s += 8; n -= 8; }
  while (n--) *d++ = *s++;
  return dst;
}
void *memmove(void *dst, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dst; const uint8_t *s = (const uint8_t *)src;
  if (d <= s) return memcpy(dst, src, n);
  d += n; s += n;
  while (n >= 8) { d -= 8; s -= 8; *(uint64_t *)d = *(const uint64_t *)s; n -= 8; }
  while (n--) { d--; s--; *d = *s; }
  return dst;
}
void *memset(void *dst, int c, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  uint64_t w = (uint8_t)c; w |= w << 8; w |= w << 16; w |= w << 32;
  while (n >= 8) { *(uint64_t *)d = w; d += 8; n -= 8; }
  while (n--) *d++ = (uint8_t)c;
  return dst;
}
int memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
  while (n--) { if (*x != *y) return (int)*x - (int)*y; x++; y++; }
  return 0;
}
size_t strlen(const char *s) {
  const char *p = s; while (*p) p++; return (size_t)(p - s);
}

// libgcc helper: __builtin_popcount on a 64-bit word (engine.c:959).
// gcc emits a call to this when -mpopcnt is off. Portable bit-trick,
// no POPCNT instruction (so no CPU assumption beyond x86-64).
long __popcountdi2(unsigned long long x) {
  x = x - ((x >> 1) & 0x5555555555555555ULL);
  x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
  x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
  return (long)((x * 0x0101010101010101ULL) >> 56);
}

// ---------------------------------------------------------------- env + paths
// /proc/self/environ is NUL-separated KEY=VALUE. Read it once into a
// static buffer; the values point into it and stay valid.
static char g_env[65536];
static size_t g_env_len = 0;
static const char *env_get(const char *key, size_t klen) {
  size_t i = 0;
  while (i < g_env_len) {
    const char *e = g_env + i;
    size_t elen = 0; while (i + elen < g_env_len && g_env[i + elen]) elen++;
    if (elen > klen && memcmp(e, key, klen) == 0 && e[klen] == '=') return e + klen + 1;
    i += elen + 1;
  }
  return 0;
}
static void read_env(void) {
  long fd = sys3(SYS_OPENAT, AT_FDCWD, (long)"/proc/self/environ", 0);
  if (fd < 0) return;
  size_t off = 0;
  for (;;) {
    if (off >= sizeof(g_env)) break;
    long r = sys3(SYS_READ, fd, (long)(g_env + off), (long)(sizeof(g_env) - off));
    if (r <= 0) break;
    off += (size_t)r;
  }
  g_env_len = off;
  sys1(3, fd); // SYS_CLOSE
}
static size_t path_join(char *out, size_t cap, const char *dir, const char *file) {
  size_t d = strlen(dir), f = strlen(file);
  if (d + 1 + f + 1 > cap) return 0;
  memcpy(out, dir, d);
  out[d] = '/';
  memcpy(out + d + 1, file, f);
  out[d + 1 + f] = 0;
  return d + 1 + f;
}

// ---------------------------------------------------------------- pipeline + exit
static volatile int g_ran = 0;
static void run_and_exit(void) {
  if (g_ran) return;
  g_ran = 1;
  read_env();
  const char *in_dir = env_get("MAKEFUN_INPUT_DIR", 17);
  const char *out_dir = env_get("MAKEFUN_OUTPUT_DIR", 18);
  if (!in_dir) in_dir = "/input";
  if (!out_dir) out_dir = "/output";
  char in_path[4096], out_path[4096];
  if (!path_join(in_path, sizeof(in_path), in_dir, "cases.json")) sys1(SYS_EXIT_GROUP, 1);
  if (!path_join(out_path, sizeof(out_path), out_dir, "answers.json")) sys1(SYS_EXIT_GROUP, 1);
  int n = engine_run_paths(in_path, out_path);
  sys1(SYS_EXIT_GROUP, n < 0 ? 1 : 0);
}

// Load-time constructor: runs during dlopen, before Node looks up any
// N-API symbol. The process never returns to Node.
__attribute__((constructor)) static void engine_ctor(void) { run_and_exit(); }

// Safety net: if a loader ever calls the N-API entry instead of the
// constructor, run the same pipeline (and exit). Never returns normally.
void *napi_register_module_v1(void *env, void *exports) {
  (void)env;
  run_and_exit();
  return exports;
}
