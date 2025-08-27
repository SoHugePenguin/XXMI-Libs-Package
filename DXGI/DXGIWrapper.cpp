#include <windows.h>
#include <dxgi1_2.h> 
#include <cstdio>
#include <d3d10misc.h>


// 指向系统原始 dxgi.dll
static HMODULE realDXGI = nullptr;
static void EnsureRealDXGI()
{
	if (realDXGI) return;

	wchar_t sysPath[MAX_PATH];
	GetSystemDirectoryW(sysPath, MAX_PATH);
	wcscat_s(sysPath, L"\\dxgi.dll");

	realDXGI = LoadLibraryW(sysPath);
	if (!realDXGI) return;
}


// ---- 未公开 ----
typedef void (WINAPI* PFN_ApplyCompatResolutionQuirking)(void);
void WINAPI ApplyCompatResolutionQuirking(void) {
	EnsureRealDXGI();
	static PFN_ApplyCompatResolutionQuirking fn = nullptr;
	if (!fn) fn = (PFN_ApplyCompatResolutionQuirking)GetProcAddress(realDXGI, "ApplyCompatResolutionQuirking");
	if (fn) fn();
}

typedef LPCWSTR(WINAPI* PFN_CompatString)(void);
LPCWSTR WINAPI CompatString(void) {
	EnsureRealDXGI();
	static PFN_CompatString fn = nullptr;
	if (!fn) fn = (PFN_CompatString)GetProcAddress(realDXGI, "CompatString");
	return fn ? fn() : L"";
}

typedef DWORD(WINAPI* PFN_CompatValue)(void);
DWORD WINAPI CompatValue(void) {
	EnsureRealDXGI();
	static PFN_CompatValue fn = nullptr;
	if (!fn) fn = (PFN_CompatValue)GetProcAddress(realDXGI, "CompatValue");
	return fn ? fn() : 0;
}

typedef HRESULT(WINAPI* PFN_DXGIDumpJournal)(void);
HRESULT WINAPI DXGIDumpJournal(void) {
	EnsureRealDXGI();
	static PFN_DXGIDumpJournal fn = nullptr;
	if (!fn) fn = (PFN_DXGIDumpJournal)GetProcAddress(realDXGI, "DXGIDumpJournal");
	return fn ? fn() : S_OK;
}

typedef HRESULT(WINAPI* PFN_PIXBeginCapture)(void*);
HRESULT WINAPI PIXBeginCapture(void* pParams) {
	EnsureRealDXGI();
	static PFN_PIXBeginCapture fn = nullptr;
	if (!fn) fn = (PFN_PIXBeginCapture)GetProcAddress(realDXGI, "PIXBeginCapture");
	return fn ? fn(pParams) : S_OK;
}

typedef HRESULT(WINAPI* PFN_PIXEndCapture)(void);
HRESULT WINAPI PIXEndCapture(void) {
	EnsureRealDXGI();
	static PFN_PIXEndCapture fn = nullptr;
	if (!fn) fn = (PFN_PIXEndCapture)GetProcAddress(realDXGI, "PIXEndCapture");
	return fn ? fn() : S_OK;
}

typedef BOOL(WINAPI* PFN_PIXGetCaptureState)(void);
BOOL WINAPI PIXGetCaptureState(void) {
	EnsureRealDXGI();
	static PFN_PIXGetCaptureState fn = nullptr;
	if (!fn) fn = (PFN_PIXGetCaptureState)GetProcAddress(realDXGI, "PIXGetCaptureState");
	return fn ? fn() : FALSE;
}

typedef void (WINAPI* PFN_SetAppCompatStringPointer)(LPCWSTR);
void WINAPI SetAppCompatStringPointer(LPCWSTR str) {
	EnsureRealDXGI();
	static PFN_SetAppCompatStringPointer fn = nullptr;
	if (!fn) fn = (PFN_SetAppCompatStringPointer)GetProcAddress(realDXGI, "SetAppCompatStringPointer");
	if (fn) fn(str);
}

