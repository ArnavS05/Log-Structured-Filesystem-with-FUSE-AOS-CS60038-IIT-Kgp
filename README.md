# Assignment 3 - LogFS

Tasks A-F are implemented. Build and run on Linux using the instructions below.


## Team information

Group number: 6
Arham J Bhansali 23CS30007
Arnav Singh 23CS30009
Shrey Patel 23CS10051

## Directory structure

| File | Purpose | Status |
| --- | --- | --- |
| `include/logfs.h` | Disk structures, constants and common helpers | Task A |
| `include/cache.h` | Cache and segment-buffer declarations | Task B |
| `include/lfs.h` | Required `lfs_*` function declarations | Task C |
| `src/disk.cpp` | CRC, bitmap and disk I/O helpers | Task A |
| `src/cache.cpp` | Data/inode caches and segment buffer | Task B |
| `src/lfs.cpp` | Mount, unmount and file/directory operations | Task C |
| `src/gc.cpp` | Garbage collection | Task D |
| `tools/mkfs.cpp` | Creates `logfs.img` | Task A |
| `tools/mount.cpp` | FUSE wrappers and mount program | Task E |
| `tests/test_lfs.cpp` | Required Phase 1 driver, including GC checks | Tasks C-D |
| `kernel/logfs_stats.c` | Flush counter module | Task F |
| `kernel/Makefile` | Kernel module build | Task F |
| `Makefile` | Builds userspace programs and kernel module | Tasks A-F |
| `README.md` | Layout, build and usage | Updated task by task |

All source files above are implemented. Executables and the image are generated
in the project root; module build files are generated in `kernel/`.

## Build and run

Requires Linux on a little-endian machine, g++, make, pkg-config, libfuse3
development files and matching kernel headers/build files. No FUSE or kernel
build is needed for `make phase1`. See Tasks E and F for package installation
and kernel build-path instructions.

```sh
make
./mkfs.logfs
make check
```

`make` builds `mkfs.logfs`, `liblogfs.a`, `test_lfs`, `mount.logfs` and
`kernel/logfs_stats.ko`. Install FUSE 3 development files and matching kernel
headers first (Tasks E and F below). `make userspace` builds everything except
the module; `make module` builds only the module.
`make phase1` builds only A-D and does not require FUSE. The library contains disk
helpers, cache/buffer code, filesystem operations and garbage collection.
The default formatter output is `logfs.img`. An optional image path is accepted:

```sh
./mkfs.logfs another.img
```

An existing file is refused. To reformat, delete only the image you intend to
replace, then run the formatter again. `make clean` removes the executable,
but does not delete any image. It also removes `test_lfs`, `mount.logfs`, object files, `liblogfs.a` and
generated module files. Source files and existing images are kept.

## Task A layout

The image is exactly 256 MiB (268435456 bytes), with 65536 blocks of 4096 bytes.

| Blocks | Contents |
| --- | --- |
| 0 | Superblock |
| 1-3 | Checkpoint |
| 4-67 | Segment 0 |
| 4 | Segment 0 header |
| 5 | Root inode and its initial MKDIR record header |
| 6-67 | Free body blocks in segment 0 |
| 68-65475 | Other 1022 segments |
| 65476-65535 | Unused tail; too small for a whole segment |

There are 1023 complete segments. Segment `s` starts at block `4 + s * 64`.
The formatter uses `ftruncate` to create the full-size file; untouched regions
can be sparse and read as zero. It writes the whole initial segment, checkpoint
and superblock, then calls `fsync`.

The root has inode number 0, mode `S_IFDIR | 0755`, size 0, the current mtime,
and twelve zero direct pointers. It is empty, so it needs no data block.
`imap[0]` is 5. Only inode bit 0 and segment bit 0 are set.

## Structures and byte offsets

All fields are little-endian. We write zero-initialized structures directly.
The formatter refuses big-endian machines. Compile-time size/offset checks
ensure compiler padding cannot silently change the layout. Reserved/padding
bytes are zero except the record header in the first inode block's tail.

