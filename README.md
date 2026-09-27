# esphome-ipv6-only

Run an ESPHome Wi‑Fi device on an IPv6‑only network: router advertisements
(SLAAC), no DHCPv4 server, no IPv4 address at all.

Upstream ESPHome can't do this. Its `wifi` component only reports *connected*
once it holds an IPv4 address, and it only starts IPv6 address configuration
from the DHCPv4 "got IP" event, so on a v6‑only network the device sits in
*connecting* forever ([esphome/issues#7117]). This is a drop‑in replacement for
the `wifi` component that adds one option, `ipv6_only: true`.

Everything downstream already works without IPv4: the socket layer binds
`AF_INET6` sockets when IPv6 is enabled, so the native API, OTA, logger and
web server listen on v6, and ESP‑IDF's mDNS announces AAAA records.

It also ships the same `ipv6_only` option for the `ethernet` component, a
small override of the `sntp` component (lwIP's SNTP client resolves server
names IPv4‑first and never falls back to AAAA), and an IPv6‑capable copy of
[esphome-gateway-watchdog](https://github.com/davidcoulson/esphome-gateway-watchdog).

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
   build of this fork, `ipv6_only` or not, and is harmless on dual‑stack.
2. **The connect gate accepts IPv6 alone** (`ipv6_only` only). `connected`
   becomes `associated && ipv6_addresses >= network.min_ipv6_addr_count`.
3. **The DHCPv4 client is never started** (`ipv6_only` only). Nothing IPv4 goes
   on the wire. This also matters for routing: esp_netif only makes an interface
   lwIP's default route on link‑up when its DHCP client is stopped, otherwise it
   waits for a lease and `ip6_route()` has no default netif for off‑link
   destinations. Cosmetic side effect: esp_netif then takes its static‑IPv4
   path on association and, seeing 0.0.0.0, logs one
   `E esp_netif_handlers: invalid static ip` line per connect. It is harmless
   and can't be silenced per tag (ESPHome builds IDF without dynamic log
   levels).

With `ipv6_only`, DNS servers are learned from the RA RDNSS option (RFC 8106)
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

The `sntp` override (ESP32 + `ipv6_only` builds only; other platforms are
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

### Ethernet

`ethernet: ipv6_only: true` does the equivalent for wired nodes. Upstream
already creates the link‑local address on link‑up (with a retry), so only two
things change: the connect gate becomes `ipv6_addresses >= min_ipv6_addr_count`
with no `got_ipv4_address_` term, and the DHCPv4 client is left stopped after
`start_connect_()` instead of being started. Stateless DHCPv6 is enabled after
the link‑local address is created, and the same RDNSS/DHCPv6 sdkconfig options
are set. Same validation rules as Wi‑Fi. ESP32 only (the RP2040 backend is
untouched). Compile‑tested on an ESP32‑POE‑ISO (LAN8720) config.

### gateway_watchdog

Upstream esphome-gateway-watchdog is inert on an IPv6‑only node rather than
wrong: it reads the IPv4 gateway from `esp_netif_get_ip_info()`, gets 0.0.0.0,
and never starts a ping session (no reboots, sensors stay NaN). The copy in
`components/gateway_watchdog/` (diff in `gateway-watchdog-ipv6.patch`, applies
to that repo including its host tests, which pass):

- targets are `ip_addr_t`, so `target:` accepts an IPv6 literal and `esp_ping`
  sends ICMPv6 echo for it;
- with no `target:` and no IPv4 gateway, it watches the first live IPv6 default
  router from lwIP's ND6 default‑router list (`lwip/priv/nd6_priv.h`, a private
  header that ESP‑IDF ships). That is the router's link‑local address, i.e.
  exactly the next hop the node forwards through.

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
| Compile‑tested | ESPHome 2026.9.0 with ESP‑IDF 5.5.5 and 6.1.0: `esp32dev` Wi‑Fi (`ipv6_only` true and false), `esp32-c3-devkitm-1` Wi‑Fi (the hardware test config), `esp32-poe-iso` Ethernet; sntp and gateway_watchdog overrides included. The esp_netif and lwIP code paths the fork relies on are identical in 5.5 and 6.1. |
| Hardware‑tested | ESP32‑C3, ESP‑IDF 6.1.0, IPv6‑only Wi‑Fi (SLAAC, no DHCPv4): connects, is discovered over mDNS, holds a Home Assistant API connection, and takes OTA updates and serves `esphome logs`, all over IPv6. |
| Not supported | ESP8266, RP2040, LibreTiny (their status comes from the Arduino `WL_CONNECTED` flag, which itself waits for IPv4). |
| Network | Router advertisements with a prefix for SLAAC. RDNSS or DHCPv6 "O" flag if the device must resolve names. |

## Install

```yaml
external_components:
  - source: github://davidcoulson/esphome-ipv6-only@main
    components: [wifi, wifi_info, sntp]   # add ethernet, ethernet_info, gateway_watchdog as needed

network:
  enable_ipv6: true
  min_ipv6_addr_count: 2    # link-local + one SLAAC address; 1 = link-local is enough

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ipv6_only: true

time:
  - platform: sntp
    servers: [2.pool.ntp.org]   # has AAAA records; or a literal IPv6 address
```

`example.yaml` is a complete device config. `c3-test.yaml` is the hardware test
config for an ESP32‑C3 on ESP‑IDF 6.1.0 with the watchdog in report‑only mode and
verbose Wi‑Fi/SNTP logs; its header lists what to look for in the log. Validation rejects `ipv6_only`
without `enable_ipv6: true`, with `min_ipv6_addr_count: 0`, or together with
`manual_ip`.

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
apply cleanly across most releases. `gateway-watchdog-ipv6.patch` applies to
the esphome-gateway-watchdog repo the same way.

## Upstream

The proper fix is a PR against esphome/esphome. Change 1 is a plain bug fix.
Changes 2 and 3 are the `ipv6_only` option and are orthogonal to the open
[esphome/esphome#14526] ("Allow disabling IPv4"), which compiles IPv4 out of
lwIP but still registers `wifi` and `ethernet` as requiring it; this fork keeps
IPv4 in the stack and only stops depending on a lease.

[esphome/esphome#14526]: https://github.com/esphome/esphome/pull/14526

## Layout

```
components/wifi/          full copy of ESPHome 2026.9.0 wifi + patch
components/ethernet/      full copy of ESPHome 2026.9.0 ethernet + patch
components/sntp/          full copy of ESPHome 2026.9.0 sntp + patch
components/wifi_info/, components/ethernet_info/  ip_address sensor fix
components/gateway_watchdog/  esphome-gateway-watchdog with IPv6 targets / ND6 router
upstream.patch            the diff against that ESPHome release (wifi, ethernet, sntp, wifi_info, ethernet_info)
gateway-watchdog-ipv6.patch   the diff against esphome-gateway-watchdog (incl. tests)
example.yaml              complete IPv6-only device config
c3-test.yaml              ESP32-C3 hardware test config (ESP-IDF 6.1.0)
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
4. `[gateway_watchdog] watching fe80::…` (the ND6 default router)

If `[sntp]` repeats `resolver not ready`, the RAs carry neither RDNSS nor the
O flag with a DHCPv6 server behind it; fix that on the router or use a literal
`servers:` address.
