# Arad Water Meter Receiver

Read your **Arad Dialog3G / Master Meter** radio water meter (916.3 MHz, used by many water utilities in Israel) with a \~US$20 **Heltec WiFi LoRa 32** board. See the reading, the water flowing *right now* and today's use. You can use:

* the board's built-in screen;
* a web page on your phone or computer;
* **Home Assistant**.

You don't need to go out to the meter any more to check whether water is running.

!\[Web page – dark mode](docs/images/web\_dark.png)

|Board screen|Web page on a phone|
|-|-|
|!\[OLED pages](docs/images/oled\_pages.png)|<img src="docs/images/web\_phone.png" width="260">|

## Features

* **Receives every Arad Dialog3G meter in range.** You get the meter reading, the meter's leak flag and the signal strength. Decoding includes the checksum and error correction (ported from [rtl\_433](https://github.com/merbanan/rtl_433)).
* **Flow now:** liters per minute since the meter's previous packet, about every 30 s. It drops to 0 as soon as the reading stops changing. There's also a smooth 5-minute average.
* **Today:** liters used since midnight. An hourly bar chart is shown on the screen.
* **Built-in web page** at **http://arad.local/**. It shows all meters, live flow, reading history, an antenna tuning helper and settings. It has dark mode and works on phones. No app and no cloud.
* **Home Assistant:** every meter you give a name appears automatically through MQTT discovery. Each one gets reading (m³, works with the Energy dashboard), flow, flow now, today, water flowing, leak and signal.
* **Screen:**

  * one page per named meter, with animated flow;
  * a list of every meter heard;
  * a receiver page with its web address.
* **Setup without coding:** Wi-Fi, MQTT, meter names and time zone are all set from the web page and saved on the board.

## Supported hardware

|Board|Radio|Firmware file|Status|
|-|-|-|-|
|**Heltec WiFi LoRa 32 V3** (recommended)|SX1262|`arad\_rx\_heltec\_v3\_…`|Developed and tested on real hardware|
|Heltec WiFi LoRa 32 V2 / V2.1|SX1276|`arad\_rx\_heltec\_v2\_…`|Builds and runs in an emulator; **not yet tested on a real board**. Please report results.|

Buy the **868/915 MHz** version of the board (also sold as 863–928 MHz). The 433 MHz version cannot receive these meters.

## What you need to buy

|Item|Notes|
|-|-|
|**Heltec WiFi LoRa 32 V3**, 868/915 MHz version|From [Heltec](https://heltec.org/project/wifi-lora-32-v3/) or the usual online shops. It comes with a small spring antenna.|
|**915 MHz antenna** (recommended)|The board has a tiny U.FL / IPEX antenna socket. A *U.FL → SMA pigtail* plus a *915 MHz SMA antenna* (a ¼-wave whip or a 3–5 dBi rubber duck) reaches much further than the spring antenna.|
|**USB cable**|V3: USB-C. V2: micro-USB. It must be a **data** cable, not charge-only.|
|**USB power supply**|Any 5 V phone charger, for permanent installation.|
|Optional: a case|Search for "Heltec V3 case". There are many 3D-printable ones.|
|Optional: Home Assistant|With the **Mosquitto broker** add-on and the **MQTT** integration.|

To flash the board you also need a computer with **Chrome or Edge**.

> ⚠️ Always connect the antenna \*\*before\*\* powering the board.

## Quick start

### 1\. Flash the firmware (5 minutes, in the browser)

1. Download the firmware for your board from the [`firmware/`](firmware) folder or the **Releases** page:

   * V3: `arad\_rx\_heltec\_v3\_full\_0x0.bin`
   * V2: `arad\_rx\_heltec\_v2\_full\_0x0.bin`
2. Plug the board into the computer.

   * On Windows, if no COM port appears, install the [CP210x USB driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers).
3. Open **https://espressif.github.io/esptool-js/** in Chrome or Edge. Click **Connect** and choose the board's port.
4. Set *Flash Address* to **`0x0`**, choose the `.bin` file, then click **Program**.
5. When it finishes, click **Disconnect** and press **RST** on the board. The screen shows *Listening for meters*.

If Connect fails, hold **PRG**, tap **RST**, release **PRG**, then try again.

Command-line alternative:

```
esptool.py --chip esp32s3 write\_flash 0x0 arad\_rx\_heltec\_v3\_full\_0x0.bin
```

For V2, use `--chip esp32` and the V2 file instead.

### 2\. First setup over USB

1. Download this repository (green **Code** button → **Download ZIP**) and unzip it.
2. Close the esptool page so the USB port is free.
3. Open **`web/arad\_monitor.html`** in Chrome or Edge, click **USB** and pick the board's port.
4. Scroll down to **Settings**:

   * **Wi-Fi:** your network name and password, then **Save Wi-Fi**. 2.4 GHz networks only.
   * **Clock:** your time zone. Israel is the default.
   * **MQTT (optional):** see step 5.

Everything is saved on the board. From now on, USB is only needed for firmware updates.

### 3\. Open the web page over Wi-Fi

Put the board wherever reception is best and power it from any USB charger. Then open:

**http://arad.local/**

If `arad.local` doesn't open (some Android phones don't support it), use the IP address shown on the board's **Receiver** screen page. A fixed DHCP reservation in your router keeps that address stable.

