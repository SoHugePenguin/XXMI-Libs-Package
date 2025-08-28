// Object			OS				DXGI version	Feature level
// IDXGIFactory		Win7			1.0				11.0
// IDXGIFactory1	Win7			1.0				11.0
// IDXGIFactory2	Platform update	1.2				11.1
// IDXGIFactory3	Win8.1			1.3
// IDXGIFactory4					1.4
// IDXGIFactory5					1.5
//
// IDXGISwapChain	Win7			1.0				11.0
// IDXGISwapChain1	Platform update	1.2				11.1
// IDXGISwapChain2	Win8.1			1.3
// IDXGISwapChain3	Win10			1.4
// IDXGISwapChain4					1.5

#include <d3d11_1.h>

#include "HookedDXGI.h"
#include "HackerDXGI.h"

#include "DLLMainHook.h"
#include "log.h"
#include "util.h"
#include "D3D11Wrapper.h"

#include "IniHandler.h"


// This class is for a different approach than the wrapping of the system objects
// like we do with ID3D11Device for example.  When we wrap a COM object like that,
// it's not a real C++ object, and consequently cannot use the superclass normally,
// and requires boilerplate call-throughs for every interface to the object.  We
// may only care about a 5 calls, but we have to wrap all 150 calls. 
//
// Rather than do that with DXGI, this approach will be to singly hook the calls we
// are interested in, using the Nektra In-Proc hooking.  We'll still create
// objects for encapsulation where necessary, by returning HackerDXGIFactory1
// and HackerDXGIFactory2 when platform_update is set.  We won't ever return
// HackerDXGIFactory because the minimum on Win7 is IDXGIFactory1.
//
// For our hooks:
// It is worth noting, since it took me 3 days to figure it out, than even though
// they are defined C style, that we must use STDMETHODCALLTYPE (or__stdcall) 
// because otherwise the stack is broken by the different calling conventions.
//
// In normal method calls, the 'this' parameter is implicitly added.  Since we are
// using the C style dxgi interface though, we are declaring these routines differently.
//
// Since we want to allow reentrancy for the calls, we need to use the returned
// fnOrig* to call the original, instead of the alternate approach offered by
// Deviare.

#ifdef NTDDI_WIN10
// 3DMigoto was built with the Win 10 SDK (vs2015 branch) - we can use the
// 11On12 compatibility mode to enable some 3DMigoto functionality on DX12 to
// get the overlay working and display a warning. This won't be enough to
// enable hunting or replace shaders or anything, and the only noteworthy call
// from the game we will be intercepting is Present().
#include <d3d11on12.h>
#include "PenguinHackerDXGI.h"

static PenguinDV* prepare_devices_for_dx12_warning(IUnknown* unknown_device)
{
	ID3D12CommandQueue* d3d12_queue = NULL;
	ID3D12Device* d3d12_device = NULL;
	ID3D11Device* d3d11_device = NULL;
	ID3D11DeviceContext* d3d11_context = NULL;
	PenguinDV* dev_wrap = NULL;
	PenguinDC* context_wrap = NULL;
	HRESULT hr;

	if (FAILED(unknown_device->QueryInterface(IID_ID3D12CommandQueue, (void**)&d3d12_queue)))
		goto out;

	LogToWindow("Preparing to enable D3D11On12 compatibility mode for overlay...\n");

	if (FAILED(d3d12_queue->GetDevice(IID_ID3D12Device, (void**)&d3d12_device)))
		goto out;

	LogToWindow(" ID3D12Device: %p\n", d3d12_device);

	// If you need the debug layer, force it in dxcpl.exe instead of here,
	// since we won't have enabled it on the D3D12 device, and doing so now
	// would reset it. If the game has used the flag to prevent the control
	// panel's registry key override we'd need to go to more heroics.
	hr = (*_D3D11On12CreateDevice)(d3d12_device,
		0, /* flags */
		NULL, 0, /* feature levels */
		(IUnknown**)&d3d12_queue,
		1, /* num queues */
		0, /* node mask */
		&d3d11_device, &d3d11_context, NULL);
	if (FAILED(hr)) {
		LogToWindow("D3D11On12CreateDevice failed: 0x%x\n", hr);
		goto out;
	}

	LogToWindow(" ID3D11Device: %p\n", d3d11_device);
	LogToWindow(" ID3D11DeviceContext: %p\n", d3d11_context);

	dev_wrap = new PenguinDV((ID3D11Device1*)d3d11_device, (ID3D11DeviceContext1*)d3d11_context);
	context_wrap = PenguinDCFactory((ID3D11Device1*)d3d11_device, (ID3D11DeviceContext1*)d3d11_context);

	LogToWindow(" PenguinDV: %p\n", dev_wrap);
	LogToWindow(" PenguinDC: %p\n", context_wrap);

	dev_wrap->SetPenguinDC(context_wrap);
	context_wrap->SetPenguinDV(dev_wrap);
	dev_wrap->Create3DMigotoResources();
	context_wrap->Bind3DMigotoResources();

	// We're going to intentionally leak the D3D11 objects, because we have
	// nothing to manage the reference to them and keep them alive - the
	// Hacker wrappers don't, because they expect the game to.

out:
	if (d3d12_device)
		d3d12_device->Release();
	if (d3d12_queue)
		d3d12_queue->Release();

	return dev_wrap;
}

#else

static PenguinDV* prepare_devices_for_dx12_warning(IUnknown* unknown_device)
{
	return NULL;
}

#endif

