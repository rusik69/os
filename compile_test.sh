#!/bin/bash -e
cd /home/ubuntu/os/userspace
gcc -Wall -Wextra -Werror -O2 -g -Ilibc/include -c bin/cmds/pvs.c -o /tmp/t_pvs.o
echo "pvs OK"
gcc -Wall -Wextra -Werror -O2 -g -Ilibc/include -c bin/cmds/mdev.c -o /tmp/t_mdev.o
echo "mdev OK"
gcc -Wall -Wextra -Werror -O2 -g -Ilibc/include -c bin/cmds/lsattr.c -o /tmp/t_lsattr.o
echo "lsattr OK"
gcc -Wall -Wextra -Werror -O2 -g -Ilibc/include -c bin/cmds/nvme.c -o /tmp/t_nvme.o
echo "nvme OK"
gcc -Wall -Wextra -Werror -O2 -g -Ilibc/include -c bin/cmds/lvcreate.c -o /tmp/t_lvcreate.o
echo "lvcreate OK"
gcc -Wall -Wextra -Werror -O2 -g -Ilibc/include -c bin/cmds/lslocks.c -o /tmp/t_lslocks.o
echo "lslocks OK"
gcc -Wall -Wextra -Werror -O2 -g -Ilibc/include -c bin/cmds/lsusb.c -o /tmp/t_lsusb.o
echo "lsusb OK"
gcc -Wall -Wextra -Werror -O2 -g -Ilibc/include -c bin/cmds/pax.c -o /tmp/t_pax.o
echo "pax OK"
gcc -Wall -Wextra -Werror -O2 -g -Ilibc/include -c bin/cmds/shar.c -o /tmp/t_shar.o
echo "shar OK"
gcc -Wall -Wextra -Werror -O2 -g -Ilibc/include -c bin/cmds/numfmt.c -o /tmp/t_numfmt.o
echo "numfmt OK"
echo "=== ALL COMPILED SUCCESSFULLY ==="