The same `web/arad\_monitor.html` file can also connect over Wi-Fi: type `arad.local` or the IP in the box at the top and click **Connect (Wi-Fi)**.

### 4\. Find and name your meter

1. The meter ID is printed on the radio unit of your water meter, for example `01234567`. In the list it appears as `01234567-00`.
2. In the web page go to **Settings → Meters**, pick the ID, type a name (*Home*, *Garden*, …) and click **Save name**.
3. Named meters get their own page on the board's screen and are **added to Home Assistant automatically**. Remove the name to remove the meter from Home Assistant.

Your neighbors' meters also show up in the list. Set *Show on the screen → Only meters I named* to hide them from the screen.

### 4b. Check the reading scale (important)

Different Arad meter models count in different units. Compare the page's **Current reading** with the number on the meter itself. If it is **10× or 100× too big** (for example the page shows `1719.24` but the meter shows `171.924` m³), fix it:

* **For all meters** (usually all meters in a building are the same model): in **Settings → Meters**, set **Reading scale – all meters** to **×0.1** (10× too big) or **×0.01** (100× too big), then click **Save for all meters**.
* **For one meter** that differs: pick it, set **Reading scale – this meter**, then click **Save for this meter**. Choose **Same as all meters** to remove its own setting again.

The receiver saves this on the board and corrects everything: reading, *Today*, flow and Home Assistant. A meter that counts in 1-liter steps also gives a 10× finer *flow now*.

If the meter was already in Home Assistant, its long-term statistics will show one jump at the moment of the change. You can fix that in **Developer tools → Statistics** if you want.

### 5\. Home Assistant (optional)

1. Install the **Mosquitto broker** add-on and enable the **MQTT** integration.
2. Create a Home Assistant user for MQTT, or use an existing one.
3. In the web page, open **Settings → MQTT**:

   * broker address: your Home Assistant IP, e.g. `192.168.1.10`;
   * the user and password;
   * then click **Save MQTT**.

Each named meter appears as a device **"Arad <name>"**:

|Entity|Meaning|
|-|-|
|Water meter|Meter reading in m³. Add it to **Energy → Water consumption**.|
|Water flow|L/min, average over \~5 minutes. Steady; good for "water running for 10 minutes" alerts.|
|Water flow now|L/min since the previous packet. Shows water running within \~30 s, and 0 as soon as it stops.|
|Water flowing|On while *flow now* is above 0.|
|Water today|Liters since local midnight.|
|Leak|The meter's own leak flag.|
|Signal|RSSI in dBm (diagnostic).|

Example automations are in [`homeassistant/`](homeassistant):

* `water\_flow\_alert.yaml`: notifies your phone when water flows for more than 10 minutes and lists today's use.
* `water\_flow\_alert\_telegram.yaml`: the same alert as a Telegram message (needs the Telegram bot integration).
* `flow\_started\_notify.yaml`: notifies you when water starts running, for example at night.

The images below are examples made with made-up data:

|A named meter in Home Assistant|Telegram alert|
|-|-|
|!\[Home Assistant example](docs/images/example\_home\_assistant.png)|<img src="docs/images/example\_telegram.png" width="300">|

