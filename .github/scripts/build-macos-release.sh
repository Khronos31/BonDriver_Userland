#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
	echo "usage: build-macos-release.sh <release-version>" >&2
	exit 2
fi
export BONDRIVER_RELEASE_VERSION="$1"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
test_dir="$repo_root/.ci-build/macos-test"
release_dir="$repo_root/.ci-build/macos-release"

cmake -S "$repo_root" -B "$test_dir" \
	-DBUILD_TESTING=ON \
	-DBONDRIVER_WERROR=ON \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build "$test_dir" --parallel 2
registered="$(ctest --test-dir "$test_dir" -N | awk '/Total Tests:/ { print $3 }')"
if [[ "$registered" != 39 ]]; then
	echo "expected 39 registered tests, found ${registered:-none}" >&2
	exit 1
fi
ctest --test-dir "$test_dir" --output-on-failure

cmake -S "$repo_root" -B "$release_dir" \
	-DBUILD_TESTING=OFF \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build "$release_dir" --parallel 2

for lib in BonDriver_Siano.dylib BonDriver_PX4.dylib; do
	path="$release_dir/$lib"
	if [[ "$(lipo -archs "$path")" != arm64 ]]; then
		echo "unexpected architecture in $path" >&2
		exit 1
	fi
	minos="$(otool -l "$path" | awk '
		$1 == "cmd" { build=($2 == "LC_BUILD_VERSION"); legacy=($2 == "LC_VERSION_MIN_MACOSX"); next }
		build && $1 == "minos" { print $2; exit }
		legacy && $1 == "version" { print $2; exit }
	')"
	if [[ "$minos" != 11.0 && "$minos" != 11.0.0 ]]; then
		echo "expected macOS deployment target 11.0 in $path, got ${minos:-unknown}" >&2
		exit 1
	fi
	self_id="$(otool -D "$path" | sed -n '2p')"
	while IFS= read -r dependency; do
		[[ -z "$dependency" ]] && continue
		[[ "$dependency" == "$self_id" ]] && continue
		case "$dependency" in
			/usr/lib/libc++.1.dylib|/usr/lib/libSystem.B.dylib) ;;
			*) echo "non-system dependency $dependency in $path" >&2; exit 1 ;;
		esac
	done < <(otool -L "$path" | tail -n +2 | sed -E 's/^[[:space:]]*//; s/ \(compatibility.*$//')
done

excluded='driver_(siano|px4)_(factory_failure|thread_start_failure|thread_start_cleanup_failure|cleanup_failure)'
registered="$(ctest --test-dir "$test_dir" -N --exclude-regex "$excluded" | awk '/Total Tests:/ { print $3 }')"
if [[ "$registered" != 31 ]]; then
	echo "expected 31 release-DSO tests, found ${registered:-none}" >&2
	exit 1
fi
cp "$release_dir/BonDriver_Siano.dylib" "$test_dir/BonDriver_Siano.dylib"
cp "$release_dir/BonDriver_PX4.dylib" "$test_dir/BonDriver_PX4.dylib"
ctest --test-dir "$test_dir" --output-on-failure --exclude-regex "$excluded"
bash "$repo_root/.github/scripts/package-release.sh" macos-arm64 "$release_dir" tar.gz
