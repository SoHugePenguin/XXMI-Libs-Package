#pragma once
#include <dxgi.h>
#include "HookedDevice.h"
#include "CommandList.h"
#include "globals.h"

class PenguinTools {
public:
    static void Init(IDXGISwapChain* swap, PenguinDV* dev, PenguinDC* ctx);
    static void DoFrameActions();
    static void Shutdown();
    static PenguinDV* sDV;
    static PenguinDC* sDC;
    static class Overlay* sOverlay;

private:
    static IDXGISwapChain* sSwap;
};