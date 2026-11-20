#include "arena/arena_view.h"
#include "arena/replay_badges.h"
#include "arena/replay_role_ink.h"
#include "platform/platform_util.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>

#ifdef _WIN32
#include "arena/arena_gpu.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "backends/imgui_impl_dx12.h"
#pragma comment(lib, "d3dcompiler")
#endif

namespace RoboGo::arena {
namespace {
NavCamera camera{};
struct MapDrag {
    ImGuiMouseButton button = -1;
    ImVec2 start{}, size{};
    Mode mode{Mode::Unavailable};
    NavCamera start_camera{};
};
MapDrag drag{};
struct PendingFocus {
    Mode mode{Mode::Unavailable};
    NavPoint pixel{};
    ImVec2 size{};
    NavCamera wheel_camera{};
    double wheel_time{};
};
PendingFocus pending_focus{};
struct SurfaceHit { std::optional<NavWorld> point; bool pending{}; };
Mode last_drawn = Mode::Unavailable;
std::string status;
#ifdef _WIN32
using Microsoft::WRL::ComPtr;
struct Mesh {
    ComPtr<ID3D12Resource> gpu, staging;
    UINT64 index_offset{}, image_offset{}, total_bytes{}, fence{};
    UINT num_indices{}, tex_w{}, tex_h{};
    bool copy_recorded{}, resident{}, failed{};
    std::string error;
};
struct DeviceResources {
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12DescriptorHeap> depth_heap;
    ComPtr<ID3D12Resource> depth;
    std::array<Mesh,2> mesh;
};
DeviceResources device_resources;
D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
UINT target_w{}, target_h{};
struct View {
    float x, y, w, h;
    NavCamera camera;
    Mode mode, fallback;
    bool prepare_only{};
    std::array<ReplayMarker,8> pieces{};
    std::size_t num_pieces{};
};
View view{};
void Require(HRESULT hr, const char* stage) {
    if (FAILED(hr)) throw std::runtime_error(std::string(stage) + " (" + std::to_string(hr) + ")");
}
void MakePipeline(ID3D12Device* device) {
    auto& g = device_resources;
    if (g.pso) return;
    static const char shader[] = R"(
cbuffer Camera : register(b0) {
    float4 camera; // cos/sin azimuth, cos/sin tilt
    float2 scale; uint imageWidth; uint imageHeight;
    float2 screenShift;
};
ByteAddressBuffer atlas : register(t0);
struct Vertex { float4 pos:SV_Position; float4 color:COLOR0; float2 uv:TEXCOORD0; };
Vertex vs(int4 p:POSITION,float4 color:COLOR0,float2 uv:TEXCOORD0) {
    float3 world = p.xyz / 1024.0;
    float x = world.x*camera.x - world.z*camera.y;
    float z = world.x*camera.y + world.z*camera.x;
    float up = world.y*camera.w - z*camera.z;
    float depth = .5 - (world.y*camera.z + z*camera.w)/80.0;
    Vertex v; v.pos=float4(float2(x*scale.x, up*scale.y)+screenShift, depth, 1); v.color=color; v.uv=uv; return v;
}
float3 sampleAtlas(uint2 pos) {
    uint v=atlas.Load((pos.y*imageWidth+pos.x)*4);
    return float3(v&255,(v>>8)&255,(v>>16)&255)/255.0;
}
float4 ps(Vertex v):SV_Target {
    float3 rgb=v.color.rgb;
    if (v.color.a>.5) {
        uint2 dims=uint2(imageWidth,imageHeight);
        float2 p=clamp(v.uv*dims-.5, 0, float2(dims-1));
        uint2 lo=(uint2)p, hi=min(lo+1,dims-1);
        float2 k=frac(p);
        rgb *= lerp(lerp(sampleAtlas(lo),sampleAtlas(uint2(hi.x,lo.y)),k.x),
                    lerp(sampleAtlas(uint2(lo.x,hi.y)),sampleAtlas(hi),k.x),k.y);
    }
    return float4(rgb,1.0);
}
)";
    auto compile = [&](const char* entry, const char* profile) {
        ComPtr<ID3DBlob> blob, errors;
        const HRESULT hr = D3DCompile(shader, sizeof(shader)-1, "arena", nullptr, nullptr, entry, profile,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
        if (FAILED(hr) && errors) throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
        Require(hr, "shader"); return blob;
    };
    auto vs=compile("vs","vs_5_0"), ps=compile("ps","ps_5_0");
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues=10;
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    signature.NumParameters=2; signature.pParameters=params;
    ComPtr<ID3DBlob> serialized;
    Require(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,nullptr),"serialize");
    ComPtr<ID3D12RootSignature> root;
    Require(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)), "root");
    const D3D12_INPUT_ELEMENT_DESC input[]={
        {"POSITION",0,DXGI_FORMAT_R16G16B16A16_SINT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,8,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R16G16_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature=root.Get();
    p.VS={vs->GetBufferPointer(),vs->GetBufferSize()}; p.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    p.InputLayout={input,3};
    auto& blend=p.BlendState.RenderTarget[0];
    blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE; blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;
    blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD; blend.LogicOp=D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable=TRUE;
    p.DepthStencilState.DepthEnable=TRUE; p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
    p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets=1; p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM; p.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    p.SampleMask=UINT_MAX; p.SampleDesc.Count=1;
    Require(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&g.pso)),"pipeline");
    g.root=std::move(root);
}
ComPtr<ID3D12Resource> Allocate(ID3D12Device* device, const D3D12_RESOURCE_DESC& desc,
                               D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                               const D3D12_CLEAR_VALUE* clear = nullptr) {
    D3D12_HEAP_PROPERTIES props{}; props.Type=type; props.CreationNodeMask=props.VisibleNodeMask=1;
    ComPtr<ID3D12Resource> result;
    Require(device->CreateCommittedResource(&props,D3D12_HEAP_FLAG_NONE,&desc,state,clear,IID_PPV_ARGS(&result)),"resource");
    return result;
}
// Cache definitions use the renderer's COM type and resource helpers.
#include "arena/arena_cache.h"
#include "arena/arena_surface_pick.h"
#include "arena/arena_pieces_gpu.h"
void PrepareDepth(ID3D12Device* device) {
    auto& g=device_resources;
    if (g.depth) return;
    D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV; heap.NumDescriptors=1;
    ComPtr<ID3D12DescriptorHeap> dsv;
    Require(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&dsv)),"depth heap");
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Format=DXGI_FORMAT_R32_TYPELESS;
    desc.Width=ArenaCache::width; desc.Height=ArenaCache::height; desc.MipLevels=desc.DepthOrArraySize=1;
    desc.SampleDesc.Count=1; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{}; clear.Format=DXGI_FORMAT_D32_FLOAT; clear.DepthStencil.Depth=1;
    g.depth=Allocate(device,desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear);
    D3D12_DEPTH_STENCIL_VIEW_DESC depth_view{};
    depth_view.Format=DXGI_FORMAT_D32_FLOAT; depth_view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(g.depth.Get(),&depth_view,dsv->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format=DXGI_FORMAT_R32_FLOAT; srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels=1;
    auto depth_slot=cache.sample_heap->GetCPUDescriptorHandleForHeapStart();
    depth_slot.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device->CreateShaderResourceView(g.depth.Get(),&srv,depth_slot);
    g.depth_heap=std::move(dsv);
}
void LoadMesh(ID3D12Device* device, Mesh& m, Mode mode) {
    namespace fsu = RoboGo::platform_util;
    auto path=fsu::ExecutableDirectory()/"arena"/(mode==Mode::Light ? "light.mesh" : "detailed.mesh");
    auto stream=fsu::OpenBinaryInput(fsu::PathToUtf8(path));
    if (!stream) throw std::runtime_error(mode==Mode::Light ? "找不到 arena/light.mesh" : "找不到 arena/detailed.mesh");
    struct Header { char magic[8]; UINT nv,ni,width,height; } h{};
    stream.read(reinterpret_cast<char*>(&h),sizeof(h));
    if (!stream || memcmp(h.magic,"RMUF27V1",8) || !h.nv || !h.ni || h.ni%3 || h.nv>30000000 ||
        h.ni>30000000 || !h.width || !h.height || h.width>4096 || h.height>4096)
        throw std::runtime_error("场馆文件头不正确");
    const UINT64 ib=UINT64(h.nv)*16, ib_bytes=UINT64(h.ni)*4;
    const UINT64 pix=UINT64(h.width)*h.height*4, texture=(ib+ib_bytes+15)&~UINT64(15);
    stream.seekg(0,std::ios::end);
    if(stream.tellg()!=static_cast<std::streamoff>(sizeof(h)+ib+ib_bytes+pix)) throw std::runtime_error("场馆文件长度不正确");
    stream.seekg(sizeof(h));
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=texture+pix;
    desc.Height=desc.MipLevels=desc.DepthOrArraySize=1; desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto staging=Allocate(device,desc,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto gpu=Allocate(device,desc,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
    unsigned char* bytes{}; D3D12_RANGE empty{};
    Require(staging->Map(0,&empty,reinterpret_cast<void**>(&bytes)),"map staging");
    stream.read(reinterpret_cast<char*>(bytes),static_cast<std::streamsize>(ib+ib_bytes));
    stream.read(reinterpret_cast<char*>(bytes+texture),static_cast<std::streamsize>(pix));
    staging->Unmap(0,nullptr);
    if(!stream) throw std::runtime_error("场馆文件读取失败");
    m.gpu=std::move(gpu); m.staging=std::move(staging); m.index_offset=ib; m.image_offset=texture;
    m.num_indices=h.ni; m.tex_w=h.width; m.tex_h=h.height; m.total_bytes=desc.Width;
}
void Render(const ImDrawList*, const ImDrawCmd* draw) {
    auto* state=static_cast<ImGui_ImplDX12_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    auto& g=device_resources;
    auto& m=g.mesh[view.mode==Mode::Detailed ? 1 : 0];
    last_drawn=view.fallback;
    if (!state || !rtv.ptr || !target_w || !target_h || m.failed) return;
    try {
        MakePipeline(state->Device); MakeCache(state->Device); PrepareDepth(state->Device);
        if(view.num_pieces) PreparePieces(state->Device);
        if (!m.gpu) LoadMesh(state->Device,m,view.mode);
    } catch(const std::exception& e) { m.error=e.what(); m.failed=true; status=m.error; return; }
    auto* list=state->CommandList;
    if (!m.resident) {
        list->CopyBufferRegion(m.gpu.Get(),0,m.staging.Get(),0,m.total_bytes);
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={m.gpu.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_GENERIC_READ};
        list->ResourceBarrier(1,&barrier); m.copy_recorded=true;
    }
    // The mesh is loaded in a render callback, after ImGui recorded its
    // overlays. Keep the 2D picture + 2D badges on this first frame. Only
    // switch both after a successful upload (a missing model stays in 2D).
    if (view.prepare_only) return;
    auto* data=ImGui::GetDrawData();
    const auto scale=data->FramebufferScale;
    const auto origin=data->DisplayPos;
    const D3D12_VIEWPORT screen{(view.x-origin.x)*scale.x,(view.y-origin.y)*scale.y,view.w*scale.x,view.h*scale.y,0,1};
    D3D12_RECT visible{LONG(std::max(screen.TopLeftX,(draw->ClipRect.x-origin.x)*scale.x)),
                       LONG(std::max(screen.TopLeftY,(draw->ClipRect.y-origin.y)*scale.y)),
                       LONG(std::min(screen.TopLeftX+screen.Width,(draw->ClipRect.z-origin.x)*scale.x)),
                       LONG(std::min(screen.TopLeftY+screen.Height,(draw->ClipRect.w-origin.y)*scale.y))};
    visible.left=std::max(0L,visible.left); visible.top=std::max(0L,visible.top);
    visible.right=std::min(static_cast<LONG>(target_w),visible.right);
    visible.bottom=std::min(static_cast<LONG>(target_h),visible.bottom);
    if (visible.right<=visible.left || visible.bottom<=visible.top) return;
    using Clock=std::chrono::steady_clock;
    static Clock::time_point last_motion{};
    const auto now=Clock::now();
    const auto& nav=view.camera;
    const auto shift=nav.OffsetNdc(view.w,view.h);
    const auto projection=ReplayProjection::ForCanvas(view.w,view.h,nav.yaw,nav.tilt,nav.zoom);
    const bool camera_changed=cache.valid.mode!=view.mode || cache.valid.yaw!=nav.yaw ||
                              cache.valid.tilt!=nav.tilt || cache.valid.zoom!=nav.zoom ||
                              cache.valid.shift_x!=shift.x || cache.valid.shift_y!=shift.y ||
                              cache.valid.sx!=projection.sx || cache.valid.sy!=projection.sy;
    if(camera_changed) last_motion=now;
    const bool moving=now-last_motion<std::chrono::milliseconds(150);
    const float raster_scale=std::min({1.f,(moving ? 960.f : float(ArenaCache::width))/screen.Width,
                                           (moving ? 720.f : float(ArenaCache::height))/screen.Height});
    const CameraKey key{nav.yaw,nav.tilt,nav.zoom,shift.x,shift.y,projection.sx,projection.sy,
                        std::max(1u,UINT(std::lround(screen.Width*raster_scale))),
                        std::max(1u,UINT(std::lround(screen.Height*raster_scale))),view.mode};
    const auto composite=[&] {
        surface_depth.CaptureIfNeeded(state->Device,list,g.depth.Get(),key,moving);
        DrawCache(list,screen,visible,key);
        DrawPieces(list,screen,key,{view.pieces.data(),view.num_pieces});
    };
    if(key==cache.valid) {
        composite();
        last_drawn=view.mode;status.clear();return;
    }
    UseCache(list,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_VIEWPORT vp{0,0,float(key.width),float(key.height),0,1};
    D3D12_RECT rect{0,0,LONG(key.width),LONG(key.height)};
    const auto map_rtv=cache.render_heap->GetCPUDescriptorHandleForHeapStart();
    auto dsv=g.depth_heap->GetCPUDescriptorHandleForHeapStart();
    const float clear[4]{.09f,.105f,.13f,1};
    list->ClearRenderTargetView(map_rtv,clear,1,&rect);
    list->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,1,&rect);
    list->OMSetRenderTargets(1,&map_rtv,FALSE,&dsv); list->RSSetViewports(1,&vp); list->RSSetScissorRects(1,&rect);
    list->SetGraphicsRootSignature(g.root.Get()); list->SetPipelineState(g.pso.Get());
    struct Constants { float co,si,ct,st,sx,sy; UINT tw,th; float ox,oy; } b{
        projection.co,projection.si,projection.ct,projection.st,projection.sx,projection.sy,m.tex_w,m.tex_h,shift.x,shift.y};
    list->SetGraphicsRoot32BitConstants(0,10,&b,0);
    const auto va=m.gpu->GetGPUVirtualAddress();
    list->SetGraphicsRootShaderResourceView(1,va+m.image_offset);
    D3D12_VERTEX_BUFFER_VIEW vertices{va,static_cast<UINT>(m.index_offset),16};
    D3D12_INDEX_BUFFER_VIEW indices{va+m.index_offset,m.num_indices*4,DXGI_FORMAT_R32_UINT};
    list->IASetVertexBuffers(0,1,&vertices); list->IASetIndexBuffer(&indices);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawIndexedInstanced(m.num_indices,1,0,0,0);
    UseCache(list,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    composite();
    cache.pending=key;
    last_drawn=view.mode; status.clear();
}
#endif
SurfaceHit PickSurface(Mode shown,const NavCamera& nav,float w,float h,NavPoint cursor) {
#ifdef _WIN32
    if(shown==Mode::Light || shown==Mode::Detailed)
        return surface_depth.Pick(shown,nav,w,h,cursor);
#endif
    return {};
}
// Moving the orbit centre must leave every point where it was on screen.
void SetOrbitCentre(NavCamera& pose,float w, float h, bool flat, NavWorld world) {
    const auto screen = pose.Project(w,h,flat,world);
    pose.pivot=world;
    pose.anchor={screen.x/w,screen.y/h};
}

void HandleMapGestures(Mode shown, ImVec2 pos, ImVec2 size, bool pointer_on_card) {
    const auto& io=ImGui::GetIO();
    if (shown==Mode::Unavailable || !ImGui::IsMousePosValid(&io.MousePos) || io.AppFocusLost) {
        drag={}; pending_focus={}; return;
    }
    const bool flat=shown==Mode::Flat;
    const bool hovered=!pointer_on_card && ImGui::IsItemHovered();
    const bool left_pressed=ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool right_pressed=ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    const NavPoint pointer{io.MousePos.x-pos.x,io.MousePos.y-pos.y};
    const double now=ImGui::GetTime();

    // If the wheel arrived just after turning, the latest matching surface
    // may still be rendering. Apply its height once, without moving the view.
    if (pending_focus.mode!=Mode::Unavailable) {
        if (flat || shown!=pending_focus.mode || size.x!=pending_focus.size.x ||
            size.y!=pending_focus.size.y || now-pending_focus.wheel_time>.5) {
            pending_focus={};
        } else {
            const auto surface=PickSurface(shown,pending_focus.wheel_camera,size.x,size.y,pending_focus.pixel);
            if (surface.point) {
                SetOrbitCentre(camera,size.x,size.y,false,*surface.point);
                if (drag.button!=-1)
                    SetOrbitCentre(drag.start_camera,size.x,size.y,false,*surface.point);
            }
            if (!surface.pending) pending_focus={};
        }
    }

    if (drag.mode!=shown || drag.size.x!=size.x || drag.size.y!=size.y ||
        (drag.button!=-1 && !io.MouseDown[drag.button] &&
         !ImGui::IsMouseReleased(drag.button))) drag={};
    if (drag.button==-1 && hovered &&
        (left_pressed || (right_pressed && !flat))) {
        const auto button=left_pressed ? ImGuiMouseButton_Left : ImGuiMouseButton_Right;
        drag={button,io.MouseClickedPos[button],size,shown,camera};
    }
    if (drag.button!=-1) {
        const ImVec2 delta{io.MousePos.x-drag.start.x,io.MousePos.y-drag.start.y};
        camera=drag.start_camera;
        if (drag.button==ImGuiMouseButton_Left) camera.Pan(size.x,size.y,delta.x,delta.y);
        // Give a just-requested surface at most 40 ms before orbiting with
        // the provisional point. The complete drag still uses the press pose.
        else if (pending_focus.mode==Mode::Unavailable || now-pending_focus.wheel_time>=.04 ||
                 ImGui::IsMouseReleased(drag.button)) camera.Orbit(delta.x,delta.y);
        // A short drag can deliver its final position on the release frame.
        if (ImGui::IsMouseReleased(drag.button)) drag={};
    }
    if (!hovered || io.MouseWheel==0.f) return;

    const auto surface=PickSurface(shown,camera,size.x,size.y,pointer);
    const auto hit=surface.point.value_or(camera.PickAtHeight(size.x,size.y,flat,pointer));
    const auto previous_pivot=camera.pivot;
    if (!camera.ZoomAt(size.x,size.y,flat,pointer,io.MouseWheel,hit)) return;
    if (!surface.point && (std::abs(hit.x)>14.f || std::abs(hit.z)>8.005f)) {
        // The wheel can still zoom over the margin, without turning empty space
        // into the next orbit centre.
        SetOrbitCentre(camera,size.x,size.y,flat,previous_pivot);
    }
    pending_focus=surface.pending ? PendingFocus{shown,pointer,size,camera,now} : PendingFocus{};
    if (drag.button!=-1) {
        drag.start=io.MousePos;
        drag.start_camera=camera;
    }
}
}

void ResetCamera() { camera={}; drag={}; pending_focus={}; }
const char* Status() { return status.c_str(); }
Mode Draw(const ImVec2& size, Mode requested, ImTextureID flat, double benchmark_seconds,
          const std::vector<ReplayMarker>& markers, ReplayCanvas* canvas, int replay_team, bool pointer_on_card) {
    if (canvas) *canvas={};
    if (size.x<1 || size.y<1) return Mode::Unavailable;
    const auto pos=ImGui::GetCursorScreenPos();
    // A card under the pointer owns a new press. A drag that began on the
    // arena keeps its original ImGui item until the release is delivered.
    const bool was_dragging=drag.button!=-1;
    if (pointer_on_card && !was_dragging) ImGui::Dummy(size);
    else ImGui::InvisibleButton("##arena-surface",size,ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    Mode shown=flat ? Mode::Flat : Mode::Unavailable;
#ifdef _WIN32
    bool use_mesh=requested==Mode::Light || requested==Mode::Detailed;
    if(use_mesh) {
        const auto& m=device_resources.mesh[requested==Mode::Detailed ? 1 : 0];
        if(m.failed) { status=m.error; use_mesh=false; }
        else if(m.resident) shown=requested;
    } else status.clear();
#endif
    if (benchmark_seconds<0) HandleMapGestures(shown,pos,size,pointer_on_card);
    auto* list=ImGui::GetWindowDrawList();
    const ImVec2 end(pos.x+size.x,pos.y+size.y);
    if (canvas) {
        canvas->lo=pos;
        canvas->hi=end;
        canvas->map_dragging=was_dragging || drag.button!=-1;
    }
    const auto draw_badges=[&](auto project,bool flat_view) {
        badges::Draw(list,markers,{pos,end},project,flat_view,
            [&](const badges::Box& label,const badges::Box& piece) {
                if (canvas && canvas->label_count<static_cast<int>(canvas->labels.size()))
                    canvas->labels[canvas->label_count++]={label.lo,label.hi,piece.lo,piece.hi};
            });
        if (!canvas) return;
        constexpr std::array<int,3> seats{1,3,4};
        for (const auto& marker:markers) {
            if (marker.team!=replay_team) continue;
            const auto seat=std::find(seats.begin(),seats.end(),marker.number);
            if (seat==seats.end()) continue;
            const ImVec2 pt=project(marker.u,marker.v,marker.floor_y);
            const bool inside=pt.x>=pos.x && pt.x<end.x && pt.y>=pos.y && pt.y<end.y;
            canvas->pins[seat-seats.begin()]={pt,true,inside,marker.age_seconds};
        }
    };
    list->PushClipRect(pos,end,true);
    list->AddRectFilled(pos,end,IM_COL32(23,27,33,255));
    if(shown==Mode::Flat) {
        const float mag=camera.FlatScale(size.x,size.y);
        const auto origin=camera.FlatOrigin(size.x,size.y);
        const ImVec2 start(pos.x+origin.x,pos.y+origin.y);
        list->AddImage(flat,start,ImVec2(start.x+903*mag,start.y+499*mag));
        draw_badges([&](float u,float v,float) {
            const auto p=RuleFigurePoint(u,v);
            return ImVec2(start.x+p.x*mag,start.y+p.y*mag);
        },true);
    }
    if(shown==Mode::Unavailable) list->AddText(ImVec2(pos.x+12,pos.y+12),IM_COL32_WHITE,"场馆资源尚未就绪");
#ifdef _WIN32
    if(use_mesh) {
        NavCamera nav=camera;
        if(benchmark_seconds>=0) {
            nav=NavCamera{};
            nav.yaw=float(std::sin(benchmark_seconds*.22)*.46);
            nav.tilt=float(.58+.16*std::sin(benchmark_seconds*.31));
        }
        view={pos.x,pos.y,size.x,size.y,nav,requested,flat ? Mode::Flat : Mode::Unavailable,shown!=requested};
        view.num_pieces=std::min(view.pieces.size(),markers.size());
        std::copy_n(markers.begin(),view.num_pieces,view.pieces.begin());
        list->AddCallback(Render,nullptr);
        list->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState,nullptr);
        if (shown==requested && !markers.empty()) {
            draw_badges([&](float u,float v,float y) {
                const auto p=nav.Project(size.x,size.y,false,{(u-.5f)*28.f,y,(v-.5f)*15.f});
                return ImVec2(pos.x+p.x,pos.y+p.y);
            },false);
        }
    } else last_drawn=shown;
#else
    if(requested!=Mode::Flat) status="当前平台显示二维场地图";
#endif
    list->PopClipRect();
    return shown;
}
#ifdef _WIN32
void SetRenderTarget(D3D12_CPU_DESCRIPTOR_HANDLE target,const D3D12_RESOURCE_DESC& desc,ID3D12DescriptorHeap* heap) {
    rtv=target; target_w=static_cast<UINT>(desc.Width); target_h=desc.Height; ui_heap=heap;
    cache.pending={};
    surface_depth.DiscardUnsubmitted();
    // A discarded command list never submitted its copy. Only ConfirmUpload()
    // may mark the mesh resident. Retry when the map is drawn in a later list.
    for(auto& m:device_resources.mesh) m.copy_recorded=false;
}
void ConfirmUpload(UINT64 submitted,UINT64 completed) {
    surface_depth.Confirm(submitted,completed);
    if(cache.pending.mode!=Mode::Unavailable) {cache.valid=cache.pending;cache.pending={};}
    for(auto& m:device_resources.mesh) {
        if(m.copy_recorded) { m.copy_recorded=false; m.resident=true; m.fence=submitted; }
        if(m.staging && m.resident && completed>=m.fence) m.staging.Reset();
    }
}
void ResetDepth() { cache.valid={}; surface_depth.Invalidate(); }
void ReleaseGpu(bool safe) {
    surface_depth.Release(safe);
    auto& g=device_resources;
    if (!safe) {
        for(auto& m:g.mesh) { m.gpu.Detach(); m.staging.Detach(); }
        g.root.Detach(); g.pso.Detach(); g.depth.Detach(); g.depth_heap.Detach();
        cache.image.Detach();cache.root.Detach();cache.pso.Detach();cache.render_heap.Detach();cache.sample_heap.Detach();
        piece_gpu.vertices.Detach();piece_gpu.pipeline.Detach();
    }
    g={}; cache={}; piece_gpu={}; ui_heap=nullptr;rtv={}; target_w=target_h=0; last_drawn=Mode::Unavailable;
}
#endif
} // namespace RoboGo::arena
