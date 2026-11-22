/*
 * Stereo downmix for the Windows spatial audio stream of GTA V Enhanced.
 *
 * The game opens an ISpatialAudioObjectRenderStream with a fixed sound object
 * for each of 16 speaker positions and writes one mono float buffer for each
 * position in every audio period. Windows mixes those positions down to the
 * speakers the device has. Wine does not: it opens one device stream with a
 * channel for each position, and on two speakers the positions after front
 * left and front right are lost. The center voice is one of them.
 *
 * This code opens the real stream with front left and front right only. The
 * game gets objects that hold their own buffers, and at the end of each period
 * the code mixes those buffers into the two real objects.
 */
#include <math.h>

#define POSITION_FRONT_LEFT   0x2       /* AudioObjectType_FrontLeft */
#define POSITION_FRONT_RIGHT  0x4       /* AudioObjectType_FrontRight */
#define MAX_MIX_OBJECTS       40
#define MAX_PERIOD_FRAMES     8192

struct mix_stream;

/* The vtable slots of the two interfaces, in the order of spatialaudioclient.h. */
struct stream_vtable
{
    HRESULT (WINAPI *QueryInterface)(void *, REFIID, void **);
    ULONG (WINAPI *AddRef)(void *);
    ULONG (WINAPI *Release)(void *);
    HRESULT (WINAPI *GetAvailableDynamicObjectCount)(void *, UINT32 *);
    HRESULT (WINAPI *GetService)(void *, REFIID, void **);
    HRESULT (WINAPI *Start)(void *);
    HRESULT (WINAPI *Stop)(void *);
    HRESULT (WINAPI *Reset)(void *);
    HRESULT (WINAPI *BeginUpdatingAudioObjects)(void *, UINT32 *, UINT32 *);
    HRESULT (WINAPI *EndUpdatingAudioObjects)(void *);
    HRESULT (WINAPI *ActivateSpatialAudioObject)(void *, DWORD, void **);
};

struct object_vtable
{
    HRESULT (WINAPI *QueryInterface)(void *, REFIID, void **);
    ULONG (WINAPI *AddRef)(void *);
    ULONG (WINAPI *Release)(void *);
    HRESULT (WINAPI *GetBuffer)(void *, BYTE **, UINT32 *);
    HRESULT (WINAPI *SetEndOfStream)(void *, UINT32);
    HRESULT (WINAPI *IsActive)(void *, BOOL *);
    HRESULT (WINAPI *GetAudioObjectType)(void *, DWORD *);
    HRESULT (WINAPI *SetPosition)(void *, float, float, float);
    HRESULT (WINAPI *SetVolume)(void *, float);
};

struct real_stream { const struct stream_vtable *vtable; };
struct real_object { const struct object_vtable *vtable; };

struct mix_object
{
    const struct object_vtable *vtable;
    LONG references;
    struct mix_stream *stream;
    DWORD position;
    float left, right;          /* share of this position in each speaker */
    float volume;
    BOOL active;
    BOOL written;               /* the game asked for the buffer in this period */
    UINT32 frames;              /* frames to mix in this period */
    float samples[MAX_PERIOD_FRAMES];
};

struct mix_stream
{
    const struct stream_vtable *vtable;
    LONG references;
    struct real_stream *real;
    struct real_object *left, *right;
    CRITICAL_SECTION lock;
    UINT32 period_frames;
    struct mix_object *objects[MAX_MIX_OBJECTS];
};

/* The share of each speaker position in the left and right speaker. A center
 * or rear center position goes to both at -3 dB. Side, rear, top, and bottom
 * positions go to their own side at -3 dB. */
static void position_shares(DWORD position, float *left, float *right)
{
    static const float full = 1.0f, part = 0.7071f;

    *left = *right = 0.0f;
    switch (position)
    {
    case 0x2:     *left = full; break;              /* front left */
    case 0x4:     *right = full; break;             /* front right */
    case 0x8:     *left = *right = part; break;     /* front center */
    case 0x10:    *left = *right = 0.5f; break;     /* low frequency */
    case 0x20000: *left = *right = 0.5f; break;     /* back center */
    case 0x20: case 0x80: case 0x200: case 0x800: case 0x2000: case 0x8000:
        *left = part; break;                        /* side, back, top, bottom left */
    case 0x40: case 0x100: case 0x400: case 0x1000: case 0x4000: case 0x10000:
        *right = part; break;                       /* side, back, top, bottom right */
    default:      *left = *right = part; break;
    }
}

/* Sound objects that the game holds. */

static HRESULT WINAPI object_QueryInterface(void *self, REFIID iid, void **out)
{
    struct mix_object *object = self;

    InterlockedIncrement(&object->references);
    *out = object;
    return S_OK;
}

