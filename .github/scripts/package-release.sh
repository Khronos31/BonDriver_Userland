#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
	echo "usage: package-release.sh <platform-name> <release-build-dir> <archive-format>" >&2
	exit 2
fi

platform="$1"
build_dir="$2"
format="$3"
version="${BONDRIVER_RELEASE_VERSION:-}"
if [[ ! "$version" =~ ^(v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?|ci-[0-9a-f]{12})$ ]]; then
	echo "BONDRIVER_RELEASE_VERSION is malformed" >&2
	exit 2
fi
case "$platform" in
	linux-glibc-x64|linux-glibc-arm64|linux-musl-x64|linux-musl-arm64|macos-arm64) ;;
	*) echo "unsupported package platform: $platform" >&2; exit 2 ;;
esac
if [[ "$format" != tar.gz ]]; then
	echo "unsupported archive format: $format" >&2
	exit 2
fi

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
stage="$repo_root/.ci-release-stage/$platform"
artifact_dir="$repo_root/artifacts"
mkdir -p "$stage/config" "$artifact_dir"
case "$platform" in
	macos-arm64) extension=dylib ;;
	*) extension=so ;;
esac

files=(
	"BonDriver_Siano.$extension"
	"BonDriver_PX4.$extension"
	"config/BonDriver_Siano.ini.example"
	"config/BonDriver_PX4.ini.example"
	"config/channels-ground.example.tsv"
	"config/channels-satellite.example.tsv"
	"config/channels-catv.example.tsv"
	"README.md"
	"docs/release-build.md"
	"LICENSE"
	"NOTICE"
	"BUILD-INFO.txt"
)
project_files=(
	"config/BonDriver_Siano.ini.example"
	"config/BonDriver_PX4.ini.example"
	"config/channels-ground.example.tsv"
	"config/channels-satellite.example.tsv"
	"config/channels-catv.example.tsv"
	"README.md"
	"docs/release-build.md"
	"LICENSE"
	"NOTICE"
)
for file in "${files[@]:0:2}"; do
	if [[ ! -f "$build_dir/$file" ]]; then
		echo "required release library missing: $build_dir/$file" >&2
		exit 1
	fi
done
for file in "${project_files[@]}"; do
	if [[ ! -f "$repo_root/$file" ]]; then
		echo "required release file missing: $file" >&2
		exit 1
	fi
done
copy_into_stage() {
	mkdir -p "$(dirname "$stage/$2")"
	cp "$1" "$stage/$2"
}
copy_into_stage "$build_dir/BonDriver_Siano.$extension" "BonDriver_Siano.$extension"
copy_into_stage "$build_dir/BonDriver_PX4.$extension" "BonDriver_PX4.$extension"
for file in "${project_files[@]}"; do
	copy_into_stage "$repo_root/$file" "$file"
done
{
	printf 'Platform: %s\n' "$platform"
	printf 'Package version: %s\n' "$version"
	printf 'Source revision: %s\n' "${GITHUB_SHA:-unknown}"
	printf 'Compiler: %s\n' "$(c++ --version | sed -n '1p')"
	if [[ "$platform" == linux-glibc-* ]]; then
		printf 'Libc: %s\n' "$(ldd --version 2>&1 | sed -n '1p')"
	elif [[ "$platform" == linux-musl-* ]]; then
		printf 'Libc: %s\n' "$(ldd --version 2>&1 | sed -n '1,2p' | tr '\n' ' ')"
	else
		printf 'System: macOS %s\n' "$(sw_vers -productVersion)"
		printf 'Deployment target: macOS 11.0\n'
	fi
} > "$stage/BUILD-INFO.txt"
if [[ "$platform" == linux-* ]]; then
	license_dir="$repo_root/licenses"
	if [[ ! -d "$license_dir" ]]; then
		echo "GNU runtime license directory missing" >&2
		exit 1
	fi
	license_count=0
	while IFS= read -r license; do
		relative="licenses/${license#"$license_dir"/}"
		files+=("$relative")
		copy_into_stage "$license" "$relative"
		license_count=$((license_count + 1))
	done < <(find "$license_dir" -type f | LC_ALL=C sort)
	if [[ "$license_count" -eq 0 ]]; then
		echo "GNU runtime license directory is empty" >&2
		exit 1
	fi
fi
hash_file() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | awk '{print $1}'
	else
		shasum -a 256 "$1" | awk '{print $1}'
	fi
}
(
	cd "$stage"
	: > SHA256SUMS
	for file in "${files[@]}"; do
		printf '%s  %s\n' "$(hash_file "$file")" "$file" >> SHA256SUMS
	done
)
archive="$artifact_dir/BonDriver_Userland-${version}-${platform}.tar.gz"
tar -czf "$archive" -C "$stage" .
echo "created $archive"
