# agent.md — Mini-UnionFS

Context file for AI agents working in this repository. Describes what the project is, how
every file fits together, the invariants that must not be broken, and the known gaps
between the code and its documentation.

---

## 1. What this project is

A **userspace union / overlay filesystem** written in C++20 against **libfuse3** (FUSE API
version 31). It stacks a read-only **lower** directory under a read-write **upper**
directory and presents the two as one merged tree at a **mount point** — the same layering
model Docker's `overlay2` storage driver uses for container images.

- License: MIT
- The codebase is organized by topic across three source files (read/path, write/CoW,
  deletion/metadata/links), all defining methods on one shared `class UnionFs`.

### The three mechanisms

1. **Path resolution** (`UnionFs::resolve_path`) — a fixed priority chain:
  whiteout marker in upper → upper → lower → not found.
2. **Copy-on-Write** (`UnionFs::cow_copy`) — the first write to a file that exists only in
  lower copies the whole file into upper, then all writes go to the upper copy. Lower is
  *never* modified (with one narrow exception — `utimens` — see §6).
3. **Whiteouts** — deleting a lower-backed file creates a zero-byte, mode `0000` marker
  file named `.wh.<basename>` in the corresponding upper directory. The resolver treats
  the presence of that marker as "deleted"; `readdir` both hides the marker itself and
  suppresses the lower entry it shadows.

---

## 2. Repository layout

```
.
├── agent.md                  # this file
├── README.md                 # user-facing docs
├── PROJECT_UNDERSTANDING.md  # detailed file-by-file walkthrough of the codebase
├── LICENSE                   # MIT
├── Makefile                  # g++ + pkg-config fuse3, -std=c++20; targets: all, run, debug, clean
├── .gitignore                # ignores upper/, mnt/, *.o, mini_unionfs, CLAUDE.md, .claude/, docs
├── unionfs_cli.sh            # interactive REPL for manual testing (executable)
├── run-in-docker.sh          # macOS -> Colima/Docker build+run wrapper
├── .demo.sh                  # scripted CoW + whiteout demo, run by run-in-docker.sh demo
├── .fuse3-headers/           # vendored fuse3 public headers, for editor/macOS syntax checking
├── src/
│   ├── unionfs.h              # class UnionFs, RAII (Fd/Dir), fuse_guard, sys_error,
│   │                          #   path helpers (upper_path/lower_path/whiteout_path),
│   │                          #   fs_ctx(), unionfs_ops() — the interface contract
│   ├── main.cpp                # arg validation, realpath, constructs UnionFs, fuse_main
│   ├── path.cpp                 # UnionFs::{whiteout_path, resolve_path, getattr,
│   │                          #   readdir, read}
│   ├── rw_ops.cpp                # CoW engine + UnionFs::{cow_copy, open, write, create,
│   │                          #   truncate, utimens, release}; free fn make_parent_dirs
│   └── del_ops.cpp                # UnionFs::{unlink, mkdir, rmdir, chmod, chown, statfs,
│                                #   symlink, readlink, rename, link}; the extern "C"
│                                #   shims (c_getattr, c_readdir, ...); make_ops() +
│                                #   unionfs_ops() (the dispatch table)
├── testing/
│   ├── lib.sh                # assertions, mount/unmount helpers, temp-env setup
│   ├── run_all_tests.sh      # suite runner (hard-coded list of 4 scripts)
│   ├── test_01_layer_visibility.sh
│   ├── test_02_cow_write.sh
│   ├── test_03_whiteout.sh
│   └── test_04_create_new_file.sh
└── unionfs_test_env/
  └── lower/                # tracked seed data for the CLI: base.txt, delete_me.txt,
      └── subdir/           #   readonly.txt, subdir/{nested,other}.txt
                            # upper/ and mnt/ are gitignored, created at runtime
```

The build output `mini_unionfs` lands in the project root and is gitignored.

**The test suite is black-box.** `testing/*.sh` seeds layer directories directly and
drives the mount from the outside via `mountpoint`, `stat`, `ls`, `cat`, etc. — it has no
coupling to any implementation detail of the code under test.

