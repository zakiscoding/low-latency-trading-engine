# Runtime image for the public demo stack (Bitstamp engine + paper exchange +
# telemetry uplink). Multi-arch: built for linux/arm64 for the iMac (Apple M1 in
# Colima) and linux/amd64 elsewhere. No compilers or source in the final image.
#   git archive HEAD | docker buildx build --platform linux/arm64 -f deploy/live.Dockerfile -t lle-live:arm64 -o type=docker,dest=- - > lle-live.tar
FROM ubuntu:24.04 AS engine
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends g++-14 cmake ninja-build libssl-dev ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
# LLE_NATIVE=OFF: the build machine (or emulator) isn't the machine it runs on.
RUN cmake -S . -B /build -G Ninja -DCMAKE_CXX_COMPILER=g++-14 -DLLE_NATIVE=OFF -DLLE_TESTS=OFF -DLLE_URING=OFF \
 && ninja -C /build crypto_engine paper_exchange recorder

FROM --platform=$BUILDPLATFORM golang:1.26 AS telemetry
ARG TARGETARCH
WORKDIR /src
COPY telemetry/ .
RUN CGO_ENABLED=0 GOARCH=$TARGETARCH go build -trimpath -ldflags "-s -w" -o /telemetry .

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libssl3 ca-certificates \
 && rm -rf /var/lib/apt/lists/* && useradd -r -u 10001 lle
COPY --from=engine /build/crypto_engine /build/paper_exchange /build/recorder /usr/local/bin/
COPY --from=telemetry /telemetry /usr/local/bin/telemetry
COPY tools/live.sh /usr/local/bin/live.sh
ENV B=/usr/local/bin TELEMETRY=/usr/local/bin/telemetry
USER lle
ENTRYPOINT ["/bin/bash", "/usr/local/bin/live.sh"]
