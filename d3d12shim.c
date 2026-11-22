/*
 * d3d12.dll shim for GTA V Enhanced on D3DMetal 2.1.
 *
 * The shim sits in the game folder and passes each call to Apple's d3d12.dll
 * in system32. Its one needed job is to load the D3D12Core.dll stand-in when
 * the game creates a device, because the game stops with ERR_GFX_D3D_NOD3D12
 * when no module of that name is in the process.
 *
 * The shim also gives the device and its command queues a vtable of small
 * functions that the Social Club overlay can patch, and it mixes the game's
 * spatial audio down to stereo, see stereomix.h and start_stereo_mix().
 *
 * The shim corrects two answers of D3DMetal that differ from what Windows
 * gives, see device_CheckFeatureSupport(). It also holds back a suspend of a
 * thread that is inside Apple's code, see the suspend guard in comhook.h.
 *
 * When the file C:\d3d12shim.debug exists, the shim logs each call to
 * C:\d3d12shim.log.
 */
#define COBJMACROS
#define CINTERFACE
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <mmdeviceapi.h>
#include "comhook.h"
#include "stereomix.h"
#include "slots.h"

static HMODULE real_module;

/* Set D3D12SHIM_NOFIX=1 to log the answers of D3DMetal without a change. */
static BOOL fix_answers = TRUE;

static FARPROC real_function(const char *name)
{
    FARPROC function;

    if (!real_module)
    {
        real_module = LoadLibraryW(L"C:\\windows\\system32\\d3d12.dll");
        shim_log("d3d12: loaded the system d3d12.dll at %p, error %lu", real_module,
                 real_module ? 0 : GetLastError());
    }
    function = real_module ? GetProcAddress(real_module, name) : NULL;
    if (!function)
        shim_log("d3d12: the system d3d12.dll has no %s", name);
    return function;
}

static HINSTANCE shim_instance;

/*
 * The game plays its sound through ISpatialAudioClient and does not start
 * without it. It opens a stream with a fixed object for each of 16 speaker
 * positions and trusts Windows to mix them down to the real speakers. Wine
 * does not do that mix, so the center voice is lost on two speakers.
 * stereomix.h does the mix. This part puts it between the game and Wine.
 *
 * All objects of a class share one vtable in mmdevapi.dll, so one changed slot
 * covers every device. The file C:\d3d12shim.spatial turns the change off.
 */
DEFINE_GUID(spatial_audio_client_iid, 0xbbf8e066, 0xaaaa, 0x49be, 0x9a, 0x4d, 0xfd, 0x2a, 0x85, 0x8e, 0xa2, 0x7f);

#define SLOT_ACTIVATE_STREAM 10         /* ISpatialAudioClient::ActivateSpatialAudioStream */

/* The start of SpatialAudioObjectRenderStreamActivationParams. */
struct stream_params
{
    const WAVEFORMATEX *format;
    DWORD static_positions;
};

static HRESULT (WINAPI *real_activate)(IMMDevice *, REFIID, DWORD, PROPVARIANT *, void **);
static HRESULT (WINAPI *real_activate_stream)(void *, const PROPVARIANT *, REFIID, void **);

static BOOL change_slot(void **slot, void *function, void **old_function)
{
    DWORD old;

    if (*slot == function)
        return TRUE;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old))
        return FALSE;
    if (old_function)
        *old_function = *slot;
    *slot = function;
    VirtualProtect(slot, sizeof(*slot), old, &old);
    return TRUE;
}

/* Open the real stream with front left and front right only, and hand the
 * game a stream that mixes every position into those two. */