---

## 3. Architecture and control flow

### The UnionFs class

```cpp
class UnionFs {
public:
    UnionFs(std::string lower, std::string upper) noexcept;

    std::string upper_path(std::string_view v) const;
    std::string lower_path(std::string_view v) const;
    std::string whiteout_path(std::string_view v) const;   // THE one builder — src/path.cpp

    std::optional<std::string> resolve_path(const char *path) const;

    int getattr(...); int readdir(...); int read(...);              // path.cpp
    int cow_copy(...); int open(...); int write(...); int create(...);
    int truncate(...); int utimens(...); int release(...);          // rw_ops.cpp
    int unlink(...); int mkdir(...); int rmdir(...); int chmod(...);
    int chown(...); int statfs(...); int symlink(...); int readlink(...);
    int rename(...); int link(...);                                 // del_ops.cpp

private:
    std::string lower_;
    std::string upper_;
};
```

`UnionFs` owns the two layer roots directly as `std::string`. Every FUSE operation is a
**method**, and method bodies are split across `path.cpp` / `rw_ops.cpp` / `del_ops.cpp`
by topic — declared together in `unionfs.h`, defined out-of-line in whichever file
matches the operation's topic.

**⚠️ POSIX name collisions — read this before adding or editing a method.** Several
methods share a name with the libc function they wrap: `open`, `read`, `write`, `unlink`,
`mkdir`, `rmdir`, `chmod`, `rename`, `link`, `symlink`, `readlink`, `truncate`, `readdir`.
Inside **any** `UnionFs` method body, an unqualified call to one of those names resolves
to the C++ member, not the libc function — C++ name lookup stops at the first scope where
a name is found and does not fall through to `::open` just because the member doesn't
match the argument list (this produces a hard compile error — wrong arg count/type — not
a silent miscompile, but you still have to fix it). **Every internal POSIX call in
`path.cpp` / `rw_ops.cpp` / `del_ops.cpp` is therefore explicitly qualified with `::`** —
do the same in any new code. Sanity check:
`grep -nE '[^:_a-zA-Z](open|read|write|unlink|mkdir|rmdir|chmod|rename|link|symlink|readlink|truncate|readdir)\(' src/*.cpp | grep -v '::'`
should always come back empty (excluding the free function `make_parent_dirs` in
`rw_ops.cpp`, which isn't a class member and needs no qualification).

### Fetching the UnionFs from a callback

```cpp
inline UnionFs &fs_ctx() noexcept {
    return *static_cast<UnionFs *>(fuse_get_context()->private_data);
}
```

`main.cpp` heap-allocates a single `UnionFs` (`new UnionFs(lower_real, upper_real)`) after
resolving and validating both paths, and passes it as `fuse_main`'s `private_data`
argument. `fs_ctx()` is the **only** way any code reaches it, and it is called **fresh on
every FUSE request** — never cached in a local or member variable. FUSE is multithreaded
by default here (no `-s` flag), and `fuse_get_context()` is documented as per-request.

### The extern "C" boundary and fuse_guard

```cpp
extern "C" {
static int c_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    return fuse_guard([&] { return fs_ctx().getattr(path, stbuf, fi); });
}
/* ...18 more, one per operation... */
}
```

Every one of the 19 wired FUSE callbacks is a `static` `extern "C"` free function in
`del_ops.cpp`, and every body is **exactly one line**: a `fuse_guard(...)` call wrapping a
lambda that calls the matching `UnionFs` method. This is deliberate and load-bearing:

- **libfuse3 is a C library.** A C++ exception unwinding across its call frames is
  undefined behaviour — in practice, a mount-killing `std::terminate`. `fuse_guard` (in
  `unionfs.h`) is the **only** place in this codebase a thrown exception is ever caught:
  ```cpp
  template <typename Fn> int fuse_guard(Fn &&fn) noexcept {
      try { return fn(); }
      catch (const std::bad_alloc &)      { return -ENOMEM; }
      catch (const std::system_error &e)  { int v = e.code().value(); return v ? -v : -EIO; }
      catch (const std::exception &)      { return -EIO; }
      catch (...)                         { return -EIO; }
  }
  ```
- The `UnionFs` methods themselves are free to throw — `std::string` allocation,
  `std::unordered_set::insert`, etc. — because the shim always catches it. Do not add
  `try`/`catch` inside a `UnionFs` method "just in case"; the shim boundary is the correct
  and only place.
- `extern "C"` on the shims documents the C-ABI boundary explicitly, so future readers
  can see exactly where the language transition happens.

### The dispatch table

```cpp
static struct fuse_operations make_ops() noexcept {
    struct fuse_operations ops{};      // every member zero-initialized
    ops.getattr = c_getattr; ops.readdir = c_readdir; /* ... */
    return ops;
}
const struct fuse_operations &unionfs_ops() {
    static const struct fuse_operations ops = make_ops();   // function-local static
    return ops;
}
```

Both live at the bottom of `del_ops.cpp`. `main.cpp` calls
`fuse_main(fuse_argc, fuse_argv, &unionfs_ops(), fs)`. This design (value-init +
assignment, wrapped in a lazily-built function-local static) buys three things over a
designated-initializer global:

- **Order-independence.** `fuse.h` declares `fuse_operations`'s members in the order
  `getattr, readlink, mknod, mkdir, unlink, rmdir, symlink, rename, link, chmod, chown,
  truncate, open, read, write, statfs, flush, release, fsync, ..., readdir, ..., create,
  lock, utimens, ...`. A C++20 designated initializer must list members in that exact
  declaration order; a plain `.member = value;` assignment sequence does not, so this
  table can stay grouped by topic (read path, write path, deletion/links) instead.
- **No `-Wmissing-field-initializers`.** A designated initializer that only sets 19 of the
  struct's ~40 members triggers one such warning per unset member under `-Wextra` in C++.
  `ops{}` value-initializes everything to `nullptr` first; the assignments after it
  produce zero warnings.
- **Fork/init-order safety.** A function-local `static` is guaranteed thread-safe
  (magic statics) and constructed on first use — relevant because `fuse_main` (called
  from `main`) **forks** with no `-f` flag (see "Invocation" below).

**Not implemented:** `access`, `opendir`/`releasedir`, `flush`, `fsync`, `mknod`,
`*xattr`, `fallocate`, `init`/`destroy`, `lock`. Adding one means adding the method (in the
topically correct file), declaring it in the `UnionFs` class, writing its `c_*` shim, and
adding one line to `make_ops()` — that function is the single registration point.

### Invocation

```
./mini_unionfs <lower_dir> <upper_dir> <mount_dir>
```

`main.cpp` builds a **2-element** FUSE argv of `{ argv[0], argv[3] }`. No `-f`, no `-o`
flags — so the process **daemonizes into the background**. The comment above that line
claiming `-f` foreground mode is stale; the flag is not actually passed. Both the CLI and
the test harness depend on the background behaviour (they call the binary and then
`sleep 1` before touching the mount). Hand-building this 2-element argv also means **no
FUSE option passthrough** (`-o allow_other`, `-d`, `-s` for single-threaded, etc. are all
unavailable).

### Key function contracts

`std::optional<std::string> UnionFs::resolve_path(const char *path) const` — `src/path.cpp`

- Returns the resolved **real host path**, or `std::nullopt` if a whiteout hides it or it
  exists in neither layer (both cases are `-ENOENT` to every caller; `resolve_path` never
  distinguishes them).
- `"/"` always resolves to `upper_dir()` (root is assumed to exist in upper).
- Existence tests use `access(p, F_OK)`, which **follows symlinks**.
- No output buffer, no `MAX_PATH_LEN` precondition.

`int UnionFs::cow_copy(const char *path)` — `src/rw_ops.cpp`

- `path` is the *virtual* path (`/subdir/f.txt`); the method derives lower/upper itself via
  `lower_path()`/`upper_path()`.
- `open(lower)` → `fstat` → `make_parent_dirs(upper)` → `open(upper, O_CREAT|O_WRONLY|O_TRUNC, st.st_mode)`
  → 64 KB read/write loop with a short-write-safe inner loop → `fchmod` → both `Fd`s close
  automatically on scope exit.
- Returns `0` or a negative errno. **Files only** — it has no directory-copy path (see §6).

`int make_parent_dirs(const char *full_path)` — `src/rw_ops.cpp`

- `mkdir -p` for the *parent* of `full_path` (it strips the last component), mode `0755`,
  tolerating `EEXIST`. Returns `0` or `-errno`.
- Deliberately a **free function, not a `UnionFs` method** — it has no notion of
  upper/lower, it just mutates an already-resolved host path. Takes a raw `const char*`
  (callers pass `.c_str()` from a `std::string`).

### The whiteout path builder

`UnionFs::whiteout_path(std::string_view v) const`, defined in `path.cpp`, is the **only**
place the `.wh.` path layout is implemented anywhere in this codebase. If you are ever
tempted to hand-roll a `.wh.` path somewhere else, call `whiteout_path()` instead; there
should never be a second implementation of this.

Layout:

| Input | Output |
|---|---|
| `/subdir/file.txt` | `<upper_dir>/subdir/.wh.file.txt` |
| `/file.txt` (root level) | `<upper_dir>/.wh.file.txt` |
| `file.txt` (no leading slash — defensive) | `<upper_dir>/.wh.file.txt` |

Built with plain `std::string` concatenation — **never** `std::filesystem::path`. FUSE
virtual paths always start with `/` (absolute), and `std::filesystem::path::operator/`
silently **discards its left operand** when the right operand is absolute:
`fs::path("/upper") / "/subdir/f.txt"` evaluates to `/subdir/f.txt`, not
`/upper/subdir/f.txt` — which would make every composed path escape both layers. This
project avoids `<filesystem>` entirely for path composition specifically to make that
class of bug structurally impossible rather than relying on code-review discipline. If
`std::filesystem` is ever introduced for something else (e.g. iterating a directory), do
not use it to build upper/lower/whiteout paths.

### RAII resource wrappers

`Fd` and `Dir` (both in `unionfs.h`) own a POSIX file descriptor / `DIR*` respectively,
move-only, `noexcept`, closing on destruction unless explicitly `release()`d. **There is
no manual `close()`/`closedir()` anywhere in `path.cpp`/`rw_ops.cpp`/`del_ops.cpp`** —
every site either lets the wrapper's destructor do it, or calls `.reset()` to close
early, or (exactly one case: `UnionFs::create`, handing the descriptor to `fi->fh`) calls
`.release()` to transfer ownership out. If you add a new syscall that opens a fd or
`DIR*`, wrap it in `Fd`/`Dir` immediately — do not call the raw POSIX function and hold a
bare `int`/`DIR*`.

### errno capture

`sys_error()` (in `unionfs.h`) is `inline int sys_error() noexcept { return -errno; }` —
call it as the *first* thing after a failing syscall, ideally on the same line
(`return sys_error();`), never after any allocation, `std::string` construction, or RAII
destructor might have run. C++ guarantees the return expression is fully evaluated before
any local `Fd`/`Dir` destructor runs at scope exit, so `return sys_error();` can never
have its `errno` read clobbered by a destructor's `close()` call.

---

## 4. Build, run, test

### Build

```bash
make              # g++ -Wall -Wextra -std=c++20 $(pkg-config --cflags/--libs fuse3)
make clean
make run          # builds, then launches ./unionfs_cli.sh
make debug        # -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
```

Requires `libfuse3-dev`, `g++` (GCC 10+ for C++20), `make`, `pkg-config` on Linux (README
targets Ubuntu 22.04). If fuse3 lives somewhere unusual:
`PKG_CONFIG_PATH=/path/to/fuse3/lib/pkgconfig make`.

**The build is warning-clean.** `-Wall -Wextra -std=c++20` produces zero warnings. Keep it
that way — a new warning on a change is a signal something is off, not something to
suppress.

### Interactive CLI

`./unionfs_cli.sh` wipes and reseeds `unionfs_test_env/{lower,upper}`, mounts at
`unionfs_test_env/mnt`, and drops into a REPL. Built-ins: `cd`, `pwd`, `layers`,
`setup`/`reset`, `mount`, `umount`, `help`, `exit`/`quit`. Anything else is `eval`'d in a
subshell with CWD set to the tracked directory inside `mnt/`. `repl_cd` refuses to escape
`$MNT`. `layers` prints lower/upper/mnt side by side and flags `.wh.*` entries in red.
On exit it unmounts.

### Test suite

```bash
bash testing/run_all_tests.sh
```

Each script `source`s `lib.sh`, calls `setup_env` (a fresh `mktemp -d
/tmp/unionfs_test_XXXXXX` with `lower/ upper/ mnt/`), seeds files **directly into the
layer directories**, calls `mount_fs`, asserts, then `teardown_env`. Assertions available:
`assert_file_exists`, `assert_file_not_exists`, `assert_contains`, `assert_not_contains`,
`assert_cmd_fails`, plus raw `pass`/`fail`. `print_summary` sets the exit code.

Override the binary with `FUSE_BINARY=/path/to/mini_unionfs bash testing/run_all_tests.sh`.

The four current tests cover: layer visibility + upper-shadows-lower (01, 8 assertions),
CoW on append/overwrite/truncate + upper-only writes (02, 13 assertions), whiteout
creation, listing suppression, and no-whiteout-for-upper-only-delete (03, 9 assertions),
new-file creation landing in upper only (04, 13 assertions) — **43 assertions total**.

### ⚠ Not buildable natively on macOS — use the Colima/Docker path

macOS lacks fuse3 via pkg-config and `fusermount`/`fusermount3`, so `make` cannot even
expand its flags. The scripts also use Linux-only tooling: `mountpoint -q`,
`fusermount -u`, and `stat -c '%s'` (GNU format; BSD `stat` uses `-f %z`). Installing
macFUSE does **not** fix this cleanly — macFUSE targets the FUSE 2.x API, while this
project requires fuse3. The C++ source itself compiles cleanly on macOS against the
vendored `.fuse3-headers/` — it's the link step and the runtime (FUSE needs a Linux
kernel driver) that fail, not the language.