The hourly chart is a *Statistics graph* card on the *Water today* sensor, set to "change" per hour and shown as bars.

## How the numbers work

* Depending on the model, the meter counts in **1-, 10- or 100-liter steps** (after the reading scale is set correctly, see step 4b), and transmits roughly every 30 seconds.
* **Flow now** is the liters added since the previous packet, divided by the time between the two.

  * A tap running at 8 L/min therefore shows as 0, \~20, \~10, … as each 10 L step passes.
  * A short use of less than 10 L appears only if it crosses a step. It still shows later, when the total crosses the next step.
  * If the previous packet is more than 5 minutes old, the 5-minute average is shown instead.
* **Water flow** is the change in the reading over about 5 minutes. It's smooth but reacts with a delay.
* **Today** is the reading minus the first reading after midnight.

  * The midnight reading is saved on the board, so a reboot keeps today's total.
  * The board needs Wi-Fi (internet time) to know when midnight is.
* **Very slow leaks** (less than 10 L between packets) are best seen with the meter's own **Leak** flag, or as *Today* increasing overnight.

## The web page

* **Meters heard:** every meter in range (click a column header to sort by it; click again to reverse), with:

  * name, flow now, today, current and previous reading, signal and status;
  * click a row to see that meter's reading history.
* **Antenna tuning:** pick a meter, move the antenna and compare the signal with the best so far. Higher **SNR** is better: above \~15 dB is good, below \~6 dB packets get lost. *Beep on packet* lets you hear each packet.
* **Receiver status and log:** live packets, Wi-Fi and MQTT state, and a command box.
* **Settings:** Wi-Fi, MQTT, meter names, screen mode, time zone, reboot and factory reset.
* 🌓 cycles between automatic, light and dark theme.

When it opens, the page loads everything the board has heard since it started. Longer history is kept in your browser.

The page has no password: anyone on your home network can open it. Don't expose the board to the internet.

## The screen

|Page|Shows|
|-|-|
|Meter (one per named meter)|Flow now (big number) with a falling-drop animation, today's liters, signal bars and a 24-hour usage chart. The header flashes on a leak.|
|Meters (4 per page)|Name or ID, flow now, today. `--` means not heard for 5 minutes.|
|Receiver|`arad.local`, the IP address, meters heard, Wi-Fi signal and open web pages.|

Pages change every 5 s. Press **PRG** to jump to the next page; automatic paging then pauses for 1 minute.

## Commands

Type these in the web page's command box, or in any serial terminal at 115200 baud:

|Command||
|-|-|
|`wifi <name>` / `wifipass <password>` / `wifi off`|Wi-Fi|
|`mqtt <host>` / `mqttuser <u>` / `mqttpass <p>` / `mqtt off`|MQTT broker (e.g. `mqtt 192.168.1.10`)|
|`name <meter id> <label>` / `unname <meter id>`|Name a meter (adds it to Home Assistant) or remove the name|
|`scale all 1\|0.1\|0.01`|Reading scale for all meters, if readings are 10× / 100× too big (see step 4b)|
|`scale <meter id> 1\|0.1\|0.01\|default`|Reading scale for one meter; `default` = same as all meters|
|`show all` / `show named`|Screen: every meter, or only named ones|
|`tz <POSIX TZ>`|Time zone, e.g. `tz CET-1CEST,M3.5.0,M10.5.0/3`|
|`meters` / `status`|List meters / receiver status|
|`freq <MHz>` / `bw <kHz>` / `pol 0\|1\|auto` / `raw on\|off`|Radio fine-tuning and diagnostics|
|`reboot` / `factory`|Restart / erase all settings|

## Updating

