#pragma once

#include "pch.h"

#include "ViewModels/LibraryViewModel.h"
#include "ViewModels/PlayerViewModel.h"

namespace wm::app
{
    class LibraryService;
    class OnlineProviderService;

    /// Process-wide singletons. Everything is created lazily on first use, and
    /// the view models are shared by every page so transport state stays in sync.
    LibraryService& Library();
    OnlineProviderService& Online();
    winrt::w_music::PlayerViewModel Player();
    winrt::w_music::LibraryViewModel LibraryVm();
}
