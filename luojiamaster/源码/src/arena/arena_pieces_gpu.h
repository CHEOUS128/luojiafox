// Included inside arena_view.cpp alongside the native venue renderer.
struct PieceResources {
    ComPtr<ID3D12Resource> vertices;
    ComPtr<ID3D12PipelineState> pipeline;
    std::array<UINT, 3> first{}, count{};
    UINT ink_offset{};
};
PieceResources piece_gpu;
void PreparePieces(ID3D12Device* device) {
    if (piece_gpu.pipeline) return;
    static constexpr char shader[] = R"(
cbuffer View : register(b0) {
    float4 camera;
    float2 scale; float2 glyphSpan;
    float3 center; float stale;
    float3 tint; uint glyphIndex;
    float2 viewOrigin; float2 depthScale;
    float2 screenShift;
};
Texture2D<float> venueDepth : register(t1);
ByteAddressBuffer roleInk : register(t2);
struct Pixel { float4 pos : SV_Position; float shade : COLOR; float2 uv : TEXCOORD; };
Pixel vs(float3 p : POSITION, float shade : COLOR) {
    float3 world = p + center;
    float right = world.x * camera.x - world.z * camera.y;
    float z = world.x * camera.y + world.z * camera.x;
    Pixel o;
    o.pos = float4(right * scale.x, (world.y * camera.w - z * camera.z) * scale.y,
                   .5 - (world.y * camera.z + z * camera.w) / 80, 1);
    o.pos.xy+=screenShift;
    o.shade=shade;
    // Rotate the inscription within the horizontal top surface. Its baseline
    // stays level on screen at any yaw; inclination still foreshortens ink.
    o.uv=float2(p.x*camera.x-p.z*camera.y,p.x*camera.y+p.z*camera.x)/glyphSpan+.5;
    return o;
}
float inkTap(int2 xy,int n,int start) {
    xy=clamp(xy,0,n-1);
    return asfloat(roleInk.Load((start + xy.y*n+xy.x)*4));
}
float sampleInk(float2 uv,int level) {
    int n=32>>level, start=glyphIndex*1365+(4096-4*n*n)/3;
    float2 xy=uv*n-.5, f=frac(xy); int2 p=(int2)floor(xy);
    return lerp(lerp(inkTap(p,n,start),inkTap(p+int2(1,0),n,start),f.x),
                lerp(inkTap(p+int2(0,1),n,start),inkTap(p+1,n,start),f.x),f.y);
}
float4 ps(Pixel p) : SV_Target {
    int2 position = int2((p.pos.xy-viewOrigin)*depthScale);
    float terrain = venueDepth.Load(int3(position,0));
    // 2 mm tolerance accounts for quantization on the slightly sloping mesh.
    clip(terrain + .002/80 - p.pos.z);
    if(p.shade<.999) return float4(tint*p.shade,1);
    float3 top=tint*.71;
    float lod=clamp(log2(max(1,32*max(length(ddx(p.uv)),length(ddy(p.uv))))),0,5);
    if(any(p.uv<0) || any(p.uv>1)) return float4(top,1);
    int low=(int)floor(lod);
    float ink=lerp(sampleInk(p.uv,low),sampleInk(p.uv,min(5,low+1)),frac(lod));
    return float4(lerp(top,lerp(float3(.996,.976,.933),float3(.75,.77,.79),stale),ink),1);
}
)";
    const auto compile = [&](const char* entry,const char* profile) {
        ComPtr<ID3DBlob> binary,errors;
        const auto hr=D3DCompile(shader,sizeof(shader)-1,"local replay pieces",nullptr,nullptr,
                                entry,profile,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&binary,&errors);
        if(FAILED(hr) && errors) throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
        Require(hr,"piece shader"); return binary;
    };
    const auto vs=compile("vs","vs_5_0"),ps=compile("ps","ps_5_0");
    D3D12_INPUT_ELEMENT_DESC input[]{
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature=cache.root.Get(); p.InputLayout={input,2};
    p.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    auto& blend=p.BlendState.RenderTarget[0];
    blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;
    blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;
    blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD; blend.LogicOp=D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode=D3D12_CULL_MODE_BACK;
    p.RasterizerState.DepthClipEnable=TRUE; p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets=1; p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM; p.SampleMask=UINT_MAX; p.SampleDesc.Count=1;
    PieceResources next;
    Require(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&next.pipeline)),"piece pipeline");
    std::vector<pieces::Vertex> vertices;
    const auto& models=pieces::Models();
    for(std::size_t i=0;i<models.size();++i) {
        next.first[i]=static_cast<UINT>(vertices.size());
        next.count[i]=static_cast<UINT>(models[i].triangles.size());
        vertices.insert(vertices.end(),models[i].triangles.begin(),models[i].triangles.end());
    }
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    const UINT vertex_bytes=static_cast<UINT>(vertices.size()*sizeof(pieces::Vertex));
    next.ink_offset=(vertex_bytes+255u)&~255u;
    constexpr int glyph_samples=1365; // 32² + 16² + ... + 1
    std::array<float,3*glyph_samples> ink{};
    for(std::size_t g=0;g<pieces::role_ink.size();++g) {
        auto* samples=ink.data()+g*glyph_samples;
        for(int i=0;i<32*32;++i) {
            const char v=pieces::role_ink[g][i];
            samples[i]=(v<='9' ? v-'0' : v-'a'+10)/15.f;
        }
        for(int n=32;n>1;n/=2) {
            auto* next_mip=samples+n*n;
            for(int y=0;y<n/2;++y) for(int x=0;x<n/2;++x) {
                const auto* v=samples+2*y*n+2*x;
                next_mip[y*(n/2)+x]=(v[0]+v[1]+v[n]+v[n+1])*.25f;
            }
            samples=next_mip;
        }
    }
    desc.Width=next.ink_offset+sizeof(ink); desc.Height=desc.MipLevels=desc.DepthOrArraySize=1;
    desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    next.vertices=Allocate(device,desc,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    D3D12_RANGE empty{}; void* memory{};
    Require(next.vertices->Map(0,&empty,&memory),"piece vertices");
    std::memcpy(memory,vertices.data(),vertex_bytes);
    std::memcpy(static_cast<char*>(memory)+next.ink_offset,ink.data(),sizeof(ink));
    next.vertices->Unmap(0,nullptr);
    piece_gpu=std::move(next);
}

