#!/system/bin/sh
MODDIR=${0%/*}
PARAMS=
TARGET_UID=$(sed -n 's/^uids=\([0-9][0-9]*\).*$/\1/p' "$MODDIR/config/targets.conf" | head -n 1)
JSON_CONFIG="$MODDIR/config.json"
if [ ! -f "$JSON_CONFIG" ] && [ -f "/data/adb/modules/Yukari/config.json" ]; then
  JSON_CONFIG="/data/adb/modules/Yukari/config.json"
fi
if [ -n "$TARGET_UID" ]; then
  PARAMS="target_uid=$TARGET_UID enabled=1"
elif [ -f "$JSON_CONFIG" ]; then
  # JSON target_uids are transferred through the control ioctl. Keep the
  # diagnostic probe enabled even when the legacy first-UID parameter is absent.
  PARAMS="enabled=1"
fi
if [ -e "$MODDIR/yukari_filter.ko" ] && [ -n "$PARAMS" ]; then
  # insmod accepts only the module parameters used here; all parsing remains
  # in the root-owned config file.
  insmod "$MODDIR/yukari_filter.ko" $PARAMS 2>/dev/null || true
fi
# Read the root-owned configuration and send its validated snapshot through
# the root-only control device. A missing device is harmless and fail-open.
if [ -x "$MODDIR/bin/service-proxy" ]; then
  "$MODDIR/bin/service-proxy" --sync >/dev/null 2>&1 || true
fi
