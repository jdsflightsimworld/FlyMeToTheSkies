using System.Runtime.InteropServices;
using Microsoft.FlightSimulator.SimConnect;

namespace MsfsTracker;

public record AircraftState(
    string Title,
    double Latitude,
    double Longitude,
    double AltitudeFt,
    double HeadingDeg,
    double IasKt,
    double GroundSpeedKt,
    double VerticalSpeedFpm,
    bool OnGround,
    DateTime TimestampUtc);

/// <summary>
/// Se conecta a MSFS 2024 mediante SimConnect, pide los datos del avión del usuario
/// una vez por segundo y se reconecta solo si el simulador no está abierto o se cierra.
/// </summary>
public sealed class SimConnectService(ILogger<SimConnectService> log) : BackgroundService
{
    private enum Definitions { Aircraft }
    private enum Requests { Aircraft }

    // El orden de los campos debe coincidir EXACTAMENTE con el de AddToDataDefinition
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi, Pack = 1)]
    private struct AircraftData
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string Title;
        public double Latitude;
        public double Longitude;
        public double AltitudeFt;
        public double HeadingDeg;
        public double IasKt;
        public double GroundSpeedKt;
        public double VerticalSpeedFpm;
        public double OnGround;
    }

    private volatile AircraftState? _latest;
    private volatile bool _connected;

    public AircraftState? Latest => _latest;
    public bool IsConnected => _connected;

    // SimConnect es bloqueante: lo ejecutamos en su propio hilo
    protected override Task ExecuteAsync(CancellationToken ct) =>
        Task.Factory.StartNew(() => Run(ct), ct, TaskCreationOptions.LongRunning, TaskScheduler.Default);

    private void Run(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            using var signal = new AutoResetEvent(false);
            SimConnect? sc = null;
            var quit = false;

            try
            {
                // Sin ventana (IntPtr.Zero): SimConnect nos avisa activando "signal"
                sc = new SimConnect("MSFS Tracker", IntPtr.Zero, 0, signal, 0);

                sc.OnRecvOpen += (_, data) =>
                {
                    _connected = true;
                    log.LogInformation("Conectado a {App}", data.szApplicationName);
                };
                sc.OnRecvQuit += (_, _) =>
                {
                    log.LogInformation("El simulador se ha cerrado");
                    quit = true;
                };
                sc.OnRecvException += (_, data) =>
                    log.LogWarning("Excepción de SimConnect: {Ex}", (SIMCONNECT_EXCEPTION)data.dwException);
                sc.OnRecvSimobjectData += OnData;

                RegisterData(sc);

                while (!ct.IsCancellationRequested && !quit)
                {
                    if (signal.WaitOne(500))
                        sc.ReceiveMessage();
                }
            }
            catch (COMException)
            {
                log.LogInformation("MSFS no está disponible, reintentando en 5 s...");
            }
            catch (Exception ex)
            {
                log.LogError(ex, "Error en la conexión con SimConnect");
            }
            finally
            {
                _connected = false;
                sc?.Dispose();
            }

            if (!ct.IsCancellationRequested)
                ct.WaitHandle.WaitOne(TimeSpan.FromSeconds(5));
        }
    }

    private static void RegisterData(SimConnect sc)
    {
        void Add(string name, string? units, SIMCONNECT_DATATYPE type = SIMCONNECT_DATATYPE.FLOAT64) =>
            sc.AddToDataDefinition(Definitions.Aircraft, name, units, type, 0.0f, SimConnect.SIMCONNECT_UNUSED);

        Add("TITLE", null, SIMCONNECT_DATATYPE.STRING256);
        Add("PLANE LATITUDE", "degrees");
        Add("PLANE LONGITUDE", "degrees");
        Add("PLANE ALTITUDE", "feet");
        Add("PLANE HEADING DEGREES TRUE", "degrees");
        Add("AIRSPEED INDICATED", "knots");
        Add("GROUND VELOCITY", "knots");
        Add("VERTICAL SPEED", "feet per minute");
        Add("SIM ON GROUND", "bool");

        sc.RegisterDataDefineStruct<AircraftData>(Definitions.Aircraft);

        // Datos del avión del usuario, una vez por segundo
        // (SIMCONNECT_PERIOD.VISUAL_FRAME si quieres más frecuencia)
        sc.RequestDataOnSimObject(
            Requests.Aircraft, Definitions.Aircraft,
            SimConnect.SIMCONNECT_OBJECT_ID_USER,
            SIMCONNECT_PERIOD.SECOND,
            SIMCONNECT_DATA_REQUEST_FLAG.DEFAULT,
            0, 0, 0);
    }

    private void OnData(SimConnect sender, SIMCONNECT_RECV_SIMOBJECT_DATA data)
    {
        if ((Requests)data.dwRequestID != Requests.Aircraft) return;

        var d = (AircraftData)data.dwData[0];
        _latest = new AircraftState(
            d.Title,
            d.Latitude,
            d.Longitude,
            d.AltitudeFt,
            d.HeadingDeg,
            d.IasKt,
            d.GroundSpeedKt,
            d.VerticalSpeedFpm,
            d.OnGround != 0,
            DateTime.UtcNow);
    }
}