static ULONG WINAPI object_AddRef(void *self)
{
    struct mix_object *object = self;

    return InterlockedIncrement(&object->references);
}

static ULONG WINAPI object_Release(void *self)
{
    struct mix_object *object = self;
    struct mix_stream *stream = object->stream;
    ULONG references = InterlockedDecrement(&object->references);
    UINT i;

    if (references)
        return references;
    EnterCriticalSection(&stream->lock);
    for (i = 0; i < MAX_MIX_OBJECTS; i++)
        if (stream->objects[i] == object)
            stream->objects[i] = NULL;
    LeaveCriticalSection(&stream->lock);
    stream->vtable->Release(stream);
    HeapFree(GetProcessHeap(), 0, object);
    return 0;
}

static HRESULT WINAPI object_GetBuffer(void *self, BYTE **buffer, UINT32 *length)
{
    struct mix_object *object = self;
    struct mix_stream *stream = object->stream;

    if (!buffer || !length)
        return E_POINTER;
    EnterCriticalSection(&stream->lock);
    if (!object->written)
    {
        memset(object->samples, 0, stream->period_frames * sizeof(float));
        object->written = TRUE;
        object->frames = stream->period_frames;
    }
    *buffer = (BYTE *)object->samples;
    *length = stream->period_frames * sizeof(float);
    LeaveCriticalSection(&stream->lock);
    return S_OK;
}

static HRESULT WINAPI object_SetEndOfStream(void *self, UINT32 frames)
{
    struct mix_object *object = self;
    struct mix_stream *stream = object->stream;

    EnterCriticalSection(&stream->lock);
    if (object->written && frames < object->frames)
        object->frames = frames;
    object->active = FALSE;
    LeaveCriticalSection(&stream->lock);
    return S_OK;
}

static HRESULT WINAPI object_IsActive(void *self, BOOL *active)
{
    struct mix_object *object = self;

    if (!active)
        return E_POINTER;
    *active = object->active;
    return S_OK;
}

static HRESULT WINAPI object_GetAudioObjectType(void *self, DWORD *position)
{
    struct mix_object *object = self;

    if (!position)
        return E_POINTER;
    *position = object->position;
    return S_OK;
}

static HRESULT WINAPI object_SetPosition(void *self, float x, float y, float z)
{
    return S_OK;
}

static HRESULT WINAPI object_SetVolume(void *self, float volume)
{
    struct mix_object *object = self;

    object->volume = volume;
    return S_OK;
}

static const struct object_vtable mix_object_vtable =
{
    object_QueryInterface, object_AddRef, object_Release, object_GetBuffer, object_SetEndOfStream,
    object_IsActive, object_GetAudioObjectType, object_SetPosition, object_SetVolume,
};

/* The stream that the game holds. */

static HRESULT WINAPI stream_QueryInterface(void *self, REFIID iid, void **out)
{
    struct mix_stream *stream = self;
    void *real_out = NULL;
    HRESULT result = stream->real->vtable->QueryInterface(stream->real, iid, &real_out);

    if (FAILED(result))
        return result;
    ((IUnknown *)real_out)->lpVtbl->Release(real_out);
    InterlockedIncrement(&stream->references);
    *out = stream;
    return S_OK;
}

static ULONG WINAPI stream_AddRef(void *self)
{
    struct mix_stream *stream = self;

    return InterlockedIncrement(&stream->references);
}

static ULONG WINAPI stream_Release(void *self)
{
    struct mix_stream *stream = self;
    ULONG references = InterlockedDecrement(&stream->references);

    if (references)
        return references;
    stream->left->vtable->Release(stream->left);
    stream->right->vtable->Release(stream->right);
    stream->real->vtable->Release(stream->real);
    DeleteCriticalSection(&stream->lock);
    HeapFree(GetProcessHeap(), 0, stream);
    return 0;
}

static HRESULT WINAPI stream_GetAvailableDynamicObjectCount(void *self, UINT32 *count)
{
    struct mix_stream *stream = self;

    return stream->real->vtable->GetAvailableDynamicObjectCount(stream->real, count);
}

static HRESULT WINAPI stream_GetService(void *self, REFIID iid, void **service)
{
    struct mix_stream *stream = self;

    return stream->real->vtable->GetService(stream->real, iid, service);
}

static HRESULT WINAPI stream_Start(void *self)
{
    struct mix_stream *stream = self;

    return stream->real->vtable->Start(stream->real);
}

static HRESULT WINAPI stream_Stop(void *self)
{
    struct mix_stream *stream = self;

    return stream->real->vtable->Stop(stream->real);
}

static HRESULT WINAPI stream_Reset(void *self)
{
    struct mix_stream *stream = self;

    return stream->real->vtable->Reset(stream->real);
}

