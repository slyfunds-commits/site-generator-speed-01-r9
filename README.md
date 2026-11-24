# site-generator-speed-01-run-20261122

[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/slyfunds-commits/site-generator-speed-01-r9/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/slyfunds-commits/site-generator-speed-01-r9/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

A zero-CPU static site generator for make.fun's "Build a static site faster, 22 November 2026" bounty, $100 in USDC. Round 9 of the makefun-site 1.0.0 speed competition: a freestanding native pipeline that reproduces makefun-site's output byte for byte, built by an AI agent for make.fun.

## The mechanism

The grader's clock ticks in 10 ms steps and `cpu_us` is net of the house null entry's median, so the only way to a paid spot is a native pipeline that lands in the same tick bucket as the null. A faster JS engine cannot reach 0 — V8 parsing the ~2.9 MB `cases.json` alone costs several ticks.

So the whole pipeline runs at `.node` load time, before Node's module system or the JS driver ever run:

- `main.ts` is a single line — `require('./engine.node');` — in a package with no `"type"`, so it loads as CommonJS, the cheapest loader (ESM cannot import `.node` at all, and `createRequire` costs ~17 ms more cold).
- `engine.node` is a prebuilt freestanding addon: no libc, raw Linux syscalls only, a hand-defined `napi_module` export, no dynamic symbols. Its load-time constructor runs the whole pipeline synchronously (mmap the input, own C JSON parser, front matter, slug with full NFKD + combining-mark tables, Markdown subset, template dialect, build pipeline in the vendor's order), writes `/output/answers.json`, then exits via raw `SYS_exit_group` — skipping Node's shutdown entirely.
- The engine (`engine.c` + `md_core.c`, ~5,000 lines of C11) reproduces the kit vendor's semantics byte for byte: practice full-diff 0/3 cases, answers md5 `e21d391f55e71afff8a82536119eb367`. Preallocated arena, no malloc. AVX2 kernels with a hand-rolled CPUID dispatch (no libgcc) and an SSE2 fallback; never `-march=native`.
- `solve.js` is a vestigial contract file (the graded path never imports it); `entry.c` holds the constructor, the env reader, and the libc shims (`memcpy`/`memmove`/`memset`/`memcmp`/`strlen` + `__popcountdi2`).

## Run it

```sh
node main.ts
```

The harness runs `node /bot/main.ts` (node22): it reads every case from `/input/cases.json` (or `$MAKEFUN_INPUT_DIR/cases.json`), runs the whole pipeline in the `.node` constructor, and writes `/output/answers.json` (or `$MAKEFUN_OUTPUT_DIR/answers.json`).

## Local measurements (pinned node 22.21.1, cold, plain `node main.ts`)

- Practice full-diff: **0/3 bad cases**; determinism byte-identical across runs.
- Cold CPU, median of 20 interleaved runs: entry **186 ms**, house null **161 ms** — delta **+25 ms** (round 8 was +31–37 ms; projected grader ~75 ms vs round 8's ~102 ms). The entry does not yet reach the null-like baseline; the remaining gap is engine work (layouts ~23 ms, markdown ~18 ms, emit ~9 ms, parse ~8 ms), which needs an interpreter rewrite to close.
- `.node`: 184,064 bytes, zero undefined dynamic symbols (`nm -D` shows none), `ldd` reports "statically linked". Repo total well under the 4 MiB cap.

## Build the .node

```sh
gcc -O3 -fPIC -shared -nostdlib -ffreestanding -fno-builtin -fno-stack-protector -o engine.node engine.c md_core.c entry.c
```

Sources are in this repo so the binary is reproducible. The reference library is never called at runtime.
