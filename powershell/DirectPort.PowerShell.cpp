// DirectPort.PowerShell -- .NET 11 / PowerShell managed D3D12 seam.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <intrin.h>
#include <vcclr.h>

#include "directport.h"
#include "DirectPort.Shader.Native.h"
#include "DirectPort.Console.Native.h"

using namespace System;
using namespace System::Runtime::InteropServices;

#pragma managed(push, off)
static void dp12_publish_store_fence() {
    _mm_sfence();
}
#pragma managed(pop)

namespace DirectPort::PowerShell {

    public enum class PixelFormat : int {
        Bgra8   = DP_FORMAT_VIDEO,
        Float32 = DP_FORMAT_FLOAT,
        Float16 = DP_FORMAT_HALF,
        UInt32  = DP_FORMAT_RAW_32BIT
    };

    public enum class MemoryLayout : int {
        // CPU-visible L0 allocation. The D3D12 texture is ROW_MAJOR and every
        // destination row begins on a D3D12_TEXTURE_DATA_PITCH_ALIGNMENT boundary.
        CpuRowMajor = 0,

        // Device-local texture with an opaque driver-selected layout. CPU data
        // requires an upload/copy operation before it can be consumed.
        GpuOptimal = 1
    };

    public ref class Device abstract sealed {
    public:
        static bool Initialize() {
            return dp12_init();
        }

        static void Shutdown() {
            dp12_shutdown();
        }

        static property bool IsUma {
            bool get() { return dp12_is_uma(); }
        }

        static property Int64 AdapterLuid {
            Int64 get() {
                int64_t luid = 0;
                if (!dp12_get_adapter_luid(&luid))
                    throw gcnew InvalidOperationException("The D3D12 device is not initialized.");
                return luid;
            }
        }

        static property int LastHResult {
            int get() { return dp12_last_hresult(); }
        }
    };

