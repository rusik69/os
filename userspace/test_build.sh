#!/bin/bash
cd /home/ubuntu/os/userspace
CROSS_COMPILE="${CROSS_COMPILE:-x86_64-linux-gnu-}"
CC="${CROSS_COMPILE}gcc"
LIBC_DIR="libc"
CFLAGS="-ffreestanding -nostdlib -nostdinc -fno-builtin -mno-red-zone -I${LIBC_DIR}/include -Wall -Wextra -Werror -O2 -g -MMD -MP"

echo "Testing with: ${CC}"
if ! command -v "${CC}" >/dev/null 2>&1; then
    echo "Cross compiler not found, trying native gcc"
    CC="gcc"
    CFLAGS="-Wall -Wextra -Werror -O2 -g -I${LIBC_DIR}/include"
fi

for cmd in nvme lvcreate pvs mdev lsattr lslocks lsusb pax shar numfmt; do
    echo -n "  ${cmd}.c ... "
    ${CC} ${CFLAGS} -c bin/cmds/${cmd}.c -o /tmp/test_${cmd}.o 2>&1 && echo "OK" || echo "FAILED"
done

echo "Done."
