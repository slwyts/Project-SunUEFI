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

HRESULT compile(const char *source, const char *entry, const char *profile, ComPtr<ID3DBlob> &code)
{
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(source, std::strlen(source), "readback", nullptr, nullptr,
                            entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(hr) && errors)
        std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
    return check(hr, "D3DCompile");
}

HRESULT create11(IDXGIAdapter1 *adapter, LUID wanted, D3D_FEATURE_LEVEL level,
                 ComPtr<ID3D11Device> &device, ComPtr<ID3D11DeviceContext> &context)
{
    TRY(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &level, 1,
                          D3D11_SDK_VERSION, &device, nullptr, &context));
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> actual;
    DXGI_ADAPTER_DESC identity{};
    TRY(device.As(&dxgi));
    TRY(dxgi->GetAdapter(&actual));
    TRY(actual->GetDesc(&identity));
    if (!equal_luid(identity.AdapterLuid, wanted))
        return check(E_UNEXPECTED, "D3D11 selected a different adapter");
    return S_OK;
}

HRESULT wait11(ID3D11DeviceContext *context, ID3D11Query *query)
{
    context->End(query);
    context->Flush();
    ULONGLONG start = GetTickCount64();
    for (;;) {
        BOOL completed = FALSE;
        HRESULT hr = context->GetData(query, &completed, sizeof(completed), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (FAILED(hr))
            return check(hr, "D3D11 GetData");
        if (hr == S_OK && completed)
            return S_OK;
        if (GetTickCount64() - start >= timeout_ms)
            return check(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "D3D11 queue completion");
        Sleep(1);
    }
}

HRESULT draw10(IDXGIAdapter1 *adapter, LUID wanted)
{
    constexpr UINT width = 31, height = 17;
    constexpr char source[] = R"(
        cbuffer Params : register(b0) { uint phase; }
        float4 vs(uint id : SV_VertexID) : SV_Position {
            float2 uv = float2((id << 1) & 2, id & 2);
            return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
        }
        float4 ps(float4 pos : SV_Position) : SV_Target {
            uint2 xy = uint2(pos.xy);
            return float4((xy.x ^ xy.y ^ phase) & 1, ((xy.x >> 1) ^ phase) & 1, (xy.y >> 1) & 1, 1);
        }
    )";
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    TRY(create11(adapter, wanted, D3D_FEATURE_LEVEL_10_0, device, context));
    ComPtr<ID3DBlob> vs_code, ps_code;
    TRY(compile(source, "vs", "vs_4_0", vs_code));
    TRY(compile(source, "ps", "ps_4_0", ps_code));
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    TRY(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
    TRY(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
    D3D11_TEXTURE2D_DESC tex{};
    tex.Width = width;
    tex.Height = height;
    tex.MipLevels = tex.ArraySize = tex.SampleDesc.Count = 1;
    tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, readback;
    TRY(device->CreateTexture2D(&tex, nullptr, &target));
    tex.Usage = D3D11_USAGE_STAGING;
    tex.BindFlags = 0;
    tex.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    TRY(device->CreateTexture2D(&tex, nullptr, &readback));
    ComPtr<ID3D11RenderTargetView> view;
    TRY(device->CreateRenderTargetView(target.Get(), nullptr, &view));
    D3D11_BUFFER_DESC buf{};
    buf.ByteWidth = 16;
    buf.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> params;
    TRY(device->CreateBuffer(&buf, nullptr, &params));
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> state;
    TRY(device->CreateRasterizerState(&raster, &state));
    D3D11_QUERY_DESC event{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query;
    TRY(device->CreateQuery(&event, &query));
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(state.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, params.GetAddressOf());
    for (UINT phase = 0; phase != 2; ++phase) {
        const UINT values[]{phase, 0, 0, 0};
        context->UpdateSubresource(params.Get(), 0, nullptr, values, 0, 0);
        context->OMSetRenderTargets(1, view.GetAddressOf(), nullptr);
        const float clear[]{0, 0, 0, 0};
        context->ClearRenderTargetView(view.Get(), clear);
        context->Draw(3, 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(readback.Get(), target.Get());
        TRY(wait11(context.Get(), query.Get()));
        D3D11_MAPPED_SUBRESOURCE mapped{};
        TRY(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        bool ok = mapped.RowPitch >= width * 4;
        for (UINT y = 0; ok && y < height; ++y) {
            const auto *row = static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch;
            for (UINT x = 0; x < width; ++x) {
                const uint8_t rgba[]{static_cast<uint8_t>(255 * ((x ^ y ^ phase) & 1)),
                                     static_cast<uint8_t>(255 * (((x >> 1) ^ phase) & 1)),
                                     static_cast<uint8_t>(255 * ((y >> 1) & 1)), 255};
                if (std::memcmp(row + x * 4, rgba, sizeof(rgba))) {
                    std::fprintf(stderr, "D3D11 FL10 frame %u: pixel (%u,%u) differs\n", phase, x, y);
                    ok = false;
                    break;
                }
            }
        }
        context->Unmap(readback.Get(), 0);
        if (!ok) return E_FAIL;
    }
    return check(device->GetDeviceRemovedReason(), "D3D11 raster device status");
}

HRESULT run11(IDXGIAdapter1 *adapter, LUID wanted, ID3DBlob *code)
{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    TRY(create11(adapter, wanted, D3D_FEATURE_LEVEL_11_0, device, context));
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
        TRY(wait11(context.Get(), query.Get()));
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

HRESULT run12(IDXGIAdapter1 *adapter, LUID wanted, ID3DBlob *code, bool timestamps)
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
    // Timestamps are mandatory on direct queues, optional on compute queues.
    queue_desc.Type = timestamps ? D3D12_COMMAND_LIST_TYPE_DIRECT : D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12Fence> fence;
    TRY(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> query_result;
    if (timestamps) {
        D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1, 0};
        TRY(device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries)));
        TRY(make_buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_FLAG_NONE, query_result));
        UINT64 frequency = 0;
        TRY(queue->GetTimestampFrequency(&frequency));
        if (!frequency) return check(E_FAIL, "D3D12 zero timestamp frequency");
        std::printf("D3D12 timestamp frequency: %llu Hz\n", static_cast<unsigned long long>(frequency));
    }
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
        if (timestamps) {
            commands->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            commands->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, query_result.Get(), 0);
        }
        TRY(commands->Close());
        ID3D12CommandList *submitted = commands.Get();
        UINT64 gpu_before = 0, cpu_before = 0;
        if (timestamps) TRY(queue->GetClockCalibration(&gpu_before, &cpu_before));
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
        if (timestamps) {
            UINT64 gpu_after = 0, cpu_after = 0;
            TRY(queue->GetClockCalibration(&gpu_after, &cpu_after));
            const D3D12_RANGE range{0, sizeof(UINT64)};
            TRY(query_result->Map(0, &range, &mapped));
            const UINT64 event_time = *static_cast<const UINT64 *>(mapped);
            query_result->Unmap(0, &not_written);
            std::printf("GPU calibration %llu..%llu, queue timestamp %llu, QPC %llu..%llu\n",
                        static_cast<unsigned long long>(gpu_before), static_cast<unsigned long long>(gpu_after),
                        static_cast<unsigned long long>(event_time), static_cast<unsigned long long>(cpu_before),
                        static_cast<unsigned long long>(cpu_after));
            if (cpu_after <= cpu_before || gpu_after <= gpu_before ||
                event_time < gpu_before || event_time > gpu_after)
                return check(E_FAIL, "GPU timestamp outside queue clock calibration");
        }
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
    bool list = false, warp = false, raster = true, use11 = true, use12 = true, timestamps = false;
    const char *requested = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--list")) list = true;
        else if (!std::strcmp(argv[i], "--warp")) warp = true;
        else if (!std::strcmp(argv[i], "--luid") && i + 1 < argc) requested = argv[++i];
        else if (!std::strcmp(argv[i], "--api") && i + 1 < argc) {
            ++i;
            use11 = !std::strcmp(argv[i], "11");
            timestamps = !std::strcmp(argv[i], "12-timestamps");
            use12 = timestamps || !std::strcmp(argv[i], "12");
            raster = use11 || !std::strcmp(argv[i], "11-fl10");
            if (!raster && !use12) return 2;
        } else {
            std::fprintf(stderr, "Usage: graphics-readback [--list | --warp | --luid HIGH:LOW] [--api 11-fl10|11|12|12-timestamps]\n");
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
    std::printf("Test adapter: %ls, LUID %08lx:%08lx, %s\n", desc.Description,
                static_cast<unsigned long>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart,
                warp ? "WARP CONTROL (not hardware)" : "hardware");
    bool passed = true;
    if (raster) {
        bool ok = SUCCEEDED(draw10(selected.Get(), desc.AdapterLuid));
        std::printf("D3D11 FL10_0 draw/event/RGBA readback: %s\n", ok ? "PASS" : "FAIL");
        passed &= ok;
    }
    ComPtr<ID3DBlob> code;
    if ((use11 || use12) && FAILED(compile(shader_source, "main", "cs_5_0", code))) return 1;
    if (use11) {
        bool ok = SUCCEEDED(run11(selected.Get(), desc.AdapterLuid, code.Get()));
        std::printf("D3D11 shader/event/readback: %s\n", ok ? "PASS" : "FAIL");
        passed &= ok;
    }
    if (use12) {
        bool ok = SUCCEEDED(run12(selected.Get(), desc.AdapterLuid, code.Get(), timestamps));
        std::printf("D3D12 shader/fence/readback: %s\n", ok ? "PASS" : "FAIL");
        passed &= ok;
    }
    return passed ? 0 : 1;
}
