#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 <fuzz-binary-directory> [corpus-directory] [--libfuzzer]" >&2
    exit 1
fi

libfuzzer=0
if [ "$#" -eq 3 ]; then
    if [ "$3" != "--libfuzzer" ]; then
        echo "usage: $0 <fuzz-binary-directory> [corpus-directory] [--libfuzzer]" >&2
        exit 1
    fi
    libfuzzer=1
fi

fuzz_runs=${UNICORN_FUZZ_RUNS:-0}
fuzz_timeout=${UNICORN_FUZZ_TIMEOUT:-5}
if [ "$libfuzzer" -eq 1 ]; then
    case "$fuzz_runs" in
        ''|*[!0-9]*)
            echo "UNICORN_FUZZ_RUNS must be a non-negative integer" >&2
            exit 1
            ;;
    esac
    case "$fuzz_timeout" in
        ''|*[!0-9]*|0*)
            echo "UNICORN_FUZZ_TIMEOUT must be a positive integer" >&2
            exit 1
            ;;
    esac
fi

source_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
binary_dir=$(CDPATH= cd -- "$1" && pwd)
manifest="$source_dir/corpus/external-corpora.tsv"
corpus_dir=${2:-"$binary_dir/corpus"}
case "$corpus_dir" in
    /*) ;;
    *) corpus_dir="$PWD/$corpus_dir" ;;
esac
mkdir -p "$corpus_dir"

sha256_file()
{
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        shasum -a 256 "$1" | awk '{print $1}'
    fi
}

download()
{
    if command -v curl >/dev/null 2>&1; then
        curl --fail --location --show-error --silent \
            --retry 5 --retry-delay 2 --output "$2" "$1"
    else
        wget --quiet --tries=5 --output-document="$2" "$1"
    fi
}

run_target()
{
    if [ "$libfuzzer" -eq 1 ]; then
        "$1" "-runs=$fuzz_runs" "-timeout=$fuzz_timeout" "$2"
    else
        "$1" "$2"
    fi
}

while read -r target expected_hash url; do
    case "$target" in
        ''|'#'*) continue ;;
    esac

    if [ ! -x "$binary_dir/$target" ]; then
        continue
    fi

    if [ "$expected_hash" = "-" ] || [ "$url" = "-" ]; then
        echo "$target: no external corpus is published; using local seeds"
        continue
    fi

    archive=$(mktemp "${TMPDIR:-/tmp}/unicorn-corpus.XXXXXX")
    download "$url" "$archive"
    actual_hash=$(sha256_file "$archive")
    if [ "$actual_hash" != "$expected_hash" ]; then
        rm -f "$archive"
        echo "$target: corpus SHA-256 mismatch" >&2
        exit 1
    fi

    target_corpus="$corpus_dir/$target"
    rm -rf "$target_corpus"
    mkdir -p "$target_corpus"
    unzip -qo "$archive" -d "$target_corpus"
    rm -f "$archive"
    run_target "$binary_dir/$target" "$target_corpus"
done < "$manifest"

for binary in "$binary_dir"/fuzz_emu_*; do
    if [ -x "$binary" ]; then
        run_target "$binary" "$source_dir/corpus/raw"
    fi
done

if [ -x "$binary_dir/fuzz_uc_api" ]; then
    run_target "$binary_dir/fuzz_uc_api" "$source_dir/corpus/fuzz_uc_api"
fi
