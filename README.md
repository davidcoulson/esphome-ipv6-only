# esphome-ipv6-only

Run an ESPHome Wi‑Fi device on an IPv6‑only network: router advertisements
(SLAAC), no DHCPv4 server, no IPv4 address at all.

Upstream ESPHome can't do this. Its `wifi` component only reports *connected*
once it holds an IPv4 address, and it only starts IPv6 address configuration
from the DHCPv4 "got IP" event, so on a v6‑only network the device sits in
*connecting* forever ([esphome/issues#7117]). This fork adds one option,
`network: enable_ipv4: false`, and drop‑in replacements for the `network`,
`wifi` and `ethernet` components that honour it. List `network` in
`external_components` along with the interface, or ESPHome's own `network`
component rejects the option. (It was `wifi: ipv6_only: true` and then
`wifi: enable_ipv4: false` earlier on 2026‑09‑27; both old forms now fail
validation with a pointer to `network:`.)

Everything downstream already works without IPv4: the socket layer binds
`AF_INET6` sockets when IPv6 is enabled, so the native API, OTA, logger and
web server listen on v6, and ESP‑IDF's mDNS announces AAAA records.

It also covers the `ethernet` component, a
small override of the `sntp` component (lwIP's SNTP client resolves server
names IPv4‑first and never falls back to AAAA), and an IPv6‑capable copy of
[esphome-gateway-watchdog](https://github.com/davidcoulson/esphome-gateway-watchdog) (now upstream there: IPv6 since v1.2.0, `prefix_router` since v1.3.0).

[esphome/issues#7117]: https://github.com/esphome/issues/issues/7117

## What it changes

Three changes to `wifi_component_esp_idf.cpp`, one option in `__init__.py`.
`upstream.patch` is the exact diff against the ESPHome release it was forked
from.

1. **SLAAC starts on association.** `esp_netif_create_ip6_linklocal()` moves
   from the `IP_EVENT_STA_GOT_IP` (DHCPv4) handler to `WIFI_EVENT_STA_CONNECTED`,
   which is also where the ESP‑IDF examples do it. This is the actual root cause
   of "never connects": without a v4 lease upstream never creates the link‑local
   address, so no router solicitation is ever sent. This change applies to every
   build of this fork, IPv4 enabled or not, and is harmless on dual‑stack.
2. **The connect gate accepts IPv6 alone** (`enable_ipv4: false` only). `connected`
   becomes `associated && ipv6_addresses >= network.min_ipv6_addr_count`.
3. **The DHCPv4 client is never started** (`enable_ipv4: false` only). Nothing IPv4 goes
   on the wire. This also matters for routing: esp_netif only makes an interface
   lwIP's default route on link‑up when its DHCP client is stopped, otherwise it
   waits for a lease and `ip6_route()` has no default netif for off‑link
   destinations. Cosmetic side effect: esp_netif then takes its static‑IPv4
   path on association and, seeing 0.0.0.0, logs one
   `E esp_netif_handlers: invalid static ip` line per connect. It is harmless
   and can't be silenced per tag (ESPHome builds IDF without dynamic log
   levels).

With `enable_ipv4: false`, DNS servers are learned from the RA RDNSS option (RFC 8106)
and from stateless DHCPv6 (RA "O" flag): the fork sets
`CONFIG_LWIP_IPV6_RDNSS_MAX_DNS_SERVERS=2`, `CONFIG_LWIP_IPV6_DHCP6=y` and calls
`dhcp6_enable_stateless()` after the link‑local address is created.

### SNTP

Upstream hands server *names* to lwIP's SNTP client, which resolves them with
`dns_gethostbyname()`, i.e. `LWIP_DNS_ADDRTYPE_IPV4_IPV6`: query A first, and
only query AAAA if the A lookup *fails*. Every public pool name has A records,
so the client gets an unreachable IPv4 address, `udp_sendto()` fails with no
route, and it retries forever. ESP‑IDF exposes no Kconfig to flip that default,
and ESPHome's `servers:` validator also rejects IPv6 literals.

The `sntp` override (ESP32 builds with `enable_ipv4: false` only; other platforms are
untouched):

- waits in `loop()` for the network *and* a usable resolver (the RDNSS / DHCPv6
  DNS server may arrive a moment after the address), retrying every 5 s;
- resolves each server itself with `dns_gethostbyname_addrtype(...,
  LWIP_DNS_ADDRTYPE_IPV6_IPV4)` and hands the *address* to lwIP with
  `esp_sntp_setserver()`, then starts the client;
- re‑resolves at every `update_interval`, since a set address is static where
  a set name would have been re‑resolved per request;
- accepts IPv4/IPv6 literals in `servers:` (e.g. your router's address).

Pick servers that have AAAA records. In the public pool only `2.pool.ntp.org`
does; `0.`/`1.pool.ntp.org` and the ESPHome default list will not sync. Or use
a literal IPv6 address, or a name in your own DNS with only an AAAA record
(that last one works even with upstream, because the A lookup then fails and
lwIP does fall back to AAAA).

### Ethernet link-local fix (dual-stack too)

Stock ESPHome tries to create the Ethernet IPv6 link-local address once when
the link comes up (usually too early) and retries exactly once after the DHCPv4
lease, then gives up. When that retry misses, the board runs with no IPv6
address at all until the link drops; seen on both GPS NTP boards (W5500 and
P4/IP101). The override keeps retrying every 5 s until link-local is usable,
logs `IPv6 link-local … ready`, and warns if duplicate address detection
fails. It applies to every Ethernet build that pulls this `ethernet`, with or
with IPv4 enabled.

It also registers the IPv6 all-nodes multicast address (`33:33:00:00:00:01`)
with the Ethernet MAC. The ESP32/P4 internal EMAC runs a hardware address
filter with "pass all multicast" off, and lwIP never adds all-nodes, so every
router advertisement was dropped in hardware: link-local worked, SLAAC never
ran, and `IPv6 Router: none`. The W5500 passes IPv6 multicast regardless (it
logs one "IPv6 multicast is always filtered in by W5500" warning at boot).

Confirmed on hardware (dual-stack, ESP-IDF 6.1.0): the ESP32-P4 GPS NTP board
(IP101) and the ESP32-S3 GPS NTP board (W5500) both get their link-local, their
SLAAC ULA and the RA default router, and serve NTP over IPv6.

### Ethernet

With `ethernet:` the same `network: enable_ipv4: false` applies to wired nodes. Upstream
already creates the link‑local address on link‑up (with a retry), so only two
things change: the connect gate becomes `ipv6_addresses >= min_ipv6_addr_count`
with no `got_ipv4_address_` term, and the DHCPv4 client is left stopped after
`start_connect_()` instead of being started. Stateless DHCPv6 is enabled after
the link‑local address is created, and the same RDNSS/DHCPv6 sdkconfig options
are set. Same validation rules as Wi‑Fi. ESP32 only (the RP2040 backend is
untouched). Compile‑tested on an ESP32‑POE‑ISO (LAN8720) config.

### gateway_watchdog

Not part of this repo any more: IPv6 support is in
[esphome-gateway-watchdog](https://github.com/davidcoulson/esphome-gateway-watchdog)
itself since **v1.2.0**. With no IPv4 gateway it watches the IPv6 default router
learned from router advertisements, and binds the ping to that interface so
link‑local echo works. Use **v1.3.0** or later, with `prefix_router`:

```yaml
external_components:
  - source: github://davidcoulson/esphome-gateway-watchdog@v1.3.0
    components: [gateway_watchdog]

gateway_watchdog:
  prefix_router: true
```

**Why `prefix_router`:** the default router is always the router's link‑local
address (router advertisements come from it), and many routers and firewalls
don't answer echo requests there. This network's firewall is one of them: it
ignores pings to `fe80::21b:17ff:fe00:140` but answers on
`fd69:deca:fbad:4::1`. Watching the link‑local address then reads as 100%
packet loss forever, so the watchdog never arms. `prefix_router: true` watches
`::1` in the node's own /64 instead (ULA before global, preferred addresses
only), and still works when no default router is advertised at all. On
hardware it logs `watching FD69:DECA:FBAD:4::1 on netif 2`, with 0% packet
loss. If your router doesn't sit at `::1`, set `target:` to its address.

### IPv6 router in the connection summary

The Wi‑Fi and Ethernet connection summaries (logged on connect and whenever
`esphome logs` attaches) list each IPv6 default router learned from router
advertisements, with its remaining lifetime:

```
[C][wifi]:   IPv6 Router: fe80::21b:17ff:fe00:140 (lifetime 1800 s)
```

`IPv6 Router: none` means no router advertisement offered a default route.

### wifi_info / ethernet_info

Upstream's `ip_address` text sensor publishes slot 0 of the address array, which
is always the IPv4 slot, so on an IPv6‑only node it reads `0.0.0.0` (found on
the first hardware test). The overrides publish IPv4 if set, else the first
non‑link‑local IPv6 address (ULA or global), else the link‑local. Confirmed on
the ESP32‑C3: the sensor shows the `fd69:…` ULA.

`ignore_link_local: true` hides `fe80::` addresses from both the main value and
the `address_0`…`address_4` sub‑sensors. Until a ULA or global address exists
the main value is then empty rather than the link‑local.

```yaml
text_sensor:
  - platform: wifi_info        # or ethernet_info
    ip_address:
      name: IP
      ignore_link_local: true
```

## Requirements

| | |
|---|---|
| ESPHome | 2026.9.0 (the fork is a copy of that release's `wifi`, `ethernet` and `sntp` components; see *Rebasing*) |
| Platform | ESP32 family, ESP‑IDF framework. The Arduino framework on ESP32 shares the same code path and validates, but is untested. |
| Compile‑tested | ESPHome 2026.9.0 with ESP‑IDF 5.5.5 and 6.1.0: `esp32dev` Wi‑Fi (`enable_ipv4` true and false), `esp32-c3-devkitm-1` Wi‑Fi (the hardware test config), `esp32-poe-iso` Ethernet; sntp and gateway_watchdog overrides included. The esp_netif and lwIP code paths the fork relies on are identical in 5.5 and 6.1. |
| Hardware‑tested | ESP32‑C3, ESP‑IDF 6.1.0, IPv6‑only Wi‑Fi (SLAAC, no DHCPv4): connects, is discovered over mDNS, holds a Home Assistant API connection, takes OTA updates and serves `esphome logs`, learns DNS from the router advertisement, syncs SNTP, and the gateway watchdog (v1.3.0, `prefix_router`) watches `::1` in the node's /64 with 0% packet loss, all over IPv6. |
| Not supported | ESP8266, RP2040, LibreTiny (their status comes from the Arduino `WL_CONNECTED` flag, which itself waits for IPv4). |
| Network | Router advertisements with a prefix for SLAAC. RDNSS or DHCPv6 "O" flag if the device must resolve names. |

## Install

```yaml
external_components:
  - source: github://davidcoulson/esphome-ipv6-only@main
    components: [network, wifi, wifi_info, sntp]   # add ethernet, ethernet_info as needed

network:
  enable_ipv6: true
  min_ipv6_addr_count: 2    # link-local + one SLAAC address; 1 = link-local is enough
  enable_ipv4: false        # needs `network` in the components list above

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

time:
  - platform: sntp
    servers: [2.pool.ntp.org]   # has AAAA records; or a literal IPv6 address
```

`example.yaml` is a complete device config. `c3-test.yaml` is the hardware test
config for an ESP32‑C3 on ESP‑IDF 6.1.0 with the watchdog in report‑only mode and
verbose Wi‑Fi/SNTP logs; its header lists what to look for in the log. Validation rejects `enable_ipv4: false`
without `enable_ipv6: true`, with `min_ipv6_addr_count: 0`, or together with a
`manual_ip` on the interface.

## Caveats

- **Only inbound and IPv6‑capable outbound traffic works.** The native API,
  OTA, logs, web server, mDNS and the patched `sntp` are fine. Anything the
  device initiates to an IPv4 address or over a v4‑only protocol does not:
  `mqtt` to a v4 broker, `wireguard`, `http_request` to v4 hosts. Other
  components that resolve names go through the same IPv4‑first lwIP default
  and hit the same problem `sntp` had.
- **Home Assistant** must reach the device over IPv6 and its zeroconf must
  listen on v6. Add the device by its `.local` name or a literal address if
  discovery doesn't find it.
- **`use_address`** defaults to `<name>.local`. `esphome run`/`logs` will
  resolve that via mDNS to the AAAA record. Set `use_address: "[2001:db8::1]"`
  if your resolver won't.
- **The AP / captive portal fallback** is untouched and still hands out IPv4
  leases on the softAP; that is independent of the station side.
- `wifi_info` sensors and the "Connected" log line show `0.0.0.0` for the v4
  fields; the IPv6 addresses are listed after it.
- One `E esp_netif_handlers: invalid static ip` line per association (see above).
- `min_ipv6_addr_count: 1` connects on link‑local alone. That's only useful when
  Home Assistant is on the same link; use `2` otherwise.

## Rebasing onto a new ESPHome release

The fork is the whole `wifi` component, so it must track upstream.

```bash
git clone --depth 1 --branch <release> https://github.com/esphome/esphome
cp -r esphome/esphome/components/network components/network
cp -r esphome/esphome/components/wifi components/wifi
cp -r esphome/esphome/components/sntp components/sntp
cp -r esphome/esphome/components/ethernet components/ethernet
cp -r esphome/esphome/components/wifi_info components/wifi_info
cp -r esphome/esphome/components/ethernet_info components/ethernet_info
patch -p1 -d . < upstream.patch          # paths are esphome/components/<name>/...
esphome config example.yaml
```

Then regenerate `upstream.patch` with `diff -ruN` against the pristine copy.
The patch is small (~300 changed lines) and touches stable code; expect it to
apply cleanly across most releases.

## Upstream

The proper fix is a PR against esphome/esphome. Change 1 is a plain bug fix.
Changes 2 and 3 are the `enable_ipv4` option and are orthogonal to the open
[esphome/esphome#14526] ("Allow disabling IPv4"), which compiles IPv4 out of
lwIP but still registers `wifi` and `ethernet` as requiring it; this fork keeps
IPv4 in the stack and only stops depending on a lease.

[esphome/esphome#14526]: https://github.com/esphome/esphome/pull/14526

## Layout

```
components/network/       full copy of ESPHome 2026.9.0 network + the enable_ipv4 option
components/wifi/          full copy of ESPHome 2026.9.0 wifi + patch
components/ethernet/      full copy of ESPHome 2026.9.0 ethernet + patch
components/sntp/          full copy of ESPHome 2026.9.0 sntp + patch
components/wifi_info/, components/ethernet_info/  ip_address sensor fix
upstream.patch            the diff against that ESPHome release (network, wifi, ethernet, sntp, wifi_info, ethernet_info)
example.yaml              complete IPv6-only device config
c3-test.yaml              ESP32-C3 hardware test config (ESP-IDF 6.1.0)
p4-eth-test.yaml          ESP32-P4 + IP101 Ethernet hardware test config
s3-eth-test.yaml          ESP32-S3 + W5500 Ethernet hardware test config
tests/compile-test.yaml   secrets-free Wi-Fi config for `esphome config` / `compile`
tests/compile-test-ethernet.yaml  same for Ethernet (ESP32-POE-ISO)
tests/components/wifi/    ESPHome-style component test config
```

## Hardware test

`c3-test.yaml` targets an ESP32‑C3 on ESP‑IDF 6.1.0 with the watchdog in
report‑only mode and verbose `wifi`/`sntp` logs. Expected log sequence:

1. `[wifi] Connected` with only IPv6 addresses listed (the v4 fields show 0.0.0.0)
2. one `E esp_netif_handlers: invalid static ip` line (expected, see above)
3. `[sntp] Server 0 '2.pool.ntp.org' -> 2xxx::…` then `Synchronized time`
4. `[gateway_watchdog] watching <prefix>::1 on netif N` (`prefix_router`:
   `::1` in the node's /64, e.g. `FD69:DECA:FBAD:4::1`)

If `[sntp]` repeats `resolver not ready`, the RAs carry neither RDNSS nor the
O flag with a DHCPv6 server behind it; fix that on the router or use a literal
`servers:` address.
