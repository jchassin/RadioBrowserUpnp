# RadioBrowserUpnp

UPnP/DLNA **Media Server** exposing the [radio-browser.info](https://www.radio-browser.info)
community radio directory (45 000+ internet radio stations, no account, no API key)
to the local network.

Built on [openhome/ohNet](https://github.com/openhome/ohNet) (vendored as a git
submodule). Ported from the Radio Browser media server originally implemented in
[ohPlayer](https://github.com/jchassin/ohPlayer), adapted to run standalone
(no ohMediaPlayer dependency, no renderer required).

Control points (BubbleUPnP, mconnect, kazoo, Linn app, VLC, Hi-Fi streamers...)
browse it like any UPnP MediaServer; playing a station pushes its stream url
to whatever renderer the control point selects.

## Features

* Standard `upnp.org:ContentDirectory:1` + `upnp.org:ConnectionManager:1` services
* Navigation: **Most popular**, **By country** (configurable short list),
  **By genre** (configurable), **By name (A-Z)**; each list is split into
  Hi-Res / Mid-Res / Low-Res quality containers
* `Search` action (`dc:title contains "..."`) for control points with a search box
* Broken stations hidden (`lastcheckok`), duplicate stream urls deduplicated
* 60 s result cache for responsive paging
* Embedded web configuration ui (default port `8028`): room name, country list,
  genre list, api server, web port; persisted as JSON

## Object ids

```
0                root
top              most popular stations
country          country list (short, configurable)
country/<code>   quality containers for that country
country/<code>/{hi,mid,low}   stations
genre            genre list
genre/<tag>/{hi,mid,low}      stations
name             A-Z
name/<letter>/{hi,mid,low}    stations
```

## Build (Linux amd64 / aarch64)

Requirements: `build-essential` (gcc/g++, make), `python3` (ohNet code generator).
`rsync` is not required (the build falls back to `cp -u`).

```bash
git clone --recursive https://github.com/jchassin/RadioBrowserUpnp
cd RadioBrowserUpnp
make            # builds thirdparty/ohNet then the server
./Build/bin/radiobrowserupnp
```

The binary is `Build/bin/radiobrowserupnp`. It announces itself via SSDP on all
adapters and picks a free UPnP port.

### Configuration

### Adapter selection / SSDP visibility

On hosts with several interfaces (docker bridges, VPNs, bonding), ohNet may pick
the wrong one and the device stays invisible to control points. The server now
logs all adapters at startup and picks the first `eth*`/`enp*`/`ens*`/`em*`
interface; you can force the subnet explicitly:

```bash
docker run ... -e RADIOBROWSERUPNP_SUBNET=192.168.1.0 ...   # subnet address
```

Diagnostic: `docker logs radiobrowserupnp` prints every adapter (name, address,
subnet) and which one was chosen.

The config file (JSON) is read at startup and written on shutdown; path:

* `$RADIOBROWSERUPNP_CONFIG` if set
* else `/data/radiobrowserupnp.json` if a `/data` directory exists (container layout)
* else `~/.config/radiobrowserupnp.json`
* else `/etc/radiobrowserupnp.json`

```json
{
  "room": "Salon",
  "api_server": "de1.api.radio-browser.info",
  "web_port": 8028,
  "countries": ["FR", "ES", "GB", "DE", "IT", "US"],
  "genres": ["jazz", "rock", "classical", "news", "pop", "electronic"]
}
```

The embedded web ui on `http://<host>:8028/` edits the same values (country/genre
lists apply immediately; api server / web port apply after restart).

## Deployment

### Ubuntu / Debian (amd64, aarch64)

```bash
sudo make install      # installs the binary to /usr/local/bin
sudo cp radiobrowserupnp.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now radiobrowserupnp
```

### Container (Docker Hub)

The image is published for `linux/amd64` and `linux/arm64`. Three things matter:

1. **Host networking is required** — UPnP discovery uses SSDP multicast, which
   does not traverse the docker bridge. Without `--network host` the server
   runs but no control point will see it.
2. **Port 8028** — the embedded web configuration ui
   (`http://<host>:8028/`). The UPnP port itself is picked dynamically and
   needs no mapping.
3. **Persistent storage at `/data`** — the config file
   (`/data/radiobrowserupnp.json`, incl. the device uuid your control points
   use to identify the server and its pins) must survive restarts. Without a
   volume a new uuid is generated at each container recreation.

```bash
docker run -d --name radiobrowserupnp --network host \
  -v /path/on/host:/data \
  --restart unless-stopped radiobrowserupnp
```

Optional environment variables:

| Variable | Default | Purpose |
|---|---|---|
| `RADIOBROWSERUPNP_CONFIG` | `/data/radiobrowserupnp.json` (if `/data` exists) | config file path override |
| `RADIOBROWSERUPNP_SUBNET` | auto | force the LAN subnet (dotted quad, e.g. `192.168.1.0`) on multi-interface hosts |

Fixing the hostname (`--hostname mynas`) is recommended so the device keeps a
stable identity even if the config volume is lost.

### Synology NAS

Two options:

1. **Container Manager (Docker)** - recommended:
   * Import the Docker Hub image (or build locally: `docker build -t radiobrowserupnp .`,
     works on x86_64 and ARMv8 models)
   * Run with **host networking** (required for SSDP discovery) and mount
     a persistent shared folder at `/data`:
     ```bash
     docker run -d --name radiobrowserupnp --network host \
       --hostname sun \
       -v /volume1/docker/radiobrowserupnp:/data \
       --restart unless-stopped radiobrowserupnp
     ```
2. **Native package**: build on a matched toolchain (DSM 7 uses its own gcc via
   the Synology toolkit); the Makefile is toolchain-agnostic (`CC/CXX` overridable,
   little-endian assumed on all supported models). Install like the systemd path
   above using the DSM Task Scheduler or a wrapper script.

### Any UPnP-capable renderer/DLNA app

The device appears as **Radio Browser \<room\>** in control points.

## Image signing & attestations (supply chain)

Each published image carries two layers of build evidence:

* **SLSA provenance + SBOM** (generated by BuildKit, stored in the image
  index): records the exact sources (repo, commit), builder and component
  inventory. Note: BuildKit attestations are not cryptographically signed;
  inspect them with `docker buildx imagetools inspect <image>`
* **cosign signature** (keyless, via the GitHub Actions OIDC identity): the
  cryptographically verifiable guarantee that the image digest was published
  by THIS repository's workflow. A CI job signs each build and immediately
  verifies the signature against the workflow identity - a mismatch fails
  the workflow

### Verify an image yourself

```bash
cosign verify \
    --certificate-identity-regexp "^https://github.com/jchassin/RadioBrowserUpnp/.github/workflows/" \
    --certificate-oidc-issuer https://token.actions.githubusercontent.com \
    ghcr.io/jchassin/radiobrowserupnp:0.1.0
```

Note: signatures live on GHCR. The Docker Hub mirror images are the same
digests, verifiable via the GHCR copy.

## Project layout

```
src/main.cpp                  entry point (ohNet init, udn, signal handling)
src/RadioBrowserClient.{h,cpp}  radio-browser.info JSON API client
                              (ohNet SocketTcpClient, http/1.1 + chunked,
                              minimal JSON parser)
src/RadioBrowserMediaServer.{h,cpp}  ContentDirectory/ConnectionManager providers
src/WebConfig.{h,cpp}         embedded configuration web ui + preferences store
src/generated/                Dv providers generated from ohNet's
                              MediaServer_3 service xml via ServiceGen.py
thirdparty/ohNet              git submodule (openhome/ohNet)
```
