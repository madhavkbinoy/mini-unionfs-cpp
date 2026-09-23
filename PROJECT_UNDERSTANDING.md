# Mini-UnionFS — Project Understanding

A file-by-file reading of this repository. Covers what the project is, how each file
works, the exact control flow through the three core mechanisms, verified behavior, and
the environment constraints on this machine. See `agent.md` for the condensed
AI-agent-facing conventions/invariants reference.

---

## 1. What it is

A **userspace union / overlay filesystem** in C++20, built against **libfuse3**
(`FUSE_USE_VERSION 31`). It stacks a read-only **lower** directory under a read-write
**upper** directory and serves the two as a single merged tree at a **mount point** — the
same layering model Docker's `overlay2` storage driver uses for container images.

```
mount_dir/        merged view the user sees
   ├── a.txt      served from upper/  (upper wins)
   └── b.txt      served from lower/  (read-only base)

upper_dir/        read-write "container" layer — all mutations land here
lower_dir/        read-only "image" layer — NEVER written to (one narrow exception, §7)
```

- License: MIT ("Copyright (c) 2026 Mini-UnionFS Contributors")
- ~940 lines of C++ across 5 files (`unionfs.h` + 4 `.cpp`), plus ~1,050 lines of bash
  tooling for the CLI, container build/run wrapper, and test suite.

### The three mechanisms

1. **Path resolution** (`UnionFs::resolve_path`) — a strict priority chain: whiteout
   marker in upper → upper → lower → not found (`std::nullopt`).
2. **Copy-on-Write** (`UnionFs::cow_copy`) — the first write-intent touch of a lower-only
   file copies the entire file into upper; every subsequent operation targets the upper
   copy.
3. **Whiteouts** — deleting a lower-backed file writes a zero-byte, mode-`0000` marker
   named `.wh.<basename>` into the matching upper directory, built by the single
   `UnionFs::whiteout_path()` function. The resolver treats that marker as "this name is
   deleted"; `readdir` hides both the marker and the lower entry it shadows.

---

## 2. Repository layout

```
.
├── agent.md                    AI-agent context file — conventions, invariants, gotchas
├── PROJECT_UNDERSTANDING.md    this file
├── README.md                   user-facing docs
├── LICENSE                     MIT
├── Makefile                    g++ + pkg-config fuse3, -std=c++20; all/run/debug/clean
├── .gitignore                  ignores upper/, mnt/, *.o, mini_unionfs, CLAUDE.md, .claude/
├── unionfs_cli.sh              interactive REPL for manual testing
├── run-in-docker.sh            macOS→Colima/Docker build+run wrapper
├── .demo.sh                    scripted CoW + whiteout demo payload
├── mini_unionfs                build output — gitignored
├── src/
│   ├── unionfs.h        class UnionFs, RAII (Fd/Dir), fuse_guard, sys_error, path
│   │                    helpers, fs_ctx(), unionfs_ops() — the interface contract
│   ├── main.cpp          arg validation, realpath, constructs UnionFs, fuse_main
│   ├── path.cpp           UnionFs::{whiteout_path, resolve_path, getattr, readdir, read}
│   ├── rw_ops.cpp           CoW engine + UnionFs::{cow_copy, open, write, create,
│   │                    truncate, utimens, release}; free fn make_parent_dirs
│   └── del_ops.cpp           UnionFs::{unlink..link}; extern "C" shims; make_ops() +
│                        unionfs_ops() (the dispatch table)
├── testing/                   black-box, no coupling to implementation details
│   ├── lib.sh
│   ├── run_all_tests.sh
│   ├── test_01_layer_visibility.sh
│   ├── test_02_cow_write.sh
│   ├── test_03_whiteout.sh
│   └── test_04_create_new_file.sh
└── unionfs_test_env/
    ├── lower/               tracked seed data: base.txt, delete_me.txt, readonly.txt,
    │   └── subdir/          subdir/{nested,other}.txt
    └── upper/               gitignored
```

⚠️ If the checkout's parent directory contains spaces, always quote the path in shell
commands.

---

## 3. File-by-file walkthrough

### `src/unionfs.h` — the interface contract

Everything shared lives here: constants, RAII wrappers, the exception-safety helpers, the
`UnionFs` class declaration, and the `fs_ctx()` / `unionfs_ops()` accessors.

