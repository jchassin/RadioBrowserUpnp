# Multi-arch (amd64, arm64/aarch64) container for RadioBrowserUpnp.
# Synology: import via Container Manager; the app needs host networking
# (UPnP uses SSDP multicast) and a persistent volume for the config file.

FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && ln -s /usr/bin/python3 /usr/local/bin/python

WORKDIR /src
COPY . .
RUN make -j"$(nproc)"

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libgcc-s1 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /config

COPY --from=build /src/Build/bin/radiobrowserupnp /usr/local/bin/radiobrowserupnp

ENV RADIOBROWSERUPNP_CONFIG=/config/radiobrowserupnp.json

# UPnP (SSDP) requires host networking to work on the LAN.
EXPOSE 8028

ENTRYPOINT ["/usr/local/bin/radiobrowserupnp"]
