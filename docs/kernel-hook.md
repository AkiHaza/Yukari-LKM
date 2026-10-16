# Binder hook boundary

`kernel/yukari_filter.c` is an observational, best-effort kretprobe. It does
not filter a Binder reply, and building it with DDK does not make Java, native,
or direct `BINDER_WRITE_READ` service queries return a filtered view. The
current module must not be described as an implementation of all-path
`listServices` / `getService` / `getServiceDebugInfo` consistency.

An external module needs an explicit driver-owned replacement interface, or
a corresponding framework implementation, to provide that guarantee. The
inspected hooks and exported helpers do not supply such an interface. This
is a limitation of the supported interface and ownership contract; arbitrary
private-symbol lookups, structure casts, or page writes are not a portable
GKI implementation. DDK supplies build integration, not missing Binder APIs.

## Evidence and source identity

The source inspected locally was
`D:/YAAPkernel/android_kernel_oneplus_sm8650`, commit
`9dd97a5eb2410ebaffed9fa6334560127c977fc1`, whose configured origin is
`https://github.com/AkiHaza/android_kernel_oneplus_sm8650.git`. The relevant
files had no working-tree changes. This is a vendor 6.1 source snapshot, not
proof that every Android 14 GKI image exposes the same hooks. Source links
below identify that exact inspected commit.

The proposed AOSP baseline, `aosp-mirror/kernel_common` branch
`android14-6.1`, commit `498f91c17b0822a09aede17d26a63bf8c6813725`, could not
be retrieved: both android.googlesource.com and github.com connections failed
on 2026-10-16. No driver patch has been checked against that commit, and no
claim of patch applicability, compilation, or device correctness is made.

