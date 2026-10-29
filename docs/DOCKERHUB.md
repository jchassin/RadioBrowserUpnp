# RadioBrowserUpnp

UPnP/DLNA **Media Server** exposing the [radio-browser.info](https://www.radio-browser.info)
community radio directory (45 000+ internet radio stations) to your local network.
Browse it from any control point (Linn Kazoo, BubbleUPnP, mconnect, ...) and play
stations on any UPnP renderer. Includes an embedded web configuration ui.

## Requirements — read this first

1. **Run with host networking.** UPnP discovery uses SSDP multicast, which does
   not work through the docker bridge. Without `--network host`, the server runs
   but no control point will ever see it.
2. **Shared port: 8028.** This is the embedded web configuration ui, served on
   `http://<host>:8028/`. The UPnP port is chosen dynamically and needs no mapping.
3. **Mount persistent storage at `/data`.** The configuration file
   (`/data/radiobrowserupnp.json`) holds your preferences and the device uuid;
   control points use that uuid to identify the server (and its pinned stations).
   Without a volume, a new uuid is generated at each container recreation.

## Quick start

```bash
docker run -d --name radiobrowserupnp --network host \
  --hostname mynas \
  -v /srv/radiobrowserupnp:/data \
  --restart unless-stopped ghcr.io/jchassin/radiobrowserupnp:latest
```

Then:

* open `http://<host>:8028/` to configure (room name, countries, genres, api server)
* refresh the media server list in your control point: **Radio Browser \<room\>** appears

## Environment variables

| Variable | Default | Purpose |
|---|---|---|
| `RADIOBROWSERUPNP_CONFIG` | `/data/radiobrowserupnp.json` | config file path override |
| `RADIOBROWSERUPNP_SUBNET` | auto | force the LAN subnet (dotted quad, e.g. `192.168.1.0`) when several interfaces exist |

## Synology DSM (Container Manager)

* Image available for x86_64 and ARMv8 models
* Network: choose **Host** (mandatory, see above)
* Volume: map a shared folder (e.g. `/volume1/docker/radiobrowserupnp`) to `/data`
* After each config change in the web ui, restart the container to apply
  api-server / web-port changes (country and genre lists apply immediately)

## Navigation

```
Most popular          top stations by click count
By country           FR ES GB DE IT US (configurable in the web ui)
By genre             jazz rock classical news pop electronic (configurable)
By name              A-Z
└── each list: Hi-Res / Mid-Res / Low-Res quality containers
```

Control points with a generic DLNA search box can also use `Search`
(`dc:title contains "..."`). Linn Kazoo does not send Search to generic DLNA
servers; use **By name** there.

## Tags / architectures

* `latest`, `<version>` — `linux/amd64`, `linux/arm64`