| Structure | Fields and offsets | Stored size |
| --- | --- | --- |
| Superblock | six u32 fields at 0-23; total_blocks at 24; checkpoint_block at 32; log_start_block at 40; CRC at 48 | 4096 |
| Checkpoint | imap at 0; inode bitmap at 8192; segment bitmap at 9216; CRC at 10240 | 12288 |
| Segment header | magic at 0; sequence at 4; flags at 8; block_map at 12; CRC at 268 | 4096 |
| Inode | ino at 0; mode at 8; reserved at 12; size at 16; mtime at 24; direct[12] at 32 | 4096 |
| Directory entry | ino at 0; name_len at 8; name[255] at 10; seven padding bytes at 265 | 272 |
| Record header | magic at 0; op at 4; nblocks at 8; CRC at 12 | 16 |

The two bitmap arrays retain the assignment's 1024-byte sizes. They are bitmaps,
not one byte per object: bit `i` is byte `i / 8`, mask `1 << (i % 8)`.
Their unused bytes remain zero.

Directory entries will use fixed 272-byte slots, at most 15 slots per data block.
Names use `name_len`, not a required trailing null. Names can contain up to
255 bytes. Inode number 0 is valid; an empty slot will use `name_len == 0`.
Directories have at most 12 direct blocks, as do regular files. No hard links
or indirect blocks are implemented in Task A.

## Segment map and flags

Index 0 is unused because it is the segment header. For body blocks:

- `-1`: unused block.
- `-2`: inode block.
- Nonnegative: data block owned by that inode number.

Flag bit 0 (`SEG_SEALED`) means sealed. Initial flags are 0 and sequence is 1.
Segment 0 has map slot 1 set to -2; all other slots are -1. Its first available
append slot is 2 (absolute block 6). The append tip is not in the superblock.

Future updates will leave stale map entries in place and use reachability to
identify dead blocks; this avoids treating a previously written body block as
an append position or rewriting old inode/data blocks.

## Log records

Every record occupies consecutive whole body blocks in one segment. It never
crosses a segment boundary. The first payload block is always an inode block.
Its layout is:

| Byte range in first block | Contents |
| --- | --- |
| 0-127 | Inode fields |
| 128-4079 | Zero padding |
| 4080-4083 | Record magic |
| 4084-4087 | Operation |
| 4088-4091 | Number of payload blocks |
| 4092-4095 | Record CRC |

The header shares the inode's unused tail, so it takes no extra block and needs
no extra block_map tag. The inode still occupies exactly one block, and data
blocks retain all 4096 bytes. `nblocks` counts all payload blocks, including this
first inode block. A record reader scans occupied slots, reads the header from
the first inode's tail, then advances by `nblocks`.

Payload order for later operations is fixed now:

| Operation | Payload order |
| --- | --- |
| CREATE | Child inode; changed parent directory data blocks in direct-index order; parent inode |
| MKDIR | Child inode; changed parent directory data blocks in direct-index order; parent inode |
| WRITE | Updated file inode; changed file data blocks in direct-index order |
| RMDIR | Updated parent inode; changed parent directory data blocks in direct-index order |

The initial root is a special MKDIR record containing only the root inode;
it has no parent. Its `nblocks` is 1. All pointers in an inode refer to absolute
disk block numbers. Task C will reserve the entire record's space before
building pointers, so inode-first order does not prevent forward references.
When a later inode block is reused as a new record, its old header/padding must
be cleared and a new header/checksum constructed. Other inode blocks in the
record have zero padding.

## CRC policy

Use standard CRC-32/IEEE (polynomial 0xedb88320, initial value 0xffffffff,
final XOR 0xffffffff), compatible with Python's `zlib.crc32`.

- Superblock: bytes 0-47.
- Checkpoint: bytes 0-10239.
- Segment header: bytes 0-267.
- Record: all `nblocks * 4096` bytes, with its CRC field at first-block
  offsets 4092-4095 temporarily zero.

