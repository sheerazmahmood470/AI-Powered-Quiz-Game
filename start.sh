#!/bin/sh
set -eu

PORT="${PORT:-10000}"

rm -f /tmp/server-stdin
mkfifo /tmp/server-stdin

tail -f /dev/null > /tmp/server-stdin &
./web_server < /tmp/server-stdin &
SERVER_PID=$!

# Do not let Nginx report a healthy website when the C API failed to start.
sleep 1
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "[STARTUP] C backend failed to start; stopping deployment."
    exit 1
fi

envsubst '${PORT}' < /app/nginx.conf.template > /etc/nginx/nginx.conf

nginx -t
exec nginx -g 'daemon off;'