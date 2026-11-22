/*
 * dxgi.dll shim for GTA V Enhanced on D3DMetal 2.1.
 *
 * The shim sits in the game folder and passes each call to Apple's dxgi.dll in
 * system32. It gives factories and swap chains a vtable of its own small
 * functions. The Social Club overlay patches the code of CreateSwapChain,
 * Present, and ResizeBuffers, and that patch fails on Apple's functions. When
 * it fails, the game waits at "Initializing" and never starts.
 *
 * In debug mode the shim also logs the calls on factories, adapters, outputs,
 * and swap chains to C:\d3d12shim.log.
 */
#define COBJMACROS
#define CINTERFACE
#define INITGUID
#include <windows.h>
#include <dxgi1_6.h>
#include "comhook.h"
#include "slots.h"

static HMODULE real_module;

static FARPROC real_function(const char *name)
{
    FARPROC function;

    if (!real_module)
    {
        real_module = LoadLibraryW(L"C:\\windows\\system32\\dxgi.dll");
        shim_log("dxgi: loaded the system dxgi.dll at %p, error %lu", real_module,
                 real_module ? 0 : GetLastError());
    }
    function = real_module ? GetProcAddress(real_module, name) : NULL;
    if (!function)
        shim_log("dxgi: the system dxgi.dll has no %s", name);
    return function;
}

static BOOL output_in_family(REFIID iid)
{
    return IsEqualGUID(iid, &IID_IDXGIOutput) || IsEqualGUID(iid, &IID_IDXGIOutput1)
           || IsEqualGUID(iid, &IID_IDXGIOutput2) || IsEqualGUID(iid, &IID_IDXGIOutput3)
           || IsEqualGUID(iid, &IID_IDXGIOutput4) || IsEqualGUID(iid, &IID_IDXGIOutput5)
           || IsEqualGUID(iid, &IID_IDXGIOutput6);
}

static const struct com_class output_class =
{
    "output", output_methods, OUTPUT_METHOD_COUNT, output_in_family, NULL, NULL,
};

static BOOL swapchain_in_family(REFIID iid)
{
    return IsEqualGUID(iid, &IID_IDXGISwapChain) || IsEqualGUID(iid, &IID_IDXGISwapChain1)
           || IsEqualGUID(iid, &IID_IDXGISwapChain2) || IsEqualGUID(iid, &IID_IDXGISwapChain3)
           || IsEqualGUID(iid, &IID_IDXGISwapChain4);
}

static const struct com_class swapchain_class =
{
    "swapchain", swapchain_methods, SWAPCHAIN_METHOD_COUNT, swapchain_in_family, NULL, NULL,
};

static BOOL adapter_in_family(REFIID iid)
{
    return IsEqualGUID(iid, &IID_IDXGIAdapter) || IsEqualGUID(iid, &IID_IDXGIAdapter1)
           || IsEqualGUID(iid, &IID_IDXGIAdapter2) || IsEqualGUID(iid, &IID_IDXGIAdapter3)
           || IsEqualGUID(iid, &IID_IDXGIAdapter4);
}

static void hook_result(void **out, const struct com_class *class)
{
    if (out && *out)
        hook_object(*out, class);
}

/* Adapters and outputs get hooks only for the log. The game does not need them. */
static void hook_for_log(void **out, const struct com_class *class)
{
    if (debug_mode)
        hook_result(out, class);
}

static void adapter_after(struct call_frame *frame, UINT_PTR result)
{
    const char *method = frame->class->methods[frame->slot];

    if (FAILED((HRESULT)result))
        return;
    /* No hook on outputs: D3DMetal crashed in CreateSwapChainForHwnd in two of
     * three starts when the output it got from EnumOutputs had a hook. */
    if (!strcmp(method, "GetDesc") || !strcmp(method, "GetDesc1")
             || !strcmp(method, "GetDesc2") || !strcmp(method, "GetDesc3"))
    {
        DXGI_ADAPTER_DESC *desc = (DXGI_ADAPTER_DESC *)frame->rdx;

        shim_log("    adapter \"%ls\" vendor 0x%x device 0x%x video memory %llu MB shared %llu MB",
                 desc->Description, desc->VendorId, desc->DeviceId,
                 (unsigned long long)desc->DedicatedVideoMemory >> 20,
                 (unsigned long long)desc->SharedSystemMemory >> 20);
    }
    else if (!strcmp(method, "QueryVideoMemoryInfo"))   /* (self, node, group, info) */
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO *info = (DXGI_QUERY_VIDEO_MEMORY_INFO *)frame->r9;

        shim_log("    video memory group %u budget %llu MB usage %llu MB", (UINT)frame->r8,
                 (unsigned long long)info->Budget >> 20, (unsigned long long)info->CurrentUsage >> 20);
    }
}

static const struct com_class adapter_class =
{
    "adapter", adapter_methods, ADAPTER_METHOD_COUNT, adapter_in_family, adapter_after, NULL,
};

static BOOL factory_in_family(REFIID iid)
{
    return IsEqualGUID(iid, &IID_IDXGIFactory) || IsEqualGUID(iid, &IID_IDXGIFactory1)
           || IsEqualGUID(iid, &IID_IDXGIFactory2) || IsEqualGUID(iid, &IID_IDXGIFactory3)
           || IsEqualGUID(iid, &IID_IDXGIFactory4) || IsEqualGUID(iid, &IID_IDXGIFactory5)
           || IsEqualGUID(iid, &IID_IDXGIFactory6) || IsEqualGUID(iid, &IID_IDXGIFactory7);
}

