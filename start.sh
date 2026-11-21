#!/bin/sh
mkfifo /tmp/server-stdin
tail -f /dev/null > /tmp/server-stdin &
exec ./web_server < /tmp/server-stdin
