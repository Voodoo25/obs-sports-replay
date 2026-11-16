#!/usr/bin/env bash
# Builds the FFmpeg that Sports Replay links statically into its own binary.
#
# OBS ships FFmpeg as shared libraries whose names carry the major version
# (avcodec-61 in OBS up to 32.1, avcodec-62 from 32.2 on). Linking against
# them tied each plugin build to a range of OBS versions. With a private,
# static FFmpeg the plugin no longer depends on the one OBS ships, so a single
# build loads in every OBS version that shares the libobs ABI.
#
# The build is trimmed to what the plugin uses: H.264 encoders (NVENC, AMF,
# x264), decoders and containers for replays and sponsor clips, the mp4 muxer
# and swscale. Versions match obs-deps 2026-07-15, the set OBS 32.2 ships.
#
# Usage: build.sh <prefix> [arch]
#   Windows: run from an MSYS2 bash that inherits a Visual Studio x64
#            developer environment (see build-windows.ps1).
#   macOS:   arch is arm64 or x86_64; build both and lipo them.
#   Linux:   native build.

set -euo pipefail

PREFIX=${1:?usage: build.sh <prefix> [arch]}
ARCH=${2:-}

FFMPEG_COMMIT=38b88335f99e76ed89ff3c93f877fdefce736c13  # n8.1.2
X264_COMMIT=eaa68fad9e5d201d42fde51665f2d137ae96baf0    # r3106
NVCODEC_COMMIT=e844e5b26f46bb77479f063029595293aa8f812d # n13.0.19.0
AMF_COMMIT=afed28d37aca1938da2eedc50599bb3535a987ec     # v1.5.0

HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$PREFIX"
PREFIX=$(cd "$PREFIX" && pwd)
# x264's and FFmpeg's makefiles do not quote their install paths
if [[ $PREFIX == *" "* ]]; then
	echo "error: the prefix cannot contain spaces: $PREFIX" >&2
	exit 1
fi
WORK="$PREFIX/.work"
mkdir -p "$WORK" "$PREFIX/include" "$PREFIX/lib/pkgconfig"

case "$(uname -s)" in
MINGW* | MSYS*) PLATFORM=windows ;;
Darwin) PLATFORM=macos ;;
*) PLATFORM=linux ;;
esac

JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

# fetch <dir> <repo> <commit>: a shallow checkout of exactly that commit
fetch() {
	local dir="$WORK/$1"
	if [[ -f "$dir/.sr-commit" && "$(cat "$dir/.sr-commit")" == "$3" ]]; then
		return
	fi
	rm -rf "$dir"
	mkdir -p "$dir"
	git -C "$dir" init -q
	git -C "$dir" -c advice.detachedHead=false fetch -q --depth 1 "$2" "$3"
	git -C "$dir" -c advice.detachedHead=false checkout -q FETCH_HEAD
	echo "$3" >"$dir/.sr-commit"
}

# --- toolchain -------------------------------------------------------------

EXTRA_CFLAGS=""
EXTRA_LDFLAGS=""
FFMPEG_TOOLCHAIN=()
X264_HOST=()

case "$PLATFORM" in
windows)
	# MSYS2's coreutils 'link' would shadow MSVC's linker
	MSVC_BIN=$(dirname "$(command -v cl)")
	export PATH="$MSVC_BIN:$PATH"
	export CC=cl
	X264_HOST=(--host=x86_64-mingw64)
	FFMPEG_TOOLCHAIN=(--toolchain=msvc --arch=x86_64 --enable-w32threads)
	EXTRA_CFLAGS="-MD -D_WIN32_WINNT=0x0A00"
	# MSVC's linker takes -libpath: with a Windows path, not -L
	EXTRA_LDFLAGS="-libpath:$(cygpath -m "$PREFIX/lib")"
	# MSYS2 may lack pkg-config; FFmpeg's configure only needs it for the
	# few .pc files installed here, which the bundled shim reads. It is
	# copied next to them because configure cannot run a path with spaces.
	if ! command -v pkg-config >/dev/null; then
		mkdir -p "$PREFIX/bin"
		cp -f "$HERE/pkg-config-shim.sh" "$PREFIX/bin/pkg-config"
		chmod +x "$PREFIX/bin/pkg-config"
		export PKG_CONFIG="$PREFIX/bin/pkg-config"
	fi
	;;
macos)
	ARCH=${ARCH:-$(uname -m)}
	export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-12.0}
	EXTRA_CFLAGS="-arch $ARCH -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
	EXTRA_LDFLAGS="$EXTRA_CFLAGS"
	X264_HOST=(--host="${ARCH/arm64/aarch64}-apple-darwin")
	FFMPEG_TOOLCHAIN=(--arch="$ARCH" --enable-cross-compile --target-os=darwin --cc=clang)
	;;
linux)
	# -fPIC: these archives end up inside a shared object
	EXTRA_CFLAGS="-fPIC"
	FFMPEG_TOOLCHAIN=(--enable-pic --enable-pthreads)
	;;
esac

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
PKG_CONFIG=${PKG_CONFIG:-pkg-config}

