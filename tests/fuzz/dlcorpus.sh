#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <fuzz-binary-directory>" >&2
    exit 1
fi

source_dir=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
binary_dir=$(CDPATH= cd -- "$1" && pwd)
cd "$source_dir"
for source in fuzz_emu_*.c; do
    target=${source%.c}
    archive=${target}.zip

    if [ ! -x "$binary_dir/$target" ]; then
        continue
    fi

    wget -O "$archive" \
        "https://storage.googleapis.com/unicorn-backup.clusterfuzz-external.appspot.com/corpus/libFuzzer/unicorn_${target}/public.zip"
    unzip -qo "$archive" -d "corpus_${target}"
    rm -f "$archive"
    "$binary_dir/$target" "$source_dir/corpus_${target}"
done