Structure CRCs cover all fields before their CRC field, not trailing padding.
The record CRC covers its header and payload blocks. Inodes and directory entries
do not have separate CRC fields in the assignment format.

## Checks completed

During Task A development, an optional checker verified image size, offsets,
all four CRCs, the root directory, allocation maps, segment maps, unused space
and formatter errors. That separate checker is omitted from the final ZIP.
`make check` runs the required Phase 1 driver.

## Task B: caches and segment buffer

The code uses fixed arrays and ordinary functions, with one mounted image at a
time. It is single-threaded; the FUSE program forces single-threaded mode.

| Memory object | Size / contents |
| --- | --- |
| Data cache | 64 entries, each with block number, 4096 bytes, valid/dirty flags, last-use counter |
| Inode cache | 64 entries, each with inode number, one inode, valid/dirty flags, last-use counter |
| Checkpoint cache | Exactly one complete checkpoint: imap and both bitmaps |
| Superblock cache | Exactly one superblock |
| Segment buffer | Exactly one 256 KiB array, including the segment header |

### Read and eviction policy

Reads first look in the appropriate cache. On a miss, they look in the open
segment buffer. If the block is not there, they use `pread` on the image and cache
the result. No log record is appended by a read.

Every cache access gets an increasing counter. A full cache replaces the entry
with the smallest counter (LRU). There is no time-based eviction or background
thread. Valid entries with the same key are replaced rather than duplicated.

Dirty cache entries already have a complete copy in the segment buffer, so an
eviction is safe and never writes an old block in place. Only record appending
creates modified data/inodes; callers must not edit a cached block directly.

### Appending and invalidation

Task C will call `cache_prepare_record` to obtain the first block number, build
the payload and pointers, then call `cache_append_record`. The map argument has
one tag per payload block: -2 for an inode and its owner inode number for data.
The first payload block must be an inode. The two calls are used consecutively
without another operation in between.

Appending clears old inode padding, inserts the record header, calculates its
CRC, updates the segment map, updates imap/inode bits, and fills the caches. It
invalidates old inode entries and displaced data entries during COW. Reusing a
free segment invalidates data entries for its old block addresses too.

For directory removal, Task C must also clear the removed child's imap/bitmap,
invalidate its inode and data entries, and mark the checkpoint changed. Cache
code does not implement path lookup or directory operations.

### Flush / write-back policy

An update is kept in memory. If the next record cannot fit, the current segment
is sealed and its complete 256 KiB buffer is written. A record never straddles
two segments. An exactly full buffer is flushed at the next prepare call or
explicit sync, after the caller completes that operation's checkpoint changes.

`cache_sync` writes the segment first, then the whole dirty checkpoint, then a
dirty superblock, and calls `fsync` once if it wrote anything. Dirty flags are
cleared only after success. There is no timer. Task C's unmount will call this
function before resetting caches and closing the image. Every flushed segment
is sealed; the next update allocates a clear segment bit. Sealing a partially
filled segment is allowed, though it can leave some unused body slots.

The superblock is fixed metadata, so it normally remains clean. Its write-back
path is available if a legitimate metadata change is introduced later. Imap is
written only in the checkpoint, never as a log payload.

### Initialization and use from Task C

`cache_init(fd)` loads and checks the superblock, checkpoint, allocated segment
headers and record CRCs. It does not replay records. It restores an existing
open segment's first free slot and loads the root through the inode cache. The
cache layer can allocate a new segment on prepare. Task C's `lfs_mount` calls
prepare once, so a fresh open segment is allocated on mount if none is open.

The caller owns the image file descriptor. Normal lifecycle:

1. Open image and call `cache_init(fd)`.
2. Read through cache functions; append changes using prepare/append.
3. Use `cache_checkpoint_changed()` after direct checkpoint changes.
4. Call `cache_sync()`, then `cache_reset()`, then close the descriptor.

`cache_reset` discards memory and does not flush. It must not be used on pending
updates unless intentionally abandoning them after an error.

