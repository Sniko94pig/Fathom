#pragma once
#include <d3d12.h>
#include <cstdint>

namespace dx12 {

ID3D12Resource* CreateTexture(ID3D12Device* device, ID3D12CommandQueue* queue, const uint8_t* rgba, int width, int height,
                              D3D12_CPU_DESCRIPTOR_HANDLE srv, bool mips = false);

}
