# ESP8266 IPv6-only port: handover (2026-10-25)

Goal: run `ipv6_only` on ESP8266 (Athom RGBCT bulbs, no UART, so a bad flash bricks them).
Test module: generic ESP8266 on USB, config `zz-esp8266-v6-test.yaml` on the HA box
(`/config/esphome`, uses the fleet's common packages and a local copy of the components in
`/config/esphome/ipv6-only-8266/components`). `esp8266-test.yaml` here is the standalone version
(needs `wifi_ssid`, `wifi_password`, `ota_password` in `secrets.yaml`).

Config and secrets: pull `zz-esp8266-v6-test.yaml`, `common/` and `secrets.yaml` from the HA box with
`ssh hassio@10.2.3.6` (read-only for that user; `sudo -n cat` works; no sftp, so use `ssh ... cat > local` or
tar over ssh). Never print `secrets.yaml`. Compile there with
`sudo docker exec app_5c53de3b_esphome esphome compile /config/esphome/zz-esp8266-v6-test.yaml`.

## Done (compiles; RAM 43.6%, flash 467 KB of 1 MB)
- `components/wifi/__init__.py`: `ipv6_only` allowed on ESP8266; ESP32-only sdkconfig options guarded.
- `components/wifi/wifi_component_esp8266.cpp` (all under `USE_WIFI_IPV6_ONLY`):
  - DHCPv4 client stopped, never started; upstream's blocking IPv6 wait loop skipped.
  - `wifi_sta_connect_status_()` reports CONNECTED once associated and >= `min_ipv6_addr_count`
    addresses are on the STA netif (`addr.ifnumber() == STATION_IF`; the softAP netif has its own link-local).
  - `wifi_loop_()`: while associated with no global address, recreate the link-local every 10 s
    (re-runs DAD, re-arms lwIP's few router solicitations). Untested guess.

## First hardware result (before the last two fixes)
Associates, gets only the link-local; no 2xxx/ULA address ever (network has RAs, other devices are fine).
It then reported "Connected" falsely (AP link-local counted) and shut the fallback AP.

## Next
1. Flash current build over USB; read the boot log for `re-sending router solicitation` lines and
   whether a global/ULA address appears.
2. If not: capture on the router/host whether the RS leaves the device and an RA comes back; check
   the lwIP2 build options in the Arduino core (`LWIP_IPV6_SEND_ROUTER_SOLICIT`, RA handling).
3. Unknowns still open: DNS (no RDNSS/DHCPv6 on ESP8266), OTA over v6, flash headroom for OTA on 1 MB.
4. Safety: do NOT flash a bulb until the test module passes, including OTA over v6 and the fallback AP
   (192.168.4.1) recovery path. Bulbs have 2 MB flash and no UART.
