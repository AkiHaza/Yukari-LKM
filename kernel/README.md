# Kernel module

The module is an Android 14 / GKI 6.1 compatibility probe. It intentionally
fails open on an unmodified GKI image: the stable external-module interface
does not expose the Binder transaction buffer or `IServiceManager` Parcel
schema needed to rewrite a reply safely.

The probe requires `CONFIG_KRETPROBES=y`, which depends on `CONFIG_KPROBES=y`
and architecture return-probe support in Linux 6.1. `enabled=1` and
`target_uid=10000` or greater only enable diagnostics for that effective UID;
they do not enable a vendor integration or hide services. `hide_token` is
currently a diagnostic label. The `pr_debug` messages require the kernel's
debug logging configuration to make them visible.

For a matching Android 14/GKI 6.1 Kleaf workspace, use the repository build
entry point with that workspace's exact `kernel_build` label. The module is
staged into the workspace and built by its DDK/Kleaf toolchain:

```sh
KERNEL_WORKSPACE=/path/to/kernel-workspace \
KERNEL_BUILD_LABEL=//common:kernel_aarch64 \
ANDROID_NDK_HOME=/path/to/ndk MAGISK_INCLUDE=/path/to/magisk/include \
./build/build.sh
```

For a direct Kbuild check after the kernel has produced its real
`Module.symvers`:

```sh
make -C "$KERNEL_DIR" O="$KERNEL_OUT" ARCH=arm64 LLVM=1 LLVM_IAS=1 \
  M="$PWD/kernel" modules
```

`modules_prepare` alone is insufficient when `CONFIG_MODVERSIONS=y`. This
module remains an observational probe unless a separately maintained,
in-tree Binder/framework adapter is supplied; the external module does not
export or install such an adapter. Verify `modinfo` vermagic, `__versions`,
and the signature before loading the `.ko`.
