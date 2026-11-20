// SPDX-License-Identifier: MIT
// Identify the executing DXGI adapter, submit a shader, wait and check its output.
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

constexpr UINT elements = 509;
constexpr UINT bytes = elements * sizeof(uint32_t);
constexpr DWORD timeout_ms = 15000;
constexpr uint32_t seeds[] = {0x19683cau, 0xc35f9ab1u, 0x1fc0137u};
constexpr char shader_source[] = R"(
cbuffer Params : register(b0) { uint count; uint seed; }
RWByteAddressBuffer outputBuffer : register(u0);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x < count)
        outputBuffer.Store(4 * id.x, (id.x * 1664525u + seed) ^ (seed >> (id.x % 13)));
}
)";

HRESULT check(HRESULT hr, const char *operation)
{
    if (FAILED(hr))
        std::fprintf(stderr, "%s: HRESULT 0x%08lx\n", operation, static_cast<unsigned long>(hr));
    return hr;
}

#define TRY(x) do { HRESULT result_ = check((x), #x); if (FAILED(result_)) return result_; } while (0)

bool equal_luid(LUID a, LUID b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

bool matches(const uint32_t *data, uint32_t seed)
{
    for (UINT i = 0; i < elements; ++i) {
        uint32_t expected = (i * 1664525u + seed) ^ (seed >> (i % 13));
        if (data[i] != expected) {
            std::fprintf(stderr, "word %u: expected %08x, received %08x\n", i, expected, data[i]);
            return false;
        }
    }
    return true;
}

HRESULT compile(ComPtr<ID3DBlob> &code)
{
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(shader_source, sizeof(shader_source) - 1, "readback", nullptr, nullptr,
                            "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr) && errors)
        std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    return check(hr, "D3DCompile");
}

HRESULT run11(IDXGIAdapter1 *adapter, LUID wanted, ID3DBlob *code)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
    TRY(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 1,
                          D3D11_SDK_VERSION, &device, nullptr, &context));
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> actual;
    DXGI_ADAPTER_DESC identity{};
    TRY(device.As(&dxgi));
    TRY(dxgi->GetAdapter(&actual));
    TRY(actual->GetDesc(&identity));
    if (!equal_luid(identity.AdapterLuid, wanted))
        return check(E_UNEXPECTED, "D3D11 selected a different adapter");

    ComPtr<ID3D11ComputeShader> shader;
    TRY(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = bytes;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    ComPtr<ID3D11Buffer> gpu, readback, constants;
    TRY(device->CreateBuffer(&desc, nullptr, &gpu));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    TRY(device->CreateBuffer(&desc, nullptr, &readback));
    desc = {};
    desc.ByteWidth = 16;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    TRY(device->CreateBuffer(&desc, nullptr, &constants));
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = elements;
    uav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    ComPtr<ID3D11UnorderedAccessView> view;
    TRY(device->CreateUnorderedAccessView(gpu.Get(), &uav, &view));
    D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query;
    TRY(device->CreateQuery(&query_desc, &query));

    for (uint32_t seed : seeds) {
        const uint32_t params[]{elements, seed, 0, 0};
        context->UpdateSubresource(constants.Get(), 0, nullptr, params, 0, 0);
        context->CSSetShader(shader.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->CSSetUnorderedAccessViews(0, 1, view.GetAddressOf(), nullptr);
        context->Dispatch((elements + 63) / 64, 1, 1);
        ID3D11UnorderedAccessView *empty = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &empty, nullptr);
        context->CopyResource(readback.Get(), gpu.Get());
        context->End(query.Get());
        context->Flush();
        ULONGLONG start = GetTickCount64();
        for (;;) {
            BOOL completed = FALSE;
            HRESULT hr = context->GetData(query.Get(), &completed, sizeof(completed),
                                          D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (FAILED(hr))
                return check(hr, "D3D11 GetData");
            if (hr == S_OK && completed)
                break;
            if (GetTickCount64() - start >= timeout_ms)
                return check(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "D3D11 queue completion");
            Sleep(1);
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        TRY(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        bool ok = matches(static_cast<const uint32_t *>(mapped.pData), seed);
        context->Unmap(readback.Get(), 0);
        if (!ok)
            return E_FAIL;
    }
    return check(device->GetDeviceRemovedReason(), "D3D11 device status");
}

HRESULT make_buffer(ID3D12Device *device, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                    D3D12_RESOURCE_FLAGS flags, ComPtr<ID3D12Resource> &resource)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                            IID_PPV_ARGS(resource.GetAddressOf()));
}

HRESULT run12(IDXGIAdapter1 *adapter, LUID wanted, ID3DBlob *code)
{
    ComPtr<ID3D12Device> device;
    TRY(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    if (!equal_luid(device->GetAdapterLuid(), wanted))
        return check(E_UNEXPECTED, "D3D12 selected a different adapter");
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 2;
    D3D12_ROOT_SIGNATURE_DESC signature{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized;
    TRY(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr));
    ComPtr<ID3D12RootSignature> root;
    TRY(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                    IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root.Get();
    pipeline_desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pipeline;
    TRY(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)));
    ComPtr<ID3D12Resource> gpu, readback;
    TRY(make_buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, gpu));
    TRY(make_buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_FLAG_NONE, readback));
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    TRY(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    TRY(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
    TRY(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), pipeline.Get(),
                                  IID_PPV_ARGS(&commands)));
    TRY(commands->Close());
    TRY(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    struct Event {
        HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ~Event() { if (handle) CloseHandle(handle); }
    } event;
    if (!event.handle)
        return check(HRESULT_FROM_WIN32(GetLastError()), "D3D12 fence event");

    UINT64 value = 0;
    for (uint32_t seed : seeds) {
        TRY(allocator->Reset());
        TRY(commands->Reset(allocator.Get(), pipeline.Get()));
        commands->SetComputeRootSignature(root.Get());
        commands->SetComputeRootUnorderedAccessView(0, gpu->GetGPUVirtualAddress());
        const uint32_t constants[]{elements, seed};
        commands->SetComputeRoot32BitConstants(1, 2, constants, 0);
        commands->Dispatch((elements + 63) / 64, 1, 1);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {gpu.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
        commands->ResourceBarrier(1, &barrier);
        commands->CopyBufferRegion(readback.Get(), 0, gpu.Get(), 0, bytes);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        commands->ResourceBarrier(1, &barrier);
        TRY(commands->Close());
        ID3D12CommandList *submitted = commands.Get();
        queue->ExecuteCommandLists(1, &submitted);
        TRY(queue->Signal(fence.Get(), ++value));
        TRY(fence->SetEventOnCompletion(value, event.handle));
        DWORD wait = WaitForSingleObject(event.handle, timeout_ms);
        if (wait != WAIT_OBJECT_0)
            return check(HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError()),
                         "D3D12 queue completion");
        TRY(device->GetDeviceRemovedReason());
        if (fence->GetCompletedValue() < value)
            return check(E_FAIL, "D3D12 fence value");
        const D3D12_RANGE reading{0, bytes}, not_written{0, 0};
        void *mapped = nullptr;
        TRY(readback->Map(0, &reading, &mapped));
        bool ok = matches(static_cast<const uint32_t *>(mapped), seed);
        readback->Unmap(0, &not_written);
        if (!ok)
            return E_FAIL;
    }
    return S_OK;
}

int main(int argc, char **argv)
{
    bool list = false, warp = false, use11 = true, use12 = true;
    const char *requested = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--list")) list = true;
        else if (!std::strcmp(argv[i], "--warp")) warp = true;
        else if (!std::strcmp(argv[i], "--luid") && i + 1 < argc) requested = argv[++i];
        else if (!std::strcmp(argv[i], "--api") && i + 1 < argc) {
            ++i;
            use11 = !std::strcmp(argv[i], "11");
            use12 = !std::strcmp(argv[i], "12");
            if (!use11 && !use12) return 2;
        } else {
            std::fprintf(stderr, "Usage: graphics-readback [--list | --warp | --luid HIGH:LOW] [--api 11|12]\n");
            return 2;
        }
    }
    if (warp && requested) return 2;
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1"))) return 1;
    ComPtr<IDXGIAdapter1> selected;
    DXGI_ADAPTER_DESC1 desc{};
    unsigned candidates = 0;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT hr = factory->EnumAdapters1(index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(check(hr, "EnumAdapters1"))) return 1;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(check(adapter->GetDesc1(&d), "GetDesc1"))) return 1;
        char luid[18];
        std::snprintf(luid, sizeof(luid), "%08lx:%08lx",
                      static_cast<unsigned long>(d.AdapterLuid.HighPart), d.AdapterLuid.LowPart);
        // The primary Microsoft Basic Render Driver may omit the SOFTWARE flag.
        bool basic_render = d.VendorId == 0x1414 && d.DeviceId == 0x008c;
        bool hardware = !basic_render && !(d.Flags & (DXGI_ADAPTER_FLAG_SOFTWARE | DXGI_ADAPTER_FLAG_REMOTE));
        std::printf("%s %04x:%04x %s %ls\n", luid, d.VendorId, d.DeviceId,
                    hardware ? "hardware" : "software/remote", d.Description);
        if (hardware && (!requested || !_stricmp(requested, luid))) {
            ++candidates;
            selected = adapter;
            desc = d;
        }
    }
    if (list) return 0;
    if (warp) {
        selected.Reset();
        if (FAILED(check(factory->EnumWarpAdapter(IID_PPV_ARGS(&selected)), "EnumWarpAdapter")) ||
            FAILED(check(selected->GetDesc1(&desc), "GetDesc1(WARP)")) ||
            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return 1;
    } else if (candidates != 1) {
        std::fprintf(stderr, "Matching local hardware adapters: %u; select one with --luid.\n", candidates);
        return 1;
    }
    ComPtr<ID3DBlob> code;
    if (FAILED(compile(code))) return 1;
    std::printf("Test adapter: %ls, LUID %08lx:%08lx, %s\n", desc.Description,
                static_cast<unsigned long>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart,
                warp ? "WARP CONTROL (not hardware)" : "hardware");
    bool passed = true;
    if (use11) {
        bool ok = SUCCEEDED(run11(selected.Get(), desc.AdapterLuid, code.Get()));
        std::printf("D3D11 shader/event/readback: %s\n", ok ? "PASS" : "FAIL");
        passed &= ok;
    }
    if (use12) {
        bool ok = SUCCEEDED(run12(selected.Get(), desc.AdapterLuid, code.Get()));
        std::printf("D3D12 shader/fence/readback: %s\n", ok ? "PASS" : "FAIL");
        passed &= ok;
    }
    return passed ? 0 : 1;
}