```cpp
#define FUSE_USE_VERSION 31           /* must precede #include <fuse.h> */
#define WH_PREFIX     ".wh."
#define WH_PREFIX_LEN 4
#define MAX_PATH_LEN  PATH_MAX
```

**`Fd` / `Dir`** — move-only, `noexcept` RAII wrappers around a POSIX file descriptor and
`DIR*` respectively. `Fd` additionally supports `.release()` (hands ownership to a caller
— used exactly once, in `UnionFs::create`, to populate `fi->fh`) and `.reset()` (closes
early without waiting for scope exit). There is no manual `close()`/`closedir()` anywhere
in `path.cpp`/`rw_ops.cpp`/`del_ops.cpp` — every site goes through one of these.

**`sys_error()`** — `inline int sys_error() noexcept { return -errno; }`. Call it
immediately after a failing syscall. Because C++ fully evaluates a `return` expression
before running any automatic object's destructor at scope exit, `return sys_error();`
can never have its `errno` read clobbered by an `Fd`/`Dir` destructor's `close()` call.

**`fuse_guard()`** — a template that wraps a callable in `try`/`catch`, mapping
`std::bad_alloc → -ENOMEM`, `std::system_error → -(its errno)`, any other
`std::exception → -EIO`, anything else → `-EIO`. This is the **only** place in the
codebase a thrown C++ exception is ever caught. It exists because libfuse3 is a C
library and an exception unwinding into it is undefined behaviour; every `UnionFs`
method is free to throw (string allocation, `unordered_set::insert`, …) because the
`extern "C"` shim around it (see `del_ops.cpp` below) always catches it first.

**Path helpers** — three `UnionFs` const methods:

```cpp
std::string upper_path(std::string_view v) const;      // upper_ + v
std::string lower_path(std::string_view v) const;      // lower_ + v
std::string whiteout_path(std::string_view v) const;   // THE one builder (defined in path.cpp)
```

All three are plain `std::string` concatenation. **Deliberately never
`std::filesystem::path::operator/`** — every virtual FUSE path is absolute (leading `/`),
and `operator/` silently discards its left operand when the right operand is absolute
(`fs::path("/upper") / "/subdir/f.txt"` → `/subdir/f.txt`, escaping both layers). This
project avoids `<filesystem>` for path composition specifically to make that class of bug
structurally impossible.

**`class UnionFs`** — owns `std::string lower_, upper_;` and declares every FUSE
operation as a method (`getattr`, `readdir`, `read`, `cow_copy`, `open`, `write`,
`create`, `truncate`, `utimens`, `release`, `unlink`, `mkdir`, `rmdir`, `chmod`, `chown`,
`statfs`, `symlink`, `readlink`, `rename`, `link`) plus `resolve_path`. Method *bodies*
are defined out-of-line across `path.cpp`/`rw_ops.cpp`/`del_ops.cpp`, split by topic.

**`fs_ctx()`** — `inline UnionFs &fs_ctx() noexcept { return *static_cast<UnionFs *>
(fuse_get_context()->private_data); }`. The only way any code reaches the `UnionFs`
instance; called fresh on every request, never cached (FUSE is multithreaded by default
here, and the context is per-request).

**`unionfs_ops()`** — declared here, defined in `del_ops.cpp`; returns the
`fuse_operations` dispatch table.

⚠️ **POSIX name collisions.** Several `UnionFs` methods share a name with the libc
function they wrap: `open`, `read`, `write`, `unlink`, `mkdir`, `rmdir`, `chmod`,
`rename`, `link`, `symlink`, `readlink`, `truncate`, `readdir`. Inside any `UnionFs`
method body, an *unqualified* call to one of those names resolves to the C++ member —
name lookup stops at the first enclosing scope where a name is found and does not fall
through to `::name` just because the member's signature doesn't match the call (this is
a hard compile error, not a silent miscompile, but still must be avoided). Every internal
POSIX call in the three `.cpp` files is explicitly `::`-qualified as a result.
`grep -nE '[^:_a-zA-Z](open|read|write|unlink|mkdir|rmdir|chmod|rename|link|symlink|readlink|truncate|readdir)\(' src/*.cpp | grep -v '::'` confirms zero unqualified instances remain
(excluding `make_parent_dirs`, a free function with no such collision).

