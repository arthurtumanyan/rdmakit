# Container image of arenakvd. Build from the repository root:
#   docker build -f arenakv/contrib/Dockerfile -t arenakvd .
# Run (TCP only; RDMA needs the host's RDMA devices and --network host):
#   docker run --rm -p 6379:6379 -p 11211:11211 -p 7600:7600 arenakvd
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends build-essential libibverbs-dev librdmacm-dev \
        libssl-dev && rm -rf /var/lib/apt/lists/*
COPY rdmaduct /src/rdmaduct
COPY arenakv /src/arenakv
RUN make -C /src/arenakv TLS=1 && cp /src/arenakv/build/$(uname -m)-tls/arenakvd /usr/local/bin/arenakvd

FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libibverbs1 librdmacm1t64 libssl3t64 \
        && rm -rf /var/lib/apt/lists/* && useradd --system --home /var/lib/arenakv arenakv \
        && mkdir -p /var/lib/arenakv && chown arenakv /var/lib/arenakv
COPY --from=build /usr/local/bin/arenakvd /usr/local/bin/arenakvd
USER arenakv
EXPOSE 6379 7600 11211
ENTRYPOINT ["/usr/local/bin/arenakvd", "--text-addr", "0.0.0.0", "--resp-port", "6379", \
            "--memcached-port", "11211", "--transport", "tcp", "--snapshot", "/var/lib/arenakv/arenakv.snap"]
