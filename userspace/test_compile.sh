#!/bin/bash
# Test compile all 10 new commands
set -e
cd /home/ubuntu/os/userspace

# Try with the project's cross compiler
CROSS_COMPILE="${CROSS_COMPILE:-x86_64-linux-gnu-}"
CC="${CROSS_COMPILE}gcc"
LIBC_DIR="libc"
CFLAGS="-ffreestanding -nostdlib -nostdinc -fno-builtin -mno-red-zone -I${LIBC_DIR}/include -Wall -Wextra -Werror -O2 -g -MMD -MP"
LDFLAGS="-T ${LIBC_DIR}/user.ld -nostdlib -z max-page-size=0x1000 -z noexecstack"

echo "Checking compiler availability..."
if ! command -v "${CC}" >/dev/null 2>&1; then
    echo "Cross compiler ${CC} not found, trying native gcc..."
    CC="gcc"
    CFLAGS="-Wall -Wextra -Werror -O2 -g -I${LIBC_DIR}/include -D_GNU_SOURCE"
    LDFLAGS=""
fi

echo "Using compiler: ${CC}"
echo ""

# Compile each new command as a standalone check
for cmd in nvme lvcreate pvs mdev lsattr lslocks lsusb pax shar numfmt; do
    echo "Compiling ${cmd}.c..."
    ${CC} ${CFLAGS} -c bin/cmds/${cmd}.c -o /tmp/${cmd}.o 2>&1 || echo "FAILED: ${cmd}.c"
done

echo ""
echo "=== Compilation check complete ==="