### `src/main.cpp` — entry point

1. Requires `argc >= 4`, else prints usage and exits 1.
2. `realpath(argv[1]/argv[2], NULL)` resolves both layer roots to absolute,
   symlink-free paths (glibc allocates these with `malloc`).
3. Validates each is non-NULL **and** `S_ISDIR`, freeing correctly on every failure path.
4. Logs the three paths to stderr.
5. **`UnionFs *fs = new UnionFs(lower_real, upper_real); free(lower_real); free(upper_real);`**
   — `UnionFs`'s constructor takes the `char*` args by implicit conversion to
   `std::string`, copying them; the `realpath()` buffers are freed immediately after.
6. Builds a **2-element** FUSE argv — `{ argv[0], argv[3] }` — and calls
   `fuse_main(2, fuse_argv, &unionfs_ops(), fs)`.
7. `delete fs;` after `fuse_main` returns.

**The `-f` comment is stale.** The comment block above the argv construction claims
`-f = foreground mode (keeps stderr visible for debugging)`, but `-f` is not actually in
`fuse_argv` — the process **daemonizes into the background**. Both `unionfs_cli.sh` and
`testing/lib.sh` depend on the background behaviour (`sleep 1` after launching, before
touching the mount) — adding `-f` for real would hang both harnesses.

A side effect of the hand-built argv: **no user FUSE options can be passed through**
(`-o allow_other`, `-d`, `-s`, etc. are all silently dropped).

`fuse_main` is a macro — `#define fuse_main(argc, argv, op, private_data)
fuse_main_real(argc, argv, op, sizeof(*(op)), private_data)`. Passing `&unionfs_ops()`
(a function returning `const fuse_operations&`) is safe: the `sizeof(*(op))` occurrence
is an *unevaluated context* (no runtime call), so `unionfs_ops()`'s function-local
static is only actually invoked once per process, on the `op` argument itself.

### `src/path.cpp` — resolution and the read side

**`UnionFs::whiteout_path()`** — the single whiteout-path builder for the whole project
(see §5). Maps a virtual path to its marker with three cases: nested path (has a
directory component), root-level file, and a defensive bare-filename fallback (FUSE
always passes a leading `/` in practice).

**`UnionFs::resolve_path()`** — returns `std::optional<std::string>`:

```
"/"                          → upper_dir(), always
whiteout exists in upper?    → std::nullopt              ← checked FIRST, beats everything
<upper><path> exists?        → that path
<lower><path> exists?        → that path
otherwise                    → std::nullopt
```

`resolve_path` never distinguishes *why* it failed (whiteout vs. genuinely absent) — both
map to `-ENOENT` at every call site. Existence tests use `access(p, F_OK)`, which
**follows symlinks** — source of the `readlink` quirk in §7.

**`UnionFs::getattr`** — `memset` the `stat`, resolve, `lstat`, return `sys_error()` on
failure. Note the asymmetry: resolution follows symlinks (`access`) but the stat does not
(`lstat`).

**`UnionFs::readdir`** — the two-pass merge, deduplicating with a stack-local
`std::unordered_set<std::string> seen`:

1. Emit `.` and `..`, seed them into `seen`.
2. **Pass 1 — upper.** `::opendir(upper_path(path))` via a `Dir`; skip `.`/`..`; skip any
   name starting with `.wh.`; `seen.insert(name).second` both checks-and-inserts in one
   call — `filler()` fires only on a true (newly-inserted) result.
3. **Pass 2 — lower.** `::opendir(lower_path(path))`; skip `.`/`..`; skip anything already
   in `seen`; skip anything with a whiteout via the local `has_whiteout()` helper (which
   calls `UnionFs::whiteout_path()` on a synthesized child virtual path); emit and insert
   the rest.

A missing `opendir` on either layer is silently tolerated — correct, a directory may
exist in only one layer. The load-bearing comment: *"seen is LOCAL — a fresh set for
every readdir call. Never make this a global: concurrent calls would corrupt it."*

**`UnionFs::read`** — resolve; if absent, `-ENOENT`; else `Fd fd(::open(resolved->c_str(),
O_RDONLY))`; `pread`; return the byte count or `sys_error()`. `fd` closes automatically.

