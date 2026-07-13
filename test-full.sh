#!/bin/bash
LOG=~/build/qemu_full.log
rm -f ~/build/qmp-sock ~/build/f1.ppm ~/build/f2.ppm ~/build/f3.ppm ~/build/f4.ppm
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

# click SYSTEM icon (~100,75) from center (512,384)
cmd('mouse_move -412 -309')
cmd('mouse_button 1'); cmd('mouse_button 0')
time.sleep(0.5)
cmd('screendump /root/build/f1.ppm')  # SYSTEM output window

# click TERMINAL icon (~100,195); cursor now at (100,75)
cmd('mouse_move 0 120')
cmd('mouse_button 1'); cmd('mouse_button 0')
time.sleep(0.5)
cmd('screendump /root/build/f2.ppm')  # terminal opened, focused

# type WITHOUT pressing enter yet -- proves raw mode delivers chars immediately
cmd('sendkey e'); cmd('sendkey c'); cmd('sendkey h'); cmd('sendkey o')
time.sleep(0.3)
cmd('screendump /root/build/f3.ppm')  # should show "echo" typed live, no enter pressed

cmd('sendkey spc'); cmd('sendkey h'); cmd('sendkey i'); cmd('sendkey ret')
time.sleep(0.5)
cmd('screendump /root/build/f4.ppm')  # should show command executed
PYEOF
sleep 1
kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null
ls -la ~/build/f*.ppm
