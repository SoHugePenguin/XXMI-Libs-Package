#include "PenguinHackerDXGI.h"
#include "IniHandler.h"
#include "profiling.h"
#include "log.h"
#include "Hunting.h"
#include "input.h"
#include "Override.h"
#include "D3D11Wrapper.h"

IDXGISwapChain* PenguinTools::sSwap = nullptr;
PenguinDV* PenguinTools::sDV = nullptr;
PenguinDC* PenguinTools::sDC = nullptr;
Overlay* PenguinTools::sOverlay = nullptr;


void PenguinTools::Init(IDXGISwapChain* swap, PenguinDV* dev, PenguinDC* ctx) {
    sSwap = swap;
    sDV = dev;
    sDC = ctx;

    // 不升级有些mod无法使用
    // 新版本 IDXGISwapChain1 开始，才暴露了 GetDesc1、GetFullscreenDesc、GetHwnd 等方法，
    // 配合 DXGI 1.2+ 的工厂接口（CreateSwapChainForHwnd、CreateSwapChainForComposition），
    // 才能更精细地拦截和替换 swapchain 的渲染对象。
    IDXGISwapChain1* swap1 = nullptr;
    if (SUCCEEDED(swap->QueryInterface(IID_PPV_ARGS(&swap1)))) {
        LogToWindow("[IDXGISwapChain] 已升级为IDXGISwapChain1");
        PenguinSC* swapchainWrap = new PenguinSC(swap1, dev, ctx);
        swap1->Release();
    }

    PenguinSC* swapchainWrap = new PenguinSC(swap1, dev, ctx);
    sDV->SetPenguinSC(swapchainWrap);

    // Bump the refcounts on the device and context to make sure they can't
    // be released as long as the swap chain is alive and we may be
    // accessing them. We probably don't actually need to do this for the
    // device, since the DirectX swap chain should already hold a reference
    // to the DirectX device, but it shouldn't hurt and makes the code more
    // semantically correct since we access the device as well. We could
    // skip both by looking them up on demand, but that would need extra
    // lookups in fast paths and there's no real need.
    //
    // The overlay also bumps these refcounts, which is technically
    // unecessary given we now do so here, but also shouldn't hurt, and is
    // safer in case we ever change this again and forget about it
    // 不保活会游戏闪退
    sDV->AddRef();
    if (sDC) {
        sDC->AddRef();
    }
    else {
        ID3D11DeviceContext* tmpContext = NULL;
        // GetImmediateContext will bump the refcount for us.
        // In the case of hooking, GetImmediateContext will not return
        // a HackerContext, so we don't use it's return directly, but
        // rather just use it to make GetHackerContext valid:
        sDV->GetImmediateContext(&tmpContext);
        sDC = sDV->GetPenguinDC();
    }

    try {
        sOverlay = new Overlay(dev, ctx, swap);
        LogToWindow("Overlay SUCCESS INIT!");
        LogOverlayW(LOG_INFO, L"If you want to use MOD normally for a longer period of time, don’t make any noise everywhere, thank you.");
        LogOverlayW(LOG_INFO, L"If you have any questions, add me QQ: 2298566583");
    }
    catch (...) {
        LogToWindow("*** Failed to create Overlay in PenguinTools::Init.\n");
        sOverlay = nullptr;
    }
}

void PenguinTools::Shutdown() {
    sSwap = nullptr;
    sDV = nullptr;
    sDC = nullptr;
}

void PenguinTools::DoFrameActions() {
    LogDebug("Running frame actions. Device=%p\n", sDV);

    if (LogFile) fflush(LogFile);

    G->gTime = (GetTickCount64() - G->ticks_at_launch) / 1000.0f;

    RunCommandList(sDV, sDC, &G->present_command_list, NULL, false);

    if (G->analyse_frame) {
        // We don't allow hold to be changed mid-frame due to potential
        // for filename conflicts, so use def_analyse_options:
        if (G->def_analyse_options & FrameAnalysisOptions::HOLD) {
            // If using analyse_options=hold we don't stop the
            // analysis at the frame boundary (it will be stopped
            // at the key up event instead), but we do increment
            // the frame count and reset the draw count:
            G->analyse_frame_no++;
        }
        else {
            G->analyse_frame = false;
            if (G->DumpUsage)
                DumpUsage(G->ANALYSIS_PATH);
            LogOverlayW(LOG_INFO, L"Frame analysis saved to %ls\n", G->ANALYSIS_PATH);
        }
    }

    // NOTE: Now that key overrides can check an ini param, the ordering of
    // this and the present_command_list is significant. We might set an
    // ini param during a frame for scene detection, which is checked on
    // override activation, then cleared from the command list run on
    // present. If we ever needed to run the command list before this
    // point, we should consider making an explicit "pre" command list for
    // that purpose rather than breaking the existing behaviour.
    bool newEvent = DispatchInputEvents(sDV);
    CurrentTransition.UpdatePresets(sDV);
    CurrentTransition.UpdateTransitions(sDV);


    // The config file is not safe to reload from within the input handler
    // since it needs to change the key bindings, so it sets this flag
    // instead and we handle it now.
    if (G->gReloadConfigPending)
        ReloadConfig(sDV);


    // Regular LoadConfigFile() on startup fails to properly load all resources in some edge cases
    // So, as bandaid solution, it has some sense to force ReloadConfig() after DLL is fully initialized
    // This way resources will be loaded properly before modded object appear on screen and cause crash
    if (G->gConfigInitialized) {
        // Autosave persistent variables every gSettingsAutoSaveInterval seconds
        if (G->gTime - G->gSettingsSaveTime > G->gSettingsAutoSaveInterval) {
            SavePersistentSettings();
            //LogOverlay(LOG_INFO, "Saved Persistent Variables\n");
        }
    }
	else if (G->gTime > G->gConfigInitializationDelay) {
		G->gConfigInitialized = true;
		ReloadConfig(sDV);
	}

    // 绘制Overlay，即渲染在游戏上的日志
    if (sOverlay && !G->suppress_overlay) {
        sOverlay->DrawOverlay();
    }
    G->suppress_overlay = false;


    // This must happen on the same side of the config and shader reloads
    // to ensure the config reload can't clear messages from the shader
    // reload. It doesn't really matter which side we do it on at the
    // moment, but let's do it last, because logically it makes sense to be
    // incremented when we call the original present call:
    G->frame_no++;

    // When not hunting most keybindings won't have been registered, but
    // still skip the below logic that only applies while hunting.
    if (G->hunting != HUNTING_MODE_ENABLED)
        return;

    // Update the huntTime whenever we get fresh user input.
    if (newEvent)
        G->huntTime = time(NULL);

    // Clear buffers after some user idle time.  This allows the buffers to be
    // stable during a hunt, and cleared after one minute of idle time.  The idea
    // is to make the arrays of shaders stable so that hunting up and down the arrays
    // is consistent, while the user is engaged.  After 1 minute, they are likely onto
    // some other spot, and we should start with a fresh set, to keep the arrays and
    // active shader list small for easier hunting.  Until the first keypress, the arrays
    // are cleared at each thread wake, just like before.
    // The arrays will be continually filled by the SetShader sections, but should
    // rapidly converge upon all active shaders.
    if (difftime(time(NULL), G->huntTime) > 60) {
        EnterCriticalSectionPretty(&G->mCriticalSection);
        TimeoutHuntingBuffers();
        LeaveCriticalSection(&G->mCriticalSection);
    }
}

