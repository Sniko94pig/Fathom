#include "common/dx12_texture.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace dx12 {

namespace {

std::vector<uint8_t> Half(const std::vector<uint8_t>& src, int w, int h, int& nw, int& nh) {
    nw = w > 1 ? w / 2 : 1;
    nh = h > 1 ? h / 2 : 1;
    std::vector<uint8_t> out((size_t)nw * nh * 4);
    for (int y = 0; y < nh; ++y) {
        int y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
        for (int x = 0; x < nw; ++x) {
            int x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
            const uint8_t* a = &src[((size_t)y0 * w + x0) * 4];
            const uint8_t* b = &src[((size_t)y0 * w + x1) * 4];
            const uint8_t* c = &src[((size_t)y1 * w + x0) * 4];
            const uint8_t* d = &src[((size_t)y1 * w + x1) * 4];
            uint8_t* o = &out[((size_t)y * nw + x) * 4];
            for (int k = 0; k < 4; ++k) o[k] = (uint8_t)((a[k] + b[k] + c[k] + d[k] + 2) / 4);
        }
    }
    return out;
}

}

ID3D12Resource* CreateTexture(ID3D12Device* device, ID3D12CommandQueue* queue, const uint8_t* rgba, int width, int height,
                              D3D12_CPU_DESCRIPTOR_HANDLE srv, bool mips) {
    UINT levels = 1;
    if (mips)
        for (int w = width, h = height; (w > 1 || h > 1) && levels < 14; w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1) ++levels;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = (UINT64)width;
    desc.Height = (UINT)height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = (UINT16)levels;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ID3D12Resource* tex = nullptr;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&tex))))
        return nullptr;

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> foot(levels);
    std::vector<UINT> rows(levels);
    std::vector<UINT64> rowBytes(levels);
    UINT64 size = 0;
    device->GetCopyableFootprints(&desc, 0, levels, 0, foot.data(), rows.data(), rowBytes.data(), &size);

    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = size;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* buffer = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;
    bool ok = false;
    void* mapped = nullptr;
    if (SUCCEEDED(device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&buffer))) &&
        SUCCEEDED(buffer->Map(0, nullptr, &mapped)) &&
        SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))) &&
        SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        std::vector<uint8_t> level;
        int lw = width, lh = height;
        for (UINT m = 0; m < levels; ++m) {
            const uint8_t* px = rgba;
            if (m > 0) {
                int nw = 0, nh = 0;
                level = m == 1 ? Half(std::vector<uint8_t>(rgba, rgba + (size_t)lw * lh * 4), lw, lh, nw, nh) : Half(level, lw, lh, nw, nh);
                lw = nw;
                lh = nh;
                px = level.data();
            }
            uint8_t* dst = (uint8_t*)mapped + foot[m].Offset;
            for (int y = 0; y < lh; ++y) std::memcpy(dst + (size_t)y * foot[m].Footprint.RowPitch, px + (size_t)y * lw * 4, (size_t)lw * 4);
        }
        buffer->Unmap(0, nullptr);
        for (UINT m = 0; m < levels; ++m) {
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = buffer;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint = foot[m];
            dst.pResource = tex;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = m;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, 1);
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, ev);
        ok = WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0;
    }
    if (ev) CloseHandle(ev);
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (buffer) buffer->Release();
    if (!ok) {
        tex->Release();
        return nullptr;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = levels;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    device->CreateShaderResourceView(tex, &sd, srv);
    return tex;
}

}