| Source at the inspected commit | Verified fact |
| --- | --- |
| [binder.c:3332](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/drivers/android/binder.c#L3332) and [binder.c:3409](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/drivers/android/binder.c#L3409) | `binder_transaction()` caches the source Parcel pointer in `user_buffer` before `android_vh_binder_reply`. Changing only `tr->data.ptr.buffer` in the hook does not redirect later payload copying. |
| [binder.c:3656](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/drivers/android/binder.c#L3656) and [binder.c:3997](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/drivers/android/binder.c#L3997) | The driver allocates the recipient buffer, translates objects, copies the remaining payload, and completes deferred copies before queueing the reply. Replacement belongs in a driver-owned stage with this ownership information. |
| [binder.c:5378](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/drivers/android/binder.c#L5378) and [binder.c:5409](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/drivers/android/binder.c#L5409) | `binder_thread_read()` fills the sizes and buffer pointers, then emits the command and transaction descriptor to userspace, and only then calls `android_vh_binder_transaction_received`. |
| [binder.c:6317](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/drivers/android/binder.c#L6317) | `android_vh_binder_ioctl_end` runs after the Binder ioctl has already processed and emitted its read stream, so it cannot replace a reply either. |
| [binder.c:6376](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/drivers/android/binder.c#L6376) | Binder mmap rejects `VM_WRITE` and clears `VM_MAYWRITE`; the recipient cannot safely rewrite its reply by ordinary userspace writes. This does not imply the driver itself cannot write its backing pages. |
| [binder_alloc.c:1383](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/drivers/android/binder_alloc.c#L1383) | `binder_alloc_copy_to_buffer()` is defined but not exported here. `binder_alloc_copy_from_buffer()` alone has `EXPORT_SYMBOL_GPL`. A read helper does not grant a replacement or lifetime-management API. |
| [binder_internal.h:449](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/drivers/android/binder_internal.h#L449), [binder_internal.h:587](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/drivers/android/binder_internal.h#L587), and [binder_alloc.h:43](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/drivers/android/binder_alloc.h#L43) | `binder_proc`, `binder_thread`, `binder_transaction`, and `binder_buffer` are private driver structures. The hook prototypes expose only opaque forward declarations, not a stable replacement ABI. |
| [binder.c:2313](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/drivers/android/binder.c#L2313) and [binder.c:4384](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/drivers/android/binder.c#L4384) | Object release is a private driver operation. The release hook is followed unconditionally by the driver's object-release and buffer-free operations. |
| [binder_alloc.c:1277](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/drivers/android/binder_alloc.c#L1277) and [binder_alloc.c:868](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/drivers/android/binder_alloc.c#L868) | Buffer bounds/lifetime checks, allocator locking, and `binder_alloc_free_buf()` are private. An LKM cannot safely mutate page contents or `data_size` outside the driver's active-buffer ownership and locking rules. |
| [binder.h:89](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/include/trace/hooks/binder.h#L89), [vendor_hooks.h:16](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/include/trace/hooks/vendor_hooks.h#L16), and [tracepoint.h:184](https://github.com/AkiHaza/android_kernel_oneplus_sm8650/blob/9dd97a5eb2410ebaffed9fa6334560127c977fc1/include/linux/tracepoint.h#L184) | Ordinary `DECLARE_HOOK` uses `DECLARE_TRACE`; callbacks run in the tracepoint's preemption-disabled section. Sleeping allocation and faulting usercopy cannot be added to that callback as though it were an ordinary sleepable driver method. |

For independent source comparison, the Git blob IDs are:

```text
drivers/android/binder.c          fcb5186855bb7dc194174c2b1e86b57eff8fbad2
drivers/android/binder_alloc.c    b11f6519ae8cfd0cb6ef1e29f09de094029d8a9a
drivers/android/binder_internal.h 0fd0e9a7044649fd1588fc0a306b24a4bf620f80
include/trace/hooks/binder.h      93641ce4ed6ed96bf0e039c7f996a706ce1aeafb
```

## Why the received and release hooks are insufficient

`android_vh_binder_transaction_received(t, proc, thread, cmd)` exposes pointers
to forward-declared Binder types. Their definitions live in private
`drivers/android/binder_internal.h` and `binder_alloc.h`, rather than in an
external replacement ABI. In the inspected source, the hook executes after
`put_user(cmd)` and `copy_to_user(&tr)`. Changing `t->buffer->data_size` there
would not update the descriptor already copied to userspace. The
`android_vh_binder_ioctl_end` hook and a return probe on
`binder_ioctl_write_read()` run even later. Neither location supplies a
supported operation for replacing both the Parcel and its emitted descriptor.

The hook being GPL-exported in `drivers/android/vendor_hooks.c:400` does not
make private Binder structures, allocator metadata, or object ownership a
stable external-module write contract. The corresponding exports for
`android_vh_binder_free_buf` and `android_vh_binder_buffer_release` are at lines
403 and 404 of that same snapshot. Actual availability in a target GKI image
must still be established from that image's configuration, KMI symbol list,
and generated `Module.symvers`.

`android_vh_binder_buffer_release(proc, thread, buffer, has_transaction)` is a
notification inside `binder_free_buf()`. Its signature has no skip flag,
replacement pointer, or transfer-of-ownership result. Immediately afterwards,
the driver calls `binder_release_entire_buffer()` and `binder_alloc_free_buf()`.
Changing object offsets or freeing references in this hook can make that
mandatory cleanup release the wrong objects or release a reference twice.
The hook is also too late to alter the already delivered reply.

For successful `getService` or `checkService`, hiding a service means replacing
an already translated Binder result with a null result and releasing its
original translated reference exactly once. For list/debug-info results,
shortening a Parcel must preserve its exception header, counts, alignment,
and any permitted object table. Editing bytes and `data_size` alone does not
satisfy these ownership and wire-format requirements. Even an object-free
list result does not provide the missing supported write/replace interface.

## Required integration contract

A complete implementation needs a verified driver interface before reply
queueing, after translation/copy, with the following properties:

* Correlate each synchronous request and reply, and verify that the actual
  request target node is the context manager in the intended Binder context.
  Neither a transaction number nor `BR_REPLY` alone identifies ServiceManager.
* Supply the original caller identity, rather than the servicemanager reply
  sender's UID. Request identity and any required request bytes must remain
  valid even when the server has already freed its incoming request buffer.
* Select the exact `IServiceManager.aidl` release profile and parse its Parcel
  bounds, exception header, UTF-16 strings, counts, and debug-info records.
  `@utf8InCpp` does not change the String wire encoding to UTF-8.
* Let the callback request an unchanged or validated replacement result while
  the driver owns copying, size/offset updates, and cleanup of translated
  Binder references. Reject unsupported FD, pointer, or object layouts.
* Keep the callback lifetime safe during module unload, for example with an
  explicitly sleepable SRCU registration contract and synchronized removal.
* Leave the servicemanager source Parcel and global registration table intact;
  only replace the specific recipient's result.

These are requirements for a future integration, not capabilities of the
current module. No unverified Binder driver patch is shipped. A complete
claim requires applying and building against the fixed kernel revision, plus
device tests for Java calls, native clients, direct ioctls, concurrent
requests, malformed Parcels, non-target/system callers, reference cleanup,
and list/get/debug-info consistency. The existing host policy/proxy tests do
not exercise these Binder guarantees.
