#!/usr/bin/env bash
set -euo pipefail

tag="${GITHUB_REF_NAME:-}"
repo="${GITHUB_REPOSITORY:-}"
expected_sha="${GITHUB_SHA:-}"
artifact_dir="${GITHUB_WORKSPACE:-}/release-assets"
if [[ ! "$tag" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$ ]]; then
	echo "refusing to publish a non-semantic release tag" >&2
	exit 1
fi
if [[ ! "$expected_sha" =~ ^[0-9a-fA-F]{40}$ || -z "$repo" ]]; then
	echo "release context is incomplete" >&2
	exit 1
fi

tag_sha="$(git rev-parse "$tag^{commit}")"
head_sha="$(git rev-parse HEAD)"
if [[ "$tag_sha" != "$expected_sha" || "$head_sha" != "$expected_sha" ]]; then
	echo "tag $tag / checkout resolve to $tag_sha / $head_sha, expected $expected_sha" >&2
	exit 1
fi
if gh release view "$tag" --repo "$repo" >/dev/null 2>&1; then
	echo "release $tag already exists; refusing to replace immutable release assets" >&2
	exit 1
fi

expected=(
	"BonDriver_Userland-$tag-linux-glibc-x64.tar.gz"
	"BonDriver_Userland-$tag-linux-glibc-arm64.tar.gz"
	"BonDriver_Userland-$tag-linux-musl-x64.tar.gz"
	"BonDriver_Userland-$tag-linux-musl-arm64.tar.gz"
	"BonDriver_Userland-$tag-windows-x64.zip"
	"BonDriver_Userland-$tag-macos-arm64.tar.gz"
)
for file in "${expected[@]}"; do
	if [[ ! -f "$artifact_dir/$file" ]]; then
		echo "missing release artifact: $file" >&2
		exit 1
	fi
done
archive_count="$(find "$artifact_dir" -maxdepth 1 -type f \( -name '*.tar.gz' -o -name '*.zip' \) | wc -l | tr -d ' ')"
if [[ "$archive_count" != 6 ]]; then
	echo "expected exactly six platform archives, found $archive_count" >&2
	exit 1
fi
file_count="$(find "$artifact_dir" -maxdepth 1 -type f | wc -l | tr -d ' ')"
if [[ "$file_count" != 6 ]]; then
	echo "expected exactly six files before aggregate checksum creation, found $file_count" >&2
	exit 1
fi
verify_dir="$(mktemp -d "${RUNNER_TEMP:-/tmp}/bondriver-release-verify.XXXXXX")"
trap 'rm -rf "$verify_dir"' EXIT
for file in "${expected[@]}"; do
	destination="$verify_dir/${file%.*}"
	mkdir -p "$destination"
	case "$file" in
		*.tar.gz) tar -xzf "$artifact_dir/$file" -C "$destination" ;;
		*.zip) unzip -q "$artifact_dir/$file" -d "$destination" ;;
		*) echo "unexpected archive extension: $file" >&2; exit 1 ;;
	esac
	(
		cd "$destination"
		sha256sum -c SHA256SUMS
	)
done
(
	cd "$artifact_dir"
	sha256sum "${expected[@]}" > SHA256SUMS
)

release_flags=()
if [[ "${tag%%+*}" == *-* ]]; then
	release_flags+=(--prerelease)
fi
gh release create "$tag" "$artifact_dir"/* \
	--repo "$repo" \
	--verify-tag \
	--draft \
	--title "$tag" \
	--notes "Automated BonDriver_Userland release; see the included README and license files." \
	"${release_flags[@]}"
gh release edit "$tag" --repo "$repo" --draft=false
