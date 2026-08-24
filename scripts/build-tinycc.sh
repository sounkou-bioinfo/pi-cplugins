#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
receipt="$root/vendor/tinycc.json"
build_root="$root/.deps/tinycc"
prefix="$build_root/prefix"
source_dir="$build_root/source"
archive="$build_root/tinycc.tar.gz"

commit=$(node -e 'const r=require(process.argv[1]); process.stdout.write(r.commit)' "$receipt")
url=$(node -e 'const r=require(process.argv[1]); process.stdout.write(r.archive_url)' "$receipt")
sha256=$(node -e 'const r=require(process.argv[1]); process.stdout.write(r.sha256)' "$receipt")
build_id="$commit-fpic"
stamp="$prefix/.pi-cplugins-tinycc-build"

if [[ -f "$stamp" ]] && [[ $(<"$stamp") == "$build_id" ]] &&
   [[ -f "$prefix/include/libtcc.h" ]] && [[ -f "$prefix/lib/libtcc.a" ]]; then
  exit 0
fi

rm -rf "$source_dir" "$prefix"
mkdir -p "$build_root" "$source_dir" "$prefix"

archive_matches() {
  local actual
  if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$archive" | cut -d' ' -f1)
  else
    actual=$(shasum -a 256 "$archive" | cut -d' ' -f1)
  fi
  [[ "$actual" == "$sha256" ]]
}

if [[ ! -f "$archive" ]] || ! archive_matches; then
  curl --fail --location --retry 3 --output "$archive" "$url"
fi
archive_matches

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || printf '2')
tar -xzf "$archive" --strip-components=1 -C "$source_dir"
(
  cd "$source_dir"
  export GIT_CEILING_DIRECTORIES="$root"
  ./configure --prefix="$prefix" --extra-cflags=-fPIC
  make -j"$jobs"
  make install
)
printf '%s\n' "$build_id" > "$stamp"