    public ref class SharedResource sealed : IDisposable {
    private:
        IntPtr _native;
        IntPtr _mapped;
        UInt32 _width;
        UInt32 _height;
        UInt32 _rowPitch;
        UInt32 _bytesPerPixel;
        PixelFormat _format;
        MemoryLayout _layout;
        String^ _textureName;
        String^ _fenceName;
        bool _disposed;

        DP_HANDLE Handle() {
            if (_disposed || _native == IntPtr::Zero)
                throw gcnew ObjectDisposedException("SharedResource");
            return _native.ToPointer();
        }

        void EnsureMapped() {
            if (_layout != MemoryLayout::CpuRowMajor)
                throw gcnew InvalidOperationException("GpuOptimal resources are not CPU-mappable.");
            if (_mapped != IntPtr::Zero)
                return;

            uint32_t pitch = 0;
            void* pointer = dp12_map_memory(Handle(), &pitch);
            if (!pointer)
                throw gcnew InvalidOperationException("ID3D12Resource::Map returned null.");
            if ((pitch & 255u) != 0)
                throw gcnew InvalidOperationException("DirectPort returned a row pitch that is not 256-byte aligned.");

            _mapped = IntPtr(pointer);
            _rowPitch = pitch;
        }

    public:
        SharedResource(
            UInt32 width,
            UInt32 height,
            PixelFormat format,
            MemoryLayout layout,
            String^ textureName,
            String^ fenceName)
            : _native(IntPtr::Zero), _mapped(IntPtr::Zero), _width(width),
              _height(height), _rowPitch(0), _format(format), _layout(layout),
              _textureName(textureName), _fenceName(fenceName), _disposed(false) {

            if (width == 0 || height == 0)
                throw gcnew ArgumentOutOfRangeException("width", "Width and height must be non-zero.");
            if (String::IsNullOrWhiteSpace(textureName))
                throw gcnew ArgumentException("A shared texture name is required.", "textureName");
            if (String::IsNullOrWhiteSpace(fenceName))
                throw gcnew ArgumentException("A shared fence name is required.", "fenceName");

            _bytesPerPixel = (format == PixelFormat::Float16) ? 2u : 4u;

            if (!dp12_init())
                throw gcnew InvalidOperationException("D3D12CreateDevice failed.");

            pin_ptr<const wchar_t> textureChars = PtrToStringChars(textureName);
            pin_ptr<const wchar_t> fenceChars = PtrToStringChars(fenceName);
            DP_HANDLE native = dp12_create_shared_resource(
                width,
                height,
                static_cast<DP_FORMAT>(format),
                layout == MemoryLayout::CpuRowMajor,
                textureChars,
                fenceChars);

            if (!native) {
                int hr = dp12_last_hresult();
                throw gcnew COMException(
                    String::Format("D3D12 shared-resource creation failed (HRESULT 0x{0:X8}).", hr),
                    hr);
            }

            _native = IntPtr(native);
            if (layout == MemoryLayout::CpuRowMajor)
                EnsureMapped();
        }

        ~SharedResource() {
            this->!SharedResource();
            GC::SuppressFinalize(this);
        }

        !SharedResource() {
            if (_disposed)
                return;
            if (_native != IntPtr::Zero) {
                if (_mapped != IntPtr::Zero)
                    dp12_unmap_memory(_native.ToPointer());
                dp12_close(_native.ToPointer());
            }
            _mapped = IntPtr::Zero;
            _native = IntPtr::Zero;
            _disposed = true;
        }

        property UInt32 Width { UInt32 get() { return _width; } }
        property UInt32 Height { UInt32 get() { return _height; } }
        property UInt32 BytesPerPixel { UInt32 get() { return _bytesPerPixel; } }
        property UInt32 RowPitch {
            UInt32 get() {
                if (_layout == MemoryLayout::CpuRowMajor)
                    EnsureMapped();
                return _rowPitch;
            }
        }
        property PixelFormat Format { PixelFormat get() { return _format; } }
        property MemoryLayout Layout { MemoryLayout get() { return _layout; } }
        property String^ TextureName { String^ get() { return _textureName; } }
        property String^ FenceName { String^ get() { return _fenceName; } }
        property IntPtr NativeHandle { IntPtr get() { Handle(); return _native; } }
        property IntPtr MappedAddress {
            IntPtr get() { EnsureMapped(); return _mapped; }
        }
        property IntPtr SharedTextureHandle {
            IntPtr get() { return IntPtr(dp12_get_resource_handle(Handle())); }
        }
        property IntPtr SharedFenceHandle {
            IntPtr get() { return IntPtr(dp12_get_fence_handle(Handle())); }
        }
        property UInt64 CompletedValue {
            UInt64 get() { return dp12_get_completed_value(Handle()); }
        }

        // Copies tightly-packed rows into the mapped row-major texture while
        // respecting its 256-byte destination pitch. This method does not signal.
        void WriteRows(array<Byte>^ source, UInt32 sourceRowPitch, UInt32 rowCount) {
            if (source == nullptr)
                throw gcnew ArgumentNullException("source");
            EnsureMapped();
            if (rowCount > _height)
                throw gcnew ArgumentOutOfRangeException("rowCount");

            UInt32 rowBytes = _width * _bytesPerPixel;
            if (sourceRowPitch < rowBytes)
                throw gcnew ArgumentOutOfRangeException("sourceRowPitch", "Source pitch is smaller than one logical row.");
            UInt64 required = static_cast<UInt64>(sourceRowPitch) * rowCount;
            if (required > static_cast<UInt64>(source->LongLength))
                throw gcnew ArgumentException("The source array does not contain every requested row.", "source");

            pin_ptr<Byte> src = &source[0];
            Byte* dst = static_cast<Byte*>(_mapped.ToPointer());
            for (UInt32 y = 0; y < rowCount; ++y)
                memcpy(dst + static_cast<size_t>(y) * _rowPitch,
                       src + static_cast<size_t>(y) * sourceRowPitch,
                       rowBytes);

            // The mapped heap is normally WRITE_COMBINE. Complete those stores
            // before the caller publishes a fence value.
            dp12_publish_store_fence();
        }

        // Fast path for the canvas: one UInt32 per logical cell.
        void WriteCells(array<UInt32>^ cells) {
            if (_format != PixelFormat::UInt32)
                throw gcnew InvalidOperationException("WriteCells requires an UInt32 resource.");
            if (cells == nullptr)
                throw gcnew ArgumentNullException("cells");
            UInt64 required = static_cast<UInt64>(_width) * _height;
            if (static_cast<UInt64>(cells->LongLength) < required)
                throw gcnew ArgumentException("The cell array is smaller than width * height.", "cells");

            EnsureMapped();
            pin_ptr<UInt32> src = &cells[0];
            Byte* dst = static_cast<Byte*>(_mapped.ToPointer());
            size_t rowBytes = static_cast<size_t>(_width) * sizeof(UInt32);
            for (UInt32 y = 0; y < _height; ++y)
                memcpy(dst + static_cast<size_t>(y) * _rowPitch,
                       src + static_cast<size_t>(y) * _width,
                       rowBytes);
            dp12_publish_store_fence();
        }

        void Signal(UInt64 value) {
            dp12_signal_fence(Handle(), value);
        }

        void PublishCells(array<UInt32>^ cells, UInt64 value) {
            WriteCells(cells);
            Signal(value);
        }

        // Explicitly blocking by design; never used by PublishCells.
        void CpuWait(UInt64 value) {
            dp12_cpu_wait(Handle(), value);
        }
    };