* To update **and keep your settings**, flash the `…\_update\_0x10000.bin` file at address **`0x10000`**.
* The `…\_full\_0x0.bin` file at `0x0` erases the settings (Wi-Fi, MQTT, names, today's totals).

## Troubleshooting

|Problem|Try|
|-|-|
|No meters after a few minutes|Is the antenna connected? Is it the 868/915 MHz board? Move closer to the meters or higher up. Polarity is detected automatically (it switches every 15 s until the first packet).|
|Log says *radio not answering*|Wrong firmware for the board (V2 file on a V3 or the other way round), or a faulty board.|
|Wi-Fi not connecting|The password; the network must be 2.4 GHz.|
|`arad.local` doesn't open|Use the IP from the board's *Receiver* screen page.|
|MQTT not connecting|Broker IP, user and password; the Mosquitto add-on must be running.|
|Meter missing in Home Assistant|Give it a name (step 4). It must have MQTT connected.|
|*Today* is wrong|Set the time zone. The board needs internet time.|
|Reading or *Today* is 10× / 100× too big|Set the reading scale for all meters or for that meter (step 4b).|
|USB page can't connect|Close other programs using the port (esptool page, serial monitors). Use Chrome or Edge.|

## Building from source

You need ESP-IDF **v5.3**:

```
git clone https://github.com/danstr1/Arad-Meter-Home-Assistant-ESP32.git
cd arad-water-meter-receiver
idf.py -B build\_v3 -D IDF\_TARGET=esp32s3 -D SDKCONFIG=build\_v3/sdkconfig build      # Heltec V3
idf.py -B build\_v2 -D IDF\_TARGET=esp32   -D SDKCONFIG=build\_v2/sdkconfig build      # Heltec V2
idf.py -B build\_v3 -p COM5 flash monitor
```

* **Board selection:** the IDF target picks the board (`main/board.h`). `esp32s3` builds for the V3 (`radio\_sx1262.h`) and `esp32` for the V2 (`radio\_sx1276.h`).
* **Web page:** `web/arad\_monitor.html` is gzipped and embedded into the firmware at build time.
* **Continuous integration:** GitHub Actions (`.github/workflows/build.yml`) builds both boards on every push. Pushing a tag such as `v3.0` attaches the images to a Release.
* **Emulator test build:**

  * Command: `-D SDKCONFIG\_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu" -D IDF\_TARGET=esp32`.
  * It runs in Espressif's QEMU with virtual Ethernet and no radio.
  * The `fake <id> <m3>` command injects packets for testing the web page and MQTT.

### How it works

* **Radio settings:** the radio runs in FSK packet mode at the meter's Manchester chip rate:

  * 8.4 µs chips (\~119 kBaud), ±77 kHz deviation, 916.29 MHz;
  * a 32-chip sync word (raw bytes `C1 96`);
  * a fixed 40-byte payload.
* **Decoding:** Manchester decoding, the 40-bit checksum and up to 3-bit error correction run on the ESP32 (`main/arad\_decode.h`).

Source files:

|File|What it does|
|-|-|
|`main/app.h`|Tracking, Wi-Fi, MQTT and the screen|
|`main/web.h`|Web server, WebSocket and mDNS|
|`main/ui.h`|Screen drawing (plain C++, can be rendered on a PC)|

## Privacy and legal

* **Neighbors' meters:** the receiver picks up the unencrypted broadcasts of every Arad meter in range, including your neighbors'. Only meters you name go to Home Assistant, and *Only meters I named* hides the rest on the screen.
* **No cloud:** data goes only to your own MQTT broker and web browser.
* **Listen-only:** the board never transmits on 916 MHz.
* **Local rules:** check your local rules about receiving radio transmissions.

## Contributing

Issues and pull requests are welcome. Reports from **Heltec V2** owners and from other Arad meter models are especially useful. Please include the output of the `status` command and a few decoded packet lines from the web page log.

## Credits and license

* **Decoder:** the Arad checksum and bit correction are ported from **rtl\_433** (`src/devices/arad\_ms\_meter.c`). This project is therefore licensed under the **GNU GPL v2.0 or later**; see [LICENSE](LICENSE).
* **mDNS:** `components/mdns` comes from [espressif/esp-protocols](https://github.com/espressif/esp-protocols) (Apache-2.0; license file included).
* **Fonts:** bitmap fonts converted from the [u8g2](https://github.com/olikraus/u8g2) project: X11 misc-fixed 5x7, Adobe Helvetica Bold 10 and Spleen 12x24, each under its own permissive license.
* **Framework:** ESP-IDF (Apache-2.0).
* **Trademarks:** Arad, Dialog3G, Master Meter and Heltec are trademarks of their owners. This project is not affiliated with them.

