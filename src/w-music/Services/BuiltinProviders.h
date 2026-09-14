#pragma once

/// Adapters for freely licensed, openly accessible music sources that ship
/// inside the binary so the online tab works with zero configuration.
///
/// Only sources whose content is explicitly licensed for redistribution are
/// built in. Anything else (personal backends, NAS gateways, ...) stays a
/// user supplied JSON adapter under %LOCALAPPDATA%\w-music\providers -- an
/// external adapter with the same "id" overrides the built-in one.

#include "pch.h"

#include <wm/core/ProviderAdapter.h>

#include <vector>

namespace wm::app
{
    /// Parsed built-in adapters, in display order. Invalid entries are skipped.
    std::vector<wm::core::ProviderAdapter> BuiltinAdapters();
} // namespace wm::app