Task B was checked with a temporary driver: 80 synthetic inode/data records,
LRU eviction, unflushed buffer reads, COW replacement, segment rollover,
exactly full segments, sync/reload, no-space errors and checksum corruption.
This driver is not included in the project ZIP. The required Phase 1 driver
is included as `tests/test_lfs.cpp`. `make check` runs the required Phase 1 driver.

## Task C: filesystem library

All assignment prototypes are in `include/lfs.h`. Operations return 0 or a
negative errno; `lfs_read` and `lfs_write` return a byte count on success.
Only the library calls the cache/buffer code. The library and Phase 1 driver have no FUSE dependency.

| API | Implementation |
| --- | --- |
| `lfs_mount` | Open image, check CRCs, load caches/root, recover or allocate append segment |
| `lfs_umount` | Sync segment and metadata, reset caches, close image |
| `lfs_lookup` | Walk absolute path through directory entries |
| `lfs_getattr` | Fill stat from the current inode |
| `lfs_readdir` | Call the supplied filler for every occupied directory entry |
| `lfs_create` / `lfs_mkdir` | Allocate inode and append child inode, changed parent block, parent inode |
| `lfs_write` | Append changed file blocks and a new file inode |
| `lfs_read` | Read cached blocks, returning zeros for holes and stopping at EOF |
| `lfs_rmdir` | Check empty directory, rewrite parent entry/inode, free child inode number |

Paths support repeated slashes and `.` / `..`. A trailing slash requires a
directory. Creating/removing a final `.` or `..` is rejected. Root exists from
format time and cannot be removed. Empty/missing or relative paths are rejected.

Regular files support up to 49152 bytes (12 direct blocks). Partial writes keep
unmodified bytes; a write beyond EOF leaves a zero-filled hole. Larger nonempty
writes return `-EFBIG`. Zero-byte writes do not grow the file. Each successful
nonempty write is one log record, not one record per modified block.

Directory data uses 15 fixed slots per block (180 entries maximum per directory).
`name_len == 0` marks an empty slot. Removed slots are reused. Directory size
tracks its allocated block extent; rmdir checks occupied entries rather than
assuming size zero means empty. Directory data blocks are retained when they
become empty until the directory itself is removed. `.` and `..` are resolved
during lookup, not stored as disk entries. The FUSE wrapper synthesizes
these names when listing a directory.

CREATE and MKDIR each use one three-block record: child inode, changed parent
data block, parent inode. WRITE starts with the file inode followed by changed
data blocks, as fixed in Task A. RMDIR uses parent inode then changed parent data.
The imap and bitmaps change in memory; flushing remains Task B's policy.

The library supports one mounted image at a time and is single-threaded.
Permissions are stored in modes but not enforced by Task C. UID/GID in getattr
come from the calling process, since the assignment inode has no owner fields.
The library has no unlink, rename, truncate, hard links, indirect pointers or
crash replay. GC is provided by Task D. No recovery is attempted for bad CRCs.

### Phase 1 test driver

```sh
make
./test_lfs
```

With no argument, the driver creates a temporary image using `mkfs.logfs`, runs
the tests, then removes that image on success. It leaves your `logfs.img` alone.
It must run from the project root so it can find `./mkfs.logfs`.

For a newly formatted image you want to keep:

```sh
./mkfs.logfs phase1.img
./test_lfs phase1.img
```

Do not point the driver at an image containing your own files: it creates fixed
test names and expects a fresh filesystem. It directly checks all six required
operations, plus lookup/getattr, partial and cross-block writes, the file/name
limits, sparse reads, common errors, removed inode reuse, directory enumeration,
cache eviction, segment rollover, unmount/remount persistence and bad checksums.

This Phase 1 test driver is required by the assignment and must stay in the
final submission. The separate optional Task A checker is omitted from the final ZIP.

## Task D: garbage collection

`src/gc.cpp` implements `int lfs_gc(void)`. It returns the number of segments
cleaned, 0 if there is no eligible victim, or a negative errno on failure.

