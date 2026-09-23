# Mini-UnionFS

A userspace union filesystem built with FUSE (libfuse3), in C++20. Replicates the core
layering mechanism Docker uses to build container images: a read-only **lower layer**
stacked under a read-write **upper layer**, presented as a single merged mount point.

## How It Works

```
mount_dir/          <-- unified view (what the user sees)
  ├── file_a.txt    <-- from upper_dir (takes precedence)
  └── file_b.txt    <-- from lower_dir (read-only base)

upper_dir/          <-- read-write container layer
lower_dir/          <-- read-only base image layer
```

Three core mechanisms make this possible:

**Path resolution** — every filesystem call resolves a virtual path through a priority
chain: check for a whiteout marker in upper, then check upper, then check lower, then
report not found.

**Copy-on-Write** — writing to a file that only exists in the lower layer triggers a full
copy to the upper layer first. The lower layer is never modified.

**Whiteout files** — deleting a lower-layer file creates a hidden `.wh.<filename>` marker
in the upper layer. The path resolver sees this marker and reports the file as deleted,
without touching the lower layer.

## Prerequisites

- Ubuntu 22.04 LTS (or compatible Linux)
- `libfuse3-dev` (FUSE 3.x development headers and library)
- G++ with C++20 support (GCC 10+)
- `pkg-config`

```bash
sudo apt-get install libfuse3-dev g++ make pkg-config
```

