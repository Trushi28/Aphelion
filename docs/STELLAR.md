# Stellar FS

`kernel/src/fs/stellar.cpp`. A `Constellation` (directory) is a star whose data is a chain of
edge sectors (7 per sector, unbounded), so one star can be linked from several directories
with no hard-link special case.

```text
sector 0          superblock: magic, version 5, catalog root, next star id, epoch, snapshot table
sectors 1..n      free-sector bitmap
sectors n+1..     B+tree nodes (1 sector each), directory sectors, file extents
```

The **catalog** is a disk-backed B+tree keyed by 64-bit star ID (leaf fanout 10, internal
fanout 30). Files are one contiguous extent with a CRC32 in the catalog entry.

### O(1) snapshots via epoch copy-on-write

The superblock holds a global `epoch`. Every catalog node, directory sector and file extent
records the epoch it was written in, and may be modified **in place only if that equals the
current epoch**; otherwise it is copied first (path-copied up to the root for the catalog).

```mermaid
flowchart LR
  subgraph before["Before snapshot, epoch 1"]
    R1["root"] --> L1["leaf: a.txt, b.txt"]
  end
  subgraph after["After snapshot, then rewrite a.txt, epoch 2"]
    S["snapshot 1 root"] --> L1b["old leaf, frozen"]
    R2["new root"] --> L2["new leaf, copied on write"]
  end
  before -->|"snapshot: save root, epoch++"| after
```

`snapshot()` is three steps: record `{catalog_root, epoch}` in the snapshot table, `epoch++`,
write the superblock. **One sector write, no reads, regardless of tree size**: everything that
existed is now older than the current epoch and becomes copy-on-write on its next change.

A snapshot is a *view*: `find`, `list`, `read_file` and `verify_file` take an optional snapshot
ID (`stellar::LIVE` = 0 is the default), and star IDs are stable across views.

- **File rewrites** allocate a fresh extent. The old one is freed immediately if it was born in
  the current epoch (no snapshot can reference it) and retained if older. Rewriting one file
  300 times with no snapshot in between costs no net space.
- **First write after a snapshot** costs O(log n) node copies in the catalog, and for a directory
  copies that directory's sector chain: O(directory size), the same order as the linear scan every
  directory operation already does.

### Deletion and reclamation

Each catalog entry carries a link count. `unlink(dir, name)` removes one edge; the star dies when
its last edge goes, and a non-empty directory is refused. A dead star's extents are returned
immediately if they were born in the current epoch, because no snapshot can reference them.
Anything older stays allocated for the snapshots that may still use it.

`delete_snapshot(id)` tombstones a slot in the snapshot table, and the next `snapshot()` reuses it.
Space is reclaimed by `gc()`, a stop-the-world mark and sweep: it marks the metadata region plus
every catalog node, directory chain and file extent reachable from the live root and from each
remaining snapshot root, then rewrites the free bitmap to match. Nothing reachable can be freed,
and sectors leaked by failed operations are recovered as a side effect. It is O(filesystem size)
and needs one bit of RAM per sector.

### I/O efficiency

Three things were making the filesystem slow, all fixed: the whole free bitmap was rewritten on
*every* allocation; there was no sector cache, so every B+tree level was a disk round trip; and the
superblock was written for every new star ID. Now only dirty bitmap sectors are written, once per
operation; a 256-slot write-through cache (128 KiB) serves repeat reads; allocation is next-fit with
whole-byte skipping in both directions.

Measured with the host-side harness (131072-sector disk), old implementation vs new in the same harness:

| Operation | Before | After |
|---|---:|---:|
| `create_file` into a directory (avg of 100) | 52.8 writes, 12.1 reads | **6.7 writes, 0.4 reads** |
| Snapshot of a ~120-entry tree | 3,999 writes, 2,742 reads | **1 write, 0 reads** |
| Snapshot of a 700+ entry tree with a 5-deep subtree | recursive copy, grows with size | **1 write, 0 reads** |
| NVMe interrupts during boot self-tests | 271 | **16** |

### Second pass

- **Directory appends are O(1).** A directory whose head sector is current has a current chain, so
  `dir_make_current` checks one sector instead of walking the chain, and `add_edge` resumes from the
  last sector it filled instead of the head. `unlink` drops that hint.
- **Extents move in batches.** Writes send every whole sector in one batched request and only the
  padded tail sector singly. `read_file` and `verify_file` batch the same way, and a read no longer
  needs an output buffer rounded up to a sector.
- **Group commit.** `begin_batch()` / `end_batch()` hold the bitmap and superblock writes until the
  outermost batch ends. Data and catalog nodes are still written through immediately, so a crash
  inside a batch loses more than a crash between single operations.
- **CRC32 is slice-by-8**, checked against the bitwise reference at every length and alignment.
- **The cache is 256 slots**, so B+tree interior nodes survive a directory walk.

