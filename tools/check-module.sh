#!/bin/sh
# Type-checks the Renguin kernel module (and the kernel files the patch adds)
# against a Haiku source checkout, using the host compiler in Haiku kernel mode.
#
# This is NOT a substitute for the real jam build with Haiku's cross-compiler,
# but it catches wrong Haiku API names, signatures and header use in seconds,
# on any Linux machine.
#
# Usage: tools/check-module.sh /path/to/haiku [c++ compiler]
#   The Haiku checkout should have the Renguin patch applied, or at least
#   contain headers/private/kernel/foreign_personality.h (the script falls
#   back to the copy in patches/ if it is missing).
set -e
HAIKU=${1:?usage: $0 /path/to/haiku [c++]}
CXX=${2:-g++}
HERE=$(cd "$(dirname "$0")/.." && pwd)
MODULE="$HERE/haiku/src/add-ons/kernel/personalities/renguin"
H="$HAIKU/headers"

TMP=$(mktemp -d)
trap '[ -n "$KEEP" ] || rm -rf "$TMP"' EXIT

# Provide the patched kernel headers if the checkout is unpatched.
if [ ! -f "$H/private/kernel/foreign_personality.h" ]; then
	mkdir -p "$TMP/overlay"
	(cd "$TMP/overlay" && mkdir -p headers/private/kernel \
		&& cp "$H/private/kernel/thread_types.h" headers/private/kernel/ \
		&& patch -s -p1 --forward -i \
			"$HERE/patches/haiku/0001-kernel-foreign-ABI-personality-hooks.patch" \
			headers/private/kernel/thread_types.h >/dev/null 2>&1 || true)
	# foreign_personality.h is a new file in the patch: extract it.
	awk '/^\+\+\+ b\/headers\/private\/kernel\/foreign_personality.h/{f=1;next}
		f&&/^diff --git/{exit} f&&/^@@/{next} f{sub(/^\+/,"");print}' \
		"$HERE/patches/haiku/0001-kernel-foreign-ABI-personality-hooks.patch" \
		> "$TMP/overlay/headers/private/kernel/foreign_personality.h"
	OVERLAY="-I$TMP/overlay/headers/private/kernel"
fi

GCCINC=$("$CXX" -print-file-name=include)

# Some kernel files use <algorithm> etc. from the compiler's libstdc++; use
# the host's (header-only parts) for those.
CXXINC=""
for d in $("$CXX" -E -x c++ -v /dev/null 2>&1 | sed -n 's/^ \(\/.*c++.*\)$/\1/p'); do
	CXXINC="$CXXINC -isystem $d"
done

# Haiku's cross-compiler ships <new>; a host -nostdinc build needs a stand-in.
mkdir -p "$TMP/shim"
cat > "$TMP/shim/new" <<'SHIM'
#ifndef _RENGUIN_SHIM_NEW
#define _RENGUIN_SHIM_NEW
#include <stddef.h>
namespace std {
	using ::size_t;
	struct nothrow_t { explicit nothrow_t() = default; };
	extern const nothrow_t nothrow;
	enum class align_val_t : size_t {};
}
void* operator new(size_t, void* p) noexcept;
void* operator new[](size_t, void* p) noexcept;
void* operator new(size_t, const std::nothrow_t&) noexcept;
void* operator new[](size_t, const std::nothrow_t&) noexcept;
#endif
SHIM

INCLUDES="$OVERLAY
	-I$H/private/kernel -I$H/private/libroot -I$H/private/shared
	-I$H/private/kernel/boot/platform/efi
	-I$H/private/kernel/arch/x86
	-I$H/private/system -I$H/private/system/arch/x86_64
	-I$H/private/kernel/util
	-I$H/private -I$H/private/system/util
	-I$HAIKU/build/config_headers
	-I$H/os -I$H/os/kernel -I$H/os/support -I$H/os/storage -I$H/os/drivers
	-I$H/os/app -I$H/os/interface
	-I$H -I$H/posix -I$H/glibc
	-isystem $TMP/shim $CXXINC -isystem $GCCINC"

