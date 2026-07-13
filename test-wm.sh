#!/bin/bash
LOG=~/build/qemu_wm.log
rm -f ~/build/qmp-sock ~/build/wm1.ppm ~/build/wm2.ppm ~/build/wm3.ppm ~/build/wm4.ppm
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

# move to TERMINAL icon (~100,195) from center (512,384) and click
cmd('mouse_move -400 -190')
cmd('mouse_button 1')
cmd('mouse_button 0')
time.sleep(0.5)
cmd('screendump /root/build/wm1.ppm')

# type "ls" + enter into the new terminal
cmd('sendkey l')
cmd('sendkey s')
cmd('sendkey ret')
time.sleep(0.5)
cmd('screendump /root/build/wm2.ppm')

# drag the titlebar: move to a point on the titlebar (not on buttons), press, drag, release
# cursor currently at (100,195); titlebar of Terminal 1 spans y=120..144, x=200..680
cmd('mouse_move 200 -70')   # -> approx (300,125)
cmd('mouse_button 1')
cmd('mouse_move 80 60')     # drag window by (+80,+60) while held
cmd('mouse_button 0')
time.sleep(0.3)
cmd('screendump /root/build/wm3.ppm')

# minimize it: window is now at (280,180) 480x320 after the drag.
# minimize "_" button center ~= (700,192); cursor currently at (392,184).
cmd('mouse_move 308 8')
cmd('mouse_button 1')
cmd('mouse_button 0')
time.sleep(0.3)
cmd('screendump /root/build/wm4.ppm')
PYEOF
sleep 1
kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null
ls -la ~/build/wm*.ppm
echo "--- boot log tail ---"
tail -5 "$LOG"
