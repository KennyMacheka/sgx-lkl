# This version of config.mak was generated by:
# ./configure --prefix=/root/sgx-lkl/build_musl/sgx-lkl-musl --lklheaderdir=/root/sgx-lkl/build_musl/lkl/include/ --lkllib=/root/sgx-lkl/build_musl/lkl/lib/liblkl.a --sgxlklincludes='/root/sgx-lkl/src/include /root/sgx-lkl/build_musl/cryptsetup/include/ /common/inc /common/inc/internal' --sgxlkllib=/root/sgx-lkl/build_musl/sgxlkl/libsgxlkl.a --sgxlkllibs='/root/sgx-lkl/build_musl/cryptsetup/lib/libcryptsetup.a /root/sgx-lkl/build_musl/cryptsetup/lib/libpopt.a /root/sgx-lkl/build_musl/cryptsetup/lib/libdevmapper.a /root/sgx-lkl/build_musl/e2fsprogs/lib/libext2fs.a /root/sgx-lkl/build_musl/cryptsetup/lib/libuuid.a /root/sgx-lkl/build_musl/cryptsetup/lib/libjson-c.a 							  /root/sgx-lkl/build_musl/curl/libcurl.a /root/sgx-lkl/build_musl/curl/liboe_stubs.a /root/sgx-lkl/build_musl/openenclave/lib/openenclave/enclave/libmbedtls.a' --disable-shared
# Any changes made here will be lost if configure is re-run
ARCH = x86_64
SUBARCH = 
ASMSUBARCH = 
srcdir = .
lklheaderdir = /root/sgx-lkl/build_musl/lkl/include/
lkllib = /root/sgx-lkl/build_musl/lkl/lib/liblkl.a
sgxlklincludes = /root/sgx-lkl/src/include /root/sgx-lkl/build_musl/cryptsetup/include/ /common/inc /common/inc/internal
sgxlkllib = /root/sgx-lkl/build_musl/sgxlkl/libsgxlkl.a
sgxlkllibs = /root/sgx-lkl/build_musl/cryptsetup/lib/libcryptsetup.a /root/sgx-lkl/build_musl/cryptsetup/lib/libpopt.a /root/sgx-lkl/build_musl/cryptsetup/lib/libdevmapper.a /root/sgx-lkl/build_musl/e2fsprogs/lib/libext2fs.a /root/sgx-lkl/build_musl/cryptsetup/lib/libuuid.a /root/sgx-lkl/build_musl/cryptsetup/lib/libjson-c.a 							  /root/sgx-lkl/build_musl/curl/libcurl.a /root/sgx-lkl/build_musl/curl/liboe_stubs.a /root/sgx-lkl/build_musl/openenclave/lib/openenclave/enclave/libmbedtls.a
prefix = /root/sgx-lkl/build_musl/sgx-lkl-musl
exec_prefix = $(prefix)
bindir = $(exec_prefix)/bin
libdir = $(prefix)/lib
includedir = $(prefix)/include
syslibdir = /lib
CC = gcc
CFLAGS = -fPIC -D__USE_GNU -O3
CFLAGS_AUTO = -pipe -fomit-frame-pointer -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -Werror=implicit-function-declaration -Werror=implicit-int -Werror=pointer-sign -Werror=pointer-arith
CFLAGS_C99FSE = -std=c99 -nostdinc -ffreestanding -fexcess-precision=standard -frounding-math -Wa,--noexecstack
CFLAGS_MEMOPS = -fno-tree-loop-distribute-patterns
CFLAGS_NOSSP = -fno-stack-protector
CFLAGS_SGX = 
CPPFLAGS = 
LDFLAGS = 
LDFLAGS_AUTO = -Wl,--sort-section,alignment -Wl,--sort-common -Wl,--gc-sections -Wl,--hash-style=both -Wl,--no-undefined -Wl,--exclude-libs=ALL -Wl,--dynamic-list=./dynamic.list
CROSS_COMPILE = 
LIBCC = -lgcc -lgcc_eh
OPTIMIZE_GLOBS = 
ALL_TOOLS =  obj/musl-gcc
TOOL_LIBS =  lib/musl-gcc.specs
ADD_CFI = no
WRAPCC_GCC = $(CC)
AOBJS = $(LOBJS)