**The working path:** build and test inside a Linux container on a Colima VM. FUSE in a
container needs `/dev/fuse` plus `SYS_ADMIN`, hence the flags below.

```bash
colima start

docker run --rm -it \
--device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor:unconfined \
-v "$PWD:/work:ro" ubuntu:22.04 bash -c '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq && apt-get install -y -qq build-essential pkg-config libfuse3-dev fuse3
  cp -r /work /build && cd /build && rm -f mini_unionfs src/*.o
  make && bash testing/run_all_tests.sh
'
```

Notes on that recipe:

- `-v "$PWD:/work:ro"` mounts the source **read-only** and the script copies to `/build`,
so the aarch64-Linux `.o` files and binary never land in the host project directory.
Drop `:ro` and build in `/work` only if you deliberately want the artifacts on the host
(they are gitignored either way).
- **Colima only mounts `$HOME`.** Paths under `/private/tmp` or `/tmp` are *not* visible
inside the VM — a `-v` of such a path silently creates an empty directory instead. Keep
everything you bind-mount under the home directory.
- The interactive CLI (`./unionfs_cli.sh`) works in the same container if you run it with
`-it`; it needs `mountpoint`/`fusermount`, both provided by the `fuse3` package.
- `run-in-docker.sh` (repo root) wraps all of the above: it starts Colima if needed, builds
a cached toolchain image, and takes a mode — `build` | `test` (default) | `cli` | `demo`
| `shell`. `.demo.sh` is its demo payload.