    // Optional general GPU node. PowerShell supplies the complete HLSL program
    // and constant payload; the node only owns scheduling and publication.
    public ref class FullscreenShaderProducer sealed : IDisposable {
    private:
        IntPtr _native;
        UInt32 _width;
        UInt32 _height;
        UInt32 _constantCapacity;
        PixelFormat _format;
        String^ _textureName;
        String^ _fenceName;
        bool _disposed;

        DPS_HANDLE Handle() {
            if (_disposed || _native == IntPtr::Zero)
                throw gcnew ObjectDisposedException("FullscreenShaderProducer");
            return _native.ToPointer();
        }

    public:
        FullscreenShaderProducer(
            UInt32 width,
            UInt32 height,
            PixelFormat format,
            String^ textureName,
            String^ fenceName,
            String^ hlsl,
            String^ vertexEntry,
            String^ pixelEntry,
            UInt32 constantCapacity)
            : _native(IntPtr::Zero), _width(width), _height(height),
              _constantCapacity(constantCapacity), _format(format),
              _textureName(textureName), _fenceName(fenceName), _disposed(false) {

            if (width == 0 || height == 0)
                throw gcnew ArgumentOutOfRangeException("width", "Width and height must be non-zero.");
            if (String::IsNullOrWhiteSpace(textureName) || String::IsNullOrWhiteSpace(fenceName))
                throw gcnew ArgumentException("Shared texture and fence names are required.");
            if (String::IsNullOrWhiteSpace(hlsl))
                throw gcnew ArgumentException("HLSL source is required.", "hlsl");
            if (String::IsNullOrWhiteSpace(vertexEntry) || String::IsNullOrWhiteSpace(pixelEntry))
                throw gcnew ArgumentException("Vertex and pixel entry points are required.");

            IntPtr textureChars = Marshal::StringToHGlobalUni(textureName);
            IntPtr fenceChars = Marshal::StringToHGlobalUni(fenceName);
            IntPtr hlslChars = Marshal::StringToHGlobalAnsi(hlsl);
            IntPtr vertexChars = Marshal::StringToHGlobalAnsi(vertexEntry);
            IntPtr pixelChars = Marshal::StringToHGlobalAnsi(pixelEntry);

            DPS_HANDLE native = dps_create(
                width,
                height,
                static_cast<int32_t>(format),
                static_cast<const wchar_t*>(textureChars.ToPointer()),
                static_cast<const wchar_t*>(fenceChars.ToPointer()),
                static_cast<const char*>(hlslChars.ToPointer()),
                static_cast<const char*>(vertexChars.ToPointer()),
                static_cast<const char*>(pixelChars.ToPointer()),
                constantCapacity);

            Marshal::FreeHGlobal(textureChars);
            Marshal::FreeHGlobal(fenceChars);
            Marshal::FreeHGlobal(hlslChars);
            Marshal::FreeHGlobal(vertexChars);
            Marshal::FreeHGlobal(pixelChars);

            if (!native)
                throw gcnew InvalidOperationException(gcnew String(dps_last_error()));
            _native = IntPtr(native);
        }

        ~FullscreenShaderProducer() {
            this->!FullscreenShaderProducer();
            GC::SuppressFinalize(this);
        }

        !FullscreenShaderProducer() {
            if (_disposed)
                return;
            if (_native != IntPtr::Zero)
                dps_close(_native.ToPointer());
            _native = IntPtr::Zero;
            _disposed = true;
        }

        property UInt32 Width { UInt32 get() { return _width; } }
        property UInt32 Height { UInt32 get() { return _height; } }
        property UInt32 ConstantCapacity { UInt32 get() { return _constantCapacity; } }
        property PixelFormat Format { PixelFormat get() { return _format; } }
        property String^ TextureName { String^ get() { return _textureName; } }
        property String^ FenceName { String^ get() { return _fenceName; } }
        property IntPtr SharedTextureHandle {
            IntPtr get() { return IntPtr(dps_get_texture_handle(Handle())); }
        }
        property IntPtr SharedFenceHandle {
            IntPtr get() { return IntPtr(dps_get_fence_handle(Handle())); }
        }
        property IntPtr DevicePointer {
            IntPtr get() { return IntPtr(dps_get_device_ptr(Handle())); }
        }
        property IntPtr ResourcePointer {
            IntPtr get() { return IntPtr(dps_get_resource_ptr(Handle())); }
        }
        property Int64 AdapterLuid {
            Int64 get() { return dps_get_adapter_luid(Handle()); }
        }
        property UInt64 CompletedValue {
            UInt64 get() { return dps_get_completed_value(Handle()); }
        }

        // False means all three GPU frame slots are busy. It never CPU-waits.
        bool TryDraw(array<Byte>^ constants, UInt64 publishValue) {
            if (constants == nullptr || constants->Length == 0)
                return dps_try_draw(Handle(), nullptr, 0, publishValue);
            if (static_cast<UInt32>(constants->Length) > _constantCapacity)
                throw gcnew ArgumentException("Constant payload exceeds ConstantCapacity.", "constants");
            pin_ptr<Byte> bytes = &constants[0];
            return dps_try_draw(Handle(), bytes, constants->Length, publishValue);
        }

        bool TryDrawFloats(array<Single>^ constants, UInt64 publishValue) {
            if (constants == nullptr || constants->Length == 0)
                return dps_try_draw(Handle(), nullptr, 0, publishValue);
            UInt32 byteCount = static_cast<UInt32>(constants->Length * sizeof(float));
            if (byteCount > _constantCapacity)
                throw gcnew ArgumentException("Constant payload exceeds ConstantCapacity.", "constants");
            pin_ptr<Single> values = &constants[0];
            return dps_try_draw(Handle(), values, byteCount, publishValue);
        }
    };

