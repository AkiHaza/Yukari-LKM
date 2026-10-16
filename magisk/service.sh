#!/system/bin/sh
MODDIR=${0%/*}
PARAMS=
TARGET_UID=$(sed -n 's/^uids=\([0-9][0-9]*\).*$/\1/p' "$MODDIR/config/targets.conf" | head -n 1)
if [ -n "$TARGET_UID" ]; then
  PARAMS="target_uid=$TARGET_UID enabled=1"
fi
if [ -e "$MODDIR/yukari_filter.ko" ] && [ -n "$PARAMS" ]; then
  # insmod accepts only the module parameters used here; all parsing remains
  # in the root-owned config file.
  insmod "$MODDIR/yukari_filter.ko" $PARAMS 2>/dev/null || true
fi