### ⚠ Shared-folder gotcha: whiteouts fail if `upper/` is on the macOS-shared path

Colima shares `$HOME` into the VM over a FUSE-based mount. That share **rejects
`open(path, O_CREAT|O_WRONLY|O_TRUNC, 0000)`** with `EACCES` — exactly the call
`UnionFs::unlink` uses to write a whiteout marker (and `UnionFs::rmdir`), so **every
deletion of a lower-backed file fails with "Permission denied"** when the upper layer
lives on the shared folder. Reads, CoW writes, and creates all work fine there — only
whiteout-creating deletion breaks.

This is an environment limitation, not a bug in the filesystem logic — the full test
suite passes when the layers are on a normal Linux filesystem. Two ways to deal with it:

- **Keep `upper/` off the share** (what `.demo.sh` does): run the layers under a
container-local path such as `/run_env`, then copy results back to
`unionfs_test_env/` for inspection from macOS. Note `cp -a` cannot copy the marker back
either — create it readable with `install -m 0644`, then `chmod 000`. The failed `open`
attempt on the share also **still creates the file** before failing, leaving an
unreadable mode-`0000` stray behind — `chmod 644` it before removal if you hit one.
- **Or make the marker mode non-zero** in `del_ops.cpp`. Nothing depends on `0000`:
`resolve_path` and the whiteout-existence checks use `access(F_OK)` (existence only,
mode-independent) and `readdir` filters by the `.wh.` name prefix. `0644` would be
functionally identical and portable. Not applied — it is a deliberate style choice.

