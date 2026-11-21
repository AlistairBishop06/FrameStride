#!/usr/bin/env bash
set -euo pipefail

download() {
    curl --fail --silent --show-error --location --retry 3 --proto '=https' --proto-redir '=https' --output "$2" "$1"
}

main() {
    if [[ $# -gt 1 || ! ${1:-latest} =~ ^(latest|v[0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
        echo 'Usage: bash install-framestride.sh [latest|vMAJOR.MINOR.PATCH]' >&2
        return 2
    fi
    if [[ $(uname -s) != Linux || $(uname -m) != aarch64 || $(id -u) == 0 ]]; then
        echo 'Run this in Konsole on the Steam Frame as steamos, without sudo.' >&2
        return 1
    fi

    local base=https://github.com/AlistairBishop06/FrameStride/releases
    local requested=${1:-latest} source filename= version= digest= line root member
    local pattern='^([0-9a-fA-F]{64})  (framestride-([0-9]+\.[0-9]+\.[0-9]+)-linux-aarch64\.tar\.gz)$'
    if [[ $requested == latest ]]; then source=$base/latest/download; else source=$base/download/$requested; fi
    scratch=$(mktemp -d)
    trap 'rm -rf -- "$scratch"' EXIT

    echo "Finding the $requested FrameStride release on GitHub..."
    download "$source/SHA256SUMS" "$scratch/SHA256SUMS" || return 1
    while IFS= read -r line || [[ -n $line ]]; do
        if [[ $line =~ $pattern ]]; then
            if [[ -n $filename ]]; then echo 'Release has more than one ARM64 archive checksum.' >&2; return 1; fi
            digest=${BASH_REMATCH[1]}
            filename=${BASH_REMATCH[2]}
            version=${BASH_REMATCH[3]}
        fi
    done < "$scratch/SHA256SUMS"
    if [[ -z $filename || ( $requested != latest && $requested != v"$version" ) ]]; then
        echo 'Release checksum file does not describe the requested FrameStride ARM64 archive.' >&2
        return 1
    fi

    # Resolve latest once, then fetch the archive from the version in the checksum file.
    echo "Downloading FrameStride $version for ARM64..."
    download "$base/download/v$version/$filename" "$scratch/$filename" || return 1
    if ! (cd "$scratch" && printf '%s  %s\n' "$digest" "$filename" | sha256sum --check --status); then
        echo 'FrameStride release checksum failed.' >&2
        return 1
    fi

    root=${filename%.tar.gz}
    tar -tzf "$scratch/$filename" > "$scratch/members"
    while IFS= read -r member; do
        if [[ ( $member != "$root" && $member != "$root/"* ) || /$member/ == */../* ]]; then
            echo 'Unexpected path in the FrameStride release archive.' >&2
            return 1
        fi
    done < "$scratch/members"
    tar -tvzf "$scratch/$filename" > "$scratch/types"
    if grep -q '^[^-d]' "$scratch/types"; then
        echo 'FrameStride archive contains entries other than files and directories.' >&2
        return 1
    fi
    tar -xzf "$scratch/$filename" -C "$scratch"
    bash "$scratch/$root/install.sh"
}

main "$@"
