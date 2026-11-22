/*
 * COM call logger for the d3d12 and dxgi shims.
 *
 * hook_object swaps the vtable pointer of a COM object for a copy. Each slot
 * of the copy points at a small thunk. The thunk records the call, sends the
 * call to the original method, and catches the return so the log shows the
 * result of every method.
 *
 * The copy keeps four pointers before its first slot. [-2] and [-1] hold the
 * same values as the original vtable has there, because Apple's objects are
 * C++ objects and a dynamic_cast reads the type information from those two.
 * [-4] holds the class description and [-3] the original vtable.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <tlhelp32.h>

#define MAX_SLOTS 128
#define MAX_COPIES 64
#define MAX_DEPTH 64
#define EXTRA_SLOTS 240   /* Apple's objects have private methods after the public ones */

struct com_class;

struct call_frame
{
    void *return_address;
    const struct com_class *class;
    UINT slot;
    void *self;
    UINT_PTR rdx, r8, r9;
    UINT_PTR *stack;            /* stack[0] is the return address, stack[5] the fifth argument */
};

struct slot_override
{
    const char *method;
    void *function;
};

struct com_class
{
    const char *name;
    const char *const *methods;
    UINT count;
    BOOL (*in_family)(REFIID iid);
    void (*after)(struct call_frame *frame, UINT_PTR result);
    const struct slot_override *overrides;
};

struct thread_frames
{
    UINT depth;
    struct call_frame frames[MAX_DEPTH];
};

static CRITICAL_SECTION hook_lock;
static LONG hook_lock_ready;
static DWORD frames_tls = TLS_OUT_OF_INDEXES;
static void **copies[MAX_COPIES];
static UINT copy_count;
static BOOL log_entries = TRUE; /* D3D12SHIM_NOENTER=1 stops the log line before each call */
static BOOL debug_mode;         /* on when the file C:\d3d12shim.debug exists: write the log */

static void hook_object(void *object, const struct com_class *class);
static void install_suspend_guard(void);
static void dump_code(void *address);

static void shim_lock(void)
{
    if (InterlockedCompareExchange(&hook_lock_ready, 1, 0) == 0)
    {
        InitializeCriticalSection(&hook_lock);
        frames_tls = TlsAlloc();
    }
    EnterCriticalSection(&hook_lock);
}

static void shim_log(const char *format, ...)
{
    FILE *file;
    va_list args;

    if (!debug_mode)
        return;
    shim_lock();
    file = fopen("C:\\d3d12shim.log", "a");
    if (file)
    {
        fprintf(file, "[%04lx] ", GetCurrentThreadId());
        va_start(args, format);
        vfprintf(file, format, args);
        va_end(args);
        fputc('\n', file);
        fclose(file);
    }
    LeaveCriticalSection(&hook_lock);
}

