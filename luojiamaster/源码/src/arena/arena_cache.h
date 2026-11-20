#pragma once
// One read-only SRV for the latest rendered 3D view. The swap chain shares
// the same ordered command queue for drawing into it and sampling it.
struct CameraKey {
    float yaw{}, tilt{}, zoom{};
    float shift_x{},shift_y{},sx{},sy{};
    UINT width{}, height{};
    Mode mode{Mode::Unavailable};
    bool operator==(const CameraKey&) const = default;
};
struct ArenaCache {
    static constexpr UINT width=1536, height=1024;
    ComPtr<ID3D12Resource> image;
    ComPtr<ID3D12DescriptorHeap> render_heap, sample_heap;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    CameraKey valid{}, pending{};
};
ArenaCache cache;
ID3D12DescriptorHeap* ui_heap{};
void MakeCache(ID3D12Device* device) {
    if(cache.pso) return;
    // Vertex-ID generates one full viewport triangle. The map viewport
    // uses only its own width and height inside the larger backing texture.
    static constexpr char shader[]=R"(
cbuffer Size : register(b0) { float2 uvScale; float2 halfTexel; }
Texture2D mapColor : register(t0);
SamplerState mapSampler : register(s0);
struct Pixel { float4 clip:SV_Position; float2 uv:TEXCOORD0; };
Pixel vs(uint i:SV_VertexID) {
    float2 uv=float2((i<<1)&2,i&2);
    Pixel p; p.uv=uv*uvScale; p.clip=float4(uv*float2(2,-2)+float2(-1,1),0,1); return p;
}
float4 ps(Pixel p):SV_Target {
    return mapColor.SampleLevel(mapSampler,clamp(p.uv,halfTexel,uvScale-halfTexel),0);
}
)";
    auto compile=[&](const char* entry,const char* profile) {
        ComPtr<ID3DBlob> blob;
        Require(D3DCompile(shader,sizeof(shader)-1,"arena_blit",nullptr,nullptr,entry,profile,
                           D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,nullptr),"map sampler shader");
        return blob;
    };
    auto vs=compile("vs","vs_5_0"),ps=compile("ps","ps_5_0");
    D3D12_DESCRIPTOR_RANGE range{};
    // t0 samples the venue color. t1 holds the matching static depth for pieces.
    range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range.NumDescriptors=2;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[0].Constants.Num32BitValues=22;
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable={1,&range}; params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; params[2].Descriptor.ShaderRegister=2;
    params[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter=D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS; sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.MaxAnisotropy=1;
    D3D12_ROOT_SIGNATURE_DESC sig{};sig.NumParameters=3; sig.pParameters=params; sig.NumStaticSamplers=1;sig.pStaticSamplers=&sampler;
    sig.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> serialized;
    Require(D3D12SerializeRootSignature(&sig,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,nullptr),"map sampler signature");
    ArenaCache next;
    Require(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&next.root)),"map sampler root");
    D3D12_RESOURCE_DESC tex{};tex.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;tex.Width=ArenaCache::width;tex.Height=ArenaCache::height;
    tex.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;tex.DepthOrArraySize=tex.MipLevels=tex.SampleDesc.Count=1;
    tex.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_CLEAR_VALUE clear{};clear.Format=tex.Format;clear.Color[0]=.09f;clear.Color[1]=.105f;clear.Color[2]=.13f;clear.Color[3]=1;
    next.image=Allocate(device,tex,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,&clear);
    D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=1;
    Require(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&next.render_heap)),"map render descriptor");
    device->CreateRenderTargetView(next.image.Get(),nullptr,next.render_heap->GetCPUDescriptorHandleForHeapStart());
    heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heap.NumDescriptors=2;
    Require(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&next.sample_heap)),"map sampler descriptor");
    device->CreateShaderResourceView(next.image.Get(),nullptr,next.sample_heap->GetCPUDescriptorHandleForHeapStart());
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{}; pipeline.pRootSignature=next.root.Get();
    pipeline.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pipeline.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    auto& blend=pipeline.BlendState.RenderTarget[0];
    blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;
    blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    pipeline.RasterizerState.DepthClipEnable=TRUE; pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets=1;pipeline.RTVFormats[0]=tex.Format;pipeline.SampleMask=UINT_MAX;pipeline.SampleDesc.Count=1;
    Require(device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&next.pso)),"map sampler pipeline");
    cache=std::move(next);
}
void UseCache(ID3D12GraphicsCommandList* list,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={cache.image.Get(),0,before,after};list->ResourceBarrier(1,&b);
}
void DrawCache(ID3D12GraphicsCommandList* list,D3D12_VIEWPORT viewport,D3D12_RECT rect,const CameraKey& key) {
    list->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
    list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&rect);
    list->SetGraphicsRootSignature(cache.root.Get());list->SetPipelineState(cache.pso.Get());
    const float uv[4]={float(key.width)/ArenaCache::width,float(key.height)/ArenaCache::height,
                       .5f/ArenaCache::width,.5f/ArenaCache::height};
    list->SetGraphicsRoot32BitConstants(0,4,uv,0);
    auto* heap=cache.sample_heap.Get();list->SetDescriptorHeaps(1,&heap);
    list->SetGraphicsRootDescriptorTable(1,heap->GetGPUDescriptorHandleForHeapStart());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);
    if(ui_heap) list->SetDescriptorHeaps(1,&ui_heap);
}
