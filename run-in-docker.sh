#!/usr/bin/env bash
# run-in-docker.sh — build & run Mini-UnionFS from macOS via the Colima Linux VM.
#
# FUSE needs a Linux kernel, which macOS does not have. This script runs the
# project inside a Linux container (on the Colima VM) while keeping the source
# and the lower/upper layers on your Mac, so you can inspect them normally.
#
#   ./run-in-docker.sh            build + run the test suite
#   ./run-in-docker.sh cli        interactive unionfs_cli.sh REPL
#   ./run-in-docker.sh demo       scripted CoW + whiteout demo
#   ./run-in-docker.sh shell      plain bash shell inside the container
#   ./run-in-docker.sh build      just compile

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="mini-unionfs-dev"
MODE="${1:-test}"

# --- 1. Colima VM (provides the Linux kernel + /dev/fuse) --------------------
if ! colima status &>/dev/null; then
    echo ">> Colima is not running — starting it (takes ~30s)..."
    colima start
fi

# --- 2. Toolchain image (built once, then cached) ----------------------------
if ! docker image inspect "$IMAGE" &>/dev/null; then
    echo ">> Building the $IMAGE toolchain image (one time only)..."
    docker build -t "$IMAGE" - <<'DOCKERFILE'
FROM ubuntu:22.04
RUN apt-get update -qq \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      build-essential pkg-config libfuse3-dev fuse3 \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /work
DOCKERFILE
fi

# --- 3. What to run inside the container -------------------------------------
# `cli` and `shell` are staged onto the container's own disk (/build) rather than
# run from /work. Colima shares your Mac folder over sshfs, which rejects
# open(O_CREAT|O_WRONLY, 0000) -- the call unionfs_unlink uses for whiteouts -- so
# `rm` of a lower-layer file would fail with "Permission denied" on the share.
# On exit, upper/ is copied back to /work so you can inspect it from macOS.
STAGE='
  set -e
  rm -rf /build; mkdir -p /build/unionfs_test_env
  # Copy selectively. A plain `cp -a /work /build` fails: a leftover mode-0000
  # whiteout in unionfs_test_env/upper/ is unreadable over sshfs, which would
  # silently abort staging and leave us running off the share.
  cp -a /work/src /work/testing /work/Makefile /work/unionfs_cli.sh /build/
  cp -a /work/unionfs_test_env/lower /build/unionfs_test_env/lower
  cd /build
  rm -f mini_unionfs src/*.o
  make >/dev/null 2>&1 || { echo "build failed"; make; exit 1; }
  # unionfs_cli.sh runs under `set -e`, so ANY failing command inside the REPL
  # (e.g. `ls missing.txt`) kills the whole session. Relax it in this staged
  # copy only; the file in your repo is untouched.
  sed -i "s/^set -euo pipefail$/set -uo pipefail/" /build/unionfs_cli.sh
  set +e
'
SYNC_BACK='
  H=/work/unionfs_test_env; B=/build/unionfs_test_env
  if [ -d "$B/upper" ]; then
    rm -rf "$H/upper"; mkdir -p "$H/upper"
    ( cd "$B/upper" && find . -mindepth 1 -type d -printf "%P\n" ) | while read -r d; do
        mkdir -p "$H/upper/$d"
    done
    ( cd "$B/upper" && find . -type f -printf "%P\n" ) | while read -r f; do
        install -m 0644 "$B/upper/$f" "$H/upper/$f" 2>/dev/null
        case "${f##*/}" in .wh.*) chmod 000 "$H/upper/$f" 2>/dev/null ;; esac
    done
    echo; echo ">> upper/ copied back to unionfs_test_env/upper on your Mac."
  fi
'

case "$MODE" in
    build) INNER='make' ;;
    test)  INNER='make && bash testing/run_all_tests.sh' ;;
    demo)  INNER='make >/dev/null 2>&1 && bash /demo.sh' ;;
    cli)   INNER="$STAGE"'./unionfs_cli.sh; '"$SYNC_BACK" ;;
    shell) INNER="$STAGE"'echo ">> Staged at /build (container disk). Run ./unionfs_cli.sh or make."; bash; '"$SYNC_BACK" ;;
    *)     echo "unknown mode: $MODE (use: build|test|cli|demo|shell)"; exit 1 ;;
esac

# --- 4. Run -------------------------------------------------------------------
# -v mounts your Mac folder into the container: edits and results flow BOTH ways.
# --device /dev/fuse + --cap-add SYS_ADMIN are what allow mounting inside a container.
TTY=(-i); [ -t 0 ] && TTY=(-it)

DEMO_MOUNT=()
if [ "$MODE" = demo ]; then
    DEMO_MOUNT=(-v "$PROJECT_DIR/.demo.sh:/demo.sh:ro")
fi

exec docker run --rm "${TTY[@]}" \
    --device /dev/fuse --cap-add SYS_ADMIN --security-opt apparmor:unconfined \
    -v "$PROJECT_DIR:/work" -w /work \
    ${DEMO_MOUNT[@]+"${DEMO_MOUNT[@]}"} \
    "$IMAGE" bash -c "$INNER"