static const char *guid_text(const GUID *guid, char *buffer)
{
    if (!guid)
        return "(null)";
    sprintf(buffer, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
            guid->Data1, guid->Data2, guid->Data3,
            guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
            guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
    return buffer;
}

static void hex_text(const void *data, UINT size, char *buffer)
{
    UINT i, count = size > 48 ? 48 : size;

    buffer[0] = 0;
    for (i = 0; i < count; i++)
        sprintf(buffer + i * 3, "%02x ", ((const unsigned char *)data)[i]);
}

/*
 * Log what the game sends to OutputDebugString, and each exception it
 * raises. Wine delivers a debug string as an exception, so one vectored
 * handler sees both.
 */
static LONG CALLBACK log_exception(EXCEPTION_POINTERS *pointers)
{
    EXCEPTION_RECORD *record = pointers->ExceptionRecord;

    if (record->ExceptionCode == DBG_PRINTEXCEPTION_C && record->NumberParameters >= 2)
        shim_log("debug: %.*s", (int)record->ExceptionInformation[0],
                 (const char *)record->ExceptionInformation[1]);
    else if (record->ExceptionCode == 0x4001000a && record->NumberParameters >= 2)
        shim_log("debug: %.*ls", (int)record->ExceptionInformation[0],
                 (const WCHAR *)record->ExceptionInformation[1]);
    else
        shim_log("exception 0x%08lx at %p, parameters %lu: %p %p %p", record->ExceptionCode,
                 record->ExceptionAddress, record->NumberParameters,
                 (void *)record->ExceptionInformation[0], (void *)record->ExceptionInformation[1],
                 (void *)record->ExceptionInformation[2]);
    return EXCEPTION_CONTINUE_SEARCH;
}

/*
 * Stack report. Some seconds after the game starts Direct3D, a watchdog
 * thread logs the call stack of every thread, and dumps the game code around
 * each frame that lies in the exe. It shows where the game sits when it has
 * an error box on screen.
 */
static void log_thread_stack(DWORD thread_id)
{
    HANDLE thread = OpenThread(THREAD_ALL_ACCESS, FALSE, thread_id);
    HMODULE exe = GetModuleHandleW(NULL);
    CONTEXT context;
    UINT frame;

    if (!thread)
        return;
    /* Hold the log lock while the thread stops, so it cannot stop inside the
     * lock and block every later log line. */
    shim_lock();
    SuspendThread(thread);
    LeaveCriticalSection(&hook_lock);
    memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &context))
    {
        shim_log("stack of thread %04lx", thread_id);
        for (frame = 0; frame < 48 && context.Rip; frame++)
        {
            DWORD64 base = 0, establisher = 0;
            RUNTIME_FUNCTION *function;
            char name[MAX_PATH] = "?";
            HMODULE module = NULL;
            void *handler_data;

            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (const WCHAR *)context.Rip, &module))
                GetModuleFileNameA(module, name, sizeof(name));
            shim_log("    %2u %p %s+0x%llx", frame, (void *)context.Rip,
                     strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name,
                     (unsigned long long)(context.Rip - (DWORD64)module));
            if (module == exe)
                dump_code((void *)context.Rip);
            function = RtlLookupFunctionEntry(context.Rip, &base, NULL);
            if (!function)
            {
                /* A leaf function: the return address is at the top of the stack. */
                if (IsBadReadPtr((void *)context.Rsp, 8))
                    break;
                context.Rip = *(DWORD64 *)context.Rsp;
                context.Rsp += 8;
                continue;
            }
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context,
                             &handler_data, &establisher, NULL);
        }
    }
    ResumeThread(thread);
    CloseHandle(thread);
}

/* Write the game image as it is in memory to a file.
 * The file on disk is packed, so this is the copy to disassemble. */
static void dump_image(const char *path)
{
    char *base = (char *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *headers = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    SIZE_T size = headers->OptionalHeader.SizeOfImage, offset;
    static char zeros[0x1000];
    FILE *file;

    file = fopen(path, "wb");
    if (!file)
        return;
    for (offset = 0; offset < size; offset += 0x1000)
        fwrite(IsBadReadPtr(base + offset, 0x1000) ? zeros : base + offset, 1, 0x1000, file);
    fclose(file);
    shim_log("wrote the game image, 0x%llx bytes", (unsigned long long)size);
}

static DWORD WINAPI stack_report_thread(void *delay)
{
    HANDLE snapshot;
    THREADENTRY32 entry;

    Sleep(15000);
    dump_image("C:\\shimimage.bin");
    Sleep((DWORD)(UINT_PTR)delay);
    dump_image("C:\\shimimage2.bin");
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32OwnerProcessID == GetCurrentProcessId()
                && entry.th32ThreadID != GetCurrentThreadId())
                log_thread_stack(entry.th32ThreadID);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    shim_log("stack report done");
    return 0;
}

static void start_stack_report(DWORD delay)
{
    static LONG started;

    if (debug_mode && InterlockedCompareExchange(&started, 1, 0) == 0)
        CloseHandle(CreateThread(NULL, 0, stack_report_thread, (void *)(UINT_PTR)delay, 0, NULL));
}

static void read_settings(void)
{
    log_entries = GetEnvironmentVariableA("D3D12SHIM_NOENTER", NULL, 0) == 0;
    debug_mode = GetFileAttributesA("C:\\d3d12shim.debug") != INVALID_FILE_ATTRIBUTES;
    install_suspend_guard();
}

static const struct com_class *class_of(void *object)
{
    return (const struct com_class *)(*(void ***)object)[-4];
}

static void **original_vtable(void *object)
{
    return (void **)(*(void ***)object)[-3];
}

static UINT slot_of(const struct com_class *class, const char *method)
{
    UINT i;

    for (i = 0; i < class->count; i++)
        if (!strcmp(class->methods[i], method))
            return i;
    return ~0u;
}