### Trigger and candidate policy

- The driver can call GC explicitly.
- Updates automatically call it before loading operation metadata when at least
  80% of segments are in use, or at most three segments are free.
- Mount makes the same pressure check before preparing its open segment.
- Ordinary updates keep three free segments reserved for relocation. They may
  continue using remaining space in the already open segment; a rollover that
  would consume this reserve returns `-ENOSPC` if cleaning cannot create room.

GC never chooses the current segment buffer. Only segments sealed before the
GC call began are candidates. Lowest live ratio wins. Below normal pressure,
the ratio must be strictly below 25%. At high usage, ratios of 25% or more are
allowed too. Fully live segments are skipped because copying them gains no
space. Empty sealed segments are eligible with ratio 0.

Each call cleans at most four victims. This keeps work bounded. Surviving blocks
from multiple victims are packed together rather than sealing one mostly empty
output segment per victim. If more work is needed, call GC again or let later
updates trigger it. There is no GC thread or timer.

### Live-block checks

The segment header's block map is scanned:

- For an inode block (-2), a current imap entry must equal that block address.
- For a data block tagged with inode `ino`, that inode must still exist and one
  of its twelve direct pointers must equal that block address.
- Unused slots (-1) are not counted.

Live ratio is live mapped body blocks divided by all mapped body blocks. Old
maps are left intact: reachability, not an in-place dead-marker write, determines
liveness. Reusing an inode number does not make its old data live unless a current
direct pointer still points to that data block.

### Relocation and freeing

For each affected inode, GC appends a WRITE record containing its updated inode
and any data blocks it still owns in the victim. The inode may currently reside
outside that victim; it must still be rewritten to update moved data pointers.
Mode, size and mtime are preserved. Direct pointers to other segments are kept.
The normal append path updates imap and invalidates stale cache entries.

Before copying, GC simulates record packing to check that there are enough free
segments. A victim remains marked allocated while its live blocks are moved.
After all selected victims have been copied:

1. Sync the new log records and their checkpoint pointers.
2. Clear the old segment bits and invalidate their cached data addresses.
3. Sync the checkpoint again, making those segments durably reusable.

Thus no segment becomes free while a durable pointer still requires its old
contents. The formatter's allocation policy can then reuse its segment number.
An error during relocation leaves the old segment allocated; no rollback or
crash recovery is claimed.

Automatic cleaning happens before an update loads parent/file inodes or data.
This matters: otherwise the operation could hold pointers that GC has just moved.
Reads themselves do not trigger GC.

### Verification

The required Phase 1 driver now overwrites a hot file repeatedly, keeps live
data nearby, cleans segments and verifies:

- Both live inode and data addresses change through relocation.
- Existing file contents and directory entries remain identical.
- The free-segment count increases.
- A newly created file uses a freed segment.
- Remount preserves the kept file's contents.

A separate temporary pressure test creates 851 live files across several
directories and flushes frequently. Automatic cleaning, high-usage cleaning,
free-space reclamation and remount/lookup checks passed. This temporary harness
is not part of the submission. `make check` runs the required Phase 1 C-D driver.

## Task E: FUSE mount

`tools/mount.cpp` builds `mount.logfs` using libfuse3. Its callbacks call the
same `lfs_*` functions used by the Phase 1 driver. No disk-layout, COW, cache
or GC logic is duplicated in the mount program.

### Build (Ubuntu / Debian)

```sh
sudo apt install build-essential pkg-config libfuse3-dev fuse3
make userspace
```

On Fedora, the corresponding packages are `gcc-c++`, `make`, `pkgconf-pkg-config`
and `fuse3-devel` / `fuse3`. The mount program requires Linux FUSE support and
an accessible `/dev/fuse`.

### Mount, use and unmount

Run these from the project directory. Keep your existing image if you already
have one; the formatter refuses to overwrite it.

Terminal 1:

