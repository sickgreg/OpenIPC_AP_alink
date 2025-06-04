# OpenIPC_AP_alink
# apaPID03

A simple, real-time PID controller for dynamically adjusting video bitrate on OpenIPC devices (e.g., SSC338Q) based on link speed and RSSI.

## Features

- Monitors WiFi bitrate and RSSI from RTL88x2EU driver. 
- Adjusts encoder bitrate using PID control logic.
- Sends HTTP requests to `localhost` to apply bitrate instantly.
- Emergency bitrate drops on link degradation.
- Optional command-line tuning of PID parameters.

## Build Instructions

### Cross-compile for SSC338Q (ARM)

```bash
make clean
make
make strip
```

> Requires `arm-linux-gnueabihf-gcc` toolchain in your path.

## Usage

```bash
./apaPID03 [-p Kp] [-i Ki] [-d Kd]
```

### Example:

```bash
./apaPID03 -p 1.2 -i 0.01 -d 0.4
```

### Default PID values:

- `Kp = 0.6`
- `Ki = 0.05`
- `Kd = 0.2`

## Notes

- Adjusts bitrate by sending GET request to:
  ```
  http://127.0.0.1/api/v1/set?video0.bitrate=XXXX
  ```
- Parses RSSI and bitrate from:
  ```
  /proc/net/rtl88x2eu/wlan0/all_sta_info
  ```

## License

MIT License
