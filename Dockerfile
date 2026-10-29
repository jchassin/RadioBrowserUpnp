# Multi-arch (amd64, arm64/aarch64) container for RadioBrowserUpnp.
# IMPORTANT for Docker Hub users:
#   - run with --network host (UPnP/SSDP discovery does not work through
#     the docker bridge)
#   - the web configuration ui is served on port 8028
#   - mount persistent storage at /data (config file: /data/radiobrowserupnp.json)

FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        apt-utils build-essential python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && ln -s /usr/bin/python3 /usr/local/bin/python

WORKDIR /src
COPY . .
RUN make -j"$(nproc)"

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        apt-utils libgcc-s1 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /data

COPY --from=build /src/Build/bin/radiobrowserupnp /usr/local/bin/radiobrowserupnp

# Persistent storage: mount a volume here; the config file (incl. the device
# uuid, which control points use to identify the server) lives in /data.
VOLUME ["/data"]

# Web configuration ui. The UPnP port is picked dynamically and requires
# host networking anyway, so only 8028 is meaningful to declare.
EXPOSE 8028

ENTRYPOINT ["/usr/local/bin/radiobrowserupnp"]
