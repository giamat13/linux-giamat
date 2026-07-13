#!/bin/bash
LOG=~/build/qemu_out.txt
timeout 25 qemu-system-x86_64 -m 512 -cdrom ~/build/linux-giamat.iso -nographic -serial mon:stdio -display none \
	-netdev user,id=n0,hostfwd=tcp::8080-:80 -device e1000,netdev=n0 > "$LOG" 2>&1 &
QEMU_PID=$!
sleep 12
for i in 1 2 3 4 5; do
	curl -s -m 3 http://localhost:8080/ > ~/build/http_out.txt 2>&1
	[ -s ~/build/http_out.txt ] && break
	sleep 2
done
echo "--- HTTP GET / (attempt $i) ---"
cat ~/build/http_out.txt
curl -s -m 3 "http://localhost:8080/cgi-bin/cmd.sh?action=sysinfo" > ~/build/http_cgi_out.txt 2>&1
echo "--- HTTP GET /cgi-bin/cmd.sh?action=sysinfo ---"
cat ~/build/http_cgi_out.txt
wait $QEMU_PID 2>/dev/null
echo "--- boot log (boot/net/httpd markers) ---"
grep -n -i "\[boot\]\|custom\|Linux version\|init process\|eth0" "$LOG"
