#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <stdlib.h>
#include <cmath>

#include <mutex>
#include <thread>
#include <atomic>

#include "SimConnect.h"
#include "resource.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shell32.lib")

// Common Controls v6, required by the SysLink control
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

/*
Window that reads the user's aircraft position from MSFS 2024 every second,
shows it and publishes it as an HTTP API:

	GET http://localhost:8080/api/position

	{"connected":true,"valid":true,"latitude":40.471926,"longitude":-3.562640,"timestamp":1730800000}

Usage: FlyMeToTheSkies.exe [port]
*/

const wchar_t* APP_NAME = L"FlyMeToTheSkies";

HANDLE  hSimConnect = NULL;

SIMCONNECT_DATA_DEFINITION_ID	DATA_DEF_POSITION = 1000;
SIMCONNECT_DATA_REQUEST_ID		DATA_REQUEST_POSITION = 100;

const UINT WM_USER_SIMCONNECT = WM_USER + 1;
const UINT WM_USER_TRAY = WM_USER + 2;
const UINT TRAY_ICON_ID = 1;

const UINT_PTR TIMER_POLL = 1;
const UINT_PTR TIMER_RECONNECT = 2;
const UINT POLL_INTERVAL_MS = 1000;
const UINT RECONNECT_INTERVAL_MS = 5000;

const int IDC_STATUS = 101;
const int IDC_COORDS = 102;
const int IDC_MAPLINK = 103;
const int IDC_API = 104;
const int IDC_PORTLABEL = 105;
const int IDC_PORTEDIT = 106;
const int IDC_APPLYPORT = 107;

const UINT ID_TRAY_OPEN = 201;
const UINT ID_TRAY_EXIT = 202;

// Sent by Explorer when the taskbar is recreated; the tray icon has to be added again
const UINT WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");

HWND g_hWnd = NULL;
HWND g_hStatus = NULL;
HWND g_hCoords = NULL;
HWND g_hMapLink = NULL;
HFONT g_hFont = NULL;
HFONT g_hBoldFont = NULL;
HICON g_hSmallIcon = NULL;
bool g_InTray = false;

#pragma pack(push, 1)

struct sPosition
{
	double latitude;
	double longitude;
};

#pragma pack(pop)

// Last known position, shared between the window (SimConnect) and the HTTP server
struct PositionState
{
	bool connected = false;
	bool valid = false;
	double latitude = 0.0;
	double longitude = 0.0;
	long long timestamp = 0;
};

std::mutex g_StateMutex;
PositionState g_State;

PositionState GetState()
{
	std::lock_guard<std::mutex> lock(g_StateMutex);
	return g_State;
}

#pragma region Ui

void UpdateTrayTip();

// Converts decimal degrees to URL-encoded degrees, minutes and seconds:
// 19.107112 -> 19%C2%B006'25.6%22N  (19°06'25.6"N)
std::wstring ToDmsUrl(double value, wchar_t positive, wchar_t negative)
{
	wchar_t hemisphere = value >= 0 ? positive : negative;

	// Work in tenths of a second so rounding never produces 60.0"
	long long tenths = llround(fabs(value) * 36000.0);
	long long degrees = tenths / 36000;
	long long minutes = (tenths % 36000) / 600;
	long long secondsTenths = tenths % 600;

	wchar_t dms[64] = L"";
	swprintf_s(dms, L"%lld%%C2%%B0%02lld'%02lld.%lld%%22%c",
		degrees, minutes, secondsTenths / 10, secondsTenths % 10, hemisphere);
	return dms;
}

std::wstring BuildMapsUrl(double latitude, double longitude)
{
	return L"https://www.google.com/maps/place/" + ToDmsUrl(latitude, L'N', L'S') + L"+" + ToDmsUrl(longitude, L'E', L'W');
}

