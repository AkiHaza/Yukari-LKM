#!/bin/sh
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out_dir=${OUT_DIR:-"$repo_dir/out"}
kernel_workspace=${KERNEL_WORKSPACE:-}
kernel_build_label=${KERNEL_BUILD_LABEL:-}
bazel=${BAZEL:-}
kernel_headers_label=${KERNEL_HEADERS_LABEL:-//common:all_headers}
ndk=${ANDROID_NDK_HOME:-}
magisk_include=${MAGISK_INCLUDE:-}
abis=${ABIS:-arm64-v8a}

if [ -z "$kernel_workspace" ] || { [ ! -f "$kernel_workspace/MODULE.bazel" ] && [ ! -f "$kernel_workspace/WORKSPACE" ] && [ ! -f "$kernel_workspace/WORKSPACE.bazel" ] && [ ! -f "$kernel_workspace/bazel.WORKSPACE" ]; }; then
  echo "KERNEL_WORKSPACE must be an Android 14/GKI 6.1 Kleaf workspace" >&2
  exit 2
fi
if [ -z "$kernel_build_label" ]; then
  echo "KERNEL_BUILD_LABEL must name the matching kernel_build target (for example //common:kernel_aarch64)" >&2
  exit 2
fi
if [ -z "$ndk" ] || [ ! -d "$ndk" ]; then
  echo "ANDROID_NDK_HOME must point to an Android NDK" >&2
  exit 2
fi
if [ -z "$magisk_include" ] || [ ! -f "$magisk_include/zygisk.hpp" ]; then
  echo "MAGISK_INCLUDE must contain zygisk.hpp; refusing a stub Zygisk build" >&2
  exit 2
fi

kernel_workspace=$(CDPATH= cd -- "$kernel_workspace" && pwd)
case "$out_dir" in
  /*) ;;
  *) out_parent=$(CDPATH= cd -- "$(dirname -- "$out_dir")" && pwd) || {
       echo "OUT_DIR parent directory does not exist" >&2
       exit 2
     }
     out_dir="$out_parent/$(basename -- "$out_dir")"
     ;;
esac

mkdir -p "$out_dir/ko"

case "$kernel_build_label" in
  //[A-Za-z0-9_./:-]*) ;;
  *) echo "KERNEL_BUILD_LABEL must be a Bazel label beginning with //" >&2; exit 2 ;;
esac
invalid_label=$(printf '%s' "$kernel_build_label" | tr -d 'A-Za-z0-9_./:-')
if [ -n "$invalid_label" ]; then
  echo "KERNEL_BUILD_LABEL contains unsupported characters" >&2
  exit 2
fi
invalid_headers=$(printf '%s' "$kernel_headers_label" | tr -d 'A-Za-z0-9_./:-')
case "$kernel_headers_label" in
  //[A-Za-z0-9_./:-]*) ;;
  *) echo "KERNEL_HEADERS_LABEL must be a Bazel label beginning with //" >&2; exit 2 ;;
esac
if [ -n "$invalid_headers" ]; then
  echo "KERNEL_HEADERS_LABEL contains unsupported characters" >&2
  exit 2
fi

if [ -z "$bazel" ]; then
  if [ -x "$kernel_workspace/tools/bazel" ]; then
    bazel="$kernel_workspace/tools/bazel"
  else
    bazel=$(command -v bazel || true)
  fi
fi
if [ -z "$bazel" ]; then
  echo "BAZEL is unset and KERNEL_WORKSPACE/tools/bazel is unavailable" >&2
  exit 2
fi

stage_name="yukari_lkm_$$"
stage_dir="$kernel_workspace/$stage_name"
if [ -e "$stage_dir" ]; then
  echo "Refusing to overwrite existing staging directory: $stage_dir" >&2
  exit 2
fi
mkdir "$stage_dir"
trap 'rm -rf "$stage_dir"' EXIT HUP INT TERM
cp "$repo_dir/kernel/yukari_filter.c" "$stage_dir/yukari_filter.c"
cp "$repo_dir/kernel/Kconfig" "$stage_dir/Kconfig"
sed "s|kernel_build = \"//common:kernel_aarch64\"|kernel_build = \"$kernel_build_label\"|" \
  "$repo_dir/kernel/BUILD.bazel" | sed "s|\"//common:all_headers\"|\"$kernel_headers_label\"|" > "$stage_dir/BUILD.bazel"

# kernel/BUILD.bazel is evaluated inside the matching Kleaf workspace. The
# kernel_build label is supplied as a make variable so the module cannot be
# accidentally built against an unrelated kernel configuration.
(
  cd "$kernel_workspace"
  "$bazel" --output_user_root="${BAZEL_OUTPUT_USER_ROOT:-$out_dir/bazel}" \
  build --noshow_progress \
  "//$stage_name:yukari_filter"
)

execution_root=$(
  cd "$kernel_workspace"
  "$bazel" --output_user_root="${BAZEL_OUTPUT_USER_ROOT:-$out_dir/bazel}" info execution_root
)
ko_rel=$(
  cd "$kernel_workspace"
  "$bazel" --output_user_root="${BAZEL_OUTPUT_USER_ROOT:-$out_dir/bazel}" \
    cquery --noshow_progress --output=files "//$stage_name:yukari_filter" | \
    awk '$0 ~ /(^|\/)yukari_filter\.ko$/ { print; count++ } END { if (count != 1) exit 1 }'
)
case "$ko_rel" in
  /*) ko_path="$ko_rel" ;;
  *) ko_path="$execution_root/$ko_rel" ;;
esac
if [ ! -s "$ko_path" ]; then
  echo "Kleaf cquery did not produce a non-empty yukari_filter.ko at $ko_path" >&2
  exit 1
fi
cp "$ko_path" "$out_dir/ko/yukari_filter.ko"

make -C "$repo_dir/userspace" NDK_HOME="$ndk" \
  MAGISK_INCLUDE="$magisk_include" ABIS="$abis" OUT="$out_dir/userspace" \
  DIST="$out_dir" package

cp "$out_dir/ko/yukari_filter.ko" "$out_dir/yukari-service-filter/yukari_filter.ko"
chmod 0644 "$out_dir/yukari-service-filter/yukari_filter.ko"

if [ -f "$repo_dir/magisk/service.sh" ]; then
  cp "$repo_dir/magisk/service.sh" "$out_dir/yukari-service-filter/service.sh"
  chmod 0755 "$out_dir/yukari-service-filter/service.sh"
fi
if [ -f "$repo_dir/magisk/post-fs-data.sh" ]; then
  cp "$repo_dir/magisk/post-fs-data.sh" "$out_dir/yukari-service-filter/post-fs-data.sh"
  chmod 0755 "$out_dir/yukari-service-filter/post-fs-data.sh"
fi

# userspace/package creates an intermediate archive. Repack after adding the
# kernel module and Magisk lifecycle scripts so the delivered ZIP is complete.
rm -f "$out_dir/yukari-service-filter.zip"
(cd "$out_dir/yukari-service-filter" && zip -q -r ../yukari-service-filter.zip .)
test -s "$out_dir/yukari-service-filter.zip"

echo "Built $out_dir/yukari-service-filter.zip"
