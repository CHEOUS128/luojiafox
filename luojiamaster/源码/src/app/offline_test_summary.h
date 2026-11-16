#pragma once

#include "platform/platform_backend.h"

namespace RoboGo::app::offline {

// Called after Shutdown as well as the final render sample: DrainForShutdown
// can itself add a timeout. Never infer a missing final value from old samples.
inline void WriteFinalRenderCounters(std::ostream &out,
                                    const platform::RenderDiagnostics &before_shutdown,
                                    const platform::RenderDiagnostics &final) {
    out << ",\n  \"backend_frames_submitted\": " << final.frames_submitted
        << ",\n  \"backend_uploads_succeeded\": " << final.uploads_succeeded
        << ",\n  \"backend_upload_failures\": " << final.upload_failures
        << ",\n  \"backend_recoveries\": " << final.recoveries
        << ",\n  \"backend_resource_creations\": " << final.resource_creations
        << ",\n  \"backend_deferred_max\": " << final.max_deferred_resources
        << ",\n  \"backend_wait_timeouts\": " << final.wait_timeouts
        << ",\n  \"backend_wait_timeouts_before_shutdown\": " << before_shutdown.wait_timeouts
        << ",\n  \"backend_diagnostics_phase\": \"after_shutdown\"";
}

} // namespace RoboGo::app::offline