```sh
./mkfs.logfs
mkdir -p mnt
./mount.logfs -f mnt
```

Terminal 2, also in the project directory:

```sh
ls mnt
mkdir mnt/docs
echo hello > mnt/docs/a.txt
cat mnt/docs/a.txt
mkdir mnt/empty
rmdir mnt/empty
fusermount3 -u mnt
```

The first terminal returns after unmounting. Remount there:

```sh
./mount.logfs -f mnt
```

Then check persistence and unmount in terminal 2:

```sh
cat mnt/docs/a.txt
fusermount3 -u mnt
```

Both `cat` commands should print `hello`. Use fresh names if repeating the test
on the same image. The assignment's `/mnt/logfs` path works too, provided you
can create and access that directory. Omitting `-f` runs FUSE in the background.
`./mount.logfs --help` and `--version` work without a disk image.

### Callbacks and lifecycle

| FUSE operation | Library call |
| --- | --- |
| getattr, open | lfs_getattr |
| readdir | lfs_readdir; lfs_lookup for the parent of `..` |
| create | lfs_create |
| mkdir | lfs_mkdir |
| rmdir | lfs_rmdir |
| read | lfs_read |
| write | lfs_write |
| startup | lfs_mount |
| destroy | lfs_umount |

The image is opened at startup before `fuse_main`, so errors are reported
immediately and background mode cannot lose the relative `logfs.img` path.
Destroy flushes and closes the library. Failed FUSE startup also closes the
image, and an unmount error is printed and produces a failing foreground exit.

The program always adds `-s`, because the shared library caches are
single-threaded. It adds `default_permissions` so the kernel checks the stored
mode bits. Files use direct I/O in FUSE; data still goes through our Task B
caches and segment buffer. Attribute/name cache timeouts are zero. Writes are
still buffered under Task B's policy and persist on rollover/unmount.

The directory adapter adds `.` and `..`, passes entry names/inodes/modes to
FUSE, and uses numbered offsets to resume if its output buffer fills. It scans
through the library again on each resumed call. Library errors are returned
unchanged as negative errno values.

The required operations and sample workflow are supported. The Task C limit
of 48 KiB per regular file still applies. Unlink, rename, chmod and a real
truncate are not part of the supplied Task C API. Truncation requests that
would change a file's size return `-EOPNOTSUPP`; an unchanged size succeeds.
Thus `echo hello >` works for a new file, but rewriting an existing nonempty
file with shell `>` is not supported. Appending with `>>` uses normal writes.
Use the Phase 1 driver for GC stress tests. Do not mount the same image in two
processes or run the driver on an image currently mounted through FUSE.

### Verification

The A-D driver remains independent of FUSE. Task E is checked against real
libfuse3 headers and library, with a temporary callback harness for create,
read/write, metadata, directory-list continuation, negative errno propagation,
empty/nonempty rmdir and persistence through destroy/startup. This harness is
not included in the submission. The execution environment has no `/dev/fuse`,
so the actual mounted shell workflow above must be checked on your Linux system.

## Task F: flush counter module

`kernel/logfs_stats.c` builds `logfs_stats.ko`. The module has no filesystem
logic. On load, it creates these attributes under `/sys/kernel/logfs_stats`:

| Attribute | Behaviour |
| --- | --- |
| `flush_count` | Read-only unsigned decimal counter followed by a newline |
| `notify` | Writing `1` or `1\n` increments the counter once; other inputs are ignored |

The counter starts at zero on each load. It uses an atomic counter so concurrent
notifications do not lose increments. Unload removes both files and the kobject.
If attribute creation fails, initialization releases the kobject as well.

### Build against the correct kernel

For a normal Ubuntu / Debian kernel:

```sh
sudo apt install build-essential pkg-config libfuse3-dev fuse3 linux-headers-"$(uname -r)"
make
```

The module Makefile uses `/lib/modules/$(uname -r)/build` by default. For the
course kernel's configured build tree, provide its path explicitly:

```sh
make KDIR=/path/to/course/kernel/build
```

