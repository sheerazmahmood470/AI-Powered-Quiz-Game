#!/bin/sh
set -eu

PORT="${PORT:-10000}"

rm -f /tmp/server-stdin
mkfifo /tmp/server-stdin

tail -f /dev/null > /tmp/server-stdin &
./web_server < /tmp/server-stdin &

envsubst '${PORT}' < /app/nginx.conf.template > /etc/nginx/nginx.conf

nginx -t
exec nginx -g 'daemon off;'