/*
 * unionfs.h — Shared header for Mini-UnionFS
 *
 * Declares class UnionFs (every filesystem operation as a method), the
 * RAII resource wrappers (Fd, Dir), the exception-safety helpers
 * (sys_error, fuse_guard), and the constants shared by path.cpp,
 * rw_ops.cpp, and del_ops.cpp. This is the interface contract between
 * all three.
 */

#ifndef UNIONFS_H
#define UNIONFS_H

#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h> //file controls
#include <unistd.h> // Posix System calls // low level os ops
#include <sys/stat.h> //File perms st.s
#include <sys/types.h> //datatype for sys calls
#include <dirent.h> //dir handling
#include <limits.h> //path lim

#include <utility>       // std::exchange
#include <system_error>  // std::system_error (fuse_guard)
#include <exception>     // std::exception (fuse_guard)
#include <string>
#include <string_view>
#include <optional>
#include <unordered_set>

/* ---------- Constants ---------- */
#define WH_PREFIX     ".wh."      /* whiteout file prefix */
#define WH_PREFIX_LEN 4
#define MAX_PATH_LEN  PATH_MAX

/* ---------- RAII resource wrappers ----------
 *
 * Every raw POSIX fd/DIR* owned by this codebase should be wrapped in one
 * of these rather than close()d / closedir()d by hand. Both are move-only,
 * noexcept, and safe to default-construct in the "empty" state.
 */

/* Owns a POSIX file descriptor; closes it on destruction unless released. */
class Fd {
    int fd_ = -1;
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    ~Fd() { reset(); }

    Fd(Fd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd &operator=(Fd &&other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    /* Hand ownership to the caller (e.g. fi->fh); this Fd no longer closes it. */
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

    void reset() noexcept {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
};

/* Owns a POSIX DIR*; closes it on destruction. */
class Dir {
    DIR *dp_ = nullptr;
public:
    Dir() noexcept = default;
    explicit Dir(DIR *dp) noexcept : dp_(dp) {}
    ~Dir() { reset(); }

    Dir(Dir &&other) noexcept : dp_(std::exchange(other.dp_, nullptr)) {}
    Dir &operator=(Dir &&other) noexcept {
        if (this != &other) {
            reset();
            dp_ = std::exchange(other.dp_, nullptr);
        }
        return *this;
    }
    Dir(const Dir &) = delete;
    Dir &operator=(const Dir &) = delete;

    [[nodiscard]] DIR *get() const noexcept { return dp_; }
    [[nodiscard]] bool valid() const noexcept { return dp_ != nullptr; }

    void reset() noexcept {
        if (dp_) ::closedir(dp_);
        dp_ = nullptr;
    }
};

/* ---------- errno / exception-safety helpers ----------
 *
 * sys_error() reads errno immediately (call it as the *first* thing after
 * a failing syscall, same statement if possible) and returns the negated
 * value FUSE callbacks are required to return.
 *
 * fuse_guard() must wrap every extern "C" FUSE callback body starting in
 * Phase 4. libfuse3 is a C library: an exception unwinding into it is
 * undefined behaviour. This is the one and only place a thrown exception
 * is allowed to be caught in this codebase.
 */

[[nodiscard]] inline int sys_error() noexcept { return -errno; }

template <typename Fn>
inline int fuse_guard(Fn &&fn) noexcept {
    try {
        return fn();
    } catch (const std::bad_alloc &) {
        return -ENOMEM;
    } catch (const std::system_error &e) {
        int v = e.code().value();
        return v ? -v : -EIO;
    } catch (const std::exception &) {
        return -EIO;
    } catch (...) {
        return -EIO;
    }
}

/* ---------- make_parent_dirs — free function, not layer-aware ----------
 *
 * "mkdir -p" for the parent of an already-resolved HOST path (defined in
 * rw_ops.cpp). Deliberately not a UnionFs method: it has no notion of
 * upper/lower, it just mutates a filesystem path handed to it.
 */
int make_parent_dirs(const char *full_path);

/* ================================================================
 * class UnionFs — owns the two layer roots and every FUSE operation.
 *
 * Method bodies live in whichever .cpp file matches the project's existing
 * per-topic split (read/path -> path.cpp, write/CoW -> rw_ops.cpp,
 * delete/dir/metadata/links -> del_ops.cpp) rather than one big file.
 *
 * IMPORTANT — name collisions with POSIX: several methods share a name
 * with the libc function they wrap (open, read, write, unlink, mkdir,
 * rmdir, chmod, rename, link, symlink, readlink, truncate, readdir).
 * Inside ANY UnionFs method body, an unqualified call to one of those
 * names resolves to the UnionFs member (C++ name lookup stops at the
 * first scope where the name is found — it does not fall through to
 * ::open just because the member doesn't match the argument list).
 * Every internal POSIX call in path.cpp/rw_ops.cpp/del_ops.cpp is
 * therefore explicitly qualified with `::` — do the same in any new code.
 * ================================================================ */

class UnionFs {
public:
    UnionFs(std::string lower, std::string upper) noexcept
        : lower_(std::move(lower)), upper_(std::move(upper)) {}

    [[nodiscard]] const std::string &lower_dir() const noexcept { return lower_; }
    [[nodiscard]] const std::string &upper_dir() const noexcept { return upper_; }

    /* ---- Path helpers — the single source of truth for turning a virtual
     * FUSE path ("/subdir/file.txt") into a real host path. Built with
     * plain std::string concatenation, never std::filesystem::path's
     * operator/ — operator/ silently discards the left-hand side when the
     * right-hand side is absolute, and every virtual path here IS
     * absolute, which would make every composed path escape both layers. */

    [[nodiscard]] std::string upper_path(std::string_view v) const {
        std::string result(upper_);
        result += v;
        return result;
    }
    [[nodiscard]] std::string lower_path(std::string_view v) const {
        std::string result(lower_);
        result += v;
        return result;
    }
    /*
     * Build the whiteout marker path for virtual path 'v':
     *   "/subdir/file.txt" -> "<upper_dir>/subdir/.wh.file.txt"
     *   "/file.txt"        -> "<upper_dir>/.wh.file.txt"    (root level)
     *   "file.txt"         -> "<upper_dir>/.wh.file.txt"    (defensive;
     *                                                          FUSE always
     *                                                          passes a
     *                                                          leading '/')
     * THE single whiteout-path builder for the whole project. Do not
     * reimplement this elsewhere.
     */
    [[nodiscard]] std::string whiteout_path(std::string_view v) const;

    /* Central lookup: whiteout marker in upper -> upper -> lower -> ENOENT.
     * Returns the resolved host path, or nullopt if the whiteout marker
     * hides it or it exists in neither layer (both cases are ENOENT to
     * every caller — resolve_path never distinguishes them). */
    [[nodiscard]] std::optional<std::string> resolve_path(const char *path) const;

    /* read path (path.cpp) */
    int getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi);
    int readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                struct fuse_file_info *fi, enum fuse_readdir_flags flags);
    int read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);

