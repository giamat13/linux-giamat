#!/bin/bash
LOG=~/build/qemu_fb.log
rm -f ~/build/qmp-sock ~/build/screen.ppm
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
time.sleep(0.5)
s.recv(4096)
s.sendall(b'screendump /root/build/screen.ppm\n')
time.sleep(1)
print(s.recv(4096))
PYEOF
sleep 1
kill $QEMU_PID 2>/dev/null
wait $QEMU_PID 2>/dev/null
ls -la ~/build/screen.ppm
echo "--- boot log tail ---"
grep -n -i "simple-framebuffer\|simpledrm\|fbcon\|Console: switching\|Run /init\|desktop" "$LOG"