static HRESULT WINAPI activate_stereo_stream(void *client, const PROPVARIANT *params, REFIID iid, void **stream)
{
    const struct stream_params *asked;
    PROPVARIANT stereo_params;
    BYTE *copy;
    HRESULT result;
    void *mix;

    if (!params || params->vt != VT_BLOB || params->blob.cbSize < sizeof(*asked) || !stream)
        return real_activate_stream(client, params, iid, stream);
    asked = (const struct stream_params *)params->blob.pBlobData;
    if (!asked->format || asked->format->nChannels != 1 || asked->format->wBitsPerSample != 32
        || !(asked->static_positions & ~(POSITION_FRONT_LEFT | POSITION_FRONT_RIGHT | 1)))
    {
        shim_log("audio: left the spatial stream alone, static position mask 0x%lx", asked->static_positions);
        return real_activate_stream(client, params, iid, stream);
    }
    copy = HeapAlloc(GetProcessHeap(), 0, params->blob.cbSize);
    if (!copy)
        return E_OUTOFMEMORY;
    memcpy(copy, params->blob.pBlobData, params->blob.cbSize);
    ((struct stream_params *)copy)->static_positions = POSITION_FRONT_LEFT | POSITION_FRONT_RIGHT;
    stereo_params = *params;
    stereo_params.blob.pBlobData = copy;
    result = real_activate_stream(client, &stereo_params, iid, stream);
    HeapFree(GetProcessHeap(), 0, copy);
    shim_log("audio: the game asked for static position mask 0x%lx, opened a stereo stream -> 0x%08lx",
             asked->static_positions, result);
    if (FAILED(result))
        return result;
    mix = make_mix_stream(*stream);
    if (!mix)
    {
        ((IUnknown *)*stream)->lpVtbl->Release(*stream);
        *stream = NULL;
        return E_FAIL;
    }
    *stream = mix;
    return result;
}

static HRESULT WINAPI activate_with_stereo_mix(IMMDevice *device, REFIID iid, DWORD context,
                                               PROPVARIANT *params, void **out)
{
    HRESULT result = real_activate(device, iid, context, params, out);

    if (SUCCEEDED(result) && out && *out && IsEqualGUID(iid, &spatial_audio_client_iid))
    {
        void **vtable = *(void ***)*out;

        change_slot(&vtable[SLOT_ACTIVATE_STREAM], activate_stereo_stream, (void **)&real_activate_stream);
        shim_log("audio: the game got a spatial audio client");
    }
    return result;
}

static void start_stereo_mix(void)
{
    static LONG done;
    IMMDeviceEnumerator *enumerator = NULL;
    IMMDevice *device = NULL;
    HRESULT started;

    if (InterlockedCompareExchange(&done, 1, 0) != 0)
        return;
    if (GetFileAttributesA("C:\\d3d12shim.spatial") != INVALID_FILE_ATTRIBUTES)
        return;
    started = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                   &IID_IMMDeviceEnumerator, (void **)&enumerator))
        && SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device)))
    {
        /* Keep mmdevapi.dll loaded, or the change is lost when COM frees it. */
        LoadLibraryW(L"mmdevapi.dll");
        if (change_slot((void **)&device->lpVtbl->Activate, activate_with_stereo_mix, (void **)&real_activate))
            shim_log("audio: the stereo mix for spatial audio is on");
    }
    else
        shim_log("audio: found no audio device, the stereo mix is off");
    if (device)
        IMMDevice_Release(device);
    if (enumerator)
        IMMDeviceEnumerator_Release(enumerator);
    if (SUCCEEDED(started))
        CoUninitialize();
}

/*
 * Load the D3D12Core.dll that sits next to the shim. Windows loads the
 * Agility SDK's D3D12Core.dll when the game creates a device, and the game
 * checks that a module with that name is in the process.
 */
static void load_core(void)
{
    static LONG loaded;
    WCHAR path[MAX_PATH], *name;
    HMODULE core;

    if (InterlockedCompareExchange(&loaded, 1, 0) != 0)
        return;
    GetModuleFileNameW(shim_instance, path, MAX_PATH);
    name = wcsrchr(path, '\\');
    if (!name)
        return;
    wcscpy(name + 1, L"D3D12Core.dll");
    core = LoadLibraryW(path);
    shim_log("d3d12: loaded %ls at %p, error %lu", path, core, core ? 0 : GetLastError());
}

/* The device. */

