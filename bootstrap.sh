#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
checkout() {
  local path="$1" url="$2" revision="$3"
  if [[ ! -d "$path/.git" ]]; then git clone --no-checkout --filter=blob:none "$url" "$path"; fi
  git -C "$path" remote set-url origin "$url"
  git -C "$path" fetch --depth 1 origin "$revision"
  git -C "$path" checkout --detach "$revision"
}
checkout deps/s2sdk https://github.com/zer0k-z/s2sdk.git 9c6974a8b507cada425a395fe9a4125bf4be6fe7
checkout deps/metamod-source https://github.com/alliedmodders/metamod-source.git 170058a2f0dbd58106b7c76bfe88a94672295356
git -C deps/metamod-source submodule update --init --depth 1 third_party/khook
checkout deps/zstd https://github.com/facebook/zstd.git f8745da6ff1ad1e7bab384bd1f9d742439278e99