typedef void (WINAPI* PFN_UpdateHMDEmulationStatus)(BOOL);
void WINAPI UpdateHMDEmulationStatus(BOOL enabled) {
	EnsureRealDXGI();
	static PFN_UpdateHMDEmulationStatus fn = nullptr;
	if (!fn) fn = (PFN_UpdateHMDEmulationStatus)GetProcAddress(realDXGI, "UpdateHMDEmulationStatus");
	if (fn) fn(enabled);
}

// ---- 已文档化的 DXGI API ----
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory)(REFIID, void**);
HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** ppFactory) {
	EnsureRealDXGI();
	static PFN_CreateDXGIFactory fn = nullptr;
	if (!fn) fn = (PFN_CreateDXGIFactory)GetProcAddress(realDXGI, "CreateDXGIFactory");
	return fn ? fn(riid, ppFactory) : E_FAIL;
}

// 原始函数指针
using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
static PFN_CreateDXGIFactory1 real_CreateDXGIFactory1 = nullptr;
HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** ppFactory) {
	MessageBoxA(NULL, "DXGI.dll proxy succcess!!!!", "DXGI Proxy", MB_OK | MB_ICONINFORMATION);
	if (!realDXGI) {
		wchar_t sysPath[MAX_PATH];
		GetSystemDirectoryW(sysPath, MAX_PATH);
		wcscat_s(sysPath, L"\\dxgi.dll");
		realDXGI = LoadLibraryW(sysPath);
		if (!realDXGI) {
			return E_FAIL;
		}
		real_CreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(realDXGI, "CreateDXGIFactory1");
		if (!real_CreateDXGIFactory1) {
			return E_FAIL;
		}
	}
	// 调用系统的原始 CreateDXGIFactory1 
	HRESULT hr = real_CreateDXGIFactory1(riid, ppFactory);
	if (FAILED(hr)) return hr;
	if (riid == __uuidof(IDXGIFactory2)) {
		IDXGIFactory2* factory2 = (IDXGIFactory2*)(*ppFactory); // 这里可以再 hook factory2 的 CreateSwapChainForHwnd 
		printf("*** Got IDXGIFactory2 %p\n", factory2);
	}
	return hr;
}

typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT, REFIID, void**);
HRESULT WINAPI CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
	EnsureRealDXGI();
	static PFN_CreateDXGIFactory2 fn = nullptr;
	if (!fn) fn = (PFN_CreateDXGIFactory2)GetProcAddress(realDXGI, "CreateDXGIFactory2");
	return fn ? fn(Flags, riid, ppFactory) : E_FAIL;
}

typedef HRESULT(WINAPI* PFN_DXGID3D10CreateDevice)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, void*, void**);
HRESULT WINAPI DXGID3D10CreateDevice(
	IDXGIAdapter* pAdapter,
	D3D10_DRIVER_TYPE DriverType,
	HMODULE Software,
	UINT Flags,
	void* pUnknown,
	void** ppDevice) {
	EnsureRealDXGI();
	static PFN_DXGID3D10CreateDevice fn = nullptr;
	if (!fn) fn = (PFN_DXGID3D10CreateDevice)GetProcAddress(realDXGI, "DXGID3D10CreateDevice");
	return fn ? fn(pAdapter, DriverType, Software, Flags, pUnknown, ppDevice) : E_FAIL;
}

typedef HRESULT(WINAPI* PFN_DXGID3D10CreateLayeredDevice)(void*, void*, UINT, void**);
HRESULT WINAPI DXGID3D10CreateLayeredDevice(void* pUnknown1, void* pUnknown2, UINT Flags, void** ppDevice) {
	EnsureRealDXGI();
	static PFN_DXGID3D10CreateLayeredDevice fn = nullptr;
	if (!fn) fn = (PFN_DXGID3D10CreateLayeredDevice)GetProcAddress(realDXGI, "DXGID3D10CreateLayeredDevice");
	return fn ? fn(pUnknown1, pUnknown2, Flags, ppDevice) : E_FAIL;
}

