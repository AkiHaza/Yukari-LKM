# Yukari-LKM

Yukari-LKM contains a per-caller service-view filter for a configured app.
The policy is intentionally narrow: it matches an explicit UID and optional
process allowlist, then hides service names containing configured literal
tokens such as `lineage`. It never edits servicemanager's registration table.

The repository contains two layers:

* `userspace/` contains the filtering core, a `service list` proxy, and the
  Zygisk integration point. The proxy keeps command output consistent with
  the core filter.
* `kernel/` contains a GKI-loadable, fail-open Binder probe. A stock GKI LKM
  cannot safely rewrite a Binder reply after `binder_ioctl_write_read()` and
  preserve Parcel object offsets. Reply rewriting requires a vendor Binder
  hook at the point described in [docs/kernel-hook.md](docs/kernel-hook.md).

This boundary is deliberate. It prevents a module from corrupting Binder or
silently returning a different result on one Android release than another.
An implementation claiming complete coverage for Java `ServiceManager`,
native `IServiceManager`, and hand-written `BINDER_WRITE_READ` must provide a
verified framework-side or vendor-driver hook and tests for that exact Android
release. The generic probe does not make that claim.

## Configuration

The Zygisk process selector must be configured with a trusted user-app UID
and, when needed, exact process names. The installed module reads:

```text
uids=12345
processes=com.example.app,com.example.app:worker
hide_tokens=lineage
```

An empty or UID-less target selector disables filtering. Core/system UIDs with
an Android appId below 10000 are always excluded. A privileged system package
that owns an application-range UID cannot be identified from the stock LKM
interface alone, so provisioning must never place such a UID in the target
allowlist. Tokens are literal, case-insensitive substrings; regular
expressions are unsupported. Populate the selector during installation or
provisioning from the package's verified UID and process list. The file is
root-owned and readable by the injected app, but not writable by it. Do not
use an untrusted app-provided environment variable as authorization in a
production module.

## Build

`build/build.sh` is the entry point used by the workflow; the complete
workflow is documented in [docs/build.md](docs/build.md). It expects a
matching Android 14/GKI 6.1 Kleaf workspace in `KERNEL_WORKSPACE`, the exact
`kernel_build` label in `KERNEL_BUILD_LABEL`, an Android NDK in
`ANDROID_NDK_HOME`, and Magisk headers in `MAGISK_INCLUDE`. The matching
headers label defaults to `//common:all_headers` and can be overridden with
`KERNEL_HEADERS_LABEL`. `BAZEL` may point to the DDK's Bazel wrapper. The
script stages the module into that workspace, builds the `.ko`, builds the
proxy and Zygisk library, then assembles a Magisk module zip.

```sh
KERNEL_WORKSPACE=/path/to/ddk KERNEL_BUILD_LABEL=//common:kernel_aarch64 \
  ANDROID_NDK_HOME=/path/to/ndk MAGISK_INCLUDE=/path/to/magisk/include \
  ./build/build.sh
```

The workflow accepts those paths as runner inputs and invokes the same script.
A successful build proves compilation only; GKI KMI symbol availability,
module signing, and device loading still need validation on the target image.

## Verification

The host tests exercise caller matching, literal service-name matching,
preservation of output formatting, and the command proxy's filtering contract.
They do not emulate a Binder driver. Device tests must cover target and
non-target callers, app and worker processes, system apps, malformed Parcels,
concurrent transactions, list/get/debug-info consistency, and a direct native
`BINDER_WRITE_READ` client before enabling any vendor rewrite hook.
