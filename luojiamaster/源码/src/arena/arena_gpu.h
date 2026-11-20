#pragma once
#ifdef _WIN32
#include <d3d12.h>
namespace RoboGo::arena {
void SetRenderTarget(D3D12_CPU_DESCRIPTOR_HANDLE target, const D3D12_RESOURCE_DESC& desc,
                     ID3D12DescriptorHeap* ui_heap = nullptr);
void ConfirmUpload(UINT64 submitted, UINT64 completed);
void ResetDepth();
void ReleaseGpu(bool safe);
}
#endif
