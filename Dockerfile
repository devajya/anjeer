FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake ninja-build g++ \
    libpq-dev libcurl4-openssl-dev libssl-dev \
    ca-certificates git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY CMakeLists.txt CMakePresets.json ./
COPY server/CMakeLists.txt server/CMakeLists.txt

RUN cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DFETCHCONTENT_QUIET=OFF \
    || true

COPY . .

RUN cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --target server --parallel

RUN cp build/server/server /usr/local/bin/anjeer_server

FROM debian:bookworm-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libpq5 libcurl4 libssl3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /usr/local/bin/anjeer_server /app/anjeer_server
COPY config/prod.json /app/config/prod.json
COPY db/migrations /app/db/migrations

ENTRYPOINT ["/app/anjeer_server", "--config", "/app/config/prod.json"]