---

## 5. Behavioural invariants — do not break these

1. **The lower directory is never written to**, except by `utimens` (see §6). No
  `open(lower, O_WRONLY)`, no `unlink`, `rename`, `chmod`, or `truncate` targeting a lower
  path. Every other mutation goes through upper, via CoW when the source is in lower.
  Tests 02 and 03 assert this directly.
2. **Whiteout markers never appear in listings.** `UnionFs::readdir` skips any name
  starting with `.wh.` in the upper pass.
3. **Upper shadows lower** for identical names, in both `resolve_path` and the `readdir`
  merge order (upper pass first, the `std::unordered_set` blocks the lower duplicate).
4. **A whiteout beats everything** — it is checked *before* upper in `resolve_path`, which
  is what makes "delete then the file stays gone" work even if a CoW copy existed
  (`unlink` PATH B removes the CoW copy *and* writes the marker).
5. **Re-creating a deleted name clears its marker.** Both `UnionFs::create` and
  `UnionFs::mkdir` unlink a stale `.wh.` before creating. Dropping this reintroduces a
  "deleted file cannot be recreated" bug.
6. **The `readdir` dedup set is per-call.** A stack-local
  `std::unordered_set<std::string>`; never promote it to a member or global — FUSE runs
  multithreaded here.
7. **Open-mode detection must be `(fi->flags & O_ACCMODE) != O_RDONLY`**
  (`UnionFs::open`, `rw_ops.cpp`). `O_RDONLY` is `0`, so a naive `fi->flags & O_WRONLY`
  test silently misses `O_RDWR`.
