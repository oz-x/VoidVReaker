#pragma once

namespace vvr {

// Loads the system version.dll and points every proxied export at it.
void proxy_resolve_exports();

}  // namespace vvr
