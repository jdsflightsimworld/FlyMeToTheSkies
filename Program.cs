using MsfsTracker;

var builder = WebApplication.CreateBuilder(args);
builder.WebHost.UseUrls("http://localhost:5000");

// Un único servicio que mantiene la conexión con el simulador en segundo plano
builder.Services.AddSingleton<SimConnectService>();
builder.Services.AddHostedService(sp => sp.GetRequiredService<SimConnectService>());

var app = builder.Build();

app.UseDefaultFiles();   // sirve wwwroot/index.html en "/"
app.UseStaticFiles();

// Posición actual del avión, la consulta el mapa Leaflet
app.MapGet("/api/position", (SimConnectService sim) => new
{
    connected = sim.IsConnected,
    aircraft = sim.Latest
});

app.Run();