# the prefix as the compiler sees it (a Windows path for MSVC)
PREFIX_NATIVE=$PREFIX
[[ $PLATFORM == windows ]] && PREFIX_NATIVE=$(cygpath -m "$PREFIX")
[[ $PLATFORM != windows ]] && EXTRA_LDFLAGS="$EXTRA_LDFLAGS -L$PREFIX/lib"

# --- x264 ------------------------------------------------------------------

fetch x264 https://github.com/mirror/x264.git "$X264_COMMIT"
(
	cd "$WORK/x264"
	rm -rf build && mkdir build && cd build
	CFLAGS="$EXTRA_CFLAGS" LDFLAGS="$EXTRA_LDFLAGS" ../configure ${X264_HOST[@]+"${X264_HOST[@]}"} \
		--prefix="$PREFIX" --enable-static --enable-pic --disable-cli \
		--disable-lsmash --disable-avs --disable-gpac --disable-ffms --disable-swscale \
		--disable-interlaced --bit-depth=8 --chroma-format=420
	make -j"$JOBS"
	make install-lib-static
)
# the MSVC build installs libx264.lib; FFmpeg's msvc toolchain links -lx264
# as x264.lib
if [[ $PLATFORM == windows && -f "$PREFIX/lib/libx264.lib" ]]; then
	cp -f "$PREFIX/lib/libx264.lib" "$PREFIX/lib/x264.lib"
fi

# --- NVENC and AMF headers (both load the driver at run time) ---------------

if [[ $PLATFORM != macos ]]; then
	fetch nv-codec-headers https://github.com/FFmpeg/nv-codec-headers.git "$NVCODEC_COMMIT"
	make -C "$WORK/nv-codec-headers" PREFIX="$PREFIX" install >/dev/null

	if [[ ! -f "$WORK/amf/.sr-commit" || "$(cat "$WORK/amf/.sr-commit")" != "$AMF_COMMIT" ]]; then
		rm -rf "$WORK/amf"
		git clone -q --filter=blob:none --no-checkout --sparse \
			https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git "$WORK/amf"
		git -C "$WORK/amf" sparse-checkout set amf/public/include
		git -C "$WORK/amf" -c advice.detachedHead=false checkout -q "$AMF_COMMIT"
		echo "$AMF_COMMIT" >"$WORK/amf/.sr-commit"
	fi
	rm -rf "$PREFIX/include/AMF"
	cp -R "$WORK/amf/amf/public/include" "$PREFIX/include/AMF"
fi

# --- FFmpeg ----------------------------------------------------------------

HW_ENCODERS=()
if [[ $PLATFORM != macos ]]; then
	HW_ENCODERS=(--enable-ffnvcodec --enable-nvenc --enable-amf
		--enable-encoder=h264_nvenc --enable-encoder=h264_amf)
fi

fetch ffmpeg https://github.com/FFmpeg/FFmpeg.git "$FFMPEG_COMMIT"
(
	cd "$WORK/ffmpeg"
	if [[ $PLATFORM == windows ]]; then
		# configure recognizes MSVC by a banner starting with "Microsoft";
		# a localized Visual Studio without the English language pack
		# prints it mid-line ("Compilador ... de Microsoft (R)")
		sed -i 's/grep -q ^Microsoft/grep -q Microsoft/; s/grep ^Microsoft/grep Microsoft/' configure
	fi
	rm -rf build && mkdir build && cd build
	../configure \
		--prefix="$PREFIX" \
		${FFMPEG_TOOLCHAIN[@]+"${FFMPEG_TOOLCHAIN[@]}"} \
		--pkg-config="$PKG_CONFIG" \
		--extra-cflags="$EXTRA_CFLAGS -I$PREFIX_NATIVE/include" \
		--extra-ldflags="$EXTRA_LDFLAGS" \
		--enable-static --disable-shared \
		--enable-gpl --enable-version3 \
		--disable-programs --disable-doc --disable-debug \
		--disable-autodetect --disable-network \
		--disable-everything \
		--disable-avdevice --disable-avfilter --disable-swresample \
		--enable-avcodec --enable-avformat --enable-avutil --enable-swscale \
		--enable-libx264 --enable-encoder=libx264 \
		${HW_ENCODERS[@]+"${HW_ENCODERS[@]}"} \
		--enable-decoder=h264,hevc,vp8,vp9,mpeg4,mpeg2video,mpeg1video,h263,flv,prores,mjpeg \
		--enable-parser=h264,hevc,vp8,vp9,av1,mpeg4video,mpegvideo,h263,mjpeg \
		--enable-demuxer=mov,matroska,avi,flv,h264 \
		--enable-muxer=mp4,mov \
		--enable-protocol=file \
		--enable-bsf=extract_extradata,h264_mp4toannexb,vp9_superframe_split
	# (the av1 parser is not only for clips: it fills the coded-bitstream
	# type table the mov muxer compiles, which MSVC fails on when empty)
	make -j"$JOBS"
	make install
)

echo "FFmpeg installed to $PREFIX"