### `src/rw_ops.cpp` — write side and the CoW engine

**`make_parent_dirs()`** (free function, not a `UnionFs` method — it has no notion of
upper/lower, it just mutates an already-resolved host path) — `mkdir -p` for the *parent*
of a full host path, walking the string in a `char tmp[MAX_PATH_LEN]` scratch buffer,
temporarily NUL-terminating at each `/`. This is one of only two `MAX_PATH_LEN` buffers
left in the tree (the other is `readlink`'s raw syscall output buffer in `del_ops.cpp`) —
both are a different category from a path-composition buffer (in-place mutation / raw
syscall output, not layer-path building).

**`UnionFs::cow_copy()`** — takes the virtual path, builds `src`/`dst` via
`lower_path()`/`upper_path()`:

```
Fd src_fd(::open(src, O_RDONLY))
fstat  → capture st_mode
make_parent_dirs(dst)                          recreate the directory tree in upper
Fd dst_fd(::open(dst, O_CREAT|O_WRONLY|O_TRUNC, st.st_mode))
loop: ::read(64 KB) → inner short-write-safe ::write loop
::fchmod(dst_fd, st.st_mode)
return 0                                       src_fd, dst_fd close automatically
```

Every error path just `return sys_error();` — no manual cleanup needed anywhere.
**Files only** — no directory branch (see §7, the `chmod`-on-a-lower-directory bug).

**`UnionFs::open`** — the CoW trigger:

```cpp
if ((fi->flags & O_ACCMODE) != O_RDONLY) {          /* write intent */
    if (!exists(upper) && exists(lower)) cow_copy(path);
}
return resolve_path(path) ? 0 : -ENOENT;             /* validity check only */
```

The `O_ACCMODE` masking is essential: `O_RDONLY == 0`, so a naive
`fi->flags & O_WRONLY` test would silently miss `O_RDWR`.

**`UnionFs::write`** — opens `upper_path(path)` with `O_WRONLY` unconditionally and
`pwrite`s; relies entirely on `open`/`create`/`truncate` having already done the CoW.

**`UnionFs::create`** — `make_parent_dirs`, then clear any stale `.wh.` marker via
`whiteout_path()` + `::unlink`, then `Fd fd(::open(..., O_CREAT|O_WRONLY|O_TRUNC, mode));
fi->fh = static_cast<uint64_t>(fd.release());`. `fd.release()` is the **one** place in
the codebase an `Fd` deliberately does *not* close on scope exit — ownership transfers to
`fi->fh`, which outlives this call and is closed later by `UnionFs::release`.

**`UnionFs::truncate`** — CoW if upper is missing and lower has it, `-ENOENT` if neither,
then `::truncate(upper, size)`.

**`UnionFs::utimens`** — resolve; if absent, `-ENOENT`; else `::utimensat(AT_FDCWD,
resolved->c_str(), tv, AT_SYMLINK_NOFOLLOW)`. **This is the one callback that can write
to the lower layer** (§7) — if the path resolves to lower, it stamps timestamps there.

**`UnionFs::release`** — `if (fi->fh) { Fd(static_cast<int>(fi->fh)); fi->fh = 0; }` — the
temporary `Fd` closes at the end of the full expression, matching a plain explicit
`close()`'s timing while still routing through the RAII wrapper.

### `src/del_ops.cpp` — deletion, directories, metadata, links, shims, and the table

**`UnionFs::unlink()`** — the whiteout engine, three-branch logic:

```
resolve_path == nullopt            → -ENOENT              (Edge Case 4.1: not visible)
in_upper && !in_lower  [PATH A]    → ::unlink(upper)      (plain delete, no marker)
in_lower               [PATH B]    → make_parent_dirs(wh)
                                     Fd(::open(wh, O_CREAT|O_WRONLY|O_TRUNC, 0000)); fd.reset()
                                     if (in_upper) ::unlink(upper)   (Edge Case 4.2: drop the CoW copy)
```

**`UnionFs::mkdir()`** — clears a stale whiteout (Edge Case 4.5) via `whiteout_path()`,
`make_parent_dirs`, `::mkdir` in upper.

**`UnionFs::rmdir()`** — if the dir exists in upper: `::rmdir` it, and if lower *also*
has that name, drop a `0000` whiteout (`Fd` constructed and left to close on scope exit
— open failure silently ignored). If it exists only in lower: return **`-EPERM`**
(Edge Case 4.4). Otherwise `-ENOENT`.

**`UnionFs::chmod` / `UnionFs::chown`** — identical shape: CoW if upper is missing and
lower has it, else `-ENOENT`; then `::chmod(upper, mode)` / `::lchown(upper, uid, gid)`.

**`UnionFs::statfs`** — `::statvfs(upper_dir().c_str(), stbuf)`, so free space reflects
the writable layer.

**`UnionFs::symlink` / `UnionFs::readlink`** — symlinks always created in upper.
`readlink` picks its layer with `access(F_OK)` (upper first, then lower) and copies
through an intermediate `char linkbuf[MAX_PATH_LEN]` — the second of the two remaining
fixed-size buffers, a raw syscall output target.

**`UnionFs::rename`** — if the source is in upper, `make_parent_dirs(to)` + `::rename`
inside upper. If lower-only, `cow_copy(from)` first, then rename the fresh copy. `flags`
(`RENAME_NOREPLACE`, `RENAME_EXCHANGE`) `(void)`-ignored.

**`UnionFs::link`** — CoW the source if needed, then `::link(upper_from, upper_to)`.

**The `extern "C"` shims** — 19 `static` free functions (`c_getattr`, `c_readlink`, …,
`c_utimens`), each body exactly:

```cpp
static int c_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    return fuse_guard([&] { return fs_ctx().getattr(path, stbuf, fi); });
}
```

This is the sole place `fuse_guard` is invoked; it's the correct boundary because it's
where a C++ exception would otherwise cross into libfuse3's C call frames.

**`make_ops()` / `unionfs_ops()`** — `make_ops()` value-initializes a local
`fuse_operations{}` (every member zero'd) then assigns the 19 wired members;
`unionfs_ops()` wraps it in a function-local `static`, built once on first use —
order-independent, warning-free, and safe across `main.cpp`'s fork into `fuse_main` (not
constructed until `main` actually asks for it).

**Not implemented:** `access`, `opendir`/`releasedir`, `flush`, `fsync`, `mknod`,
`*xattr`, `fallocate`, `init`/`destroy`, `lock`, `ioctl`, `poll`. Adding one means writing
the method, declaring it in the class, writing its `c_*` shim, and adding one line to
`make_ops()` — that function is the single registration point.

### `Makefile`

```make
CXX      = g++
CXXFLAGS = -Wall -Wextra -std=c++20 $(shell pkg-config --cflags fuse3)
LDFLAGS  = $(shell pkg-config --libs fuse3)
```

Targets: `all` (default), `run` (builds then launches `./unionfs_cli.sh`), `debug`
(`-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer`), `clean`. Objects depend
on `unionfs.h`, so header edits force a full rebuild.

### `unionfs_cli.sh` — interactive REPL (373 lines)

Runs under `set -euo pipefail`. Layout is fixed at `unionfs_test_env/{lower,upper,mnt}`.

- **`do_setup`** — unmounts if mounted, `rm -rf` lower+upper, recreates all three, seeds
  lower with `base.txt` (2 lines), `delete_me.txt`, `readonly.txt`, `subdir/nested.txt`,
  `subdir/other.txt`, launches the binary in the background, `sleep 1`, verifies with
  `mountpoint -q`.
- **`do_layers`** — prints all three layers side by side with colour; `.wh.*` entries are
  flagged red as `[whiteout]`. The single most useful command for seeing the mechanism
  work.
- **`repl_cd`** — tracks `$REPL_CWD` across commands, normalises with `realpath -m`, and
  **refuses to escape `$MNT`**.
- **REPL loop** — built-ins `layers`/`ls-layers`/`show`, `setup`/`reset`, `mount`,
  `umount`/`unmount`, `cd`, `pwd`, `help`, `exit`/`quit`. Everything else is `eval`'d in a
  subshell with `cd "$REPL_CWD"` and `MNT`/`LOWER`/`UPPER` exported; non-zero exit status
  is echoed.

### `testing/` — the suite

**`lib.sh`** provides:
- `setup_env` — a fresh `mktemp -d /tmp/unionfs_test_XXXXXX` with `lower/ upper/ mnt/`,
  exported as `$LOWER_DIR`/`$UPPER_DIR`/`$MOUNT_DIR`/`$TEST_ENV`
- `mount_fs` (launch + `sleep 1` + `mountpoint -q` check), `unmount_fs`, `teardown_env`
- assertions: `assert_file_exists`, `assert_file_not_exists`, `assert_contains`,
  `assert_not_contains`, `assert_cmd_fails`, plus raw `pass`/`fail`
- `print_summary` — prints the tally and sets the exit code

Tests seed files **directly into the layer directories**, not through the mount — that
is what lets them assert on layer state independently of the filesystem under test.

| Test | Covers |
|---|---|
| `test_01_layer_visibility.sh` | lower and upper both visible; upper shadows lower for the same name; lower file itself unchanged; `stat` on a missing path fails |
| `test_02_cow_write.sh` | CoW on append, overwrite, and `truncate`; original content preserved in the copy; lower byte-for-byte untouched; upper-only writes need no CoW |
| `test_03_whiteout.sh` | deleting a lower file creates `.wh.victim.txt`; lower preserved; no `.wh.` entries in the listing; `stat` on the whited-out path fails; deleting an **upper-only** file creates **no** marker |
| `test_04_create_new_file.sh` | new files (redirect, `touch`, loop) land in upper only; an untouched lower file is **not** CoW'd |

**`run_all_tests.sh`** holds a **hard-coded `TESTS` array** — it does *not* glob, so a new
test file must be added to that array by hand. It auto-builds if the binary is missing,
warns when `fusermount` is absent, runs each script with `bash`, and aggregates per-script
pass/fail into a final summary + exit code. `FUSE_BINARY=/path/to/mini_unionfs` overrides
the binary.

### `run-in-docker.sh` + `.demo.sh` — the macOS escape hatch

`run-in-docker.sh` modes: `build` | `test` (default) | `cli` | `demo` | `shell`.

1. Starts Colima if it is not running.
2. Builds a cached toolchain image (`ubuntu:22.04` + `build-essential pkg-config
   libfuse3-dev fuse3`) on first use.
3. Runs with `--device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor:unconfined`
   and `-v "$PROJECT_DIR:/work"`.
4. For `cli`/`shell`, a `STAGE` snippet copies the source to **`/build` on the container's
   own disk** and rebuilds there, then `SYNC_BACK` copies `upper/` back to the Mac
   afterwards, dodging the shared-folder whiteout issue (§4.2 below).

`.demo.sh` runs the whole story end to end under `/run_env` (container disk): show the
merged view → append through the mount and show the CoW copy next to the untouched
original → `rm` through the mount and show the marker with lower intact → unmount → copy
results back with `install -m 0644` + `chmod 000`.

---

## 4. Build, run, test

### On Linux

```bash
make                              # → ./mini_unionfs
./mini_unionfs <lower> <upper> <mount>
bash testing/run_all_tests.sh
./unionfs_cli.sh                  # or: make run
fusermount3 -u <mount>
```

Needs `libfuse3-dev`, `g++` (GCC 10+ for C++20), `make`, `pkg-config`. Targets Ubuntu
22.04.

### 4.1 ⚠️ Not buildable natively on macOS

macOS lacks fuse3 via pkg-config and `fusermount`/`fusermount3`, so `make` cannot even
expand its flags. The scripts also assume GNU/Linux tooling: `mountpoint -q`,
`fusermount -u`, and `stat -c '%s'` (BSD `stat` uses `-f %z`). Installing macFUSE does not
fix it — macFUSE targets the FUSE **2.x** API and this project requires fuse3.

The C++ source itself compiles cleanly on macOS against the standard fuse3 public
headers (`g++ -std=c++20 -I<path-to-fuse3-headers> -c ...`, zero warnings) — it's the
link step and the FUSE kernel driver requirement that fail, not the language.

**Use the container path:**

```bash
./run-in-docker.sh          # build + full test suite
./run-in-docker.sh cli      # interactive REPL
./run-in-docker.sh demo     # scripted CoW + whiteout walkthrough
```

Note: **Colima only mounts `$HOME`.** A `-v` of anything under `/tmp` or `/private/tmp`
silently produces an empty directory inside the VM — keep every bind-mount under the
home directory.

### 4.2 ⚠️ Shared-folder gotcha — whiteouts fail on the macOS shared folder

Colima shares `$HOME` into the VM over a FUSE-based mount that **rejects
`open(path, O_CREAT|O_WRONLY|O_TRUNC, 0000)` with `EACCES`** — exactly the call
`UnionFs::unlink` uses to write a whiteout marker (and `UnionFs::rmdir` at its
whiteout-creation site). So **every deletion of a lower-backed file fails with
"Permission denied"** whenever `upper/` lives on the share. Mode `0644` creates fine
there; `0000` does not; both work on the container's own disk.

This is an environment limitation, not a filesystem-logic bug — the suite passes when
the layers are on a normal Linux filesystem. Two ways out:

- **Keep `upper/` off the share** (what `.demo.sh` and `run-in-docker.sh`'s `STAGE` do).
- **Or change the marker mode** in `del_ops.cpp`. *Nothing depends on `0000`*:
  `resolve_path` and the whiteout-existence checks detect markers with `access(F_OK)`
  (existence only, mode-independent), and `readdir` filters by the `.wh.` name prefix.
  `0644` would be functionally identical and portable. Left as-is — a deliberate "system
  marker" style choice.

---

## 5. The whiteout-path builder

`UnionFs::whiteout_path()`, defined once in `path.cpp`, is the **only** place the `.wh.`
path layout is implemented anywhere in this codebase:

```
<upper_dir><dirpart>/.wh.<basename>
```

with the root-level special case `<upper_dir>/.wh.<basename>`. `has_whiteout()` (a small
free helper in `path.cpp`, used only by `readdir`'s lower pass) calls it on a synthesized
child virtual path rather than hand-rolling its own construction. If you are ever tempted
to build a `.wh.` path anywhere else in this codebase, call `whiteout_path()` instead.

---

## 6. Behavioural invariants — do not break these

1. **The lower directory is never written to**, except by `utimens` (§7). Every other
   mutation routes through upper, via CoW when the source is in lower. Tests 02 and 03
   assert this directly.
2. **Whiteout markers never appear in listings** — `UnionFs::readdir`'s upper pass skips
   any name starting with `.wh.`.
3. **Upper shadows lower** for identical names, in both `resolve_path` (step "upper"
   before step "lower") and the `readdir` merge order (upper pass first, `seen` — a
   `std::unordered_set` — blocks the duplicate).
4. **A whiteout beats everything** — checked *before* upper in `resolve_path`. That is
   what makes "delete stays deleted" work even when a CoW copy exists, since `unlink`
   PATH B removes the copy *and* writes the marker.
5. **Re-creating a deleted name clears its marker.** Both `UnionFs::create` and
   `UnionFs::mkdir` unlink a stale `.wh.` first.
6. **The `readdir` dedup set is per-call.** A stack-local
   `std::unordered_set<std::string>` — never promote it to a member or global; FUSE runs
   multithreaded here.
7. **Open-mode detection must be `(fi->flags & O_ACCMODE) != O_RDONLY`**
   (`UnionFs::open`, `rw_ops.cpp`). `O_RDONLY == 0`, so a naive `fi->flags & O_WRONLY`
   silently misses `O_RDWR`.
8. **Callbacks return negated errno** (`-ENOENT`, `sys_error()`), never `-1`.
   `read`/`write` return the byte count on success. `fuse_guard` preserves this contract
   for thrown exceptions too.

---

## 7. Known limitations and latent bugs

Documented so they don't get "discovered" later as regressions. Some are deliberate
scope limits of a teaching project; others are real defects.

### Deliberate / documented limits
- **`rmdir` on a lower-only directory returns `-EPERM`** (`del_ops.cpp`). Correctly
  hiding it would require whiting out every descendant.
- **No opaque-directory support** — a whiteout hides a *name*; there is no
  `.wh..wh..opq` equivalent to mark a whole directory as opaque.

### Real gaps
- **Parent whiteouts are not consulted for children.** `resolve_path("/subdir/nested.txt")`
  checks only `.wh.nested.txt`, never `.wh.subdir`. After a directory is whited out, its
  children stay reachable by direct path even though the directory itself is hidden.
- **`cow_copy` on a directory is wrong.** `chmod`/`chown` of a lower-only *directory*
  calls `cow_copy`, which `open()`s the directory read-only and then creates a **regular
  file** at the upper path. It should `mkdir` instead. `rename`/`link` of a lower-only
  directory hit the same hazard.
- **`rename` leaves the lower original visible.** After renaming a lower-backed file, the
  CoW copy moves to `to` but no whiteout is written for `from`, so `from` reappears from
  the lower layer. `rename` flags are also ignored, so `RENAME_NOREPLACE` silently
  clobbers.
- **`link` creates a hard link inside upper only** — the two names share the CoW copy's
  inode, not anything in lower.
- **`utimens` can write to the lower layer.** It calls `utimensat` on whatever
  `resolve_path` returned; for a lower-only file that is the lower path. A bare `touch` of
  an existing lower file therefore mutates lower's mtime — the one place invariant #1
  leaks.
- **`fi->fh` is set by `create` but ignored by `read` and `write`,** which re-`open()` the
  upper/resolved path on every call. Beyond the per-syscall overhead, a file created with
  a read-only mode (e.g. `0444`) will fail the subsequent `open(upper, O_WRONLY)` inside
  `write`.
- **`write` assumes CoW already happened.** It opens `upper_path(path)` unconditionally;
  any path that reaches `write` without a prior `open`/`create`/`truncate` fails with
  `-ENOENT` rather than copying up.
- **`readlink` picks its layer with `access(F_OK)`, which follows the link** — a dangling
  symlink in upper falls through to the lower branch or `-ENOENT` instead of being read.
- **`.wh.`-named files in the *lower* layer are not filtered.** The `.wh.` skip only runs
  in `readdir`'s upper pass, so a file genuinely named `.wh.foo` in lower shows up in
  listings. Conversely, such a file in upper is hidden from `readdir` but still
  resolvable by `getattr`.

---

## 8. Conventions for editing

- **C++ style:** 4-space indent, `snake_case` functions/variables, `PascalCase` class
  names (`UnionFs`, `Fd`, `Dir`), opening brace on the same line. Paths are
  `std::string`/`std::string_view` built with `+=`/concatenation — never
  `std::filesystem::path::operator/` (§3, "Path helpers"). `Fd`/`Dir` own every raw
  fd/`DIR*`; no bare `close()`/`closedir()`.
- **The build is warning-clean.** Keep `-Wall -Wextra -std=c++20` producing zero
  warnings; a new warning on a change is a signal, not something to suppress.
- **POSIX name collisions inside `UnionFs` methods** — `::`-qualify any internal call to
  `open`/`read`/`write`/`unlink`/`mkdir`/`rmdir`/`chmod`/`rename`/`link`/`symlink`/
  `readlink`/`truncate`/`readdir` (§3). This is the single easiest way to introduce a
  compile error when adding new code to this class.
- **New FUSE callback** = write it as a `UnionFs` method in the topically correct file
  (read/path → `path.cpp`, write/CoW → `rw_ops.cpp`, delete/dir/metadata/links →
  `del_ops.cpp`), declare it in the class body in `unionfs.h`, add its one-line
  `extern "C" static int c_<name>(...)` shim in `del_ops.cpp`, add one line to
  `make_ops()`.
- **New test** = copy an existing `test_0N_*.sh`, `source lib.sh`, use `setup_env` /
  `mount_fs` / `teardown_env` / `print_summary`, and **add the filename to the `TESTS`
  array in `run_all_tests.sh`** — the runner does not glob.
- **Commit messages** follow loose conventional-commit style: `feat: ...`,
  `refactor: ...`, `build: ...`, `fix: ...`.
- **Do not commit** `mini_unionfs`, `*.o`, `unionfs_test_env/upper/`, or
  `unionfs_test_env/mnt/`. `.gitignore` also excludes `CLAUDE.md` and `.claude/`;
  `agent.md` and `PROJECT_UNDERSTANDING.md` are **not** ignored and are tracked.
- Always **quote the project path** if it contains spaces.
- **Fixing one of the §7 "real gaps"** — write a failing test first (extend `testing/`,
  add it to `run_all_tests.sh`'s array), then fix, then verify the rest of the suite
  still passes.