// Takes an IUnknown device and finds the corresponding HackerDevice and
// DirectX device interfaces. The passed in IUnknown may be modified to point
// to the real DirectX device so ensure that it will be safe to pass to the
// original CreateSwapChain call.
static PenguinDV* sort_out_swap_chain_device_mess(IUnknown** device)
{
	PenguinDV* PenguinDV;

	// pDevice could be one of several different things:
	// - It could be a HackerDevice, if the game called CreateSwapChain()
	//   with the HackerDevice we returned from CreateDevice().
	// - It could be the original ID3D11Device if we have hooking enabled,
	//   as this has hooks to call into our code instead of being wrapped.
	// - It could be an IDXGIDevice if the game is being tricky (e.g. UE4
	//   finds this from QueryInterface on the ID3D11Device). This is
	//   legal, as an IDXGIDevice is just an interface to a D3D Device,
	//   same as ID3D11Device is an interface to a D3D Device.
	// - Since we have hooked this function, CreateDeviceAndSwapChain()
	//   could also call in here straight from the real d3d11.dll with an
	//   ID3D11Device that we haven't seen or wrapped yet. We avoid this
	//   case by re-implementing CreateDeviceAndSwapChain() ourselves, so
	//   now we will get a HackerDevice in that case.
	// - It could be an ID3D10Device
	// - It could be an ID3D12CommandQueue
	// - It could be some other thing we haven't heard of yet
	//
	// We call lookup_hacker_device to look it up from the IUnknown,
	// relying on COM's guarantee that IUnknown will match for different
	// interfaces to the same object, and noting that this call will bump
	// the refcount on hackerDevice:
	PenguinDV = lookup_hacker_device(*device);
	if (PenguinDV) {
		// Ensure that pDevice points to the real DX device before
		// passing it into DX for safety. We can probably get away
		// without this since it's an IUnknown and DX will have to
		// QueryInterface() it, but let's not tempt fate:
		*device = (PenguinDV)->GetPossiblyHookedOrigDevice1();
	}
	else {
		LogToWindow("WARNING: Could not locate PenguinDV for %p\n", *device);
		analyse_iunknown(*device);

		if (check_interface_supported(*device, IID_ID3D11Device)) {
			// If we do end up in another situation where we are
			// seeing a device for the first time (like
			// CreateDeviceAndSwapChain calling back into us), we
			// could consider creating our HackerDevice here. But
			// for now we aren't expecting this to happen, so treat
			// it as fatal if it does.
			//
			// D3D11On12CreateDevice() could possibly lead us here,
			// depending on how that works.
			LogToWindow("BUG: Unwrapped ID3D11Device!\n");
			DoubleBeepExit();
		}

		LogToWindow("FATAL: Unsupported DirectX Version!\n");

		// Normally we flush the log file on the Present() call, but if
		// we didn't wrap the swap chain that will probably never
		// happen. Flush it now to ensure the above message shows up so
		// we know why:
		fflush(LogFile);

		// The swap chain is being created with a device that does NOT
		// support the DX11 API. 3DMigoto is probably doomed to fail at
		// this point, unless the game is about to retry with a
		// different device. Maybe we are better off just doing a
		// DoubleBeepExit(), but let's try to make sure we at least
		// don't crash things, which may be important if an application
		// uses mixed APIs for some reason - I've certainly seen the
		// Origin overlay try to init every DX version under the sun,
		// though I don't think it actually tried creating swap chains
		// for them. Still issue an audible warning as a hint at what
		// has happened:
		PenguinDV = prepare_devices_for_dx12_warning(*device);
		if (PenguinDV)
			LogOverlayW(LOG_DIRE, L"3DMigoto does not support DirectX 12\nPlease set the game to use DirectX 11\n");
		else
			BeepProfileFail();
	}

	return PenguinDV;
}

void ForceDisplayMode(DXGI_MODE_DESC* BufferDesc)
{
	// Historically we have only forced the refresh rate when full-screen.
	// I don't know if we ever had a good reason for that, but it
	// complicates forcing the refresh rate in games that start windowed
	// and later switch to full screen, so now forcing it unconditionally
	// to see how that goes. Helps Unity games work with 3D TV Play.
	//
	// UE4 does SetFullscreenState -> ResizeBuffers -> ResizeTarget
	// Unity does ResizeTarget -> SetFullscreenState -> ResizeBuffers
	if (G->SCREEN_REFRESH >= 0)
	{
		// FIXME: This may disable flipping (and use blitting instead)
		// if the forced numerator and denominator does not exactly
		// match a mode enumerated on the output. e.g. We would force
		// 60Hz as 60/1, but the display might actually use 60000/1001
		// for 60Hz and we would lose flipping and degrade performance.
		BufferDesc->RefreshRate.Numerator = G->SCREEN_REFRESH;
		BufferDesc->RefreshRate.Denominator = 1;
		LogToWindow("->Forcing refresh rate to = %f\n",
			(float)BufferDesc->RefreshRate.Numerator / (float)BufferDesc->RefreshRate.Denominator);
	}
	if (G->SCREEN_WIDTH >= 0)
	{
		BufferDesc->Width = G->SCREEN_WIDTH;
		LogToWindow("->Forcing Width to = %d\n", BufferDesc->Width);
	}
	if (G->SCREEN_HEIGHT >= 0)
	{
		BufferDesc->Height = G->SCREEN_HEIGHT;
		LogToWindow("->Forcing Height to = %d\n", BufferDesc->Height);
	}
}


// -----------------------------------------------------------------------------
// This tweaks the parameters passed to the real CreateSwapChain, to change behavior.
// These global parameters come originally from the d3dx.ini, so the user can
// change them.
//
// There is now also ForceDisplayParams1 which has some overlap.

static void ForceDisplayParams(DXGI_SWAP_CHAIN_DESC* pDesc)
{
	if (pDesc == NULL)
		return;

	LogToWindow("     Windowed = %d\n", pDesc->Windowed);
	LogToWindow("     Width = %d\n", pDesc->BufferDesc.Width);
	LogToWindow("     Height = %d\n", pDesc->BufferDesc.Height);
	LogToWindow("     Refresh rate = %f\n",
		(float)pDesc->BufferDesc.RefreshRate.Numerator / (float)pDesc->BufferDesc.RefreshRate.Denominator);
	LogToWindow("     BufferCount = %d\n", pDesc->BufferCount);
	LogToWindow("     SwapEffect = %d\n", pDesc->SwapEffect);
	LogToWindow("     Flags = 0x%x\n", pDesc->Flags);

	if (G->SCREEN_UPSCALING == 0 && G->SCREEN_FULLSCREEN > 0)
	{
		pDesc->Windowed = false;
		LogToWindow("->Forcing Windowed to = %d\n", pDesc->Windowed);
	}

	if (G->SCREEN_FULLSCREEN == 2 || G->SCREEN_UPSCALING > 0)
	{
		// We install this hook on demand to avoid any possible
		// issues with hooking the call when we don't need it:
		// Unconfirmed, but possibly related to:
		// https://forums.geforce.com/default/topic/685657/3d-vision/3dmigoto-now-open-source-/post/4801159/#4801159
		//
		// This hook is also very important in case of Upscaling
		InstallSetWindowPosHook();
	}

	ForceDisplayMode(&pDesc->BufferDesc);
}

// Different variant for the CreateSwapChainForHwnd.
//
// We absolutely need the force full screen in order to enable 3D.  
// Batman Telltale needs this.
// The rest of the variants are less clear.