void DrawPieces(ID3D12GraphicsCommandList* list,const D3D12_VIEWPORT& vp,const CameraKey& key,
                std::span<const ReplayMarker> markers) {
    if(markers.empty()) return;
    const auto camera=ReplayProjection::ForCanvas(view.w,view.h,key.yaw,key.tilt,key.zoom);
    struct Instance { const ReplayMarker* m; float distance; };
    std::array<Instance,8> ordered{};
    const auto count=std::min(ordered.size(),markers.size());
    for(std::size_t i=0;i<count;++i) {
        const auto& m=markers[i];
        const float z=(m.u-.5f)*28.f*camera.si+(m.v-.5f)*15.f*camera.co;
        ordered[i]={&m,z*camera.st + m.floor_y*camera.ct};
    }
    // The eight non-intersecting convex pieces use back-face culling; paint
    // farther instances first without writing a single pixel into the map cache.
    std::sort(ordered.begin(),ordered.begin()+count,[](auto a,auto b){return a.distance<b.distance;});
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={device_resources.depth.Get(),0,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1,&barrier);
    auto* heap=cache.sample_heap.Get();list->SetDescriptorHeaps(1,&heap);
    list->SetGraphicsRootSignature(cache.root.Get());list->SetPipelineState(piece_gpu.pipeline.Get());
    list->SetGraphicsRootDescriptorTable(1,heap->GetGPUDescriptorHandleForHeapStart());
    list->SetGraphicsRootShaderResourceView(2,piece_gpu.vertices->GetGPUVirtualAddress()+piece_gpu.ink_offset);
    D3D12_VERTEX_BUFFER_VIEW buffer{piece_gpu.vertices->GetGPUVirtualAddress(),
                        piece_gpu.ink_offset,sizeof(pieces::Vertex)};
    list->IASetVertexBuffers(0,1,&buffer); list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for(std::size_t i=0;i<count;++i) {
        const auto& m=*ordered[i].m;
        const bool stale=m.age_seconds>=3;
        const auto color=pieces::Tint(m.team,stale);
        const auto model=pieces::Index(m.number);
        const auto span=pieces::InkSize(pieces::Models()[model],camera.co,camera.si);
        const float constants[]{
            camera.co,camera.si,camera.ct,camera.st,camera.sx,camera.sy,span.x,span.z,
            (m.u-.5f)*28.f,m.floor_y,(m.v-.5f)*15.f,stale?1.f:0.f,color.r,color.g,color.b,0,
            vp.TopLeftX,vp.TopLeftY,key.width/vp.Width,key.height/vp.Height,key.shift_x,key.shift_y};
        list->SetGraphicsRoot32BitConstants(0,22,constants,0);
        list->SetGraphicsRoot32BitConstant(0,static_cast<UINT>(model),15);
        list->DrawInstanced(piece_gpu.count[model],1,piece_gpu.first[model],0);
    }
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
    list->ResourceBarrier(1,&barrier);
    if(ui_heap) list->SetDescriptorHeaps(1,&ui_heap);
}
