/*
 * D3D12Core.dll stand-in for GTA V Enhanced on D3DMetal.
 *
 * On Windows, d3d12.dll loads the Agility SDK's D3D12Core.dll from the path
 * that the game exports. The game then looks for a loaded module named
 * D3D12Core.dll, reads its D3D12SDKVersion, and stops with
 * ERR_GFX_D3D_NOD3D12 if the module is missing or the version is 610 or
 * lower. D3DMetal loads no such module, so the d3d12 shim loads this one.
 */
#include <windows.h>

/* The version of the Agility SDK that the game ships, replaced at load time
 * by the version that the game exe asks for. */
DWORD D3D12SDKVersion = 611;

HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID iid, void **out)
{
    if (out)
        *out = NULL;
    return E_NOINTERFACE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        const DWORD *wanted = (const DWORD *)GetProcAddress(GetModuleHandleW(NULL), "D3D12SDKVersion");

        DisableThreadLibraryCalls(instance);
        if (wanted && *wanted > D3D12SDKVersion)
            D3D12SDKVersion = *wanted;
    }
    return TRUE;
}
