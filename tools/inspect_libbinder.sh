#!/bin/sh
# Inspect the ELF/linkage conditions required by the userspace Binder PLT hook.
#
# The target image is deliberately supplied by the caller.  This script does
# not assume a firmware path, ABI, or device mount point.  Binutils can be
# selected explicitly for cross inspection with READELF=/path/readelf and
# OBJDUMP=/path/objdump.
set -u

LC_ALL=C
export LC_ALL

TRANSACT_MANGLED='_ZN7android14IPCThreadState8transactEijRKNS_6ParcelEPS1_j'
TRANSACT_DEMANGLED='android::IPCThreadState::transact'

readelf_bin=${READELF:-readelf}
objdump_bin=${OBJDUMP:-objdump}

if [ "$#" -ne 1 ]; then
  printf '%s\n' "usage: $0 /path/to/libbinder.so" >&2
  exit 2
fi

libbinder=$1

have_command() {
  command -v "$1" >/dev/null 2>&1
}

elf_status=unknown
dynamic_status=1
dynamic_dump=
reloc_status=1
reloc_dump=
disasm_status=1
disasm_dump=

if [ ! -r "$libbinder" ]; then
  elf_status=unreadable
elif ! have_command "$readelf_bin"; then
  elf_status=unknown
else
  if "$readelf_bin" -h -- "$libbinder" >/dev/null 2>&1; then
    elf_status=ok
  else
    elf_status=invalid
  fi
  if [ "$elf_status" = ok ]; then
    if dynamic_dump=$("$readelf_bin" -dW -- "$libbinder" 2>/dev/null); then
      dynamic_status=0
    fi
    if reloc_dump=$("$readelf_bin" -rW -- "$libbinder" 2>/dev/null); then
      reloc_status=0
    fi
  fi
fi

if [ "$elf_status" = ok ] && have_command "$objdump_bin"; then
  # GNU objdump and llvm-objdump both accept the combined -d/-r form used
  # here.  -C keeps the BpServiceManager and transact call sites readable.
  if disasm_dump=$("$objdump_bin" -drC -- "$libbinder" 2>/dev/null); then
    disasm_status=0
  elif disasm_dump=$("$objdump_bin" -d --demangle -- "$libbinder" 2>/dev/null); then
    disasm_status=0
  fi
fi

symbolic=unknown
bind_now=unknown
if [ "$dynamic_status" -eq 0 ]; then
  if printf '%s\n' "$dynamic_dump" | awk '
      /\(SYMBOLIC\)/ || (/\(FLAGS\)|\(FLAGS_1\)/ && /SYMBOLIC/) {
        found = 1
      }
      END { exit(found ? 0 : 1) }
    '; then
    symbolic=yes
  else
    symbolic=no
  fi
  if printf '%s\n' "$dynamic_dump" | awk '
      /\(BIND_NOW\)/ ||
      (/\(FLAGS\)/ && /BIND_NOW/) ||
      (/\(FLAGS_1\)/ && ( /BIND_NOW/ || /(^|[[:space:]])NOW([[:space:]]|$)/ )) {
        found = 1
      }
      END { exit(found ? 0 : 1) }
    '; then
    bind_now=yes
  else
    bind_now=no
  fi
fi

transact_plt=unknown
if [ "$reloc_status" -eq 0 ]; then
  # JUMP_SLOT is the normal ELF PLT relocation.  PLT32 is included for
  # toolchains that print a direct PLT relocation under that name.
  if printf '%s\n' "$reloc_dump" | awk -v symbol="$TRANSACT_MANGLED" '
      index($0, symbol) && ($0 ~ /JUMP_SLOT/ || $0 ~ /PLT32/) {
        found = 1
      }
      END { exit(found ? 0 : 1) }
    '; then
    transact_plt=yes
  else
    transact_plt=no
  fi
fi

bp_call_form=unknown
if [ "$disasm_status" -eq 0 ]; then
  # Track function labels so only calls made from BpServiceManager methods
  # contribute.  A call target ending in @plt is the form a PLT hook can
  # redirect; a named target without @plt is a direct branch.
  bp_call_form=$(printf '%s\n' "$disasm_dump" | awk \
    -v mangled="$TRANSACT_MANGLED" \
    -v demangled="$TRANSACT_DEMANGLED" '
      function target(line) {
        return index(line, mangled) || index(line, demangled)
      }
      BEGIN {
        in_bp = 0
        bp_seen = 0
        plt = 0
        direct = 0
        other = 0
      }
      /^[[:space:]]*[[:xdigit:]]+[[:space:]]+<[^>]+>:/ {
        in_bp = ($0 ~ /BpServiceManager/)
        if (in_bp) bp_seen = 1
        next
      }
      {
        if (!in_bp || !target($0)) next
        if ($0 ~ /@plt|@PLT/) {
          plt = 1
        } else if ($0 ~ /(^|[[:space:]])(bl|blx|call|callq)([[:space:]]|$)/) {
          direct = 1
        } else {
          other = 1
        }
      }
      END {
        if (!bp_seen) print "not_found"
        else if (plt && direct) print "both"
        else if (plt) print "plt_jump"
        else if (direct) print "direct_branch"
        else if (other) print "unknown"
        else print "no_transact_call"
      }
    ')
fi

conclusion=unknown
if [ "$elf_status" = ok ] && [ "$dynamic_status" -eq 0 ] &&
   [ "$reloc_status" -eq 0 ] && [ "$transact_plt" = no ]; then
  # Without a JUMP_SLOT/PLT32 relocation there is no PLT edge for Zygisk to
  # redirect, even when disassembly is unavailable.
  conclusion=PLT_HOOK_LIKELY_INEFFECTIVE
elif [ "$elf_status" = ok ] && [ "$dynamic_status" -eq 0 ] &&
     [ "$reloc_status" -eq 0 ] && [ "$disasm_status" -eq 0 ] &&
     [ "$transact_plt" = yes ]; then
  case "$bp_call_form:$symbolic" in
    plt_jump:no|plt_jump:unknown|both:no|both:unknown)
      conclusion=PLT_HOOK_POSSIBLE
      ;;
    direct_branch:*|no_transact_call:*|not_found:*|unknown:*|plt_jump:yes|both:yes)
      conclusion=PLT_HOOK_LIKELY_INEFFECTIVE
      ;;
  esac
fi

conclusion_text='unable to inspect PLT hook conditions'
case "$conclusion" in
  PLT_HOOK_POSSIBLE) conclusion_text='PLT hook may work' ;;
  PLT_HOOK_LIKELY_INEFFECTIVE) conclusion_text='PLT hook likely ineffective' ;;
esac

# Keep these keys stable: docs and CI can consume the output without parsing
# binutils' version-specific wording.
printf 'INPUT=%s\n' "$libbinder"
printf 'ELF_STATUS=%s\n' "$elf_status"
printf 'DYNAMIC_SYMBOLIC=%s\n' "$symbolic"
printf 'DYNAMIC_BIND_NOW=%s\n' "$bind_now"
printf 'TRANSACT_PLT_RELOCATION=%s\n' "$transact_plt"
printf 'BP_SERVICEMANAGER_CALL_FORM=%s\n' "$bp_call_form"
printf 'CONCLUSION=%s\n' "$conclusion"
printf 'CONCLUSION_TEXT=%s\n' "$conclusion_text"

if [ "$elf_status" != ok ] || [ "$dynamic_status" -ne 0 ] ||
   [ "$reloc_status" -ne 0 ] || [ "$disasm_status" -ne 0 ]; then
  exit 1
fi
exit 0
