// --- directport.h ---
// DirectPort Interprocess GPU Memory Protocol
// Version 6.0 - Singleton Device Architecture (Global Device Initialization)
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DP_EXPORT __declspec(dllexport)

// Data Format Enum (Media Agnostic)
// Defines memory layout only. Semantic meaning is determined by the Producer/Consumer.
typedef enum {
    DP_FORMAT_VIDEO     = 0, // DXGI_FORMAT_B8G8R8A8_UNORM (4 bytes per unit)
    DP_FORMAT_FLOAT     = 1, // DXGI_FORMAT_R32_FLOAT      (4 bytes per unit)
    DP_FORMAT_HALF      = 2, // DXGI_FORMAT_R16_FLOAT      (2 bytes per unit)
    DP_FORMAT_RAW_32BIT = 3  // DXGI_FORMAT_R32_UINT       (4 bytes per unit)
} DP_FORMAT;

typedef void* DP_HANDLE;

// ============================================================================
// D3D12 API (Primary Implementation)
// ============================================================================

// Initializes the DirectPort D3D12 Subsystem (Global Singleton State).
// MUST be called once per process before creating/opening handles.
DP_EXPORT bool      dp12_init(void);

// Tears down the global D3D12 subsystem on application exit.
DP_EXPORT void      dp12_shutdown(void);

// Creates a shared GPU resource with an NT Handle.
DP_EXPORT DP_HANDLE dp12_create_shared_resource(uint32_t width, uint32_t height, DP_FORMAT format, bool is_system_ram, const wchar_t* tex_name, const wchar_t* fence_name);

// Opens an existing shared GPU resource by NT Handle Name.
DP_EXPORT DP_HANDLE dp12_open_shared_resource(const wchar_t* tex_name, const wchar_t* fence_name);

// Maps resource memory to CPU address. Returns row_pitch aligned to 256 bytes.
DP_EXPORT void*     dp12_map_memory(DP_HANDLE handle, uint32_t* out_row_pitch);

// Unmaps resource memory.
DP_EXPORT void      dp12_unmap_memory(DP_HANDLE handle);

// Signals the DirectX Fence (GPU Async). Pushes frame availability.
DP_EXPORT void      dp12_signal_fence(DP_HANDLE handle, uint64_t frame_value);

// Waits for the DirectX Fence to complete (CPU Block).
DP_EXPORT void      dp12_wait_fence(DP_HANDLE handle, uint64_t target_value);

// Returns the current completed fence value.
DP_EXPORT uint64_t  dp12_get_completed_value(DP_HANDLE handle);

// Returns the underlying NT Handle for the resource (For external interop).
DP_EXPORT void*     dp12_get_resource_handle(DP_HANDLE handle);

// Returns the underlying NT Handle for the fence (For external interop).
DP_EXPORT void*     dp12_get_fence_handle(DP_HANDLE handle);

// Releases resources and closes NT Handles for this specific connection.
DP_EXPORT void      dp12_close(DP_HANDLE handle);


// ============================================================================
// D3D11 API (D3D11 Compatibility Layer)
// ============================================================================

// Initializes the DirectPort D3D11 Subsystem and the NT Name Resolver.
// MUST be called once per process before creating/opening D3D11 handles.
DP_EXPORT bool      dp11_init(void);

// Tears down the global D3D11 subsystem on application exit.
DP_EXPORT void      dp11_shutdown(void);

DP_EXPORT DP_HANDLE dp11_create_shared_resource(uint32_t width, uint32_t height, DP_FORMAT format, const wchar_t* tex_name, const wchar_t* fence_name);
DP_EXPORT DP_HANDLE dp11_open_shared_resource(const wchar_t* tex_name, const wchar_t* fence_name);

DP_EXPORT void      dp11_signal_fence(DP_HANDLE handle, uint64_t frame_value);
DP_EXPORT void      dp11_wait_fence(DP_HANDLE handle, uint64_t target_value);
DP_EXPORT void      dp11_close(DP_HANDLE handle);

#ifdef __cplusplus
}
#endif