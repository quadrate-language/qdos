#!/usr/bin/env bash
#
# Build QDOS as an Android app.
#
#   ./android/build.sh                   # debug APK for phones (arm64-v8a)
#   QDOS_ABIS="arm64-v8a x86_64" ./android/build.sh   # and the emulator
#   ./android/build.sh install           # build and install over adb
#
# The shell runs on the SDL3 simulator backend, loaded by SDLActivity as
# libmain.so. For each ABI this cross-compiles the Quadrate libraries, SDL3 and
# QDOS with the NDK, then Gradle packages them.
#
# Output: android/app/build/outputs/apk/debug/app-debug.apk
set -euo pipefail

QDOS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
QUADRATE_DIR=${QUADRATE_DIR:-$(cd "$QDOS_DIR/../quadrate" && pwd)}
CACHE=${QDOS_BUILD_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/qdos-android}
ABIS=${QDOS_ABIS:-arm64-v8a}
API=24
TARGET=${1:-}

SDL_VERSION=3.4.16
SDL_SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68

SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}
if [ -z "${ANDROID_NDK_HOME:-}" ]; then
	# Newest installed; r28 and later align for 16 KB pages, which Android 15
	# devices can require
	ANDROID_NDK_HOME=$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1)
fi
if [ ! -d "$ANDROID_NDK_HOME" ]; then
	echo "$0: no NDK found; set ANDROID_NDK_HOME" >&2
	exit 1
fi
BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"

# Gradle 8 runs on neither 11 nor 21+ with this plugin; take 17 when it is there
if [ -z "${JAVA_HOME:-}" ] && [ -d /usr/lib/jvm/java-17-openjdk ]; then
	export JAVA_HOME=/usr/lib/jvm/java-17-openjdk
fi

mkdir -p "$CACHE"

SDL_SRC="$CACHE/SDL3-$SDL_VERSION"
if [ ! -d "$SDL_SRC" ]; then
	tarball="$CACHE/SDL3-$SDL_VERSION.tar.gz"
	curl -fsSL -o "$tarball" \
		"https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz"
	echo "$SDL_SHA256  $tarball" | sha256sum -c -
	tar xzf "$tarball" -C "$CACHE"
fi

# SDLActivity must be the Java half of the very SDL it loads
rm -rf "$CACHE/java"
mkdir -p "$CACHE/java"
cp -r "$SDL_SRC/android-project/app/src/main/java/org" "$CACHE/java/"