static BOOL device_in_family(REFIID iid)
{
    return IsEqualGUID(iid, &IID_ID3D12Device) || IsEqualGUID(iid, &IID_ID3D12Device1)
           || IsEqualGUID(iid, &IID_ID3D12Device2) || IsEqualGUID(iid, &IID_ID3D12Device3)
           || IsEqualGUID(iid, &IID_ID3D12Device4) || IsEqualGUID(iid, &IID_ID3D12Device5)
           || IsEqualGUID(iid, &IID_ID3D12Device6) || IsEqualGUID(iid, &IID_ID3D12Device7)
           || IsEqualGUID(iid, &IID_ID3D12Device8) || IsEqualGUID(iid, &IID_ID3D12Device9)
           || IsEqualGUID(iid, &IID_ID3D12Device10);
}

static HRESULT WINAPI device_CheckFeatureSupport(void *self, UINT feature, void *data, UINT size)
{
    HRESULT (WINAPI *function)(void *, UINT, void *, UINT);
    char before[160], after[160];
    UINT asked_shader_model = 0;
    HRESULT result;

    function = original_vtable(self)[slot_of(class_of(self), "CheckFeatureSupport")];
    hex_text(data, size, before);
    if (feature == D3D12_FEATURE_SHADER_MODEL && size >= sizeof(D3D12_FEATURE_DATA_SHADER_MODEL))
        asked_shader_model = ((D3D12_FEATURE_DATA_SHADER_MODEL *)data)->HighestShaderModel;
    result = function(self, feature, data, size);
    if (SUCCEEDED(result) && feature == D3D12_FEATURE_FEATURE_LEVELS
        && size >= sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS))
    {
        /* Windows answers with the highest level from the list that the
         * device supports. D3DMetal 2.1 answers with the highest level in
         * the list, even one that does not exist, so pick the answer the way
         * Windows does. */
        D3D12_FEATURE_DATA_FEATURE_LEVELS *levels = data;
        D3D_FEATURE_LEVEL best = 0;
        UINT i;

        for (i = 0; i < levels->NumFeatureLevels; i++)
        {
            D3D_FEATURE_LEVEL level = levels->pFeatureLevelsRequested[i];

            if (level <= D3D_FEATURE_LEVEL_12_2 && level > best)
                best = level;
        }
        if (fix_answers && best && levels->MaxSupportedFeatureLevel != best)
        {
            shim_log("    changed the highest feature level from 0x%x to 0x%x",
                     levels->MaxSupportedFeatureLevel, best);
            levels->MaxSupportedFeatureLevel = best;
        }
    }
    if (SUCCEEDED(result) && asked_shader_model)
    {
        /* Windows never answers with a shader model above the one asked. */
        D3D12_FEATURE_DATA_SHADER_MODEL *model = data;

        if (fix_answers && model->HighestShaderModel > asked_shader_model)
        {
            shim_log("    changed the shader model from 0x%x to 0x%x",
                     model->HighestShaderModel, asked_shader_model);
            model->HighestShaderModel = asked_shader_model;
        }
    }
    hex_text(data, size, after);
    if (feature != D3D12_FEATURE_FORMAT_SUPPORT)
        shim_log("device CheckFeatureSupport feature %u size %u -> 0x%08lx, caller %p\n        in  %s\n        out %s",
                 feature, size, result, __builtin_return_address(0), before, after);
    return result;
}

static const struct slot_override device_overrides[] =
{
    { "CheckFeatureSupport", device_CheckFeatureSupport },
    { NULL, NULL },
};

static const struct com_class queue_class =
{
    "queue", queue_methods, QUEUE_METHOD_COUNT, NULL, NULL, NULL,
};

static void device_after(struct call_frame *frame, UINT_PTR result)
{
    /* CreateCommandQueue(self, description, iid, queue) */
    if (frame->slot == slot_of(frame->class, "CreateCommandQueue") && SUCCEEDED((HRESULT)result)
        && frame->r9 && *(void **)frame->r9)
        hook_object(*(void **)frame->r9, &queue_class);
}

