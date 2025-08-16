#!/usr/bin/env bash
export ARCH=arm64
export SUBARCH=arm64
TOOLKIT="/home/lenovo/wrk/prelude-clang" # replace it with your tookit path
MPATH="$TOOLKIT/bin/:$PATH"
export O=out
# make O=out mrproper -j8 # Uncomment this line when first compile
# make O=out clean -j8 # Uncomment this line when first compile
# make O=out crux_defconfig # Uncomment this line when first compile
PATH="$MPATH" \
make -j8 \
  HOSTCC=gcc\
  O=out \
  NM=llvm-nm \
  OBJCOPY=llvm-objcopy \
  LD=ld.lld \
  CLANG_TRIPLE=aarch64-linux-gnu- \
  CROSS_COMPILE=aarch64-linux-gnu- \
  CROSS_COMPILE_ARM32=arm-linux-gnueabi- \
  CC=clang \
  AR=llvm-ar \
  OBJDUMP=llvm-objdump \
  STRIP=llvm-strip \


