FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    build-essential \
    libmicrohttpd-dev \
    libcurl4-openssl-dev \
    libcjson-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN gcc -o web_server server.c quiz_engine.c ai.c \
    -lmicrohttpd -lcurl -lcjson -pthread -lm -Wall -Wextra

COPY start.sh /app/start.sh
RUN chmod +x /app/start.sh

EXPOSE 8080

CMD ["/app/start.sh"]