static void ForceDisplayParams1(DXGI_SWAP_CHAIN_DESC1* pDesc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc)
{
	if (pFullscreenDesc) {
		LogToWindow("     Windowed = %d\n", pFullscreenDesc->Windowed);
		LogToWindow("     Refresh rate = %f\n",
			(float)pFullscreenDesc->RefreshRate.Numerator / (float)pFullscreenDesc->RefreshRate.Denominator);

		if (G->SCREEN_FULLSCREEN > 0)
		{
			pFullscreenDesc->Windowed = false;
			LogToWindow("->Forcing Windowed to = %d\n", pFullscreenDesc->Windowed);
		}

		if (G->SCREEN_REFRESH >= 0)
		{
			// Historically we have only forced the refresh rate when full-screen.
			// I don't know if we ever had a good reason for that, but it
			// complicates forcing the refresh rate in games that start windowed
			// and later switch to full screen, so now forcing it unconditionally
			// to see how that goes. Helps Unity games work with 3D TV Play.
			//
			// UE4 does SetFullscreenState -> ResizeBuffers -> ResizeTarget
			// Unity does ResizeTarget -> SetFullscreenState -> ResizeBuffers
			pFullscreenDesc->RefreshRate.Numerator = G->SCREEN_REFRESH;
			pFullscreenDesc->RefreshRate.Denominator = 1;
			LogToWindow("->Forcing refresh rate to = %f\n",
				(float)pFullscreenDesc->RefreshRate.Numerator / (float)pFullscreenDesc->RefreshRate.Denominator);
		}
	}

	if (G->SCREEN_FULLSCREEN == 2)
	{
		// We install this hook on demand to avoid any possible
		// issues with hooking the call when we don't need it:
		// Unconfirmed, but possibly related to:
		// https://forums.geforce.com/default/topic/685657/3d-vision/3dmigoto-now-open-source-/post/4801159/#4801159

		InstallSetWindowPosHook();
	}

	if (pDesc)
	{
		LogToWindow("     Width = %d\n", pDesc->Width);
		LogToWindow("     Height = %d\n", pDesc->Height);
		LogToWindow("     BufferCount = %d\n", pDesc->BufferCount);
		LogToWindow("     SwapEffect = %d\n", pDesc->SwapEffect);
		LogToWindow("     Flags = 0x%x\n", pDesc->Flags);

		if (G->SCREEN_WIDTH >= 0)
		{
			LogOverlay(LOG_DIRE, "*** Unimplemented feature to force screen width in CreateSwapChainForHwnd\n");
		}
		if (G->SCREEN_HEIGHT >= 0)
		{
			LogOverlay(LOG_DIRE, "*** Unimplemented feature to force screen height in CreateSwapChainForHwnd\n");
		}
	}
}

// If we ever restored D3D11CreateDeviceAndSwapChain to call through to the
// original it would need to call these two functions that have been refactored
// out of CreateSwapChain. The factory 2 variants have their own override /
// wrap helpers refactored out for now, because there are a few small
// differences between the two, and we currently lack upscaling on the factory
// 2 variants - we could likely go further and share more code if we wanted
// though.

void override_swap_chain(DXGI_SWAP_CHAIN_DESC* pDesc, DXGI_SWAP_CHAIN_DESC* origSwapChainDesc)
{
	if (pDesc == nullptr)
		return;

	// Save window handle so we can translate mouse coordinates to the window:
	G->hWnd = pDesc->OutputWindow;

	if (G->SCREEN_UPSCALING > 0)
	{
		// Copy input swap chain desc in case it's modified
		memcpy(origSwapChainDesc, pDesc, sizeof(DXGI_SWAP_CHAIN_DESC));

		// For the upscaling case, fullscreen has to be set after swap chain is created
		pDesc->Windowed = true;
	}

	// Required in case the software mouse and upscaling are on at the same time
	// TODO: Use a helper class to track *all* different resolutions
	G->GAME_INTERNAL_WIDTH = pDesc->BufferDesc.Width;
	G->GAME_INTERNAL_HEIGHT = pDesc->BufferDesc.Height;

	if (G->mResolutionInfo.from == GetResolutionFrom::SWAP_CHAIN)
	{
		// TODO: Use a helper class to track *all* different resolutions
		G->mResolutionInfo.width = pDesc->BufferDesc.Width;
		G->mResolutionInfo.height = pDesc->BufferDesc.Height;
		LogToWindow("Got resolution from swap chain: %ix%i\n",
			G->mResolutionInfo.width, G->mResolutionInfo.height);
	}

	ForceDisplayParams(pDesc);
}

static void override_factory2_swap_chain(
	_In_ const DXGI_SWAP_CHAIN_DESC1** ppDesc,
	_In_ DXGI_SWAP_CHAIN_DESC1* descCopy,
	_In_opt_ DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenCopy)
{
	if (ppDesc && *ppDesc != nullptr)
	{
		// Required in case the software mouse and upscaling are on at the same time
		// TODO: Use a helper class to track *all* different resolutions
		G->GAME_INTERNAL_WIDTH = (*ppDesc)->Width;
		G->GAME_INTERNAL_HEIGHT = (*ppDesc)->Height;

		if (G->mResolutionInfo.from == GetResolutionFrom::SWAP_CHAIN)
		{
			// TODO: Use a helper class to track *all* different resolutions
			G->mResolutionInfo.width = (*ppDesc)->Width;
			G->mResolutionInfo.height = (*ppDesc)->Height;
			LogToWindow("  Got resolution from swap chain: %ix%i\n",
				G->mResolutionInfo.width, G->mResolutionInfo.height);
		}
	}

	// Inputs structures are const, so copy them to allow modification. The
	// storage for the copies is allocated by the caller, but the caller
	// doesn't directly use the copies themselves - we update the pointers
	// to point at the copies instead, which allows the cases where these
	// pointers were originally NULL to maintain that.
	if (ppDesc && *ppDesc) {
		memcpy(descCopy, *ppDesc, sizeof(DXGI_SWAP_CHAIN_DESC1));
		*ppDesc = descCopy;
	}
	ForceDisplayParams1(descCopy, fullscreenCopy);

	// FIXME: Implement upscaling
}



// 定义 Present 原型
typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain* This, UINT SyncInterval, UINT Flags);

// 全局保存原始函数指针
PresentFn g_oPresent = nullptr;
PenguinDV* g_pPenguinDV = nullptr;
PenguinDC* g_pPenguinDC = nullptr;
IDXGISwapChain1* g_pSwapChain = nullptr;