8. **Callbacks return negated errno** (`-ENOENT`, `sys_error()`), never `-1`. Read/write
  return the byte count on success. `fuse_guard` preserves this contract for thrown
  exceptions too (`-ENOMEM`, `-EIO`, or the `errno` value carried by a `std::system_error`).

---

## 6. Known limitations and latent bugs

Documented so an agent does not "discover" them as surprises or file them as regressions.
Several are deliberate scope limits of a teaching project; others are real defects.

**Deliberate / documented limits**

- **`rmdir` on a lower-only directory returns `-EPERM`** (`UnionFs::rmdir`,
  `del_ops.cpp`). Removing a lower directory would require whiteout-ing every
  descendant; not implemented.
- **No opaque-directory support.** A whiteout hides a *name*, but there is no
  `.wh..wh..opq` equivalent.

**Real gaps worth knowing**

- **Parent whiteouts are not consulted for children.** `resolve_path("/subdir/nested.txt")`
only checks `.wh.nested.txt`, never `.wh.subdir`. So after a directory is whited out, its
children remain reachable by direct path even though the directory itself is hidden.
- **`cow_copy` on a directory is wrong.** `chmod`/`chown` of a lower-only *directory* call
`cow_copy`, which `open()`s the directory read-only and then creates a regular file at
the upper path. It should `mkdir` instead. Same hazard applies to `rename`/`link` of a
lower-only directory.
- **`rename` leaves the lower original visible.** After `rename(from, to)` on a lower-backed
file, the CoW copy moves to `to` but no whiteout is written for `from`, so `from`
reappears from the lower layer. `rename`'s `flags` (`RENAME_NOREPLACE`,
`RENAME_EXCHANGE`) are also still ignored.
- **`link` creates a hard link inside upper only**, so the two names are only linked in the
CoW copy, not with anything in lower.
- **`utimens` can write to the lower layer.** It calls `utimensat` on whatever
`resolve_path` returned; for a lower-only file that is the lower path. A bare `touch` of
an existing lower file therefore mutates lower's mtime — the one place invariant #1 leaks.
- **`read`/`write` don't use `fi->fh`.** `UnionFs::create` sets `fi->fh` via
`Fd::release()`, but `UnionFs::read`/`UnionFs::write` still re-`open()` the resolved/upper
path on every call rather than reusing it. A file created with a read-only `mode` (e.g.
`0444`) will fail the subsequent `open(upper, O_WRONLY)` inside `write`.
- **`readlink` uses `access(F_OK)` to pick the layer**, which follows the symlink — a
dangling symlink in upper falls through to the lower branch or `-ENOENT`.
- **`.wh.`-named files in the *lower* layer are not filtered.** The `.wh.` skip only runs
in `readdir`'s upper pass, so a file genuinely named `.wh.foo` in lower shows up in
listings. Conversely, such a file in upper is hidden from `readdir` but still resolvable
by `getattr`.