void UpdateUi()
{
	PositionState s = GetState();

	SetWindowTextW(g_hStatus, s.connected ? L"Connected to the simulator" : L"Not connected");

	if (s.valid)
	{
		wchar_t coords[128] = L"";
		swprintf_s(coords, L"Latitude: %.6f    Longitude: %.6f", s.latitude, s.longitude);
		SetWindowTextW(g_hCoords, coords);
		SetWindowTextW(g_hMapLink, L"<a>Open in Google Maps</a>");
	}
	else
	{
		SetWindowTextW(g_hCoords, L"Latitude: -    Longitude: -");
		SetWindowTextW(g_hMapLink, L"Open in Google Maps");
	}

	// Forces WM_CTLCOLORSTATIC so the status is repainted with the new color
	InvalidateRect(g_hStatus, NULL, TRUE);

	UpdateTrayTip();
}

void OpenMaps()
{
	PositionState s = GetState();
	if (!s.valid)
		return;

	ShellExecuteW(NULL, L"open", BuildMapsUrl(s.latitude, s.longitude).c_str(), NULL, NULL, SW_SHOWNORMAL);
}

void FillTrayData(NOTIFYICONDATAW& nid)
{
	nid = {};
	nid.cbSize = sizeof(nid);
	nid.hWnd = g_hWnd;
	nid.uID = TRAY_ICON_ID;
	nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	nid.uCallbackMessage = WM_USER_TRAY;
	nid.hIcon = g_hSmallIcon;

	PositionState s = GetState();
	if (s.valid)
		swprintf_s(nid.szTip, L"%s\n%.6f, %.6f", APP_NAME, s.latitude, s.longitude);
	else
		swprintf_s(nid.szTip, L"%s\n%s", APP_NAME, s.connected ? L"Connected" : L"Not connected");
}

void UpdateTrayTip()
{
	if (!g_InTray)
		return;

	NOTIFYICONDATAW nid;
	FillTrayData(nid);
	Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void MinimizeToTray()
{
	NOTIFYICONDATAW nid;
	FillTrayData(nid);
	if (Shell_NotifyIconW(NIM_ADD, &nid))
	{
		g_InTray = true;
		ShowWindow(g_hWnd, SW_HIDE);
	}
}

void RestoreFromTray()
{
	if (g_InTray)
	{
		NOTIFYICONDATAW nid;
		FillTrayData(nid);
		Shell_NotifyIconW(NIM_DELETE, &nid);
		g_InTray = false;
	}

	ShowWindow(g_hWnd, SW_SHOW);
	ShowWindow(g_hWnd, SW_RESTORE);
	SetForegroundWindow(g_hWnd);
}

void ShowTrayMenu()
{
	HMENU hMenu = CreatePopupMenu();
	AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN, L"Open");
	AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");
	SetMenuDefaultItem(hMenu, ID_TRAY_OPEN, FALSE);

	POINT pt;
	GetCursorPos(&pt);
	// Required so the menu closes when clicking outside of it
	SetForegroundWindow(g_hWnd);
	TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hWnd, NULL);
	DestroyMenu(hMenu);
}

#pragma endregion

#pragma region SimConnect

void SetDisconnected()
{
	if (hSimConnect != NULL)
	{
		SimConnect_Close(hSimConnect);
		hSimConnect = NULL;
	}

	{
		std::lock_guard<std::mutex> lock(g_StateMutex);
		g_State.connected = false;
		g_State.valid = false;
	}

	KillTimer(g_hWnd, TIMER_POLL);
	SetTimer(g_hWnd, TIMER_RECONNECT, RECONNECT_INTERVAL_MS, NULL);
	UpdateUi();
}

