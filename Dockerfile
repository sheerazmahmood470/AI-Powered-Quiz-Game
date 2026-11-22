
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
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN gcc -o web_server server.c quiz_engine.c ai.c storage.c \
    -I/usr/include/postgresql \
    -lmicrohttpd -lcurl -lcjson -lpq -pthread -lm -Wall -Wextra

COPY start.sh /app/start.sh
RUN chmod +x /app/start.sh

EXPOSE 10000

CMD ["/app/start.sh"]