/*
 * Suspend guard.
 *
 * D3DMetal runs Apple's native code on the Windows stack of the thread that
 * makes the call. When another thread suspends that thread in the middle of
 * such a call, Wine takes the stop for a stop in Windows code, and resumes
 * the thread with the Windows thread pointer in the gs register. The next
 * macOS call that reads thread data then faults, and the game exits with
 * 0xc0000005. One game thread suspends the other threads about five seconds
 * after the game window opens, most likely the Social Club overlay when it
 * installs its hooks.
 *
 * The guard puts a hook on NtSuspendThread. A suspend waits until no thread
 * is inside a hooked call, and no thread starts a hooked call while a suspend
 * waits. The d3d12 and dxgi shims share the state through a named mapping.
 */
struct suspend_state
{
    LONG installed;     /* one of the shim DLLs made the hook */
    LONG lock;          /* one thread at a time suspends another */
    LONG pending;       /* a suspend waits: no thread may start a hooked call */
    LONG active;        /* count of threads inside a hooked call */
    DWORD depth_tls;    /* for each thread: the depth of hooked calls */
};

#define SUSPEND_WAIT_MS 100     /* the longest time a suspend waits for the calls to end */
#define SYSCALL_STUB_SIZE 32    /* size of a Wine system call stub in ntdll.dll */

static struct suspend_state *suspend_state;
static LONG (WINAPI *real_suspend_thread)(HANDLE thread, ULONG *count);

static unsigned char *emit(const void *bytes, UINT size);

static void guard_enter(void)
{
    struct suspend_state *state = suspend_state;
    UINT_PTR depth;

    if (!state)
        return;
    depth = (UINT_PTR)TlsGetValue(state->depth_tls);
    TlsSetValue(state->depth_tls, (void *)(depth + 1));
    if (depth)
        return;
    for (;;)
    {
        while (state->pending)
            Sleep(1);
        InterlockedIncrement(&state->active);
        if (!state->pending)
            return;
        InterlockedDecrement(&state->active);
    }
}

static void guard_exit(void)
{
    struct suspend_state *state = suspend_state;
    UINT_PTR depth;

    if (!state)
        return;
    depth = (UINT_PTR)TlsGetValue(state->depth_tls);
    if (!depth)
        return;
    TlsSetValue(state->depth_tls, (void *)(depth - 1));
    if (depth == 1)
        InterlockedDecrement(&state->active);
}

static LONG WINAPI guarded_suspend_thread(HANDLE thread, ULONG *count)
{
    struct suspend_state *state = suspend_state;
    LONG own = TlsGetValue(state->depth_tls) ? 1 : 0;
    DWORD start;
    LONG status;

    while (InterlockedCompareExchange(&state->lock, 1, 0))
        Sleep(1);
    InterlockedExchange(&state->pending, 1);
    start = GetTickCount();
    while (state->active > own && GetTickCount() - start < SUSPEND_WAIT_MS)
        Sleep(1);
    status = real_suspend_thread(thread, count);
#ifdef SUSPEND_LOG
    {
        FILE *file = fopen("C:\\d3d12shim.suspend.log", "a");
        if (file)
        {
            fprintf(file, "[%04lx] suspend of thread %lu: waited %lu ms, active %ld, own %ld, status 0x%lx\n",
                    GetCurrentThreadId(), GetThreadId(thread), GetTickCount() - start,
                    state->active, own, status);
            fclose(file);
        }
    }
#endif
    InterlockedExchange(&state->pending, 0);
    InterlockedExchange(&state->lock, 0);
    return status;
}