---

## 7. Conventions to follow when editing

- **C++ style:** 4-space indent, `snake_case` for functions/variables, `PascalCase` for
class names (`UnionFs`, `Fd`, `Dir`), opening brace on the same line. Paths are
`std::string`/`std::string_view`, built with `+=` concatenation — never
`std::filesystem::path::operator/` (see §3, "The whiteout path builder"). The two fixed-
size `char[MAX_PATH_LEN]` buffers that remain (`readlink`'s raw syscall output buffer,
`make_parent_dirs`' in-place `mkdir -p` working buffer) are a different category from a
path-composition buffer — don't "fix" them into `std::string` without a reason; they're
not part of the whiteout/layer-path system.
- **The build is warning-clean.** `-Wall -Wextra -std=c++20` currently produces zero
warnings. Keep it that way.
- **Every raw fd/`DIR*` goes through `Fd`/`Dir`.** No bare `close()`/`closedir()` calls.
- **New FUSE callback** = write it as a `UnionFs` method in the topically correct file
(read/path → `path.cpp`, write/CoW → `rw_ops.cpp`, delete/dir/metadata/links →
`del_ops.cpp`), declare it in the `UnionFs` class body in `unionfs.h`, write its
`extern "C" static int c_<name>(...)` shim next to the others in `del_ops.cpp` (one line:
`return fuse_guard([&]{ return fs_ctx().<name>(...); });`), and add one line to
`make_ops()`. Remember to `::`-qualify any internal call whose name collides with the new
method's own name (see §3's POSIX-collision warning).
- **New test** = copy an existing `test_0N_*.sh`, `source lib.sh`, use `setup_env` /
`mount_fs` / `teardown_env` / `print_summary`, and add the filename to the `TESTS` array
in `run_all_tests.sh` (the runner does not glob).
- **Commit messages** follow loose conventional-commit style: `feat: add link FUSE op`,
`refactor: ...`, `build: ...`, `fix: ...`.
- Do not commit `mini_unionfs`, `*.o`, `unionfs_test_env/upper/`, or
`unionfs_test_env/mnt/`. Note that `.gitignore` also excludes `CLAUDE.md` and `.claude/`
— `agent.md` and `PROJECT_UNDERSTANDING.md` are **not** ignored and are tracked.
- **If you are asked to fix one of the §6 "real gaps" bugs,** write a failing test first
(extend `testing/`, add it to `run_all_tests.sh`'s array), then fix, then verify the rest
of the suite still passes. Keep behavior changes and structural/refactoring changes in
separate commits.