void CALLBACK MySignalProc(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext)
{
	switch (pData->dwID)
	{
	case SIMCONNECT_RECV_ID_SIMOBJECT_DATA:
	{
		SIMCONNECT_RECV_SIMOBJECT_DATA* pObjData = (SIMCONNECT_RECV_SIMOBJECT_DATA*)pData;

		if (pObjData->dwRequestID == DATA_REQUEST_POSITION)
		{
			sPosition* pos = (sPosition*)&pObjData->dwData;

			{
				std::lock_guard<std::mutex> lock(g_StateMutex);
				g_State.valid = true;
				g_State.latitude = pos->latitude;
				g_State.longitude = pos->longitude;
				g_State.timestamp = (long long)time(nullptr);
			}
			UpdateUi();
		}
	}
	break;

	case SIMCONNECT_RECV_ID_QUIT:
	{
		SetDisconnected();
	}
	break;

	case SIMCONNECT_RECV_ID_EXCEPTION:
	{
		SIMCONNECT_RECV_EXCEPTION* pException = (SIMCONNECT_RECV_EXCEPTION*)pData;
		pException = pException;
	}
	break;
	}
}

bool ConnectToSim()
{
	// SimConnect messages arrive at the window as WM_USER_SIMCONNECT
	HRESULT hr = SimConnect_Open(&hSimConnect, "FlyMeToTheSkies", g_hWnd, WM_USER_SIMCONNECT, 0, 0);
	if (hr != S_OK)
	{
		hSimConnect = NULL;
		return false;
	}

	hr = SimConnect_AddToDataDefinition(hSimConnect, DATA_DEF_POSITION, "PLANE LATITUDE", "degrees");
	hr = SimConnect_AddToDataDefinition(hSimConnect, DATA_DEF_POSITION, "PLANE LONGITUDE", "degrees");

	{
		std::lock_guard<std::mutex> lock(g_StateMutex);
		g_State.connected = true;
	}

	KillTimer(g_hWnd, TIMER_RECONNECT);
	SetTimer(g_hWnd, TIMER_POLL, POLL_INTERVAL_MS, NULL);
	UpdateUi();
	return true;
}

void RequestPosition()
{
	if (hSimConnect == NULL)
		return;

	HRESULT hr = SimConnect_RequestDataOnSimObject(hSimConnect, DATA_REQUEST_POSITION, DATA_DEF_POSITION, SIMCONNECT_OBJECT_ID_USER, SIMCONNECT_PERIOD_ONCE);
	if (hr != S_OK)
	{
		SetDisconnected();
	}
}

#pragma endregion

#pragma region Http

std::string BuildPositionJson()
{
	PositionState s = GetState();

	char json[256] = "";
	sprintf_s(json, "{\"connected\":%s,\"valid\":%s,\"latitude\":%.8f,\"longitude\":%.8f,\"timestamp\":%lld}",
		s.connected ? "true" : "false", s.valid ? "true" : "false", s.latitude, s.longitude, s.timestamp);
	return json;
}

void SendResponse(SOCKET client, const char* status, const char* contentType, const std::string& body)
{
	std::string response = std::string("HTTP/1.1 ") + status + "\r\n"
		"Content-Type: " + contentType + "\r\n"
		"Content-Length: " + std::to_string(body.size()) + "\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"Access-Control-Allow-Methods: GET, OPTIONS\r\n"
		"Access-Control-Allow-Headers: *\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: close\r\n"
		"\r\n" + body;

	send(client, response.c_str(), (int)response.size(), 0);
}

void HandleClient(SOCKET client)
{
	char buffer[4096] = "";
	int received = recv(client, buffer, sizeof(buffer) - 1, 0);
	if (received > 0)
	{
		buffer[received] = '\0';
		std::string request(buffer);

		// First line: "METHOD /path HTTP/1.1"
		std::string firstLine = request.substr(0, request.find("\r\n"));
		size_t sp1 = firstLine.find(' ');
		size_t sp2 = firstLine.find(' ', sp1 + 1);
		std::string method = firstLine.substr(0, sp1);
		std::string path = (sp1 != std::string::npos) ? firstLine.substr(sp1 + 1, sp2 - sp1 - 1) : "";
		path = path.substr(0, path.find('?'));

		if (method == "OPTIONS")
		{
			SendResponse(client, "204 No Content", "text/plain", "");
		}
		else if (method == "GET" && (path == "/api/position" || path == "/api/position/"))
		{
			SendResponse(client, "200 OK", "application/json; charset=utf-8", BuildPositionJson());
		}
		else
		{
			SendResponse(client, "404 Not Found", "application/json; charset=utf-8", "{\"error\":\"not found\"}");
		}
	}

	shutdown(client, SD_BOTH);
	closesocket(client);
}