HRESULT __stdcall hkPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
{
	if (!(Flags & DXGI_PRESENT_TEST)) {
		// --- pre-present ---
		PenguinTools::DoFrameActions();

		if (PenguinTools::sOverlay && !G->suppress_overlay) {
			PenguinTools::sOverlay->DrawOverlay();
		}
		G->suppress_overlay = false;
	}

	get_tls()->hooking_quirk_protection = true;
	HRESULT hr = g_oPresent(This, SyncInterval, Flags);
	get_tls()->hooking_quirk_protection = false;

	return hr;
}


// 初始化 Hook
void HookSwapChain(IDXGISwapChain* pSwapChain)
{
	void** pVTable = *(void***)pSwapChain;

	// Present 在 vtable 第 8 个位置
	g_oPresent = (PresentFn)pVTable[8];

	DWORD oldProtect;
	VirtualProtect(&pVTable[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
	pVTable[8] = (void*)&hkPresent;
	VirtualProtect(&pVTable[8], sizeof(void*), oldProtect, &oldProtect);

    PenguinTools::Init(pSwapChain, g_pPenguinDV, g_pPenguinDV->GetPenguinDC());

	LogToWindow("[Hook] Present 已替换\n");
}






void wrap_swap_chain(PenguinDV* PenguinDV,
	IDXGISwapChain** ppSwapChain,
	DXGI_SWAP_CHAIN_DESC* overrideSwapChainDesc,
	DXGI_SWAP_CHAIN_DESC* origSwapChainDesc)
{
	LogToWindow(" wrap_swap_chain 1111111111111111111111111111111111");
	PenguinDC* PenguinDC = NULL;
	PenguinSC* swapchainWrap = NULL;
	IDXGISwapChain1* origSwapChain = NULL;

	if (!PenguinDV || !ppSwapChain || !*ppSwapChain)
		return;

	// Always upcast to IDXGISwapChain1 whenever possible.
	// If the upcast fails, that means we have a normal IDXGISwapChain,
	// but we'll still store it as an IDXGISwapChain1.  It's a little
	// weird to reinterpret this way, but should cause no problems in
	// the Win7 no platform_udpate case.
	if (SUCCEEDED((*ppSwapChain)->QueryInterface(IID_PPV_ARGS(&origSwapChain))))
		(*ppSwapChain)->Release();
	else
		origSwapChain = reinterpret_cast<IDXGISwapChain1*>(*ppSwapChain);

	PenguinDC = PenguinDV->GetPenguinDC();

	// Original swapchain has been successfully created. Now we want to
	// wrap the returned swapchain as either HackerSwapChain or HackerUpscalingSwapChain.

	if (G->SCREEN_UPSCALING == 0)		// Normal case
	{
		swapchainWrap = new PenguinSC(origSwapChain, PenguinDV, PenguinDC);
		LogToWindow("  PenguinSC %p created to wrap %p\n", swapchainWrap, origSwapChain);
	}
	else								// Upscaling case
	{
		swapchainWrap = new HackerUpscalingSwapChain(origSwapChain, PenguinDV, PenguinDC,
			origSwapChainDesc, overrideSwapChainDesc->BufferDesc.Width, overrideSwapChainDesc->BufferDesc.Height);
		LogToWindow("  HackerUpscalingSwapChain %p created to wrap %p.\n", swapchainWrap, origSwapChain);

		if (G->SCREEN_UPSCALING == 2 || !origSwapChainDesc->Windowed)
		{
			// Some games react very strange (like render nothing) if set full screen state is called here)
			// Other games like The Witcher 3 need the call to ensure entering the full screen on start
			// (seems to be game internal stuff)  ToDo: retest if this is still necessary, lots of changes.
			origSwapChain->SetFullscreenState(TRUE, nullptr);
		}
	}

	// For 3DMigoto's crash handler emergency switch to windowed mode function:
	if (overrideSwapChainDesc && !overrideSwapChainDesc->Windowed)
		last_fullscreen_swap_chain = origSwapChain;

	// When creating a new swapchain, we can assume this is the game creating
	// the most important object. Return the wrapped swapchain to the game so it
	// will call our Present.
	*ppSwapChain = swapchainWrap;

	LogToWindow("-> PenguinSC = %p wrapper of ppSwapChain = %p\n", swapchainWrap, origSwapChain);
}

static void wrap_factory2_swap_chain(
	_In_ PenguinDV* PenguinDV,
	_Out_ IDXGISwapChain1** ppSwapChain)
{
	LogToWindow("wrap_factory2_swap_chain called");

	if (!PenguinDV || !ppSwapChain || !*ppSwapChain)
		return;

	g_pPenguinDV = PenguinDV;
	g_pPenguinDC = PenguinDV->GetPenguinDC();
	g_pSwapChain = *ppSwapChain;

	// 不再替换为 PenguinSC，直接 Hook Present
	HookSwapChain(*ppSwapChain);

	LogToWindow("Saved PenguinDV=%p, PenguinDC=%p, SwapChain=%p\n",
		g_pPenguinDV, g_pPenguinDC, g_pSwapChain);
}


// -----------------------------------------------------------------------------
// Actual hook for any IDXGICreateSwapChainForHwnd calls the game makes.
// This can only be called with Win7+platform_update or greater, using
// the IDXGIFactory2.
// 
// This type of SwapChain cannot be made through the CreateDeviceAndSwapChain,
// so there is only one logical path to create this, which is 
// IDXGIFactory2->CreateSwapChainForHwnd.  That means that the Device has
// already been created with CreateDevice, and dereferenced through the 
// chain of QueryInterface calls to get the IDXGIFactory2.

HRESULT(__stdcall* fnOrigCreateSwapChainForHwnd)(
	IDXGIFactory2* This,
	/* [annotation][in] */
	_In_  IUnknown* pDevice,
	/* [annotation][in] */
	_In_  HWND hWnd,
	/* [annotation][in] */
	_In_  const DXGI_SWAP_CHAIN_DESC1* pDesc,
	/* [annotation][in] */
	_In_opt_  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
	/* [annotation][in] */
	_In_opt_  IDXGIOutput* pRestrictToOutput,
	/* [annotation][out] */
	_Out_  IDXGISwapChain1** ppSwapChain) = nullptr;

HRESULT __stdcall Hooked_CreateSwapChainForHwnd(
	IDXGIFactory2* This,
	/* [annotation][in] */
	_In_  IUnknown* pDevice,
	/* [annotation][in] */
	_In_  HWND hWnd,
	/* [annotation][in] */
	_In_  const DXGI_SWAP_CHAIN_DESC1* pDesc,
	/* [annotation][in] */
	_In_opt_  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
	/* [annotation][in] */
	_In_opt_  IDXGIOutput* pRestrictToOutput,
	/* [annotation][out] */
	_Out_  IDXGISwapChain1** ppSwapChain)
{
	if (get_tls()->hooking_quirk_protection) {
		LogToWindow("Hooking Quirk: Unexpected call back into IDXGIFactory2::CreateSwapChainForHwnd, passing through\n");
		// No known cases
		return fnOrigCreateSwapChainForHwnd(This, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
	}

	PenguinDV* PenguinDV = NULL;
	DXGI_SWAP_CHAIN_DESC1 descCopy = { 0 };
	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenCopy = { 0 };

	LogToWindow("*** Hooked IDXGIFactory2::CreateSwapChainForHwnd(%p) called\n", This);
	LogToWindow("  Device = %p\n", pDevice);
	LogToWindow("  SwapChain = %p\n", ppSwapChain);
	LogToWindow("  Description1 = %p\n", pDesc);
	LogToWindow("  FullScreenDescription = %p\n", pFullscreenDesc);

	// Save window handle so we can translate mouse coordinates to the window:
	G->hWnd = hWnd;

	PenguinDV = sort_out_swap_chain_device_mess(&pDevice);

	// The game may pass in NULL for pFullscreenDesc, but we may still want
	// to override it. To keep things simpler we always use our own full
	// screen struct, which is either a copy of the one the game passed in,
	// or specifies windowed mode, which should be equivelent to NULL.
	fullscreenCopy.Windowed = true;
	if (pFullscreenDesc)
		memcpy(&fullscreenCopy, pFullscreenDesc, sizeof(DXGI_SWAP_CHAIN_FULLSCREEN_DESC));

	override_factory2_swap_chain(&pDesc, &descCopy, &fullscreenCopy);

	get_tls()->hooking_quirk_protection = true;
	HRESULT hr = fnOrigCreateSwapChainForHwnd(This, pDevice, hWnd, pDesc, &fullscreenCopy, pRestrictToOutput, ppSwapChain);
	get_tls()->hooking_quirk_protection = false;
	if (FAILED(hr))
	{
		LogToWindow("->Failed result %#x\n\n", hr);
		goto out_release;
	}

	LogToWindow("ppSwapChain =============================");
	LogToWindow("Hooked_CreateSwapChainForHwnd   wrap_factory2_swap_chain");
	wrap_factory2_swap_chain(PenguinDV, ppSwapChain);

	LogToWindow("->return result %#x\n", hr);
out_release:
	if (PenguinDV)
		PenguinDV->Release();
	return hr;
}

// This is used for Windows Store apps:

HRESULT(__stdcall* fnOrigCreateSwapChainForCoreWindow)(
	IDXGIFactory2* This,
	/* [annotation][in] */
	_In_  IUnknown* pDevice,
	/* [annotation][in] */
	_In_  IUnknown* pWindow,
	/* [annotation][in] */
	_In_  const DXGI_SWAP_CHAIN_DESC1* pDesc,
	/* [annotation][in] */
	_In_opt_  IDXGIOutput* pRestrictToOutput,
	/* [annotation][out] */
	_COM_Outptr_  IDXGISwapChain1** ppSwapChain) = nullptr;

HRESULT __stdcall Hooked_CreateSwapChainForCoreWindow(
	IDXGIFactory2* This,
	/* [annotation][in] */
	_In_  IUnknown* pDevice,
	/* [annotation][in] */
	_In_  IUnknown* pWindow,
	/* [annotation][in] */
	_In_  const DXGI_SWAP_CHAIN_DESC1* pDesc,
	/* [annotation][in] */
	_In_opt_  IDXGIOutput* pRestrictToOutput,
	/* [annotation][out] */
	_COM_Outptr_  IDXGISwapChain1** ppSwapChain)
{
	if (get_tls()->hooking_quirk_protection) {
		LogToWindow("Hooking Quirk: Unexpected call back into IDXGIFactory2::CreateSwapChainForCoreWindow, passing through\n");
		// No known cases
		return fnOrigCreateSwapChainForCoreWindow(This, pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
	}

	PenguinDV* PenguinDV = NULL;
	DXGI_SWAP_CHAIN_DESC1 descCopy = { 0 };

	LogToWindow("*** Hooked IDXGIFactory2::CreateSwapChainForCoreWindow(%p) called\n", This);
	LogToWindow("  Device = %p\n", pDevice);
	LogToWindow("  SwapChain = %p\n", ppSwapChain);
	LogToWindow("  Description1 = %p\n", pDesc);

	// FIXME: Need the hWnd for mouse support

	PenguinDV = sort_out_swap_chain_device_mess(&pDevice);

	override_factory2_swap_chain(&pDesc, &descCopy, NULL);

	get_tls()->hooking_quirk_protection = true;
	HRESULT hr = fnOrigCreateSwapChainForCoreWindow(This, pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
	get_tls()->hooking_quirk_protection = false;
	if (FAILED(hr))
	{
		LogToWindow("->Failed result %#x\n\n", hr);
		goto out_release;
	}

	LogToWindow("Hooked_CreateSwapChainForCoreWindow   wrap_factory2_swap_chain");
	wrap_factory2_swap_chain(PenguinDV, ppSwapChain);

	LogToWindow("->return result %#x\n", hr);
out_release:
	if (PenguinDV)
		PenguinDV->Release();
	return hr;
}

// Not sure we actually care about anything using DirectComposition, but for
// completeness do so anyway, since it is virtually identical to the last two
// anyway, just with no window.

HRESULT(__stdcall* fnOrigCreateSwapChainForComposition)(
	IDXGIFactory2* This,
	/* [annotation][in] */
	_In_  IUnknown* pDevice,
	/* [annotation][in] */
	_In_  const DXGI_SWAP_CHAIN_DESC1* pDesc,
	/* [annotation][in] */
	_In_opt_  IDXGIOutput* pRestrictToOutput,
	/* [annotation][out] */
	_COM_Outptr_  IDXGISwapChain1** ppSwapChain) = nullptr;

HRESULT __stdcall Hooked_CreateSwapChainForComposition(
	IDXGIFactory2* This,
	/* [annotation][in] */
	_In_  IUnknown* pDevice,
	/* [annotation][in] */
	_In_  const DXGI_SWAP_CHAIN_DESC1* pDesc,
	/* [annotation][in] */
	_In_opt_  IDXGIOutput* pRestrictToOutput,
	/* [annotation][out] */
	_COM_Outptr_  IDXGISwapChain1** ppSwapChain)
{
	if (get_tls()->hooking_quirk_protection) {
		LogToWindow("Hooking Quirk: Unexpected call back into IDXGIFactory2::CreateSwapChainForComposition, passing through\n");
		// No known cases
		return fnOrigCreateSwapChainForComposition(This, pDevice, pDesc, pRestrictToOutput, ppSwapChain);
	}

	PenguinDV* PenguinDV = NULL;
	DXGI_SWAP_CHAIN_DESC1 descCopy = { 0 };

	LogToWindow("*** Hooked IDXGIFactory2::CreateSwapChainForComposition(%p) called\n", This);
	LogToWindow("  Device = %p\n", pDevice);
	LogToWindow("  SwapChain = %p\n", ppSwapChain);
	LogToWindow("  Description1 = %p\n", pDesc);

	// FIXME: Need the hWnd for mouse support

	PenguinDV = sort_out_swap_chain_device_mess(&pDevice);

	override_factory2_swap_chain(&pDesc, &descCopy, NULL);

	get_tls()->hooking_quirk_protection = true;
	HRESULT hr = fnOrigCreateSwapChainForComposition(This, pDevice, pDesc, pRestrictToOutput, ppSwapChain);
	get_tls()->hooking_quirk_protection = false;
	if (FAILED(hr))
	{
		LogToWindow("->Failed result %#x\n\n", hr);
		goto out_release;
	}

	LogToWindow("Hooked_CreateSwapChainForComposition   wrap_factory2_swap_chain");
	wrap_factory2_swap_chain(PenguinDV, ppSwapChain);

	LogToWindow("->return result %#x\n", hr);
out_release:
	if (PenguinDV)
		PenguinDV->Release();
	return hr;
}

// -----------------------------------------------------------------------------
// This hook should work in all variants, including the CreateSwapChain1
// and CreateSwapChainForHwnd

static void HookFactory2CreateSwapChainMethods(IDXGIFactory2* dxgiFactory)
{
	DWORD dwOsErr;
	SIZE_T hook_id;

	LogToWindow("*** IDXGIFactory2 creating hooks for CreateSwapChain variants. \n");

	// 最常见，绑定到一个传统的 Win32 窗口。
	dwOsErr = cHookMgr.Hook(&hook_id, (void**)&fnOrigCreateSwapChainForHwnd,
		lpvtbl_CreateSwapChainForHwnd(dxgiFactory), Hooked_CreateSwapChainForHwnd, 0);

	//if (dwOsErr == ERROR_SUCCESS)
	//	LogToWindow("  Successfully installed IDXGIFactory2->CreateSwapChainForHwnd hook.\n");
	//else
	//	LogToWindow("  *** Failed install IDXGIFactory2->CreateSwapChainForHwnd hook.\n");


	//dwOsErr = cHookMgr.Hook(&hook_id, (void**)&fnOrigCreateSwapChainForCoreWindow,
	//	lpvtbl_CreateSwapChainForCoreWindow(dxgiFactory), Hooked_CreateSwapChainForCoreWindow, 0);

	//if (dwOsErr == ERROR_SUCCESS)
	//	LogToWindow("  Successfully installed IDXGIFactory2->CreateSwapChainForCoreWindow hook.\n");
	//else
	//	LogToWindow("  *** Failed install IDXGIFactory2->CreateSwapChainForCoreWindow hook.\n");


	//dwOsErr = cHookMgr.Hook(&hook_id, (void**)&fnOrigCreateSwapChainForComposition,
	//	lpvtbl_CreateSwapChainForComposition(dxgiFactory), Hooked_CreateSwapChainForComposition, 0);

	//if (dwOsErr == ERROR_SUCCESS)
	//	LogToWindow("  Successfully installed IDXGIFactory2->CreateSwapChainForComposition hook.\n");
	//else
	//	LogToWindow("  *** Failed install IDXGIFactory2->CreateSwapChainForComposition hook.\n");
}

// -----------------------------------------------------------------------------

static HRESULT(__stdcall* fnOrigCreateSwapChain)(
	IDXGIFactory* This,
	/* [annotation][in] */
	_In_  IUnknown* pDevice,
	/* [annotation][in] */
	_In_  DXGI_SWAP_CHAIN_DESC* pDesc,
	/* [annotation][out] */
	_Out_  IDXGISwapChain** ppSwapChain) = nullptr;


// Actual hook for any IDXGICreateSwapChain calls the game makes.
//
// There are two primary paths that can arrive here.
//
// ---1. d3d11->CreateDeviceAndSwapChain
//	This path arrives here with a normal ID3D11Device1 device, not a HackerDevice.
//	This is called implictly from the middle of CreateDeviceAndSwapChain by
//	merit of the fact that we have hooked that call. This is really an
//	implementation detail of DirectX and (nowadays) we explicitly want to
//	ignore this call, which we do via the reentrant hooking quirk
//	detection. Our CreateDeviceAndSwapChain wrapper will handle wrapping
//	the swap chain in this case.
//
//	Note that the Steam overlay is known to rely on this same DirectX
//	implementation detail and in the past we inadvertently bypassed them by
//	redirecting the swap chain creation in CreateDeviceAndSwapChain
//	ourselves in such a way that their hook may never have been called,
//	depending on which tool managed to hook in first (3DMigoto getting in
//	first was the fail case as we could then call the original
//	CreateSwapChain without going through Steam's hook).
//
// 2. IDXGIFactory->CreateSwapChain after CreateDevice
//	This path requires a pDevice passed in, which is a HackerDevice.  This is the
//	secret path, where they take the Device and QueryInterface to get IDXGIDevice
//	up to getting Factory, where they call CreateSwapChain. In this path, we can
//	expect the input pDevice to have already been setup as a HackerDevice.
//
//	It's not really secret, given the procedure is readily documented on MSDN:
//	https://docs.microsoft.com/en-gb/windows/desktop/api/dxgi/nn-dxgi-idxgifactory#remarks
//	  -DSS
//
//
// In prior code, we were looking for possible IDXGIDevice's as the pDevice input.
// That should not be a problem now, because we are specifically trying to cast
// that input into an ID3D11Device1 using QueryInterface.  Leaving the original
// code commented out at the bottom of the file, for reference.

HRESULT __stdcall Hooked_CreateSwapChain(
	IDXGIFactory* This,
	/* [annotation][in] */
	_In_  IUnknown* pDevice,
	/* [annotation][in] */
	_In_  DXGI_SWAP_CHAIN_DESC* pDesc,
	/* [annotation][out] */
	_Out_  IDXGISwapChain** ppSwapChain)
{
	if (get_tls()->hooking_quirk_protection) {
		LogToWindow("Hooking Quirk: Unexpected call back into IDXGIFactory::CreateSwapChain, passing through\n");
		// Known case: DirectX implements D3D11CreateDeviceAndSwapChain
		//             by calling DXGIFactory::CreateSwapChain (if
		//             ppSwapChain is not NULL), triggering this if we
		//             call the former and have hooked the later.
		//             Note that the Steam overlay depends on this.
		return fnOrigCreateSwapChain(This, pDevice, pDesc, ppSwapChain);
	}

	LogToWindow("\n*** Hooked IDXGIFactory::CreateSwapChain(%p) called\n", This);
	LogToWindow("  Device = %p\n", pDevice);
	LogToWindow("  SwapChain = %p\n", ppSwapChain);
	LogToWindow("  Description = %p\n", pDesc);

	PenguinDV* PenguinDV = NULL;
	DXGI_SWAP_CHAIN_DESC origSwapChainDesc;

	PenguinDV = sort_out_swap_chain_device_mess(&pDevice);

	override_swap_chain(pDesc, &origSwapChainDesc);

	get_tls()->hooking_quirk_protection = true;
	HRESULT hr = fnOrigCreateSwapChain(This, pDevice, pDesc, ppSwapChain);
	get_tls()->hooking_quirk_protection = false;
	if (FAILED(hr))
	{
		LogToWindow("->Failed result %#x\n\n", hr);
		goto out_release;
	}

	IDXGISwapChain* retChain = ppSwapChain ? *ppSwapChain : nullptr;
	LogToWindow("  CreateSwapChain returned handle = %p\n", retChain);
	analyse_iunknown(retChain);

	wrap_swap_chain(PenguinDV, ppSwapChain, pDesc, &origSwapChainDesc);

	LogToWindow("->IDXGIFactory::CreateSwapChain return result %#x\n\n", hr);
out_release:
	if (PenguinDV)
		PenguinDV->Release();
	return hr;
}


// -----------------------------------------------------------------------------
// This hook should work in all variants, including the CreateSwapChain1
// and CreateSwapChainForHwnd

static void HookCreateSwapChain(void* factory)
{
	LogToWindow("*** IDXGIFactory creating hook for CreateSwapChain. \n");

	IDXGIFactory* dxgiFactory = reinterpret_cast<IDXGIFactory*>(factory);

	SIZE_T hook_id;
	DWORD dwOsErr = cHookMgr.Hook(&hook_id, (void**)&fnOrigCreateSwapChain,
		lpvtbl_CreateSwapChain(dxgiFactory), Hooked_CreateSwapChain, 0);

	if (dwOsErr == ERROR_SUCCESS)
		LogToWindow("  Successfully installed IDXGIFactory->CreateSwapChain hook.\n");
	else
		LogToWindow("  *** Failed install IDXGIFactory->CreateSwapChain hook.\n");
}


// -----------------------------------------------------------------------------
// Actual function called by the game for every CreateDXGIFactory they make.
// This is only called for the in-process game, not system wide.
//
// We are going to always upcast to an IDXGIFactory2 for any calls here.
// The only time we'll not use Factory2 is on Win7 without the evil update.

HRESULT(__stdcall* fnOrigCreateDXGIFactory)(
	REFIID riid,
	_Out_ void** ppFactory
	) = CreateDXGIFactory;

HRESULT __stdcall Hooked_CreateDXGIFactory(REFIID riid, void** ppFactory)
{
	LogToWindow("*** Hooked_CreateDXGIFactory called with riid: %s\n", NameFromIID(riid).c_str());

	// If this happens to be first call from the game, let's make sure to load
	// up our d3d11.dll and the .ini file.
	LoadRealD3D11();

	if (!G->bIntendedTargetExe) {
		LogToWindow("   Not intended target exe, passing through to real DX\n");
		return fnOrigCreateDXGIFactory(riid, ppFactory);
	}

	// If we are being requested to create a DXGIFactory2, lie and say it's not possible.
	if (riid == __uuidof(IDXGIFactory2) && !G->enable_platform_update)
	{
		LogToWindow("  returns E_NOINTERFACE as error for IDXGIFactory2.\n");
		*ppFactory = NULL;
		return E_NOINTERFACE;
	}

	HRESULT hr = fnOrigCreateDXGIFactory(riid, ppFactory);
	if (FAILED(hr))
	{
		LogToWindow("->failed with HRESULT=%x\n", hr);
		return hr;
	}

	if (!fnOrigCreateSwapChain) {
		LogToWindow("HookCreateSwapChain");
		HookCreateSwapChain(*ppFactory);
	}


	// With the addition of the platform_update, we need to allow for specifically
	// creating a DXGIFactory2 instead of DXGIFactory1.  We want to always upcast
	// the highest supported object for each scenario, to properly suppport
	// QueryInterface and GetParent upcasts.

	/*IUnknown* factoryUnknown = reinterpret_cast<IUnknown*>(*ppFactory);
	IDXGIFactory2* dxgiFactory = reinterpret_cast<IDXGIFactory2*>(*ppFactory);
	HRESULT res = factoryUnknown->QueryInterface(IID_PPV_ARGS(&dxgiFactory));
	if (SUCCEEDED(res))
	{
		factoryUnknown->Release();
		*ppFactory = (void*)dxgiFactory;
		LogToWindow("  Upcast QueryInterface(IDXGIFactory2) returned result = %x, factory = %p\n", res, dxgiFactory);

		if (!fnOrigCreateSwapChainForHwnd) {
            LogToWindow("Hooked_CreateDXGIFactory HookFactory2CreateSwapChainMethods");
			HookFactory2CreateSwapChainMethods(dxgiFactory);
		}

	}

	LogToWindow("  CreateDXGIFactory returned factory = %p, result = %x\n", *ppFactory, hr);*/
	return hr;
}


// -----------------------------------------------------------------------------
//
// We are going to always upcast to an IDXGIFactory2 for any calls here.
// The only time we'll not use Factory2 is on Win7 without the evil update.
//
// ToDo: It is probably possible for a game to fetch a Factory2 via QueryInterface,
//  and we might need to hook that as well.  However, in order to Query, they
//  need a Factory or Factory1 to do so, which will call us here anyway.  At least
//  until Win10, where the d3d11.dll also then includes CreateDXGIFactory2. We only 
//  really care about installing a hook for CreateSwapChain which will still get done.

HRESULT(__stdcall* fnOrigCreateDXGIFactory1)(
	REFIID riid,
	_Out_ void** ppFactory
	) = CreateDXGIFactory1;

HRESULT __stdcall Hooked_CreateDXGIFactory1(REFIID riid, void** ppFactory1)
{
	LogToWindow("*** Hooked_CreateDXGIFactory1 called with riid: %s\n", NameFromIID(riid).c_str());

	// If this happens to be first call from the game, let's make sure to load
	// up our d3d11.dll and the .ini file.
	LoadRealD3D11();

	if (!G->bIntendedTargetExe) {
		LogToWindow("   Not intended target exe, passing through to real DX\n");
		return fnOrigCreateDXGIFactory1(riid, ppFactory1);
	}

	// If we are being requested to create a DXGIFactory2, lie and say it's not possible.
	if (riid == __uuidof(IDXGIFactory2) && !G->enable_platform_update)
	{
		LogToWindow("  returns E_NOINTERFACE as error for IDXGIFactory2.\n");
		*ppFactory1 = NULL;
		return E_NOINTERFACE;
	}

	// Call original factory, regardless of what they requested, to keep the
	// same expected sequence from their perspective.  (Which includes refcounts)
	HRESULT hr = fnOrigCreateDXGIFactory1(riid, ppFactory1);
	if (FAILED(hr))
	{
		LogToWindow("->failed with HRESULT=%x\n", hr);
		return hr;
	}

	if (!fnOrigCreateSwapChain)
		HookCreateSwapChain(*ppFactory1);

	// With the addition of the platform_update, we need to allow for specifically
	// creating a DXGIFactory2 instead of DXGIFactory1.  We want to always upcast
	// the highest supported object for each scenario, to properly suppport
	// QueryInterface and GetParent upcasts.

	IUnknown* factoryUnknown = reinterpret_cast<IUnknown*>(*ppFactory1);
	IDXGIFactory2* dxgiFactory = reinterpret_cast<IDXGIFactory2*>(*ppFactory1);
	HRESULT res = factoryUnknown->QueryInterface(IID_PPV_ARGS(&dxgiFactory));
	if (SUCCEEDED(res))
	{
		factoryUnknown->Release();
		*ppFactory1 = (void*)dxgiFactory;
		LogToWindow("  Upcast QueryInterface(IDXGIFactory2) returned result = %x, factory = %p\n", res, dxgiFactory);

		if (!fnOrigCreateSwapChainForHwnd) {
            LogToWindow("Hooked_CreateDXGIFactory1 HookFactory2CreateSwapChainMethods");
            HookFactory2CreateSwapChainMethods(dxgiFactory); // mod need
        }

	}

	LogToWindow("  CreateDXGIFactory1 returned factory = %p, result = %x\n", *ppFactory1, hr);
	return hr;
}

// We cannot statically initialise this, since the function doesn't exist until
// Win 8.1, and refering to it would prevent the dynamic linker from loading us
// on Win 7 (this warning is only applicable to the vs2015 branch with newer
// Windows SDKs, since it is not possible to refer to this on the older SDK):
HRESULT(__stdcall* fnOrigCreateDXGIFactory2)(
	UINT Flags,
	REFIID riid,
	_Out_ void** ppFactory
	) = nullptr;

HRESULT __stdcall Hooked_CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory2)
{
	LogToWindow("*** Hooked_CreateDXGIFactory2 called with riid: %s\n", NameFromIID(riid).c_str());

	// If this happens to be first call from the game, let's make sure to load
	// up our d3d11.dll and the .ini file.
	LoadRealD3D11();

	if (!G->bIntendedTargetExe) {
		LogToWindow("   Not intended target exe, passing through to real DX\n");
		return fnOrigCreateDXGIFactory2(Flags, riid, ppFactory2);
	}

	// If we are being requested to create a DXGIFactory2, lie and say it's not possible.
	if (riid == __uuidof(IDXGIFactory2) && !G->enable_platform_update)
	{
		LogToWindow("  returns E_NOINTERFACE as error for IDXGIFactory2.\n");
		*ppFactory2 = NULL;
		return E_NOINTERFACE;
	}

	// Call original factory, regardless of what they requested, to keep the
	// same expected sequence from their perspective.  (Which includes refcounts)
	HRESULT hr = fnOrigCreateDXGIFactory2(Flags, riid, ppFactory2);
	if (FAILED(hr))
	{
		LogToWindow("->failed with HRESULT=%x\n", hr);
		return hr;
	}

	if (!fnOrigCreateSwapChain)
		HookCreateSwapChain(*ppFactory2);

	// We still upcast, even in CreateDXGIFactory2, because the game could
	// have passed a lower version riid, and this way is safer. The version
	// of this function isn't actually strongly related to the interface
	// version it returns at all - CreateFactory2 is for DXGI 1.3, but
	// really it just has an extra flags field compared to the previous
	// version. There's also a Factory 4, 5 and 6, but no CreateFactory 4,
	// 5 or 6 - the version numbers aren't related.

	IUnknown* factoryUnknown = reinterpret_cast<IUnknown*>(*ppFactory2);
	IDXGIFactory2* dxgiFactory = reinterpret_cast<IDXGIFactory2*>(*ppFactory2);
	HRESULT res = factoryUnknown->QueryInterface(IID_PPV_ARGS(&dxgiFactory));
	if (SUCCEEDED(res))
	{
		factoryUnknown->Release();
		*ppFactory2 = (void*)dxgiFactory;
		LogToWindow("  Upcast QueryInterface(IDXGIFactory2) returned result = %x, factory = %p\n", res, dxgiFactory);

		if (!fnOrigCreateSwapChainForHwnd) {
            LogToWindow("Hooked_CreateDXGIFactory2 HookFactory2CreateSwapChainMethods");
            HookFactory2CreateSwapChainMethods(dxgiFactory);
        }

	}

	LogToWindow("  CreateDXGIFactory2 returned factory = %p, result = %x\n", *ppFactory2, hr);
	return hr;
}