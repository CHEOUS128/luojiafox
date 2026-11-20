// The static venue's depth image is also its picking surface. Retain a
// completed snapshot of the last settled view. While dragging, request a
// fresh snapshot only when a wheel pick actually lacks one.
class SurfaceDepth {
    struct Snapshot {
        ComPtr<ID3D12Resource> buffer;
        CameraKey camera{};
        UINT64 fence{};
        bool recording{};
    };
    std::array<Snapshot,2> slots{};
    UINT64 completed{};
    bool requested{};
    bool unavailable{};
    static constexpr UINT pitch=ArenaCache::width*sizeof(float);
    static_assert(pitch%D3D12_TEXTURE_DATA_PITCH_ALIGNMENT==0);
public:
    void CaptureIfNeeded(ID3D12Device* device,ID3D12GraphicsCommandList* list,ID3D12Resource* source,
                         const CameraKey& key,bool moving) {
        if(unavailable || (moving && !requested)) return;
        for(const auto& slot:slots)
            if((slot.recording || slot.fence) && slot.camera==key) { requested=false; return; }

        Snapshot* writable=nullptr;
        for(auto& slot:slots) {
            if(slot.recording || slot.fence>completed) continue;
            if(!writable || slot.fence<writable->fence) writable=&slot;
        }
        if(!writable) return; // Submitted readbacks may never be overwritten.
        auto& slot=*writable;
        if(!slot.buffer) {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width=static_cast<UINT64>(pitch)*ArenaCache::height;
            desc.Height=1; desc.DepthOrArraySize=1; desc.MipLevels=1; desc.SampleDesc.Count=1;
            desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            try { slot.buffer=Allocate(device,desc,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST); }
            catch(const std::exception&) { unavailable=true; return; }
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={source,0,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};
        from.pResource=source; from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=slot.buffer.Get(); to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint={DXGI_FORMAT_R32_TYPELESS,ArenaCache::width,ArenaCache::height,1,pitch};
        // D3D12 requires a full-subresource copy from a depth target.
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
        list->ResourceBarrier(1,&barrier);
        slot.camera=key; slot.recording=true; slot.fence=0; requested=false;
    }

    SurfaceHit Pick(Mode shown,const NavCamera& nav,float w,float h,NavPoint cursor) {
        if(unavailable || w<=0 || h<=0) return {};
        const auto origin=nav.Project(w,h,false,{});
        const float ppm=NavCamera::PixelsPerMetre(w,h,nav.zoom);
        const float right=(cursor.x-origin.x)/ppm, down=(cursor.y-origin.y)/ppm;
        // Zoom/pan do not change the ray direction. A stored view with that
        // direction can still supply its depth after the image was scaled.
        const Snapshot* best=nullptr;
        UINT x{},y{};
        for(const auto& slot:slots) {
            const auto& k=slot.camera;
            if(slot.recording || !slot.fence || slot.fence>completed ||
               k.mode!=shown || k.yaw!=nav.yaw || k.tilt!=nav.tilt) continue;
            const float u=.5f*(1+k.shift_x+right*k.sx);
            const float v=.5f*(1-k.shift_y+down*k.sy);
            if(!(u>=0 && u<1 && v>=0 && v<1)) continue;
            if(best && best->fence>=slot.fence) continue;
            best=&slot;
            x=std::min(k.width-1,static_cast<UINT>(u*k.width));
            y=std::min(k.height-1,static_cast<UINT>(v*k.height));
        }
        if(!best) { requested=true; return {{},true}; }
        const SIZE_T offset=static_cast<SIZE_T>(pitch)*y+sizeof(float)*x;
        D3D12_RANGE range{offset,offset+sizeof(float)};
        void* ptr=nullptr;
        if(FAILED(best->buffer->Map(0,&range,&ptr))) { unavailable=true; return {}; }
        float depth{};
        std::memcpy(&depth,static_cast<const unsigned char*>(ptr)+offset,sizeof(float));
        D3D12_RANGE no_writes{};
        best->buffer->Unmap(0,&no_writes);
        if(!(depth>=0 && depth<1.f)) return {};
        const float towards=(.5f-depth)*80.f;
        const float ct=std::cos(nav.tilt), st=std::sin(nav.tilt);
        const float height=towards*ct-down*st;
        // Reconstruct using the exact pointer XY rather than the centre of
        // the sampled depth texel, so this point projects back to the pointer.
        return {nav.PickAtHeight(w,h,false,cursor,height),false};
    }
    void DiscardUnsubmitted() {
        for(auto& slot:slots) if(slot.recording) { slot.recording=false; slot.camera={}; }
    }
    void Confirm(UINT64 submitted,UINT64 finished) {
        completed=finished;
        for(auto& slot:slots) if(slot.recording) {slot.fence=submitted;slot.recording=false;}
    }
    void Invalidate() { for(auto& slot:slots) slot.camera={}; requested=false; }
    void Release(bool safe) {
        if(!safe) for(auto& slot:slots) slot.buffer.Detach();
        *this={};
    }
};
SurfaceDepth surface_depth;