FLAGS="-std=gnu++17 -fsyntax-only -nostdinc -ffreestanding -fno-exceptions
	-fno-builtin -m64 -mno-red-zone -D_KERNEL_MODE -D__HAIKU__
	-DHAIKU_TARGET_PLATFORM_HAIKU -DARCH_x86_64 -D__x86_64__
	-D__GLIBC_PREREQ(a,b)=0
	-U__linux__ -U__linux -Ulinux -U__gnu_linux__
	-Wall -Wextra -Wno-unused-parameter -Wno-multichar -Wno-unused-function -Wno-missing-field-initializers
	-Wno-attributes"

status=0
for src in "$MODULE"/*.cpp; do
	if "$CXX" $FLAGS $INCLUDES -I"$MODULE" -c "$src" 2>"$TMP/err"; then
		echo "ok    $(basename "$src")"
		grep -A3 "warning:" "$TMP/err" | grep -v "^--$" | sed 's/^/      /'
	else
		echo "FAIL  $(basename "$src")"; cat "$TMP/err"; status=1
	fi
done

# The kernel files the patch adds or changes, if the checkout is patched.
if [ -f "$HAIKU/src/system/kernel/foreign_personality.cpp" ]; then
	for f in foreign_personality.cpp team.cpp thread.cpp \
			arch/x86/arch_thread.cpp; do
		if "$CXX" $FLAGS $INCLUDES -I"$HAIKU/src/system/kernel" \
				-I"$HAIKU/src/system/kernel/arch/x86" \
				-c "$HAIKU/src/system/kernel/$f" 2>"$TMP/err"; then
			echo "ok    $f (kernel)"
		else
			echo "FAIL  $f (kernel)"; cat "$TMP/err"; status=1
		fi
	done

	# The syscall-entry assembly: generate asm_offsets.h the way Haiku's
	# build does (CreateAsmStructOffsetsHeader in build/jam/MainBuildRules),
	# then assemble the patched interrupts.S.
	mkdir -p "$TMP/gen"
	# syscall_numbers.h comes from Haiku's gensyscalls tool; the values do
	# not matter for checking that the code assembles.
	printf '#define SYSCALL_COUNT 300\n#define SYSCALL_RESTORE_SIGNAL_FRAME 100\n' \
		> "$TMP/gen/syscall_numbers.h"
	printf '#define SYSCALL_INFO_sizeof 16\n#define SYSCALL_INFO_function 0\n#define SYSCALL_INFO_parameter_size 8\n' \
		> "$TMP/gen/syscall_table.h"
	if "$CXX" -S $(echo "$FLAGS" | sed 's/-fsyntax-only//') $INCLUDES \
			-I"$HAIKU/src/system/kernel" \
			"$HAIKU/src/system/kernel/arch/x86/asm_offsets.cpp" -o - \
			2>"$TMP/err" | sed 's/@define/#define/g' | grep "#define" \
			| sed -e 's/[$#]\([0-9]\)/\1/' > "$TMP/gen/asm_offsets.h" \
		&& grep -q THREAD_user_fpu_state "$TMP/gen/asm_offsets.h" \
		&& "$CXX" -c -x assembler-with-cpp -nostdinc -D_KERNEL_MODE \
			-D__HAIKU__ -DARCH_x86_64 -D__x86_64__ -D_ASSEMBLER \
			-I"$TMP/gen" $INCLUDES -I"$HAIKU/src/system/kernel/arch/x86" \
			"$HAIKU/src/system/kernel/arch/x86/64/interrupts.S" \
			-o "$TMP/interrupts.o" 2>>"$TMP/err"; then
		echo "ok    arch/x86/64/interrupts.S (kernel, assembled)"
	else
		echo "FAIL  arch/x86/64/interrupts.S"; cat "$TMP/err"; status=1
	fi
else
	echo "note  checkout is unpatched; kernel files not checked"
fi
exit $status
