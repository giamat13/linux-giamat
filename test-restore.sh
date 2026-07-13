#!/bin/bash
LOG=~/build/qemu_restore.log
rm -f ~/build/qmp-sock ~/build/wr1.ppm ~/build/wr2.ppm
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
def cmd(c, wait=0.3):
    s.sendall((c + '\n').encode())
    time.sleep(wait)
    s.recv(8192)

cmd('mouse_move -400 -190')   # -> (112,194) TERMINAL icon
cmd('mouse_button 1'); cmd('mouse_button 0')
time.sleep(0.5)
cmd('mouse_move 588 -2')      # -> (700,192) minimize button of the just-opened window
cmd('mouse_button 1'); cmd('mouse_button 0')
time.sleep(0.3)
cmd('screendump /root/build/wr1.ppm')  # should show minimized (no window, taskbar entry present)

cmd('mouse_move -636 560')    # -> (64,752) taskbar "Terminal 1" button
cmd('mouse_button 1'); cmd('mouse_button 0')
time.sleep(0.3)
cmd('screendump /root/build/wr2.ppm')  # should show window restored
PYEOF
sleep 1
kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null
ls -la ~/build/wr*.ppm
