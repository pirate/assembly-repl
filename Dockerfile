FROM debian:bookworm-slim AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential clang make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY Makefile ./Makefile
COPY src ./src

RUN make clean \
    && make CC=clang CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -O2" all \
    && mkdir -p /out \
    && cp asmrepl /out/assembly-repl \
    && cp language-repl /out/language-repl \
    && strip /out/assembly-repl /out/language-repl \
    && chmod 0755 /out/assembly-repl /out/language-repl

FROM scratch AS export
COPY --from=build /out/ /
