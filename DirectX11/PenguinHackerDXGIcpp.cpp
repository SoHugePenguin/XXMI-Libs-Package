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
    if (!sDV || !sDC) return;

    LogDebug("Running frame actions. Device=%p\n", sDV);

    if (LogFile) fflush(LogFile);

    G->gTime = (GetTickCount64() - G->ticks_at_launch) / 1000.0f;

    RunCommandList(sDV, sDC, &G->present_command_list, NULL, false);
    RunCommandList(sDV, sDC, &G->post_present_command_list, NULL, true);

    if (G->analyse_frame) {
        if (G->def_analyse_options & FrameAnalysisOptions::HOLD) {
            G->analyse_frame_no++;
        }
        else {
            G->analyse_frame = false;
            if (G->DumpUsage) DumpUsage(G->ANALYSIS_PATH);
            LogOverlayW(LOG_INFO, L"Frame analysis saved to %ls\n", G->ANALYSIS_PATH);
        }
    }

    bool newEvent = DispatchInputEvents(sDV);
    CurrentTransition.UpdatePresets(sDV);
    CurrentTransition.UpdateTransitions(sDV);

    if (G->gReloadConfigPending)
        ReloadConfig(sDV);

    if (!G->gConfigInitialized && G->gTime > G->gConfigInitializationDelay) {
        G->gConfigInitialized = true;
        ReloadConfig(sDV);
    }

    if (G->gConfigInitialized &&
        (G->gTime - G->gSettingsSaveTime > G->gSettingsAutoSaveInterval)) {
        SavePersistentSettings();
    }

    G->frame_no++;
    if (newEvent) G->huntTime = time(NULL);

    if (difftime(time(NULL), G->huntTime) > 60) {
        EnterCriticalSectionPretty(&G->mCriticalSection);
        TimeoutHuntingBuffers();
        LeaveCriticalSection(&G->mCriticalSection);
    }
}