static void factory_after(struct call_frame *frame, UINT_PTR result)
{
    const char *method = frame->class->methods[frame->slot];

    if (FAILED((HRESULT)result))
        return;
    if (!strcmp(method, "EnumAdapters") || !strcmp(method, "EnumAdapters1"))
        hook_for_log((void **)frame->r8, &adapter_class);            /* (self, index, adapter) */
    else if (!strcmp(method, "EnumAdapterByLuid") || !strcmp(method, "EnumWarpAdapter"))
        hook_for_log((void **)frame->r9, &adapter_class);            /* (self, luid, iid, adapter) */
    else if (!strcmp(method, "EnumAdapterByGpuPreference"))
        hook_for_log((void **)frame->stack[5], &adapter_class);      /* (self, index, preference, iid, adapter) */
    else if (!strcmp(method, "CreateSwapChain"))
        hook_result((void **)frame->r9, &swapchain_class);          /* (self, device, description, chain) */
    else if (!strcmp(method, "CreateSwapChainForHwnd"))
        hook_result((void **)frame->stack[7], &swapchain_class);    /* (self, device, window, description, fullscreen, output, chain) */
    else if (!strcmp(method, "CreateSwapChainForComposition"))
        hook_result((void **)frame->stack[5], &swapchain_class);    /* (self, device, description, output, chain) */
}

static const struct com_class factory_class =
{
    "factory", factory_methods, FACTORY_METHOD_COUNT, factory_in_family, factory_after, NULL,
};

HRESULT WINAPI CreateDXGIFactory(REFIID iid, void **factory)
{
    HRESULT (WINAPI *function)(REFIID, void **);
    char text[64];
    HRESULT result;

    function = (void *)real_function("CreateDXGIFactory");
    if (!function)
        return E_NOTIMPL;
    result = function(iid, factory);
    shim_log("CreateDXGIFactory iid %s -> 0x%08lx", guid_text(iid, text), result);
    if (SUCCEEDED(result))
        hook_result(factory, &factory_class);
    return result;
}

HRESULT WINAPI CreateDXGIFactory1(REFIID iid, void **factory)
{
    HRESULT (WINAPI *function)(REFIID, void **);
    char text[64];
    HRESULT result;

    function = (void *)real_function("CreateDXGIFactory1");
    if (!function)
        return E_NOTIMPL;
    result = function(iid, factory);
    shim_log("CreateDXGIFactory1 iid %s -> 0x%08lx", guid_text(iid, text), result);
    if (SUCCEEDED(result))
        hook_result(factory, &factory_class);
    return result;
}

HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID iid, void **factory)
{
    HRESULT (WINAPI *function)(UINT, REFIID, void **);
    char text[64];
    HRESULT result;

    function = (void *)real_function("CreateDXGIFactory2");
    if (!function)
        return E_NOTIMPL;
    result = function(flags, iid, factory);
    shim_log("CreateDXGIFactory2 flags 0x%x iid %s -> 0x%08lx", flags, guid_text(iid, text), result);
    if (SUCCEEDED(result))
        hook_result(factory, &factory_class);
    return result;
}

HRESULT WINAPI shim_DXGIGetDebugInterface1(UINT flags, REFIID iid, void **debug)
{
    HRESULT (WINAPI *function)(UINT, REFIID, void **);
    char text[64];
    HRESULT result;

    function = (void *)real_function("DXGIGetDebugInterface1");
    if (!function)
        return E_NOTIMPL;
    result = function(flags, iid, debug);
    shim_log("DXGIGetDebugInterface1 flags 0x%x iid %s -> 0x%08lx", flags, guid_text(iid, text), result);
    return result;
}

#define PASS_THROUGH(name)                                                    \
    UINT_PTR WINAPI shim_##name(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d, \
                                UINT_PTR e, UINT_PTR f)                       \
    {                                                                         \
        UINT_PTR (WINAPI *function)(UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR,   \
                                    UINT_PTR, UINT_PTR);                      \
        UINT_PTR result;                                                      \
        function = (void *)real_function(#name);                              \
        if (!function)                                                        \
            return (UINT_PTR)E_NOTIMPL;                                       \
        result = function(a, b, c, d, e, f);                                  \
        shim_log(#name " -> 0x%08lx", (unsigned long)result);                 \
        return result;                                                        \
    }

PASS_THROUGH(DXGID3D10CreateDevice)
PASS_THROUGH(DXGID3D10RegisterLayers)
PASS_THROUGH(DXGIDeclareAdapterRemovalSupport)

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        char path[MAX_PATH];
        HMODULE pinned;

        DisableThreadLibraryCalls(instance);
        /* DXGI objects can outlive a FreeLibrary of dxgi.dll. Pin the shim so
         * the hooks stay valid. */
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (const WCHAR *)DllMain, &pinned);
        GetModuleFileNameA(NULL, path, sizeof(path));
        read_settings();
        shim_log("dxgi shim attached to %s", path);
    }
    return TRUE;
}
