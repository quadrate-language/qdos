#!/usr/bin/env bash
#
# Cross-compile QDOS and its Quadrate dependencies for a Raspberry Pi Zero, then
# run every test under emulation.
#
#   QDOS_ARCH=armv6     Zero W   (ARM1176, ARMv6)
#   QDOS_ARCH=aarch64   Zero 2 W (Cortex-A53, ARMv8)
#
# Runs inside the container from cross/Dockerfile. Use cross/run.sh to drive it.
set -euo pipefail

QUADRATE_SRC=${QUADRATE_SRC:-/work/quadrate}
QDOS_SRC=${QDOS_SRC:-/work/qdos}
ARCH=${QDOS_ARCH:-armv6}

case "$ARCH" in
armv6)
	TRIPLE=arm-linux-gnueabihf
	CROSS_FILE="$QDOS_SRC/cross/armv6-linux-gnueabihf.ini"
	QEMU=qemu-arm
	;;
aarch64)
	TRIPLE=aarch64-linux-gnu
	CROSS_FILE="$QDOS_SRC/cross/aarch64-linux-gnu.ini"
	QEMU=qemu-aarch64
	;;
*)
	echo "Unknown QDOS_ARCH '$ARCH' (expected armv6 or aarch64)" >&2
	exit 1
	;;
esac

# Where qemu-user finds the target's shared libraries
export QEMU_LD_PREFIX="/usr/$TRIPLE"

BUILD="$QUADRATE_SRC/build/$ARCH"
STAGE="$QUADRATE_SRC/build/$ARCH-dist"

echo "=== Cross-compiling Quadrate libraries for $ARCH ($TRIPLE) ==="
# LLVM is optional in this tree, so llvmgen and lib/qd simply do not get built.
# Nothing QDOS needs depends on them, and -Dbuild_tools=false drops the command
# line tools that do require it.
#
# werror is off for one specific reason: cross GCC 14 raises a
# -Wnull-dereference false positive inside libstdc++'s <streambuf>, reached from
# the read-whole-file idiom at lib/qc/src/semantic_validator.cc:540. The warning
# is in a system header, not in Quadrate. Native builds keep werror.
meson setup "$BUILD" "$QUADRATE_SRC" \
	--cross-file "$CROSS_FILE" \
	--buildtype=release \
	-Dbuild_tests=true \
	-Dbuild_tools=false \
	-Dbuild_stdlib=false \
	-Dwerror=false \
	--wipe 2>/dev/null || \
meson setup "$BUILD" "$QUADRATE_SRC" \
	--cross-file "$CROSS_FILE" \
	--buildtype=release \
	-Dbuild_tests=true

meson compile -C "$BUILD" interp qc rt_static test_interp

echo
echo "=== Staging a dist tree for the cross build ==="
rm -rf "$STAGE"
mkdir -p "$STAGE/lib/quadrate" "$STAGE/include/quadrate"

# meson emits some of these as thin archives that only reference objects back in
# the build tree, so each is repacked with the cross ar before being staged.
repack() {
	local dir="$1" src="$2" out="$3"
	(cd "$dir" && "$TRIPLE-ar" rcs "$out" $("$TRIPLE-ar" -t "$src"))
	cp "$dir/$out" "$STAGE/lib/quadrate/"
}
repack "$BUILD/lib/interp"       libinterp.a     libinterp_packed.a
repack "$BUILD/lib/qc"           libqc.a         libqc_packed.a
repack "$BUILD/lib/rt"           librt_static.a  librt_packed.a
repack "$BUILD/subprojects/u8t"  libu8t.a        libu8t_packed.a

# Stage under the names QDOS links
mv "$STAGE/lib/quadrate/libinterp_packed.a" "$STAGE/lib/quadrate/libinterp.a"
mv "$STAGE/lib/quadrate/libqc_packed.a"     "$STAGE/lib/quadrate/libqc.a"
mv "$STAGE/lib/quadrate/librt_packed.a"     "$STAGE/lib/quadrate/librt.a"
mv "$STAGE/lib/quadrate/libu8t_packed.a"    "$STAGE/lib/quadrate/libu8t.a"

for mod in interp qc rt; do
	cp -r "$QUADRATE_SRC/lib/$mod/include/quadrate/$mod" "$STAGE/include/quadrate/"
done

echo "Staged:"
for f in "$STAGE"/lib/quadrate/*.a; do
	printf '  %-16s %s\n' "$(basename "$f")" "$(file -b "$f" | cut -c1-40)"
done

echo
echo "=== Verifying the archives are actually $ARCH ==="
obj=$("$TRIPLE-ar" -t "$STAGE/lib/quadrate/libinterp.a" | head -1)
(cd /tmp && "$TRIPLE-ar" x "$STAGE/lib/quadrate/libinterp.a" "$obj" && file "$obj")

echo
echo "=== Cross-compiling QDOS ==="
# No SDL3 for the target, so only the device backend is built — which is the
# one that matters for hardware anyway.
rm -rf "$QDOS_SRC/build/$ARCH"
meson setup "$QDOS_SRC/build/$ARCH" "$QDOS_SRC" \
	--cross-file "$CROSS_FILE" \
	--buildtype=release \
	-Dsim=false \
	-Dstatic=true \
	-Ddevice=true \
	-Dquadrate_src="$QUADRATE_SRC" \
	-Dquadrate_dist=build/$ARCH-dist

meson compile -C "$QDOS_SRC/build/$ARCH"

echo
echo "=== Binary check ==="
file "$QDOS_SRC/build/$ARCH/qdos"

echo
echo "=== Running Quadrate's interpreter tests under $QEMU ==="
"$QEMU" "$BUILD/lib/interp/tests/test_interp" | tail -3

echo
echo "=== Running QDOS's tests under $QEMU ==="
meson test -C "$QDOS_SRC/build/$ARCH" --print-errorlogs

echo
echo "=== ALL $ARCH CHECKS PASSED ==="
