# Changelog

## 3.2
- Reading scale can be set **for all meters** (`scale all <factor>`) and overridden **per meter** (`scale <id> <factor>`; `scale <id> default` to follow the global setting again). Both are in Settings → Meters.

## 3.1
- Per-meter **reading scale** (×1 / ×0.1 / ×0.01) for meters that report a 10× or 100× bigger reading. Set it in Settings → Meters or with `scale <id> <factor>`; reading, today, flow and Home Assistant are converted.
- Readings are shown with the meter's real resolution (up to 4 decimals).
- Web page: click a column header in the meters table to sort by it.

## 3.0
- Built-in web page over Wi-Fi: http://arad.local/ (or the board's IP). Same page also works offline over USB.
- Every meter you name is added to Home Assistant automatically (MQTT discovery); unnamed meters stay off Home Assistant.
- "Flow now" = change since the meter's previous packet (drops to 0 as soon as the reading stops changing).
- New "Water flowing" binary sensor per named meter.
- Web page: dark mode, flow-now and today columns, phone layout, history replay from the board.
- Screen: "Receiver" page with the web address, IP and number of meters heard.

## 2.2
- First public version: Heltec V2 (SX1276) and V3 (SX1262), OLED pages, MQTT / Home Assistant, USB setup page.
