#!/bin/bash
LOG=~/build/qemu_click.log
rm -f ~/build/qmp-sock ~/build/screen2.ppm
qemu-system-x86_64 -m 512 -cdrom ~/build/linux-giamat.iso \
	-serial mon:stdio -display none -vga std \
	-monitor unix:/root/build/qmp-sock,server,nowait \
	> "$LOG" 2>&1 &
QEMU_PID=$!
sleep 12
python3 - <<'PYEOF'
import socket, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/root/build/qmp-sock')
time.sleep(0.3)
s.recv(4096)
def cmd(c):
    s.sendall((c + '\n').encode())
    time.sleep(0.3)
    s.recv(4096)
cmd('mouse_move -400 -300')
cmd('mouse_button 1')
cmd('mouse_button 0')
time.sleep(0.5)
cmd('screendump /root/build/screen2.ppm')
PYEOF
sleep 1
kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null
ls -la ~/build/screen2.ppm
