cd /home/ubuntu/os/userspace

echo "=== Compilation test with -Wall -Wextra -Werror ==="
echo ""

for cmd in nvme lvcreate pvs mdev lsattr lslocks lsusb pax shar numfmt; do
    echo -n "  $cmd.c ... "
    if gcc -Wall -Wextra -Werror -O2 -g -Ilibc/include -c bin/cmds/$cmd.c -o /tmp/t_$cmd.o 2>/tmp/err_$cmd.txt; then
        echo "OK"
    else
        echo "FAILED"
        cat /tmp/err_$cmd.txt
    fi
done

echo ""
echo "=== Done ==="