static void install_suspend_guard(void)
{
    static const unsigned char stub_head[] = { 0x4c, 0x8b, 0xd1, 0xb8 };   /* mov r10, rcx; mov eax, number */
    unsigned char jump[12] = { 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,         /* mov rax, guarded_suspend_thread */
                               0xff, 0xe0 };                               /* jmp rax */
    struct suspend_state *state;
    unsigned char *stub;
    void *target = guarded_suspend_thread;
    HANDLE mapping;
    HMODULE pinned;
    DWORD protect;
    char name[64];

    sprintf(name, "d3d12shim-suspend-%lu", GetCurrentProcessId());
    mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(*state), name);
    state = mapping ? MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0) : NULL;
    if (!state)
        return;
    if (InterlockedCompareExchange(&state->installed, 1, 0) == 0)
    {
        state->depth_tls = TlsAlloc();
        stub = (unsigned char *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSuspendThread");
        if (state->depth_tls == TLS_OUT_OF_INDEXES || !stub || memcmp(stub, stub_head, sizeof(stub_head))
            || !VirtualProtect(stub, sizeof(jump), PAGE_EXECUTE_READWRITE, &protect))
        {
            shim_log("suspend guard: no hook, NtSuspendThread at %p", stub);
            return;
        }
        /* The stub has no relative address in it, so a copy works as the original. */
        real_suspend_thread = (void *)emit(stub, SYSCALL_STUB_SIZE);
        memcpy(jump + 2, &target, sizeof(target));
        memcpy(stub, jump, sizeof(jump));
        VirtualProtect(stub, sizeof(jump), protect, &protect);
        FlushInstructionCache(GetCurrentProcess(), stub, sizeof(jump));
        /* The hook points into this DLL, so the DLL must stay. */
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           (const WCHAR *)guarded_suspend_thread, &pinned);
        shim_log("suspend guard: hook on NtSuspendThread at %p", stub);
    }
    if (state->depth_tls != TLS_OUT_OF_INDEXES)
        suspend_state = state;
}

static struct thread_frames *thread_frames(void)
{
    struct thread_frames *frames = TlsGetValue(frames_tls);

    if (!frames)
    {
        frames = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*frames));
        TlsSetValue(frames_tls, frames);
    }
    return frames;
}

static const unsigned char *return_stub;

/* Write the game code around an address to C:\shimdump_<address>.bin, once per address. */
static void dump_code(void *address)
{
    static void *dumped[96];
    static UINT count;
    char path[64];
    FILE *file;
    UINT i;

    for (i = 0; i < count; i++)
        if (dumped[i] == address)
            return;
    if (count >= 96)
        return;
    dumped[count++] = address;
    sprintf(path, "C:\\shimdump_%p.bin", address);
    file = fopen(path, "wb");
    if (!file)
        return;
    fwrite((const char *)address - 0x4000, 1, 0x6000, file);
    fclose(file);
}

/* The thunk calls this before the method. entry points at the return address. */
static void WINAPI log_enter(UINT slot, void *self, UINT_PTR *entry)
{
    struct thread_frames *frames = thread_frames();
    struct call_frame *frame;
    char text[64];

    if (frames->depth >= MAX_DEPTH)
        return;                 /* too deep to track: let the call return to its caller */
    frame = &frames->frames[frames->depth++];
    frame->return_address = (void *)entry[0];
    frame->class = class_of(self);
    frame->slot = slot;
    frame->self = self;
    frame->rdx = entry[-2];
    frame->r8 = entry[-3];
    frame->r9 = entry[-4];
    frame->stack = entry;
    entry[0] = (UINT_PTR)return_stub;
    guard_enter();
    if (!debug_mode)
        return;
    if (slot == 0)
        shim_log("%s::QueryInterface asks for %s", frame->class->name,
                 guid_text((const GUID *)frame->rdx, text));
    else if (log_entries)
        shim_log("%s::%s enter, caller %p", frame->class->name, frame->class->methods[slot],
                 frame->return_address);
    if (slot == 2 && !strcmp(frame->class->name, "device"))
        dump_code(frame->return_address);
}

/* The return stub calls this after the method. It answers with the real return address. */
static void *WINAPI log_exit(UINT_PTR result)
{
    struct thread_frames *frames = thread_frames();
    struct call_frame *frame = &frames->frames[--frames->depth];
    const struct com_class *class = frame->class;

    guard_exit();
    if (debug_mode && frame->slot != 1 && frame->slot != 2)
        shim_log("%s::%s(%p, %p, %p) -> 0x%08lx", class->name, class->methods[frame->slot],
                 (void *)frame->rdx, (void *)frame->r8, (void *)frame->r9, (unsigned long)result);
    if (frame->slot == 0 && SUCCEEDED((HRESULT)result) && class->in_family
        && class->in_family((REFIID)frame->rdx) && frame->r8 && *(void **)frame->r8)
        hook_object(*(void **)frame->r8, class);
    if (class->after)
        class->after(frame, result);
    return frame->return_address;
}