static const struct com_class device_class =
{
    "device", device_methods, DEVICE_METHOD_COUNT, device_in_family, device_after, device_overrides,
};

/* Exports that the game can call with the documented signatures. */

HRESULT WINAPI D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL level,
                                 REFIID iid, void **device)
{
    HRESULT (WINAPI *function)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    char text[64];
    HRESULT result;

    function = (void *)real_function("D3D12CreateDevice");
    if (!function)
        return E_NOTIMPL;
    start_stack_report(90000);
    load_core();
    start_stereo_mix();
    result = function(adapter, level, iid, device);
    shim_log("D3D12CreateDevice adapter %p level 0x%x iid %s device %p -> 0x%08lx",
             adapter, level, guid_text(iid, text), device, result);
    if (SUCCEEDED(result) && device && *device)
        hook_object(*device, &device_class);
    return result;
}

HRESULT WINAPI D3D12GetDebugInterface(REFIID iid, void **debug)
{
    HRESULT (WINAPI *function)(REFIID, void **);
    char text[64];
    HRESULT result;

    function = (void *)real_function("D3D12GetDebugInterface");
    if (!function)
        return E_NOTIMPL;
    result = function(iid, debug);
    shim_log("D3D12GetDebugInterface iid %s -> 0x%08lx", guid_text(iid, text), result);
    return result;
}

HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT count, const IID *iids,
                                               void *configs, UINT *sizes)
{
    HRESULT (WINAPI *function)(UINT, const IID *, void *, UINT *);
    char text[64];
    HRESULT result;
    UINT i;

    function = (void *)real_function("D3D12EnableExperimentalFeatures");
    if (!function)
        return E_NOTIMPL;
    result = function(count, iids, configs, sizes);
    for (i = 0; i < count; i++)
        shim_log("D3D12EnableExperimentalFeatures feature %s", guid_text(&iids[i], text));
    shim_log("D3D12EnableExperimentalFeatures count %u -> 0x%08lx", count, result);
    return result;
}

/* Exports that the shim passes through without a look at the arguments. */

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

PASS_THROUGH(D3D12CreateRootSignatureDeserializer)
PASS_THROUGH(D3D12CreateVersionedRootSignatureDeserializer)
PASS_THROUGH(D3D12SerializeRootSignature)
PASS_THROUGH(D3D12SerializeVersionedRootSignature)
PASS_THROUGH(D3D12CoreCreateLayeredDevice)
PASS_THROUGH(D3D12CoreGetLayeredDeviceSize)
PASS_THROUGH(D3D12CoreRegisterLayers)
PASS_THROUGH(GetBehaviorValue)

/*
 * Windows exports D3D12GetInterface and Apple's d3d12.dll does not. The game
 * never calls it. Answer for the debug class, which Apple has, and for
 * nothing else.
 */
HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID iid, void **out)
{
    char class_text[64], iid_text[64];
    HRESULT result = E_NOINTERFACE;

    if (IsEqualGUID(clsid, &CLSID_D3D12Debug))
        result = D3D12GetDebugInterface(iid, out);
    else if (out)
        *out = NULL;
    shim_log("D3D12GetInterface class %s iid %s -> 0x%08lx",
             guid_text(clsid, class_text), guid_text(iid, iid_text), result);
    return result;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        char path[MAX_PATH];
        HMODULE pinned;

        shim_instance = instance;
        DisableThreadLibraryCalls(instance);
        /* The game loads and frees d3d12.dll more than once, and the device
         * object outlives that. Pin the shim so the hooks stay valid. */
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (const WCHAR *)DllMain, &pinned);
        GetModuleFileNameA(NULL, path, sizeof(path));
        read_settings();
        if (GetEnvironmentVariableA("D3D12SHIM_NOFIX", NULL, 0))
            fix_answers = FALSE;
        shim_log("d3d12 shim attached to %s at %p, fixes %s", path, GetModuleHandleW(NULL),
                 fix_answers ? "on" : "off");
        if (debug_mode)
            AddVectoredExceptionHandler(1, log_exception);
    }
    return TRUE;
}
