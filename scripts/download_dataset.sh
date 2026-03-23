#!/bin/bash

set -euo pipefail

mkdir -p data

download_file() {
    local output_path="$1"
    local url="$2"

    if [ -f "$output_path" ]; then
        echo "Found $(basename "$output_path"), skipping download."
        return
    fi

    echo "Downloading $(basename "$output_path")..."
    if command -v wget >/dev/null 2>&1; then
        wget -O "$output_path" "$url"
    elif command -v curl >/dev/null 2>&1; then
        curl -L "$url" -o "$output_path"
    else
        echo "Error: neither wget nor curl is available."
        exit 1
    fi
    echo "Finished downloading $(basename "$output_path")."
}

download_file "data/fb_100M_public_uint64" \
    "https://www.dropbox.com/scl/fi/hngvfbz1a2tkwpebjngb9/fb_100M_public_uint64?rlkey=px31l6wj9tnic4z604bt6s55n&st=d3iuhhgx&dl=0"
download_file "data/books_100M_public_uint64" \
    "https://www.dropbox.com/scl/fi/q9zg3shi16xduo7mis3t8/books_100M_public_uint64?rlkey=f6bhaibqsugmo2yo2l4ir3v2r&st=tin2fnoj&dl=0"
download_file "data/osmc_100M_public_uint64" \
    "https://www.dropbox.com/scl/fi/mamy2obtasfm898l0lrj1/osmc_100M_public_uint64?rlkey=vlzl311wa1q0cwsnc0nfow1qc&st=b30vck9r&dl=0"

echo "All Task 1 datasets are available in data/."

