#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
	echo "usage: build-linux.sh <glibc|musl> <x64|arm64> <release-version>" >&2
	exit 2
fi
libc="$1"
arch="$2"
version="$3"
case "$libc:$arch" in
	glibc:x64|glibc:arm64|musl:x64|musl:arm64) ;;
	*) echo "unsupported Linux target: $libc:$arch" >&2; exit 2 ;;
esac
export BONDRIVER_RELEASE_VERSION="$version"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
test_dir="$repo_root/.ci-build/test-$libc-$arch"
release_dir="$repo_root/.ci-build/release-$libc-$arch"
mkdir -p "$repo_root/.ci-build"

if [[ "$libc" == musl ]]; then
	if ! command -v apk >/dev/null 2>&1; then
		echo "musl job requires the pinned Alpine build image" >&2
		exit 1
	fi
	apk add --no-cache build-base cmake bash python3 binutils
fi

cmake -S "$repo_root" -B "$test_dir" \
	-DBUILD_TESTING=ON \
	-DBONDRIVER_WERROR=ON \
	-DBONDRIVER_STATIC_CXX_RUNTIME=ON \
	-DCMAKE_BUILD_TYPE=Release
cmake --build "$test_dir" --parallel 2
registered="$(ctest --test-dir "$test_dir" -N | awk '/Total Tests:/ { print $3 }')"
if [[ "$registered" != 39 ]]; then
	echo "expected 39 registered tests, found ${registered:-none}" >&2
	exit 1
fi
ctest --test-dir "$test_dir" --output-on-failure

cmake -S "$repo_root" -B "$release_dir" \
	-DBUILD_TESTING=OFF \
	-DBONDRIVER_STATIC_CXX_RUNTIME=ON \
	-DCMAKE_BUILD_TYPE=Release
cmake --build "$release_dir" --parallel 2

machine_pattern='Advanced Micro Devices X86-64'
if [[ "$arch" == arm64 ]]; then machine_pattern='AArch64'; fi
for lib in BonDriver_Siano.so BonDriver_PX4.so; do
	path="$release_dir/$lib"
	if ! readelf -h "$path" | grep -Eq "Machine:.*$machine_pattern"; then
		echo "unexpected ELF architecture in $path" >&2
		readelf -h "$path" >&2
		exit 1
	fi
	dynamic="$(readelf -d "$path")"
	if grep -Eq 'NEEDED.*(libstdc\+\+|libgcc_s)' <<< "$dynamic"; then
		echo "C++/unwind runtime remained dynamically linked in $path" >&2
		exit 1
	fi
	dynamic_deps="$(awk -F'[][]' '/NEEDED/ {print $2}' <<< "$dynamic")"
	while IFS= read -r dep; do
		[[ -z "$dep" ]] && continue
		case "$dep" in
			libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|ld-linux-x86-64.so.2|ld-linux-aarch64.so.1|libc.musl-x86_64.so.1|libc.musl-aarch64.so.1) ;;
			*) echo "unexpected dynamic dependency $dep in $path" >&2; exit 1 ;;
		esac
	done <<< "$dynamic_deps"
	if [[ "$libc" == glibc ]]; then
		python3 - "$path" <<'PY'
import re
import subprocess
import sys

text = subprocess.check_output(["readelf", "--version-info", sys.argv[1]], text=True)
if "GLIBC_PRIVATE" in text:
    raise SystemExit(f"forbidden GLIBC_PRIVATE reference in {sys.argv[1]}")
versions = [tuple(map(int, (v.split(".") + ["0"])[:3]))
            for v in re.findall(r"GLIBC_(\d+(?:\.\d+){1,2})", text)]
if not versions:
    raise SystemExit(f"no GLIBC symbol-version requirements found in {sys.argv[1]}")
maximum = max(versions)
if maximum > (2, 28, 0):
    raise SystemExit(f"GLIBC symbol floor exceeds 2.28: {maximum} in {sys.argv[1]}")
print(f"GLIBC symbol floor ok: {maximum} in {sys.argv[1]}")
PY
	else
		if readelf --version-info "$path" | grep -q 'GLIBC_'; then
			echo "glibc symbol-version import in musl artifact $path" >&2
			exit 1
		fi
	fi
done

excluded='driver_(siano|px4)_(factory_failure|thread_start_failure|thread_start_cleanup_failure|cleanup_failure)'
registered="$(ctest --test-dir "$test_dir" -N --exclude-regex "$excluded" | awk '/Total Tests:/ { print $3 }')"
if [[ "$registered" != 31 ]]; then
	echo "expected 31 release-DSO tests, found ${registered:-none}" >&2
	exit 1
fi
cp "$release_dir/BonDriver_Siano.so" "$test_dir/BonDriver_Siano.so"
cp "$release_dir/BonDriver_PX4.so" "$test_dir/BonDriver_PX4.so"
ctest --test-dir "$test_dir" --output-on-failure --exclude-regex "$excluded"
bash "$repo_root/.github/scripts/package-release.sh" "linux-$libc-$arch" "$release_dir" tar.gz