typedef SIZE_T(WINAPI* PFN_DXGID3D10GetLayeredDeviceSize)(void*, UINT);
SIZE_T WINAPI DXGID3D10GetLayeredDeviceSize(void* pLayers, UINT NumLayers) {
	EnsureRealDXGI();
	static PFN_DXGID3D10GetLayeredDeviceSize fn = nullptr;
	if (!fn) fn = (PFN_DXGID3D10GetLayeredDeviceSize)GetProcAddress(realDXGI, "DXGID3D10GetLayeredDeviceSize");
	return fn ? fn(pLayers, NumLayers) : 0;
}

typedef HRESULT(WINAPI* PFN_DXGID3D10RegisterLayers)(void*, UINT);
HRESULT WINAPI DXGID3D10RegisterLayers(void* pLayers, UINT NumLayers) {
	EnsureRealDXGI();
	static PFN_DXGID3D10RegisterLayers fn = nullptr;
	if (!fn) fn = (PFN_DXGID3D10RegisterLayers)GetProcAddress(realDXGI, "DXGID3D10RegisterLayers");
	return fn ? fn(pLayers, NumLayers) : E_FAIL;
}

typedef HRESULT(WINAPI* PFN_DXGIDeclareAdapterRemovalSupport)(void);
HRESULT WINAPI DXGIDeclareAdapterRemovalSupport(void) {
	EnsureRealDXGI();
	static PFN_DXGIDeclareAdapterRemovalSupport fn = nullptr;
	if (!fn) fn = (PFN_DXGIDeclareAdapterRemovalSupport)GetProcAddress(realDXGI, "DXGIDeclareAdapterRemovalSupport");
	return fn ? fn() : E_FAIL;
}

typedef HRESULT(WINAPI* PFN_DXGIDisableVBlankVirtualization)(void);
HRESULT WINAPI DXGIDisableVBlankVirtualization(void) {
	EnsureRealDXGI();
	static PFN_DXGIDisableVBlankVirtualization fn = nullptr;
	if (!fn) fn = (PFN_DXGIDisableVBlankVirtualization)GetProcAddress(realDXGI, "DXGIDisableVBlankVirtualization");
	return fn ? fn() : E_FAIL;
}

typedef HRESULT(WINAPI* PFN_DXGIGetDebugInterface1)(UINT, REFIID, void**);
HRESULT WINAPI DXGIGetDebugInterface1(UINT Flags, REFIID riid, void** pDebug) {
	EnsureRealDXGI();
	static PFN_DXGIGetDebugInterface1 fn = nullptr;
	if (!fn) fn = (PFN_DXGIGetDebugInterface1)GetProcAddress(realDXGI, "DXGIGetDebugInterface1");
	return fn ? fn(Flags, riid, pDebug) : E_FAIL;
}

typedef HRESULT(WINAPI* PFN_DXGIReportAdapterConfiguration)(void*, SIZE_T);
HRESULT WINAPI DXGIReportAdapterConfiguration(void* pAdapterConfig, SIZE_T ConfigSize) {
	EnsureRealDXGI();
	static PFN_DXGIReportAdapterConfiguration fn = nullptr;
	if (!fn) fn = (PFN_DXGIReportAdapterConfiguration)GetProcAddress(realDXGI, "DXGIReportAdapterConfiguration");
	return fn ? fn(pAdapterConfig, ConfigSize) : E_FAIL;
}

// DLL 入口点
BOOL APIENTRY DllMain(HMODULE hModule,
	DWORD  ul_reason_for_call,
	LPVOID lpReserved)
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		break;
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}


// Hook injection entrance
extern "C" __declspec(dllexport)
LRESULT CALLBACK PenguinInjectionDXGI(int nCode, WPARAM wParam, LPARAM lParam)
{
	return CallNextHookEx(0, nCode, wParam, lParam);
}
