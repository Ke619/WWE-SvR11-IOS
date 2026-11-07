// WWE SmackDown vs. Raw 2011 - which graphics API this PC can draw the game with.

#pragma once

#include <filesystem>
#include <string>

namespace svr2011 {

// gpu_backend "any" on Windows: "d3d12" when Direct3D 12 can run the native
// renderer, else "vulkan" when Vulkan can, else "d3d11" (older GPUs and
// drivers). `why` gets the reasons the better ones were passed over (empty
// for "d3d12"). The probes (a device each, ~0.4 s) run when the GPU or its
// driver changed: the answer is kept in `cache_file` with their ids.
// `cached` tells whether it came from there.
std::string PickGraphicsApi(const std::filesystem::path& cache_file, std::string* why, bool* cached);

// The API "any" picked failed on this GPU and driver (the device lost in the native renderer's first minute - a
// GeForce GT 710 on Direct3D 12): the cache is rewritten so the next start uses the next one (Vulkan when it can run,
// else Direct3D 11). Returns its name for the message ("Vulkan" / "Direct3D 11"), empty when "any" wasn't used or
// Direct3D 11 itself failed.
std::string AvoidPickedGraphicsApi(const std::string& reason);

}  // namespace svr2011
