#!/usr/bin/env bash
# Keep the same files, permissions and links when moving build inputs between jobs.
set -euo pipefail

die() {
    printf '%s\n' "$*" >&2
    exit 1
}

operation=${1-}
archive=${2-}
[[ -n $archive ]] || die 'Specify the build input archive.'
workspace=$(pwd -P)
archive=$(realpath -m -- "$archive")
if [[ $archive == "$workspace" || $archive == "$workspace/"* ]]; then
    die 'Put the build input archive outside the workspace.'
fi

case "$operation" in
    upload)
        selected=()
        while IFS= read -r path; do
            [[ -n $path ]] || continue
            case "/$path/" in
                *//* | */./* | */../* | */.git/*)
                    die "Invalid build input path: $path"
                    ;;
            esac
            [[ -e $path || -L $path ]] || die "Build input path does not exist: $path"
            parent=$(dirname -- "$path")
            if [[ $parent != . && $(realpath -e -- "$parent") != "$workspace/$parent" ]]; then
                die "Build input path must not traverse a symbolic link: $path"
            fi
            selected+=("$path")
        done <<< "${3-}"
        ((${#selected[@]})) || die 'Uploading a build input requires paths.'
        tar --use-compress-program='zstd -T2 -3' --create --file="$archive" \
            --exclude=.git -- "${selected[@]}"
        ;;
    download)
        [[ -f $archive ]] || die 'The downloaded build input archive is missing.'
        tar --extract --zstd --file="$archive" --same-permissions --no-same-owner
        ;;
    *)
        die 'Build input operation must be upload or download.'
        ;;
esac
