# DirectPort SDK

GPU-resident inter-process communication for Windows using DirectX shared handles and fence synchronization.

## Overview

DirectPort provides a minimal C API for sharing GPU memory between processes without CPU staging. The core library is format-agnostic and handles only resource creation, NT handle resolution, and GPU synchronization.

**Key characteristics:**
- Singleton D3D11/D3D12 device per process
- NT handle named sharing (no raw handle passing)
- Fence-based GPU synchronization
- Format-agnostic memory layout (`DP_FORMAT_VIDEO`, `FLOAT`, `HALF`, `RAW_32BIT`)
- Optional CPU access via `is_system_ram` flag (D3D12 only)

## Files

```
directport.h          // C API header, format enum, handle typedef
directportd3d12.cpp   // Primary implementation: D3D12 resources, fences, mapping
directportd3d11.cpp   // Compatibility layer: D3D11 resources + D3D12 NT resolver
```

## Build Requirements

- Windows 10 (1809+) or Windows 11
- MSVC 2019+ or Clang/LLVM for Windows
- Windows SDK 10.0.17763.0+
- Link against: `d3d12.lib`, `d3d11.lib`, `dxgi.lib`, `advapi32.lib`

## API Usage Pattern

### Initialization (once per process)
```c
// D3D12 producer/consumer
if (!dp12_init()) { /* handle error */ }

// D3D11 consumer (includes D3D12 resolver for NT names)
if (!dp11_init()) { /* handle error */ }
```

### Create/Open Shared Resource
```c
// Producer: create shared resource
DP_HANDLE port = dp12_create_shared_resource(
    width, height, format, is_system_ram,
    L"MyTexture", L"MyFence"
);

// Consumer: open by name
DP_HANDLE port = dp12_open_shared_resource(L"MyTexture", L"MyFence");
// or for D3D11 consumer:
DP_HANDLE port = dp11_open_shared_resource(L"MyTexture", L"MyFence");
```

### Synchronization
```c
// Producer: signal after rendering
dp12_signal_fence(port, frame_counter++);

// Consumer: Asynchronous GPU-side wait (preferred — zero CPU overhead)
uint64_t latest = dp12_get_completed_value(port);
if (latest > last_seen) {
    dp12_queue_wait(port, pCommandQueue, latest);  // GPU hardware wait, CPU returns immediately
    // safe to access shared resource
    last_seen = latest;
}

// Consumer: CPU-side wait (use only when CPU readback is required)
dp12_cpu_wait(port, latest);  // blocks via OS scheduler, 1–15ms latency
```

### CPU Access (D3D12, optional)
```c
// Only valid if resource was created with is_system_ram = true
uint32_t pitch;
void* cpu_ptr = dp12_map_memory(port, &pitch);
// ... access memory, respecting pitch alignment (256-byte) ...
dp12_unmap_memory(port);
```

### Cleanup
```c
dp12_close(port);  // or dp11_close(port)
dp12_shutdown();   // or dp11_shutdown()
```

## API Reference

### Lifecycle
| Function | Description |
|----------|-------------|
| `bool dp12_init(void)` | Initialize global D3D12 subsystem. Call once per process. |
| `bool dp11_init(void)` | Initialize global D3D11 subsystem + D3D12 NT resolver. |
| `void dp12_shutdown(void)` / `dp11_shutdown(void)` | Tear down subsystems. |

### Resource Management
| Function | Description |
|----------|-------------|
| `DP_HANDLE dp12_create_shared_resource(...)` | Create shared D3D12 resource with NT handle names. |
| `DP_HANDLE dp11_create_shared_resource(...)` | Create shared D3D11 resource with NT handle names. |
| `DP_HANDLE dp12_open_shared_resource(...)` | Open existing resource by NT name (D3D12). |
| `DP_HANDLE dp11_open_shared_resource(...)` | Open existing resource by NT name (D3D11). |
| `void dp12_close(DP_HANDLE)` / `dp11_close(DP_HANDLE)` | Release connection resources. |

### Synchronization
| Function | Description |
|----------|-------------|
| `void dp12_signal_fence(DP_HANDLE, uint64_t)` | Signal fence on GPU command stream. |
| `void dp11_signal_fence(DP_HANDLE, uint64_t)` | Signal fence via D3D11 context. |
| `void dp12_queue_wait(DP_HANDLE, ID3D12CommandQueue*, uint64_t)` | GPU hardware queue wait. CPU returns immediately. Use for all pipeline synchronization. |
| `void dp12_cpu_wait(DP_HANDLE, uint64_t)` | CPU-block until fence completes via OS scheduler (1–15ms). Use only for final readback where CPU access is required. |
| `void dp11_wait_fence(DP_HANDLE, uint64_t)` | GPU hardware queue wait via D3D11 context. Non-blocking CPU. |
| `uint64_t dp12_get_completed_value(DP_HANDLE)` | Query latest completed fence value (non-blocking). |

### Memory Access (D3D12 only)
| Function | Description |
|----------|-------------|
| `void* dp12_map_memory(DP_HANDLE, uint32_t* pitch)` | Map to CPU address. Valid only if `is_system_ram=true`. |
| `void dp12_unmap_memory(DP_HANDLE)` | Unmap CPU memory. |

### Interop Helpers
| Function | Description |
|----------|-------------|
| `void* dp12_get_resource_handle(DP_HANDLE)` | Get raw NT handle for external interop (Vulkan, OpenGL, etc.). |
| `void* dp12_get_fence_handle(DP_HANDLE)` | Get raw NT handle for fence. |

### Data Formats
```c
typedef enum {
    DP_FORMAT_VIDEO     = 0,  // DXGI_FORMAT_B8G8R8A8_UNORM
    DP_FORMAT_FLOAT     = 1,  // DXGI_FORMAT_R32_FLOAT
    DP_FORMAT_HALF      = 2,  // DXGI_FORMAT_R16_FLOAT
    DP_FORMAT_RAW_32BIT = 3   // DXGI_FORMAT_R32_UINT
} DP_FORMAT;
```

## Architecture Notes

**Singleton Device**: The library maintains one global D3D11/D3D12 device per process. Call `*_init()` once at process start; do not create additional devices for DirectPort operations.

**NT Handle Resolution**: D3D11 lacks native support for named shared handles. The D3D11 implementation maintains a minimal D3D12 device internally solely for `OpenSharedHandleByName` resolution. This is transparent to the caller.

**CPU Access**: Setting `is_system_ram=true` in `dp12_create_shared_resource` enables `dp12_map_memory`. This uses a CUSTOM heap with write-combine memory and row-major layout. Row pitch is aligned to 256 bytes per D3D12 requirements. For GPU-only access (recommended for performance), set `is_system_ram=false` and access via SRV/UAV.

**Security**: Shared handles use a permissive security descriptor (`D:P(A;;GA;;;AU)`) allowing any authenticated local process to connect. Adjust the SDDL string in `CreateSharedHandle` calls for production deployments.

**Adapter Pattern**: Domain-specific logic (video conversion, ML tensor binding, OpenGL interop) is intentionally excluded from the core. Adapters consume the `dp12_*`/`dp11_*` APIs to handle format translation, synchronization semantics, and framework integration. Reference implementations exist externally; the core remains minimal and agnostic.

## License

MIT