    /* write path & CoW engine (rw_ops.cpp) */
    int cow_copy(const char *path);
    int open(const char *path, struct fuse_file_info *fi);
    int write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
    int create(const char *path, mode_t mode, struct fuse_file_info *fi);
    int truncate(const char *path, off_t size, struct fuse_file_info *fi);
    int utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi);
    int release(const char *path, struct fuse_file_info *fi);

    /* deletion, directories, metadata, links (del_ops.cpp) */
    int unlink(const char *path);
    int mkdir(const char *path, mode_t mode);
    int rmdir(const char *path);
    int chmod(const char *path, mode_t mode, struct fuse_file_info *fi);
    int chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi);
    int statfs(const char *path, struct statvfs *stbuf);
    int symlink(const char *target, const char *linkpath);
    int readlink(const char *path, char *buf, size_t size);
    int rename(const char *from, const char *to, unsigned int flags);
    int link(const char *from, const char *to);

private:
    std::string lower_;
    std::string upper_;
};

/* Fetch the UnionFs for the current FUSE call. Always call this fresh —
 * never cache the reference — fuse_get_context() is per-request and FUSE
 * is multithreaded by default here. */
[[nodiscard]] inline UnionFs &fs_ctx() noexcept {
    return *static_cast<UnionFs *>(fuse_get_context()->private_data);
}

/* Dispatch table (assembled in del_ops.cpp via make_ops()). Lazily built
 * on first call via a function-local static — sidesteps static-init-order
 * concerns across translation units and is safe across main.cpp's fork
 * into fuse_main() since it isn't constructed until actually needed. */
[[nodiscard]] const struct fuse_operations &unionfs_ops();

#endif /* UNIONFS_H */