`make module KDIR=/path/to/course/kernel/build` builds only the module. The
kernel version/configuration used to build it must match the running kernel
where you load it. If your WSL or custom kernel has no Ubuntu header package,
use its matching configured build tree or the course Linux VM. Headers for
some other kernel can check compilation but cannot produce a module loadable
into your running kernel.

### Load and test notifications

From the project root, after building for the running kernel:

```sh
sudo insmod kernel/logfs_stats.ko
cat /sys/kernel/logfs_stats/flush_count
sudo chown "$(id -u):$(id -g)" /sys/kernel/logfs_stats/notify
```

The first count should be `0`. `notify` is initially root-write-only (0200).
The `chown` gives the current user permission to notify while running FUSE or
the driver without sudo. Do this again after each module reload. Only the
statistics counter is affected by this permission change.

A direct module check:

```sh
echo 1 > /sys/kernel/logfs_stats/notify
cat /sys/kernel/logfs_stats/flush_count
echo 0 > /sys/kernel/logfs_stats/notify
cat /sys/kernel/logfs_stats/flush_count
```

Both reads should print `1`: the second write is ignored. This direct write is
only a module test; it counts as a notification even though no image was flushed.
For a fresh zero counter before the filesystem check:

```sh
sudo rmmod logfs_stats
sudo insmod kernel/logfs_stats.ko
sudo chown "$(id -u):$(id -g)" /sys/kernel/logfs_stats/notify
cat /sys/kernel/logfs_stats/flush_count
./test_lfs
cat /sys/kernel/logfs_stats/flush_count
```

The final count should be greater than zero. The driver formats its own image,
performs updates, GC and remounts. Formatting also uses the same flush helper,
so its successful `fsync` contributes one notification.

You can instead run the Task E mount workflow with the module loaded. Compare
the counter before mounting and after unmounting. Small writes stay buffered,
so the count need not increase immediately after every write. Segment rollover,
explicit library sync, GC sync and unmount write-back use the flush helper.
A clean sync that writes nothing performs no `fsync` and adds no notification.

After all mounts are unmounted:

```sh
sudo rmmod logfs_stats
test ! -e /sys/kernel/logfs_stats && echo "sysfs entries removed"
```

### Library connection

`logfs_fsync` in `src/disk.cpp` calls `fsync` on the image. Only after success
it opens `/sys/kernel/logfs_stats/notify`, writes `1\n` once and closes it.
Interrupted calls are retried. A failed image `fsync` returns its negative errno
without notifying. Task B's `cache_sync` and the formatter both use this helper;
there are no other direct image `fsync` call sites.

If the module is absent, the helper quietly skips notification. Other notify
errors, including permission denied, print a diagnostic but do not turn a
successful image flush into a filesystem failure. Make sure `notify` belongs
to the user running the mount/driver when measuring flushes.

### Verification

The module compiled against Ubuntu Linux 6.8.0-71 headers. Userspace programs
also built, and the required Phase 1 driver passed. A temporary test wrapping
syscalls checked one notification per successful `fsync`, no notification after
a failed flush, interrupted-call retries, missing-module behaviour and the
rollover/GC/unmount paths. This development harness is not in the submission.
The execution environment does not have a matching running-kernel build tree
or module-loading setup, so real insmod/sysfs/rmmod checks must be run on your
course kernel. The mounted FUSE workflow likewise needs a machine with `/dev/fuse`.

## Final submission cleanup

The final ZIP includes only source files, both Makefiles and this README.
The optional Task A checker, temporary development harnesses, object files,
executables, module binaries, disk images and obsolete ZIPs are excluded.
Keep `tests/test_lfs.cpp`: the Phase 1 driver is required by the assignment.

Before submitting, fill in the team information above, run the mounted FUSE
and kernel-module checks on the target kernel, run `make clean` to remove build
outputs, and rename the final archive to `assignment3_<group_number>.zip`.
Do not include your generated `logfs.img` in the submission.
