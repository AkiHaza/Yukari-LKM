#!/system/bin/sh
MODDIR=${0%/*}

# The file is root-owned by Magisk.  An empty target selector keeps the
# module disabled until the installer writes a verified package UID.
chown 0:0 "$MODDIR/config/targets.conf" 2>/dev/null || true
# The injected app must be able to read the selector, while only root may
# replace it. loadConfig() rejects group/world-writable files.
chmod 0644 "$MODDIR/config/targets.conf" 2>/dev/null || true
if [ -f "$MODDIR/config.json" ]; then
  chown 0:0 "$MODDIR/config.json" 2>/dev/null || true
  chmod 0644 "$MODDIR/config.json" 2>/dev/null || true
fi
if [ -f "/data/adb/modules/Yukari/config.json" ]; then
  chown 0:0 "/data/adb/modules/Yukari/config.json" 2>/dev/null || true
  chmod 0644 "/data/adb/modules/Yukari/config.json" 2>/dev/null || true
fi