## Directory index and snapshot filters

Directories stay a chain of 7-entry sectors on disk, and nothing in the on-disk format changed. Lookups
are served from RAM instead of walking that chain.

**Live view: a hash index per directory.** The first access to a directory scans its chain once and builds an
open-addressing table (4 directories cached, least recently used evicted). Each entry is
`hash -> (star, sector, slot)`: the *address of the node* holding the entry. A lookup probes the table, then
reads that one sector and compares the real name, so a hash collision can never return a wrong answer.
Appends go to a slot from a small free-slot stack (filled by `unlink`) or to the tail sector, and
`unlink` finds its entry the same way instead of scanning. Measured in the host harness: 1000 creates in one
directory cost the same number of sector lookups per create at the start and at the end, and a find in a
1000-entry directory costs about 4 sector lookups.

The index is keyed on the directory's head sector. A snapshot starts a new epoch and the next write to the
directory copies its chain to new sectors, which changes the head, so a stale index is never used and is
rebuilt on the next access (one pass over the directory, the same order as the copy). If memory for an
index cannot be had, every operation falls back to the chain scan and still works.

**Snapshot views: a Bloom filter per (snapshot, directory).** Snapshots are immutable, so one pass builds a
filter of about 16 bits per entry (4 probes) and it never goes stale. A name the filter rejects is reported
absent without touching the directory: in the host harness 1996 of 2000 absent-name lookups were rejected,
and the 4 that were not were the filter's false positives. A name the filter passes is then looked up by the
scan, so positive lookups in a snapshot are still linear in the directory. 8 filters are cached; they are
dropped when a snapshot is deleted, on mount and on format.

## Status codes

Every operation that can fail takes an optional trailing `Status* why`. The return value keeps its old shape
(`bool`, a star ID or `INVALID_STAR`), and `*why` says which of these happened: `NotMounted`, `NotFormatted`,
`InvalidArgument`, `InvalidName`, `NotFound`, `Exists`, `NotADirectory`, `IsADirectory`, `NotEmpty`,
`NoSpace`, `NoMemory`, `Io`, `Checksum`, `Corrupt`, `Busy`, `NoSuchSnapshot`, `TooManySnapshots`. `Internal`
means a failure path forgot to say why; the tests assert it never appears. It is an out-parameter rather than
the return type because `Ok` is 0, so a function returning `Status` would silently invert every existing
`if (!unlink(...))`.

`read_file` returns `READ_ERROR` (`INVALID_STAR`) on failure. `0` now only means a legitimately empty read, and
a checksum mismatch is `READ_ERROR` with `Checksum`. Names must be 1 to 51 bytes with no `/`, and `.` and `..`
are refused; duplicates are refused with `Exists`.

`resolve("/a/b/c")` walks a path from the root in any view, and `resolve_parent` splits a path into the parent
directory and a validated leaf name. Repeated and trailing slashes are accepted (a trailing slash requires a
directory), and relative paths, `.` and `..` are refused because directories keep no parent pointer.

## Concurrency

Every public function takes one recursive `orbital::Mutex` (owned by the Satellite, not the core), and
`begin_batch` holds it until `end_batch`, so one thread's group commit cannot interleave with another's.
`mount` and `format` refuse to run while a batch is open. Each block driver has its own mutex around its HAL
entry points, always taken after Stellar's. There are no sleeping locks yet: a contended waiter yields and
sinks to the lowest scheduler ring so a runnable holder always outranks it.

---

## Known limitations

Stated plainly:

- **Not crash-consistent.** Data is written before the superblock, but nodes modified in place
  within an epoch are not atomic, there is no journal, and no block-device FLUSH is issued. The
  persistence checked so far is a clean QEMU reboot, not a pulled plug.
- **gc() is stop-the-world**, O(filesystem size): it holds the filesystem lock for its whole run. Space held
  by older epochs only comes back via `delete_snapshot` and `gc()`.
- Dead stars keep a 40-byte catalog entry and their IDs are never reused; the B+tree has no delete.
- A deleted snapshot's ID can be handed out again by a later `snapshot()`. The snapshot table holds 24.
- Snapshots are **whole-filesystem**, not per-subtree.
- The on-disk directory is still a linear chain. The live view is indexed in RAM, but `list`, the first access
  after a snapshot, and positive lookups in a snapshot walk the chain.
- Free slots in a directory beyond the 32 the index tracks are only reused after the next rebuild.
- `read_file` can only verify the checksum when the whole file is read; partial reads are unchecked.
- Free space is a linear bitmap, not the Universe/Orbit buddy machinery.
- Per-file CRC32 only; no per-extent or per-node checksums.
- Bumping the superblock version reformats an existing disk on next boot (v3 and v4 images aren't migrated).
- The boot self-tests that take snapshots are skipped on a mounted disk to avoid exhausting the table.

---
