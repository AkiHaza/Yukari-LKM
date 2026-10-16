# Android 14 Parcel transformation core

`src/parcel.c` implements a bounded, allocation-free transformation of already
identified Android 14 IServiceManager reply data. It is a codec, not a Binder
hook: it does not identify a transaction, select a caller, access another
process, maintain Binder references, or publish rewritten transaction buffers.
Transport integration must independently supply that context and must reject
nonempty Binder object-offset tables for the two vector operations.

The supported format is little-endian kernel Binder IPC, beginning with a
single successful `binder::Status` int32 value of zero. Exception and richer
reply headers, RPC Binder, null nonnullable values, malformed UTF-16, nonzero
String16 alignment bytes, extra bytes after the vector, and configured work
limits are rejected before writing output. The caller must keep input and
tokens stable for the entire call. It may use a distinct caller-owned output
buffer; overlapping output and in-place filtering are rejected. With no tokens,
an exact in-place validated copy is allowed. Every error leaves output and the
result structure unchanged.

`listServices` is a count followed by String16 values. `getServiceDebugInfo` is
a count followed by `1` non-null parcelable markers and size-prefixed
ServiceDebugInfo values containing a String16 name and int32 debugPid. The
size includes its own size word. Unknown trailing fields of retained
parcelables are copied unchanged. Matching is literal ASCII case-insensitive
substring matching against UTF-16 code units; no Unicode folding is applied.

`yukari_parcel_make_null_binder` emits a successful status, a null
flat_binder_object, and Android 14's stability int32. The Binder ABI pointer
width selects 24-byte or 32-byte replies. A null object has no Binder object
offset entry because AOSP uses `writeObject(obj, false)` with `obj.binder == 0`.
The integration must separately release the original references correctly.

These assumptions were checked against AOSP `android14-release`:

* [IServiceManager.aidl](https://android.googlesource.com/platform/frameworks/native/+/android14-release/libs/binder/aidl/android/os/IServiceManager.aidl) defines the operations and nonnullable lists.
* [ServiceDebugInfo.aidl](https://android.googlesource.com/platform/frameworks/native/+/android14-release/libs/binder/aidl/android/os/ServiceDebugInfo.aidl) defines name and debugPid.
* [Parcel.h](https://android.googlesource.com/platform/frameworks/native/+/android14-release/libs/binder/include/binder/Parcel.h) implements vector counts and Parcelable non-null markers in writeData.
* [Parcel.cpp](https://android.googlesource.com/platform/frameworks/native/+/android14-release/libs/binder/Parcel.cpp) implements String16 padding, flattenBinder, finishFlattenBinder and writeObject metadata rules.
* [Status.cpp](https://android.googlesource.com/platform/frameworks/native/+/android14-release/libs/binder/Status.cpp) writes only EX_NONE for a successful status.
* [generate_cpp.cpp](https://android.googlesource.com/platform/system/tools/aidl/+/android14-release/generate_cpp.cpp) generates size-prefixed parcelables including the size word.

Host verification command (C99):

```sh
cc -std=c99 -Wall -Wextra -Werror -Iinclude src/parcel.c tests/parcel_test.c -o parcel_test
./parcel_test
```

The tests exercise mixed case matching, empty names/vectors, all entries
removed, Unicode surrogate pairs, malformed surrogate/padding, byte-exact
unknown-field retention, every truncation of two representative replies,
token limits, output capacity, and null Binder construction. They do not
validate a running Android Binder driver or establish complete path coverage.
