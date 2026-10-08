#!/usr/bin/env bash
set -euo pipefail

if [[ "${GITHUB_REF:-}" == refs/tags/* ]]; then
	version="${GITHUB_REF_NAME:-}"
	if [[ ! "$version" =~ ^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$ ]]; then
		echo "release tags must use semantic vMAJOR.MINOR.PATCH form" >&2
		exit 1
	fi
else
	sha="${GITHUB_SHA:-}"
	if [[ ! "$sha" =~ ^[0-9a-fA-F]{40}$ ]]; then
		echo "GITHUB_SHA is missing or malformed" >&2
		exit 1
	fi
	version="ci-${sha:0:12}"
fi

printf 'version=%s\n' "$version" >> "${GITHUB_OUTPUT:?GITHUB_OUTPUT is required}"