    public value struct ConsoleWindowState {
        bool Alive;
        Int32 Width;
        Int32 Height;
        Int32 MouseX;
        Int32 MouseY;
        bool LeftDown;
        Int32 WheelDelta;
        Int32 KeyCode;
        UInt64 ResizeSerial;
    };

    // Packed-cell GPU console. PowerShell owns the cells and cadence; this
    // object owns only the D3D12/MSDF draw, shared texture, and fence signal.
    public ref class GpuConsole sealed : IDisposable {
    private:
        IntPtr _native;
        UInt32 _width;
        UInt32 _height;
        UInt32 _maxCells;
        String^ _textureName;
        String^ _fenceName;
        bool _disposed;

        DPC_HANDLE Handle() {
            if (_disposed || _native == IntPtr::Zero)
                throw gcnew ObjectDisposedException("GpuConsole");
            return _native.ToPointer();
        }

    public:
        GpuConsole(
            UInt32 width,
            UInt32 height,
            UInt32 maxCells,
            String^ textureName,
            String^ fenceName,
            String^ atlasPng,
            String^ metricsJson)
            : _native(IntPtr::Zero), _width(width), _height(height), _maxCells(maxCells),
              _textureName(textureName), _fenceName(fenceName), _disposed(false) {
            if (width == 0 || height == 0 || maxCells == 0)
                throw gcnew ArgumentOutOfRangeException("width", "Width, height, and maxCells must be non-zero.");
            if (String::IsNullOrWhiteSpace(textureName) || String::IsNullOrWhiteSpace(fenceName))
                throw gcnew ArgumentException("Shared texture and fence names are required.");
            if (String::IsNullOrWhiteSpace(atlasPng) || String::IsNullOrWhiteSpace(metricsJson))
                throw gcnew ArgumentException("MSDF atlas PNG and metrics JSON paths are required.");

            IntPtr textureChars = Marshal::StringToHGlobalUni(textureName);
            IntPtr fenceChars = Marshal::StringToHGlobalUni(fenceName);
            IntPtr atlasChars = Marshal::StringToHGlobalUni(atlasPng);
            IntPtr metricsChars = Marshal::StringToHGlobalUni(metricsJson);
            DPC_HANDLE native = dpc_create(
                width, height, maxCells,
                static_cast<const wchar_t*>(textureChars.ToPointer()),
                static_cast<const wchar_t*>(fenceChars.ToPointer()),
                static_cast<const wchar_t*>(atlasChars.ToPointer()),
                static_cast<const wchar_t*>(metricsChars.ToPointer()));
            Marshal::FreeHGlobal(textureChars);
            Marshal::FreeHGlobal(fenceChars);
            Marshal::FreeHGlobal(atlasChars);
            Marshal::FreeHGlobal(metricsChars);
            if (!native)
                throw gcnew InvalidOperationException(gcnew String(dpc_last_error()));
            _native = IntPtr(native);
        }

        ~GpuConsole() {
            this->!GpuConsole();
            GC::SuppressFinalize(this);
        }

        !GpuConsole() {
            if (_disposed) return;
            if (_native != IntPtr::Zero) dpc_close(_native.ToPointer());
            _native = IntPtr::Zero;
            _disposed = true;
        }

        property UInt32 Width { UInt32 get() { return _width; } }
        property UInt32 Height { UInt32 get() { return _height; } }
        property UInt32 MaxCells { UInt32 get() { return _maxCells; } }
        property String^ TextureName { String^ get() { return _textureName; } }
        property String^ FenceName { String^ get() { return _fenceName; } }
        property IntPtr SharedTextureHandle { IntPtr get() { return IntPtr(dpc_get_texture_handle(Handle())); } }
        property IntPtr SharedFenceHandle { IntPtr get() { return IntPtr(dpc_get_fence_handle(Handle())); } }
        property IntPtr DevicePointer { IntPtr get() { return IntPtr(dpc_get_device_ptr(Handle())); } }
        property IntPtr ResourcePointer { IntPtr get() { return IntPtr(dpc_get_resource_ptr(Handle())); } }
        property Int64 AdapterLuid { Int64 get() { return dpc_get_adapter_luid(Handle()); } }
        property UInt64 CompletedValue { UInt64 get() { return dpc_get_completed_value(Handle()); } }
        property UInt64 DroppedFrames { UInt64 get() { return dpc_get_dropped_frames(Handle()); } }

        void Show(String^ title, UInt32 width, UInt32 height) {
            if (String::IsNullOrWhiteSpace(title)) title = "DirectPort Console";
            IntPtr chars = Marshal::StringToHGlobalUni(title);
            const bool shown = dpc_show_window(Handle(), static_cast<const wchar_t*>(chars.ToPointer()), width, height);
            Marshal::FreeHGlobal(chars);
            if (!shown) throw gcnew InvalidOperationException(gcnew String(dpc_last_error()));
        }

        ConsoleWindowState Pump(UInt32 waitMilliseconds) {
            DPC_WINDOW_STATE native = {};
            dpc_pump_window(Handle(), waitMilliseconds, &native);
            ConsoleWindowState managed;
            managed.Alive = native.alive != 0;
            managed.Width = native.width;
            managed.Height = native.height;
            managed.MouseX = native.mouse_x;
            managed.MouseY = native.mouse_y;
            managed.LeftDown = native.left_down != 0;
            managed.WheelDelta = native.wheel_delta;
            managed.KeyCode = native.key_code;
            managed.ResizeSerial = native.resize_serial;
            return managed;
        }

        void EnableManifest(String^ manifestName) {
            if (String::IsNullOrWhiteSpace(manifestName))
                throw gcnew ArgumentException("Manifest name is required.", "manifestName");
            IntPtr chars = Marshal::StringToHGlobalUni(manifestName);
            const bool enabled = dpc_enable_manifest(Handle(), static_cast<const wchar_t*>(chars.ToPointer()));
            Marshal::FreeHGlobal(chars);
            if (!enabled) throw gcnew InvalidOperationException(gcnew String(dpc_last_error()));
        }

        // False means the next one of three upload slots is still in flight.
        // No wait, allocation, conversion, or per-cell managed call occurs here.
        bool TryPresent(array<UInt32>^ cells, UInt32 columns, UInt32 rows, Single timeSeconds, UInt64 publishValue) {
            if (cells == nullptr) throw gcnew ArgumentNullException("cells");
            if (columns == 0 || rows == 0 || static_cast<UInt64>(columns) * rows != static_cast<UInt64>(cells->LongLength))
                throw gcnew ArgumentException("cells.Length must equal columns * rows.", "cells");
            if (cells->LongLength > _maxCells)
                throw gcnew ArgumentException("Cell payload exceeds MaxCells.", "cells");
            pin_ptr<UInt32> packed = &cells[0];
            const bool submitted = dpc_try_present(Handle(), packed, cells->Length, columns, rows, timeSeconds, publishValue);
            if (!submitted && dpc_last_error()[0])
                throw gcnew InvalidOperationException(gcnew String(dpc_last_error()));
            return submitted;
        }
    };
}