static unsigned char *code_page;
static UINT code_used;

static unsigned char *emit(const void *bytes, UINT size)
{
    unsigned char *start;

    if (!code_page)
        code_page = VirtualAlloc(NULL, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    start = code_page + code_used;
    memcpy(start, bytes, size);
    code_used += size;
    return start;
}

static void make_return_stub(void)
{
    static const unsigned char head[] = { 0x50,                         /* push rax */
                                          0x48, 0x83, 0xec, 0x28,       /* sub rsp, 0x28 */
                                          0x48, 0x89, 0xc1,             /* mov rcx, rax */
                                          0x48, 0xb8 };                 /* mov rax, log_exit */
    static const unsigned char tail[] = { 0xff, 0xd0,                   /* call rax */
                                          0x49, 0x89, 0xc3,             /* mov r11, rax */
                                          0x48, 0x83, 0xc4, 0x28,       /* add rsp, 0x28 */
                                          0x58,                         /* pop rax */
                                          0x41, 0xff, 0xe3 };           /* jmp r11 */
    void *target = log_exit;

    return_stub = emit(head, sizeof(head));
    emit(&target, sizeof(target));
    emit(tail, sizeof(tail));
}

static void *slot_thunk(UINT slot)
{
    static void *made[MAX_SLOTS];
    static const unsigned char head[] = { 0x51, 0x52, 0x41, 0x50, 0x41, 0x51,   /* push rcx, rdx, r8, r9 */
                                          0x48, 0x83, 0xec, 0x28,               /* sub rsp, 0x28 */
                                          0x48, 0x89, 0xca,                     /* mov rdx, rcx */
                                          0x4c, 0x8d, 0x44, 0x24, 0x48,         /* lea r8, [rsp + 0x48] */
                                          0xb9 };                               /* mov ecx, slot */
    static const unsigned char call[] = { 0x48, 0xb8 };                         /* mov rax, log_enter */
    static const unsigned char tail[] = { 0xff, 0xd0,                           /* call rax */
                                          0x48, 0x83, 0xc4, 0x28,               /* add rsp, 0x28 */
                                          0x41, 0x59, 0x41, 0x58, 0x5a, 0x59,   /* pop r9, r8, rdx, rcx */
                                          0x48, 0x8b, 0x01,                     /* mov rax, [rcx] */
                                          0x48, 0x8b, 0x40, 0xe8,               /* mov rax, [rax - 0x18] */
                                          0xff, 0xa0 };                         /* jmp [rax + offset] */
    void *target = log_enter;
    UINT offset = slot * sizeof(void *);

    if (made[slot])
        return made[slot];
    if (!return_stub)
        make_return_stub();
    made[slot] = emit(head, sizeof(head));
    emit(&slot, 4);
    emit(call, sizeof(call));
    emit(&target, sizeof(target));
    emit(tail, sizeof(tail));
    emit(&offset, 4);
    return made[slot];
}

static void hook_object(void *object, const struct com_class *class)
{
    void **vtable, **copy = NULL;
    const struct slot_override *override;
    UINT i;

    if (!object)
        return;
    shim_lock();
    vtable = *(void ***)object;
    for (i = 0; i < copy_count; i++)
    {
        if (copies[i] == vtable)
            goto done;          /* the object has a copy already */
        if (copies[i][-3] == (void *)vtable && copies[i][-4] == (void *)class)
            copy = copies[i];
    }
    if (!copy && copy_count < MAX_COPIES)
    {
        copy = HeapAlloc(GetProcessHeap(), 0, (class->count + EXTRA_SLOTS + 4) * sizeof(void *));
        copy += 4;
        copy[-4] = (void *)class;
        copy[-3] = vtable;
        copy[-2] = vtable[-2];
        copy[-1] = vtable[-1];
        memcpy(copy, vtable, (class->count + EXTRA_SLOTS) * sizeof(void *));
        for (i = 0; i < class->count && i < MAX_SLOTS; i++)
            copy[i] = slot_thunk(i);
        for (override = class->overrides; override && override->method; override++)
            copy[slot_of(class, override->method)] = override->function;
        copies[copy_count++] = copy;
    }
    if (copy)
        *(void ***)object = copy;
done:
    LeaveCriticalSection(&hook_lock);
    if (copy)
        shim_log("hooked %s %p, vtable %p", class->name, object, vtable);
}
