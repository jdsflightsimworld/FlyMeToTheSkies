# FlyMeToTheSkies

A small Windows app that reads your aircraft's position from **Microsoft Flight Simulator 2024** every second and serves it as a local HTTP API, so you can show where you are flying on a web page.

![Icon](FlyMeToTheSkies/icon.png)

## Features

- Shows the simulator connection status (green when connected, red when not).
- Shows the current latitude and longitude, with a link that opens the position in Google Maps.
- Publishes the position as JSON at `http://localhost:8080/api/position`, with CORS enabled (`Access-Control-Allow-Origin: *`) so any web page can call it.
- Configurable port, remembered between sessions.
- Lives in the system tray: the window's close button sends it to the tray, and double-clicking the tray icon brings it back. To quit, right-click the tray icon and choose **Exit**.
- Reconnects automatically when the simulator is started or restarted.

## Requirements

- Windows 10 or 11 (64-bit).
- Microsoft Flight Simulator 2024.

The executable is self-contained: you don't need to install the SDK or any runtime to use it.

## Usage

1. Run `FlyMeToTheSkies.exe`.
2. Start a flight in MSFS 2024. The status turns green and the coordinates start updating.
3. Call the API from your web page or browser:

```
GET http://localhost:8080/api/position
```

```json
{"connected":true,"valid":true,"latitude":40.47192600,"longitude":-3.56264000,"timestamp":1730800000}
```

| Field | Description |
|---|---|
| `connected` | `true` when the app is connected to the simulator. |
| `valid` | `true` once at least one position has been received. |
| `latitude`, `longitude` | Aircraft position in decimal degrees. |
| `timestamp` | Time of the last reading (Unix seconds). |

A minimal example page is included in [`FlyMeToTheSkies/test.html`](FlyMeToTheSkies/test.html).

### Changing the port

Type a new port in the **Port** field and click **Apply** (or press Enter). The app stops listening on the old port and starts on the new one. If the new port can't be opened, it keeps using the previous one. A port can also be passed on the command line, which takes precedence over the saved one:

```
FlyMeToTheSkies.exe 9000
```

To reach the API from another computer on your network, use this PC's IP address instead of `localhost`. Windows Firewall may ask for permission the first time.

## Building from source

1. Install Visual Studio with the **Desktop development with C++** workload.
2. Install the **MSFS 2024 SDK**. Its installer sets the `MSFS2024_SDK` environment variable, which the project uses to find SimConnect.
3. Open `FlyMeToTheSkies/FlyMeToTheSkies.sln` and build the **Release | x64** configuration.

The executable is written to `FlyMeToTheSkies/x64/Release/FlyMeToTheSkies.exe`.