Not buildable natively on macOS — no fuse3, no `fusermount`, and macFUSE targets the
older FUSE 2.x API. Use `./run-in-docker.sh` (build/run inside a Linux container via
[Colima](https://github.com/abiosoft/colima)) instead; see that script's header comment
for details.

## Build

```bash
make
```

This produces the `mini_unionfs` binary in the project root.

```bash
make clean      # remove build artifacts
make debug      # build with AddressSanitizer + UndefinedBehaviorSanitizer
```

## Usage

```bash
./mini_unionfs <lower_dir> <upper_dir> <mount_dir>
```

All three directories must already exist. Example:

```bash
mkdir -p /tmp/lower /tmp/upper /tmp/mount

# Populate the base image layer
echo "base config" > /tmp/lower/config.txt
echo "base data"   > /tmp/lower/data.txt

# Mount the union filesystem
./mini_unionfs /tmp/lower /tmp/upper /tmp/mount

# In another terminal:
cat /tmp/mount/config.txt               # reads from lower
echo "modified" > /tmp/mount/config.txt # CoW: copies to upper, writes there
rm /tmp/mount/data.txt                  # creates /tmp/upper/.wh.data.txt
```

To unmount:

```bash
fusermount3 -u /tmp/mount
```

## Interactive CLI

`unionfs_cli.sh` is an interactive shell for manually exploring and testing the
filesystem. It creates the test environment, mounts the FS, and drops you into a REPL
where every command runs inside `mnt/`.

```bash
./unionfs_cli.sh
```

### Test environment layout

```
unionfs_test_env/
├── lower/    read-only base layer  (seeded with sample files on startup)
├── upper/    writable CoW layer    (empty on startup; writes land here)
└── mnt/      FUSE mount point      (merged view of lower + upper)
```

### Built-in commands

| Command | Description |
|---------|-------------|
| `cd [dir]` | Change directory inside `mnt/` — bare `cd` returns to root |
| `pwd` | Print current path relative to `mnt/` |
| `layers` | Show files in all three layers side-by-side; highlights whiteout entries |
| `setup` / `reset` | Wipe and reseed layers, remount the FS |
| `mount` | Mount without resetting layer data |
| `umount` | Unmount `mnt/` |
| `help` | Show usage and example commands |
| `exit` / `quit` | Unmount and exit |

All other input runs as a Linux command with CWD set to the current directory inside
`mnt/`. The prompt updates as you navigate:

```
unionfs:mnt/$ cd subdir
unionfs:mnt/subdir$ ls
unionfs:mnt/subdir$ cd ..
unionfs:mnt/$
```

### Example test session

```bash
# Layer visibility
ls -la

# Copy-on-Write
echo "new line" >> base.txt       # triggers CoW copy to upper/
layers                             # base.txt appears in upper/, lower/ unchanged

# Whiteout
rm delete_me.txt                  # creates upper/.wh.delete_me.txt
ls delete_me.txt                  # No such file

# New file in upper
touch newfile.txt
layers                             # newfile.txt only in upper/

# Nested paths and directory ops
mkdir newdir
cd newdir
echo "hello" > hello.txt
ls
cd ..

# Inspect all three layers at any point
layers
```

## Testing

All tests live in the `testing/` directory. Run the full suite with:

```bash
bash testing/run_all_tests.sh
```

The suite mounts and unmounts the filesystem for each test in an isolated temporary
directory, then cleans up. Each test prints `[PASS]` / `[FAIL]` per assertion and a
per-script result. The suite is black-box — it seeds layer directories directly and
drives the mount from the outside (`mountpoint`, `stat`, `ls`, `cat`, …) — with no
coupling to any implementation detail of the code under test.

### Test cases

| # | File | What it covers |
|---|------|----------------|
| 01 | `test_01_layer_visibility.sh` | Files from lower and upper visible through mount; upper shadows lower for same filename |
| 02 | `test_02_cow_write.sh` | Copy-on-Write on write, append, overwrite, and truncate; lower layer never modified |
| 03 | `test_03_whiteout.sh` | Deleting a lower file creates `.wh.` marker; deleting an upper-only file does not |
| 04 | `test_04_create_new_file.sh` | New files created through mount land in upper only |

## Project Structure

```
.
├── Makefile
├── unionfs_cli.sh              # Interactive CLI for manual testing
├── run-in-docker.sh            # macOS -> Colima/Docker build+run wrapper
├── unionfs_test_env/           # CLI test environment (created by unionfs_cli.sh)
│   ├── lower/                  #   read-only base layer
│   ├── upper/                  #   writable CoW layer
│   └── mnt/                    #   FUSE mount point
├── src/
│   ├── unionfs.h      # class UnionFs, RAII (Fd/Dir), fuse_guard, path helpers, all declarations
│   ├── main.cpp        # Entry point: arg validation, realpath, constructs UnionFs, fuse_main
│   ├── path.cpp         # UnionFs::{resolve_path, whiteout_path, getattr, readdir, read}
│   ├── rw_ops.cpp        # UnionFs::{cow_copy, open, write, create, truncate, utimens, release}
│   └── del_ops.cpp        # UnionFs::{unlink..link}, extern "C" shims, make_ops() dispatch table
├── testing/
│   ├── run_all_tests.sh   # Test suite runner
│   ├── lib.sh             # Shared test utilities (setup, assertions, mount helpers)
│   ├── test_01_layer_visibility.sh
│   ├── test_02_cow_write.sh
│   ├── test_03_whiteout.sh
│   └── test_04_create_new_file.sh
```

## Architecture

Every filesystem operation is a method on `class UnionFs` (declared in `unionfs.h`), which
owns the two layer roots as `std::string`. Method bodies are split across `path.cpp` /
`rw_ops.cpp` / `del_ops.cpp` by topic — read path, write path + CoW engine,
deletion/metadata/links.

`main.cpp` heap-allocates one `UnionFs` and passes it as FUSE's `private_data`; every
callback fetches it fresh via `fs_ctx()` (never cached — `fuse_get_context()` is
per-request and FUSE is multithreaded by default here). The FUSE dispatch table
(`struct fuse_operations`) is assembled by `make_ops()` in `del_ops.cpp` and exposed
through a function-local static in `unionfs_ops()`.

Between `UnionFs`'s methods and libfuse3 itself sits a thin `extern "C"` shim per
operation (`c_getattr`, `c_readdir`, ...), each body a single
`fuse_guard([&]{ return fs_ctx().op(...); })` call. `fuse_guard` is the only place a
thrown C++ exception is ever caught — libfuse3 is a C library, and letting an exception
unwind into it would be undefined behaviour. The `UnionFs` methods themselves are free to
throw (`std::string` allocation, `std::unordered_set::insert`, ...); the shim is what
stops it at the C boundary.

Key design decisions:
- `resolve_path()` returns `std::optional<std::string>` — no output buffer, no
  `PATH_MAX`-sized scratch space
- Layer and whiteout paths are built with plain `std::string` concatenation, **never**
  `std::filesystem::path::operator/` — `operator/` silently discards its left operand
  when the right operand is absolute, and every virtual FUSE path here is absolute
- There is exactly **one** whiteout-path builder (`UnionFs::whiteout_path`, in
  `path.cpp`)
- Every raw `fd`/`DIR*` is owned by the `Fd`/`Dir` RAII wrappers in `unionfs.h`; no
  manual `close()`/`closedir()` appears anywhere in the tree
- `readdir` does a two-pass merge (upper first, then lower) with a
  `std::unordered_set<std::string>` to deduplicate entries and suppress whiteout targets
  — scoped fresh per call
- Open-mode detection uses `(fi->flags & O_ACCMODE) != O_RDONLY` to correctly catch both
  `O_WRONLY` and `O_RDWR` (since `O_RDONLY == 0`, a naive bitwise AND fails)
- CoW copies preserve the full directory tree in upper before writing, using a 64 KB
  buffer loop for large files
- `unionfs_create` clears any stale whiteout marker before creating a new file, matching
  the behaviour of `unionfs_mkdir`
- Whiteout markers follow the `.wh.<filename>` convention used by Docker's overlay driver

See `agent.md` for the full conventions/invariants reference and
`PROJECT_UNDERSTANDING.md` for a detailed file-by-file walkthrough.

## License

MIT — see [LICENSE](LICENSE).
