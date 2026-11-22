
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    libmicrohttpd-dev \
    libcurl4-openssl-dev \
    libcjson-dev \
    libpq-dev \
    nginx \
    gettext-base \
    ca-certificates \
    nodejs \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN gcc -o web_server server.c quiz_engine.c ai.c storage.c \
    -I/usr/include/postgresql \
    -lmicrohttpd -lcurl -lcjson -lpq -pthread -lm -Wall -Wextra

RUN gcc -o /tmp/mcq_quality_test tests/mcq_quality_test.c -lcurl -lcjson \
    && /tmp/mcq_quality_test \
    && node --check frontend/script.js \
    && node tests/mcq_format.test.js \
    && rm -f /tmp/mcq_quality_test

COPY start.sh /app/start.sh
RUN chmod +x /app/start.sh

EXPOSE 10000

CMD ["/app/start.sh"]