static HRESULT WINAPI stream_BeginUpdatingAudioObjects(void *self, UINT32 *dynamic_count, UINT32 *frames)
{
    struct mix_stream *stream = self;
    UINT32 real_frames = 0;
    HRESULT result = stream->real->vtable->BeginUpdatingAudioObjects(stream->real, dynamic_count, &real_frames);

    if (FAILED(result))
        return result;
    EnterCriticalSection(&stream->lock);
    stream->period_frames = real_frames > MAX_PERIOD_FRAMES ? MAX_PERIOD_FRAMES : real_frames;
    LeaveCriticalSection(&stream->lock);
    if (frames)
        *frames = real_frames;
    return result;
}

static HRESULT WINAPI stream_EndUpdatingAudioObjects(void *self)
{
    struct mix_stream *stream = self;
    BYTE *left_bytes = NULL, *right_bytes = NULL;
    UINT32 left_length = 0, right_length = 0, frames, frame, i;

    EnterCriticalSection(&stream->lock);
    if (stream->period_frames
        && SUCCEEDED(stream->left->vtable->GetBuffer(stream->left, &left_bytes, &left_length))
        && SUCCEEDED(stream->right->vtable->GetBuffer(stream->right, &right_bytes, &right_length)))
    {
        float *left = (float *)left_bytes, *right = (float *)right_bytes;

        frames = left_length < right_length ? left_length : right_length;
        frames /= sizeof(float);
        memset(left, 0, left_length);
        memset(right, 0, right_length);
        for (i = 0; i < MAX_MIX_OBJECTS; i++)
        {
            struct mix_object *object = stream->objects[i];
            UINT32 count;
            float to_left, to_right;

            if (!object || !object->written)
                continue;
            count = object->frames < frames ? object->frames : frames;
            to_left = object->left * object->volume;
            to_right = object->right * object->volume;
            for (frame = 0; frame < count; frame++)
            {
                left[frame] += object->samples[frame] * to_left;
                right[frame] += object->samples[frame] * to_right;
            }
            object->written = FALSE;
        }
    }
    stream->period_frames = 0;
    LeaveCriticalSection(&stream->lock);
    return stream->real->vtable->EndUpdatingAudioObjects(stream->real);
}

static HRESULT WINAPI stream_ActivateSpatialAudioObject(void *self, DWORD position, void **out)
{
    struct mix_stream *stream = self;
    struct mix_object *object;
    UINT i;

    if (!out)
        return E_POINTER;
    *out = NULL;
    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*object));
    if (!object)
        return E_OUTOFMEMORY;
    object->vtable = &mix_object_vtable;
    object->references = 1;
    object->stream = stream;
    object->position = position;
    object->volume = 1.0f;
    object->active = TRUE;
    position_shares(position, &object->left, &object->right);
    EnterCriticalSection(&stream->lock);
    for (i = 0; i < MAX_MIX_OBJECTS && stream->objects[i]; i++)
        ;
    if (i < MAX_MIX_OBJECTS)
        stream->objects[i] = object;
    LeaveCriticalSection(&stream->lock);
    if (i == MAX_MIX_OBJECTS)
    {
        HeapFree(GetProcessHeap(), 0, object);
        return E_OUTOFMEMORY;
    }
    InterlockedIncrement(&stream->references);
    shim_log("audio: the game activated the object for position 0x%lx, left %.2f, right %.2f",
             position, object->left, object->right);
    *out = object;
    return S_OK;
}

static const struct stream_vtable mix_stream_vtable =
{
    stream_QueryInterface, stream_AddRef, stream_Release, stream_GetAvailableDynamicObjectCount,
    stream_GetService, stream_Start, stream_Stop, stream_Reset, stream_BeginUpdatingAudioObjects,
    stream_EndUpdatingAudioObjects, stream_ActivateSpatialAudioObject,
};

/* Take the real two-position stream and give back the stream for the game.
 * On a failure the real stream is released and NULL comes back. */
static void *make_mix_stream(void *real)
{
    struct mix_stream *stream = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*stream));

    if (!stream)
        return NULL;
    stream->vtable = &mix_stream_vtable;
    stream->references = 1;
    stream->real = real;
    if (FAILED(stream->real->vtable->ActivateSpatialAudioObject(real, POSITION_FRONT_LEFT, (void **)&stream->left))
        || FAILED(stream->real->vtable->ActivateSpatialAudioObject(real, POSITION_FRONT_RIGHT, (void **)&stream->right)))
    {
        if (stream->left)
            stream->left->vtable->Release(stream->left);
        HeapFree(GetProcessHeap(), 0, stream);
        return NULL;
    }
    InitializeCriticalSection(&stream->lock);
    return stream;
}
