# gta5-d3dmetal-shim

Three small DLLs that let Grand Theft Auto V Enhanced (build 1.0.1158.16, Rockstar Games Launcher) run in story mode on Apple's D3DMetal 2.1 under Wine on macOS. They go in the game folder and pass every call to Apple's `d3d12.dll` and `dxgi.dll` in `system32`.

This is the source that [frankea/Whisky issue #408](https://github.com/frankea/Whisky/issues/408) offers. It is a workaround and a record of four causes, not a finished product. Each cause has a better home in Wine, in Whisky, or in the game.

AI disclosure: an AI coding assistant did the reverse engineering and wrote this code and this text. The owner of the repository played the game for several hours with these DLLs, but has not checked every line. Treat the addresses and the causes as leads to verify, not as proven facts.

## The four problems

| Problem | Cause | Where the shim handles it |
| --- | --- | --- |
| The game stops at start with `ERR_GFX_D3D_NOD3D12` | The game looks for a loaded module named `D3D12Core.dll` and reads its `D3D12SDKVersion`. It stops when the module is missing or the value is 610 or lower. D3DMetal loads no module of that name. | `d3d12core.c` is a stand-in that exports the version the game exe asks for. `load_core()` in `d3d12shim.c` loads it on the first `D3D12CreateDevice`. |
| The game stays at "Initializing" | The Social Club overlay patches the code of `CreateSwapChain*`, `ExecuteCommandLists`, `Signal`, `Present`, and `ResizeBuffers`. On Apple's functions that patch fails (`Failed to hook DirectX12 DLL` in `socialclub.log`), and the game waits for the overlay with no time limit. | `comhook.h` gives the device, the command queues, the DXGI factories, and the swap chains a copy of their vtable. Each slot points at a small thunk that the overlay can patch. |
| Music and effects play, but the voice of the character in focus is missing | The game plays all sound through `ISpatialAudioClient` with a static object for each of 16 speaker positions (mask `0x3ffee`). Wine's `mmdevapi` opens one device channel for each position and does no downmix, so two speakers get front left and front right only. | `stereomix.h` opens the real stream with front left and front right and mixes the 16 positions into them. `start_stereo_mix()` in `d3d12shim.c` installs it through two vtable slots in `mmdevapi.dll`. |
| The game exits with `0xc0000005` about five seconds after its window opens | One game thread calls `NtSuspendThread` 622 times at start (most likely the overlay when it installs its hooks, not proven). D3DMetal runs Apple's native code on the Windows stack of the calling thread. When a suspend stops a thread inside that code, Wine resumes it with the Windows thread pointer in gs, and the next macOS call that reads thread data faults in `pthread_self`, `pthread_setspecific`, or the autorelease pool. | The suspend guard in `comhook.h` puts a hook on `NtSuspendThread`. A suspend waits, 100 ms at most, until no thread is inside a hooked call, and no thread starts a hooked call while a suspend waits. |

The fourth problem showed up as seven crashes in seven starts on a 60 Hz external display, and not on the built-in display. With the guard, five of five starts worked and a session of several hours ran without a crash. The proper fix is in Wine's signal code (`dlls/ntdll/unix/signal_x86_64.c`), which must keep the macOS gs base when it stops a thread inside native code that runs on a Windows stack.

The shim also corrects two answers of `ID3D12Device::CheckFeatureSupport` that differ from Windows (the highest feature level and the highest shader model). The game runs without those corrections. `D3D12SHIM_NOFIX=1` turns them off.

## Files

| File | Purpose |
| --- | --- |
| `d3d12shim.c` | `d3d12.dll`: the pass-through exports, the hooks on the device and the command queues, the load of the stand-in, and the start of the stereo mix |
| `dxgishim.c` | `dxgi.dll`: the pass-through exports and the hooks on factories and swap chains |
| `d3d12core.c` | `D3D12Core.dll`: the stand-in with `D3D12SDKVersion` |
| `comhook.h` | The vtable copy, the thunks, the call log, and the suspend guard. Both shims include it. |
| `stereomix.h` | The downmix of the spatial audio stream to two speakers |
| `gen_slots.py` | Writes `slots.h`, the method names of each hooked interface, from the mingw headers |
| `build.sh` | Builds the three DLLs |

## Build and install

The build needs `mingw-w64` (`brew install mingw-w64`) and Python 3.

```
./build.sh
```

Copy `d3d12.dll`, `dxgi.dll`, and `D3D12Core.dll` into the game folder, next to `GTA5_Enhanced.exe`. The game loads them from its own folder before the ones in `system32`. Start the game from the Rockstar Games Launcher.

The launcher logs `Unexpected file` for the three DLLs and does not remove them. A "verify integrity" run or a game update can remove them.

## Switches

| Switch | Effect |
| --- | --- |
| The file `C:\d3d12shim.debug` exists | Debug mode. The shims log each hooked call, the game's `OutputDebugString` text, and each exception to `C:\d3d12shim.log`. Fifteen seconds after the first device they write the game image from memory to `C:\shimimage.bin`, and ninety seconds later they log the call stack of each thread. The log grows about 1 MB each second, so remove the file for play. |
| The file `C:\d3d12shim.spatial` exists | The stereo mix is off. |
| `D3D12SHIM_NOFIX=1` | The two `CheckFeatureSupport` corrections are off. |
| `D3D12SHIM_NOENTER=1` | Debug mode logs no line before a call, only the line after it. |
| Build with `-DSUSPEND_LOG` | One line for each suspend in `C:\d3d12shim.suspend.log`. |

## Details that cost time

- The vtable copy must keep the two pointers before the vtable start and about 240 slots after the public methods. Apple's objects are C++ objects with private methods, and D3DMetal crashes without both.
- Do not hook DXGI outputs. D3DMetal crashed in `CreateSwapChainForHwnd` in two of three starts when the output it got from `EnumOutputs` had a hook.
- The hooks must be on in normal play, not only in debug mode. The overlay needs functions that it can patch.
- Debug mode hides the suspend crash, because the file work of the log changes the timing.

## Limits

- Tested on one Mac (M2 Max, macOS 15, Whisky 3.10.2 with engine 4.8.3 and D3DMetal 2.1) and one game build, 1.0.1158.16.
- Story mode only. BattlEye rules out GTA Online.
- The suspend guard covers calls on the hooked objects only. Calls on command lists and resources have no guard.
- The stereo mix assumes a two-speaker output device.
- About one start in ten closed itself about 83 seconds after start in the tests before the suspend guard. It is not known if the guard stops that too.

## License

MIT, see `LICENSE`.
