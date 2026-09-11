# Heap introspection

`mi_heap_snapshot` + `mi-heapview` give a full census of the heap structure (arenas, pages,
blocks, fragmentation): "what is in memory, and how fragmented is it?". It costs nothing until
it is called. To find out *which code* allocated the memory, use the sampled profiling hooks in
`include/mimalloc-profile.h` (`mi_profiler_t` with `on_alloc`/`on_free`).

## Heap snapshot

Offline analysis of mimalloc heap snapshots written by `mi_heap_snapshot()`.

## Producing a snapshot

```c
#include <mimalloc.h>
int fd = open("/tmp/heap.bin", O_WRONLY|O_CREAT|O_TRUNC, 0644);
mi_heap_snapshot(fd, MI_SNAPSHOT_BLOCKS);
close(fd);
```

The snapshot is best-effort and does not stop other threads. Per-block free
maps (`MI_SNAPSHOT_BLOCKS`) are only recorded for pages owned by the calling
thread; pages owned by other threads still report accurate `used`/`committed`
counts but not which individual blocks are live.

Optionally write a sidecar `<snapshot>.meta.json` to name threads:

```json
{"threads": {"0x1f77d58c0": "main", "0x70000a8c2000": "http-fetch"}}
```

## Investigating "why is this process using so much memory"

1. **Overview** — what's big?
   ```
   mi-heapview heap.bin summary --human
   mi-heapview heap.bin sizes --human --top 20
   ```
   High `frag%` on a size class = fragmentation (committed pages with few used
   blocks). Low `frag%` + high `committed` = lots of live objects.

2. **Is it a leak or working set?** — take two snapshots N requests apart:
   ```
   mi-heapview s1.bin diff s2.bin --human
   ```
   `Δblocks` per request × request count ≈ total growth → leak confirmed.

3. **Who?** — group by heap (survives page abandonment) or thread:
   ```
   mi-heapview s1.bin diff s2.bin --by-heap --human
   mi-heapview heap.bin sizes --by-tid --human
   ```
   `--by-heap` is meaningful if the application creates separate `mi_heap_t`
   per subsystem via `mi_heap_new()`. `--by-tid` shows current page owner;
   note that full pages are abandoned (tid=0).

4. **What?** — dump sample block contents from a coredump:
   ```
   # Linux: gcore -o core $PID
   # macOS: lldb -p $PID -o 'process save-core --style modified-memory core'
   mi-heapview heap.bin peek --core core --size 320 --sample 8
   ```
   If the first 8 bytes are constant across samples, it's likely a vtable
   pointer or type tag. Look it up with `nm <binary> | grep <addr>`.

5. **Where exactly?** — drill into pages and blocks:
   ```
   mi-heapview heap.bin pages --size 320 --sort waste --top 50
   mi-heapview heap.bin blocks --addr 0x7f8a01200140
   ```

## Commands

| cmd | purpose |
|---|---|
| `summary` | totals: arenas, pages, committed/used, frag% |
| `sizes [--top N] [--by-tid\|--by-heap]` | per-size-class histogram |
| `diff <s2> [--by-tid\|--by-heap]` | per-(size,group) delta: s2 − s1 |
| `peek --core F --size B [--tid T] [--sample N] [--bytes N]` | hexdump sample blocks from coredump |
| `frag [--top N] [--min-waste B]` | pages by waste (committed − used) |
| `arenas` | per-arena commit/free/purge slice counts |
| `pages [--size B] [--sort waste\|addr\|used] [--top N]` | page list |
| `blocks --addr A` | live block addresses in page containing A |
| `json` | full machine-readable dump |

`--human` prints sizes as KiB/MiB/GiB (default is raw bytes for scripting).

## Limitations

- Live OS-direct (non-arena) pages owned by other threads are not enumerated.
  Almost everything goes through arenas, so this is rarely material.
- `peek` requires the snapshot and coredump to be from the same process state
  (take the snapshot, then immediately dump core).
- Snapshots are not portable across endianness.
