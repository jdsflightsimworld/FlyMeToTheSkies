# MSFS Tracker

Muestra en un mapa (Leaflet + OpenStreetMap) la posición en tiempo real del avión en
**Microsoft Flight Simulator 2024**, leída mediante SimConnect desde una pequeña
aplicación ASP.NET Core en C#.

## Requisitos

- Windows 10/11 x64
- [.NET 10 SDK](https://dotnet.microsoft.com/download)
- Microsoft Flight Simulator 2024 y su **SDK** (activa *Developer Mode* en el simulador
  y descárgalo desde el menú superior). Ruta por defecto: `C:\MSFS 2024 SDK\`.
  Si está en otra ruta, define la variable de entorno `MSFS2024_SDK` o edita
  `MsfsSdk` en `MsfsTracker.csproj`.

## Uso

```bash
dotnet run
```

Abre <http://localhost:5000> e inicia un vuelo en MSFS. La aplicación se reconecta sola
si el simulador se abre o se cierra después.

## API

`GET /api/position`

```json
{
  "connected": true,
  "aircraft": {
    "title": "...", "latitude": 40.47, "longitude": -3.56,
    "altitudeFt": 3500, "headingDeg": 182, "iasKt": 180,
    "groundSpeedKt": 195, "verticalSpeedFpm": -700, "onGround": false,
    "timestampUtc": "..."
  }
}
```

## Estructura

| Archivo | Descripción |
|---|---|
| `SimConnectService.cs` | Conexión con MSFS vía SimConnect y lectura de datos del avión |
| `Program.cs` | Servidor web y endpoint `/api/position` |
| `wwwroot/index.html` | Mapa Leaflet que consulta la posición cada segundo |