void HttpServerLoop(SOCKET listenSocket)
{
	while (true)
	{
		SOCKET client = accept(listenSocket, NULL, NULL);
		if (client == INVALID_SOCKET)
		{
			// When the listening socket has been closed (StopHttp) the thread ends
			if (WSAGetLastError() == WSAECONNRESET)
				continue;
			break;
		}

		DWORD timeout = 2000;
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
		HandleClient(client);
	}
}

SOCKET StartHttpServer(unsigned short port)
{
	SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listenSocket == INVALID_SOCKET)
		return INVALID_SOCKET;

	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);

	if (bind(listenSocket, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
		listen(listenSocket, SOMAXCONN) == SOCKET_ERROR)
	{
		closesocket(listenSocket);
		return INVALID_SOCKET;
	}

	return listenSocket;
}

SOCKET g_ListenSocket = INVALID_SOCKET;
std::thread g_HttpThread;
unsigned short g_ListenPort = 0;

bool StartHttp(unsigned short port)
{
	SOCKET listenSocket = StartHttpServer(port);
	if (listenSocket == INVALID_SOCKET)
		return false;

	g_ListenSocket = listenSocket;
	g_ListenPort = port;
	g_HttpThread = std::thread(HttpServerLoop, listenSocket);
	return true;
}

void StopHttp()
{
	if (g_ListenSocket == INVALID_SOCKET)
		return;

	// Closing the socket unblocks accept() and ends the HTTP thread
	closesocket(g_ListenSocket);
	g_ListenSocket = INVALID_SOCKET;
	g_ListenPort = 0;
	if (g_HttpThread.joinable())
		g_HttpThread.join();
}

#pragma endregion

#pragma region Settings

const wchar_t* REGISTRY_KEY = L"Software\\FlyMeToTheSkies";
const DWORD DEFAULT_PORT = 8080;

unsigned short LoadPort()
{
	DWORD port = DEFAULT_PORT;
	DWORD size = sizeof(port);
	if (RegGetValueW(HKEY_CURRENT_USER, REGISTRY_KEY, L"Port", RRF_RT_REG_DWORD, NULL, &port, &size) != ERROR_SUCCESS ||
		port == 0 || port > 65535)
	{
		port = DEFAULT_PORT;
	}
	return (unsigned short)port;
}

void SavePort(unsigned short port)
{
	DWORD value = port;
	RegSetKeyValueW(HKEY_CURRENT_USER, REGISTRY_KEY, L"Port", REG_DWORD, &value, sizeof(value));
}

#pragma endregion

#pragma region Window

unsigned short g_Port = 8080;
HWND g_hApi = NULL;
HWND g_hPortEdit = NULL;

void UpdateApiLabel()
{
	wchar_t api[128] = L"";
	if (g_ListenSocket != INVALID_SOCKET)
		swprintf_s(api, L"API: http://localhost:%u/api/position", g_ListenPort);
	else
		swprintf_s(api, L"API stopped: could not open port %u", g_Port);
	SetWindowTextW(g_hApi, api);
}

void StartHttpOrWarn()
{
	if (!StartHttp(g_Port))
	{
		wchar_t error[128] = L"";
		swprintf_s(error, L"Could not open port %u. Another application may be using it.", g_Port);
		MessageBoxW(g_hWnd, error, APP_NAME, MB_ICONERROR);
	}
	UpdateApiLabel();
}

