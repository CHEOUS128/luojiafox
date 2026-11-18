#pragma once
#include "app/application_types.h"

namespace RoboGo::platform { class PlatformBackend; }
namespace RoboGo::app {
struct SessionChoice {
    Competition competition = Competition::Ul2027;
    bool local_trial = false;
};
std::optional<SessionChoice> RunCompetitionLauncher(platform::PlatformBackend &backend);

// Legacy offline window diagnostics, reachable only by an explicit maintenance argument.
enum class EntryScreen { Welcome, CheckUl, CheckUc };
int RunUlPreview(platform::PlatformBackend &backend, EntryScreen entry);
}
