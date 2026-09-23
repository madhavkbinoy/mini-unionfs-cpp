#!/bin/bash
# Demo run by run-in-docker.sh.
#
# NOTE: the upper layer runs on the container's own disk, not on the macOS
# shared folder. Colima shares your Mac folder over sshfs, and sshfs refuses
# open(O_CREAT|O_WRONLY, 0000) -- the exact call unionfs_unlink uses to create
# a whiteout marker. Results are copied back to unionfs_test_env/ at the end
# so you can inspect everything from macOS.
set -e
cd /work
HOST_ENV=/work/unionfs_test_env
RUN=/run_env

rm -rf "$RUN"; mkdir -p "$RUN/upper" "$RUN/mnt"
cp -a "$HOST_ENV/lower" "$RUN/lower"

./mini_unionfs "$RUN/lower" "$RUN/upper" "$RUN/mnt"
sleep 1

echo
echo "=== 1. merged view (mnt/) — lower files appear, upper is empty ==="
ls "$RUN/mnt"

echo
echo "=== 2. COPY-ON-WRITE: append to base.txt through the mount ==="
echo "WRITTEN BY THE DEMO" >> "$RUN/mnt/base.txt"
echo "  upper/base.txt  (CoW copy created on demand):"
sed 's/^/      /' "$RUN/upper/base.txt"
echo "  lower/base.txt  (original, NEVER modified):"
sed 's/^/      /' "$RUN/lower/base.txt"

echo
echo "=== 3. WHITEOUT: delete delete_me.txt through the mount ==="
rm "$RUN/mnt/delete_me.txt"
echo "      mnt/ listing : $(ls "$RUN/mnt" | tr '\n' ' ')"
echo "      upper marker : $(ls -a "$RUN/upper" | grep '\.wh\.')"
echo "      lower intact : $(ls "$RUN/lower" | tr '\n' ' ')"

fusermount -u "$RUN/mnt"

# Copy results back to the Mac so they are visible in Finder / your terminal.
# `cp -a` cannot be used: it recreates the whiteout with mode 0000, which sshfs
# rejects. Create each file readable first, then restore the 0000 mode.
rm -rf "$HOST_ENV/upper" "$HOST_ENV/mnt"
mkdir -p "$HOST_ENV/upper" "$HOST_ENV/mnt"
( cd "$RUN/upper" && find . -mindepth 1 -type d -printf '%P\n' ) | while read -r d; do
    mkdir -p "$HOST_ENV/upper/$d"
done
( cd "$RUN/upper" && find . -type f -printf '%P\n' ) | while read -r f; do
    install -m 0644 "$RUN/upper/$f" "$HOST_ENV/upper/$f"
    case "${f##*/}" in .wh.*) chmod 000 "$HOST_ENV/upper/$f" ;; esac
done

echo
echo "=== unmounted. Results copied to unionfs_test_env/upper/ on your Mac. ==="