void ApplyPort()
{
	wchar_t text[16] = L"";
	GetWindowTextW(g_hPortEdit, text, _countof(text));
	int port = _wtoi(text);
	if (port < 1 || port > 65535)
	{
		MessageBoxW(g_hWnd, L"Enter a port between 1 and 65535.", APP_NAME, MB_ICONWARNING);
		return;
	}

	if (g_ListenSocket != INVALID_SOCKET && g_ListenPort == port)
		return;

	unsigned short previousPort = g_ListenPort;
	StopHttp();

	if (StartHttp((unsigned short)port))
	{
		g_Port = (unsigned short)port;
		SavePort(g_Port);
	}
	else
	{
		wchar_t error[160] = L"";
		swprintf_s(error, L"Could not open port %d. Another application may be using it.", port);
		MessageBoxW(g_hWnd, error, APP_NAME, MB_ICONERROR);

		// Go back to listening on the previous port
		if (previousPort != 0 && StartHttp(previousPort))
		{
			swprintf_s(text, L"%u", previousPort);
			SetWindowTextW(g_hPortEdit, text);
		}
		else
		{
			g_Port = (unsigned short)port;
		}
	}

	UpdateApiLabel();
}

HWND CreateLabel(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h, HFONT font)
{
	HWND hCtrl = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
		x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
	SendMessageW(hCtrl, WM_SETFONT, (WPARAM)font, TRUE);
	return hCtrl;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_CREATE:
	{
		g_hWnd = hWnd;

		NONCLIENTMETRICSW ncm = { sizeof(ncm) };
		SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
		ncm.lfMessageFont.lfHeight = -16;
		g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);
		ncm.lfMessageFont.lfWeight = FW_BOLD;
		g_hBoldFont = CreateFontIndirectW(&ncm.lfMessageFont);

		g_hStatus = CreateLabel(hWnd, IDC_STATUS, L"", 20, 20, 400, 24, g_hBoldFont);
		g_hCoords = CreateLabel(hWnd, IDC_COORDS, L"", 20, 56, 400, 24, g_hFont);

		g_hMapLink = CreateWindowExW(0, WC_LINK, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
			20, 88, 400, 24, hWnd, (HMENU)(INT_PTR)IDC_MAPLINK, GetModuleHandleW(NULL), NULL);
		SendMessageW(g_hMapLink, WM_SETFONT, (WPARAM)g_hFont, TRUE);

		g_hApi = CreateLabel(hWnd, IDC_API, L"", 20, 128, 400, 24, g_hFont);

		CreateLabel(hWnd, IDC_PORTLABEL, L"Port:", 20, 168, 60, 24, g_hFont);

		wchar_t port[16] = L"";
		swprintf_s(port, L"%u", g_Port);
		g_hPortEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", port, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
			84, 165, 80, 28, hWnd, (HMENU)(INT_PTR)IDC_PORTEDIT, GetModuleHandleW(NULL), NULL);
		SendMessageW(g_hPortEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
		SendMessageW(g_hPortEdit, EM_SETLIMITTEXT, 5, 0);

		HWND hApply = CreateWindowExW(0, L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			176, 164, 100, 30, hWnd, (HMENU)(INT_PTR)IDC_APPLYPORT, GetModuleHandleW(NULL), NULL);
		SendMessageW(hApply, WM_SETFONT, (WPARAM)g_hFont, TRUE);

		UpdateUi();

		if (!ConnectToSim())
		{
			SetTimer(hWnd, TIMER_RECONNECT, RECONNECT_INTERVAL_MS, NULL);
		}
	}
	return 0;

	case WM_USER_SIMCONNECT:
	{
		if (hSimConnect != NULL)
		{
			SimConnect_CallDispatch(hSimConnect, MySignalProc, NULL);
		}
	}
	return 0;

	case WM_TIMER:
	{
		if (wParam == TIMER_POLL)
		{
			// Request the user's aircraft position every second
			RequestPosition();
		}
		else if (wParam == TIMER_RECONNECT)
		{
			ConnectToSim();
		}
	}
	return 0;

	case WM_COMMAND:
	{
		switch (LOWORD(wParam))
		{
		case IDC_APPLYPORT:
		case IDOK: // Enter in the port field
			ApplyPort();
			break;
		case ID_TRAY_OPEN:
			RestoreFromTray();
			break;
		case ID_TRAY_EXIT:
			DestroyWindow(hWnd);
			break;
		}
	}
	return 0;

	case WM_CLOSE:
	{
		// The title bar X sends the window to the tray; "Exit" in the tray icon menu quits the app
		MinimizeToTray();
	}
	return 0;

	case WM_USER_TRAY:
	{
		switch (LOWORD(lParam))
		{
		case WM_LBUTTONDBLCLK:
			RestoreFromTray();
			break;
		case WM_RBUTTONUP:
		case WM_CONTEXTMENU:
			ShowTrayMenu();
			break;
		}
	}
	return 0;

	case WM_NOTIFY:
	{
		NMHDR* pHdr = (NMHDR*)lParam;
		if (pHdr->idFrom == IDC_MAPLINK && (pHdr->code == NM_CLICK || pHdr->code == NM_RETURN))
		{
			OpenMaps();
		}
	}
	return 0;

	case WM_CTLCOLORSTATIC:
	{
		HDC hdc = (HDC)wParam;
		SetBkMode(hdc, TRANSPARENT);
		if ((HWND)lParam == g_hStatus)
		{
			SetTextColor(hdc, GetState().connected ? RGB(0, 150, 0) : RGB(200, 0, 0));
		}
		return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
	}

	case WM_DESTROY:
	{
		KillTimer(hWnd, TIMER_POLL);
		KillTimer(hWnd, TIMER_RECONNECT);
		if (hSimConnect != NULL)
		{
			SimConnect_Close(hSimConnect);
			hSimConnect = NULL;
		}
		if (g_InTray)
		{
			NOTIFYICONDATAW nid;
			FillTrayData(nid);
			Shell_NotifyIconW(NIM_DELETE, &nid);
			g_InTray = false;
		}
		DeleteObject(g_hFont);
		DeleteObject(g_hBoldFont);
		PostQuitMessage(0);
	}
	return 0;
	}

	if (msg == WM_TASKBARCREATED && g_InTray)
	{
		NOTIFYICONDATAW nid;
		FillTrayData(nid);
		Shell_NotifyIconW(NIM_ADD, &nid);
		return 0;
	}

	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

#pragma endregion

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow)
{
	// Last port used; another one can be forced from the command line
	g_Port = LoadPort();
	if (lpCmdLine != NULL && lpCmdLine[0] != L'\0')
	{
		int port = _wtoi(lpCmdLine);
		if (port >= 1 && port <= 65535)
			g_Port = (unsigned short)port;
	}

	INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LINK_CLASS };
	InitCommonControlsEx(&icc);

	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
		return(-1);

	WNDCLASSEXW wc = { sizeof(wc) };
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	wc.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
		GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
	g_hSmallIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
		GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
	wc.hIconSm = g_hSmallIcon;
	wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
	wc.lpszClassName = L"FlyMeToTheSkiesWindow";
	RegisterClassExW(&wc);

	RECT rc = { 0, 0, 440, 212 };
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	AdjustWindowRect(&rc, style, FALSE);

	HWND hWnd = CreateWindowExW(0, wc.lpszClassName, APP_NAME, style,
		CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
		NULL, NULL, hInstance, NULL);

	ShowWindow(hWnd, nCmdShow);
	UpdateWindow(hWnd);

	StartHttpOrWarn();

	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0)
	{
		if (!IsDialogMessageW(hWnd, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	StopHttp();
	WSACleanup();
	return 0;
}