for abi in $ABIS; do
	case "$abi" in
	arm64-v8a) triple=aarch64-linux-android cpu_family=aarch64 cpu=armv8-a ;;
	x86_64) triple=x86_64-linux-android cpu_family=x86_64 cpu=x86_64 ;;
	*)
		echo "Unknown ABI '$abi' (expected arm64-v8a or x86_64)" >&2
		exit 1
		;;
	esac

	echo "=== $abi ==="
	work="$CACHE/$abi"
	mkdir -p "$work"

	cat > "$work/cross.ini" <<-EOF
		[binaries]
		c = '$BIN/$triple$API-clang'
		cpp = '$BIN/$triple$API-clang++'
		ar = '$BIN/llvm-ar'
		strip = '$BIN/llvm-strip'
		pkg-config = 'pkg-config'

		[host_machine]
		system = 'android'
		cpu_family = '$cpu_family'
		cpu = '$cpu'
		endian = 'little'
	EOF

	echo "--- SDL3"
	if [ ! -f "$work/sdl/lib/libSDL3.so" ]; then
		cmake -S "$SDL_SRC" -B "$work/sdl-build" -G Ninja \
			-DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
			-DANDROID_ABI="$abi" \
			-DANDROID_PLATFORM="android-$API" \
			-DCMAKE_BUILD_TYPE=Release \
			-DSDL_SHARED=ON \
			-DSDL_STATIC=OFF \
			-DSDL_TEST_LIBRARY=OFF \
			-DCMAKE_INSTALL_PREFIX="$work/sdl"
		cmake --build "$work/sdl-build"
		cmake --install "$work/sdl-build"
	fi

	echo "--- Quadrate"
	# Only math of the standard library is linked, and the others would want
	# OpenSSL and friends built for Android. Rebuilt every run, so a change in
	# the Quadrate tree is never shipped stale.
	meson setup "$work/quadrate" "$QUADRATE_DIR" \
		--cross-file "$work/cross.ini" \
		--buildtype=release \
		-Dbuild_tests=false \
		-Dbuild_tools=false \
		-Dbuild_compiler=false \
		-Dstdlib_modules=math \
		-Dwerror=false \
		--reconfigure 2>/dev/null >/dev/null || \
	meson setup "$work/quadrate" "$QUADRATE_DIR" \
		--cross-file "$work/cross.ini" \
		--buildtype=release \
		-Dbuild_tests=false \
		-Dbuild_tools=false \
		-Dbuild_compiler=false \
		-Dstdlib_modules=math \
		-Dwerror=false
	meson compile -C "$work/quadrate" interp qc math rt_static u8t

	# Staged under the names and layout qdos's meson.build links against.
	# Some are thin archives, which point into the build tree, so each is
	# repacked whole.
	stage="$work/quadrate-dist"
	rm -rf "$stage"
	mkdir -p "$stage/lib/quadrate" "$stage/include/quadrate"
	repack() {
		local archive="$1" name="$2"
		local members
		members=$("$BIN/llvm-ar" t "$archive")
		(cd "$(dirname "$archive")" && "$BIN/llvm-ar" rcs "$stage/lib/quadrate/lib$name.a" $members)
	}
	repack "$work/quadrate/lib/interp/libinterp.a" interp
	repack "$work/quadrate/lib/qc/libqc.a" qc
	repack "$work/quadrate/lib/rt/librt_static.a" rt
	repack "$work/quadrate/stdlib/math/libmath.a" math
	repack "$work/quadrate/subprojects/u8t/libu8t.a" u8t
	for mod in lib/interp lib/qc lib/rt stdlib/math; do
		cp -r "$QUADRATE_DIR/$mod/include/quadrate/$(basename "$mod")" "$stage/include/quadrate/"
	done

	echo "--- QDOS"
	PKG_CONFIG_LIBDIR="$work/sdl/lib/pkgconfig" meson setup "$work/qdos" "$QDOS_DIR" \
		--cross-file "$work/cross.ini" \
		--buildtype=release \
		-Dsim=true \
		-Ddevice=false \
		-Dquadrate_src="$QUADRATE_DIR" \
		-Dquadrate_dist="$stage" \
		--reconfigure 2>/dev/null >/dev/null || \
	PKG_CONFIG_LIBDIR="$work/sdl/lib/pkgconfig" meson setup "$work/qdos" "$QDOS_DIR" \
		--cross-file "$work/cross.ini" \
		--buildtype=release \
		-Dsim=true \
		-Ddevice=false \
		-Dquadrate_src="$QUADRATE_DIR" \
		-Dquadrate_dist="$stage"
	meson compile -C "$work/qdos" main

	libs="$CACHE/jniLibs/$abi"
	mkdir -p "$libs"
	cp "$work/sdl/lib/libSDL3.so" "$libs/"
	# Shipped rather than linked in: meson names -lc++ itself, and on the NDK
	# that is the shared one whatever the driver is told
	cp "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/$triple/libc++_shared.so" "$libs/"
	"$BIN/llvm-strip" -o "$libs/libmain.so" "$work/qdos/libmain.so"
done

# Only the ABIs asked for this time, or a stale one rides along in the APK
for dir in "$CACHE"/jniLibs/*/; do
	case " $ABIS " in
	*" $(basename "$dir") "*) ;;
	*) rm -rf "$dir" ;;
	esac
done

echo "=== APK ==="
cd "$QDOS_DIR/android"
task=assembleDebug
if [ "$TARGET" = install ]; then
	task=installDebug
fi
./gradlew --no-daemon "-PqdosNative=$CACHE" "$task"
