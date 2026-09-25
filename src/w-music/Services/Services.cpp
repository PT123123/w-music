#include "pch.h"

#include "Services/LibraryService.h"
#include "Services/OnlineProviderService.h"
#include "Services/RecommendService.h"
#include "Services/Services.h"

#include "ViewModels/LibraryViewModel.h"
#include "ViewModels/PlayerViewModel.h"
#include "ViewModels/RecommendViewModel.h"

namespace wm::app
{
    LibraryService& Library()
    {
        static LibraryService instance;
        return instance;
    }

    OnlineProviderService& Online()
    {
        static OnlineProviderService instance;
        return instance;
    }

    RecommendService& Recommend()
    {
        static RecommendService instance;
        return instance;
    }

    winrt::w_music::PlayerViewModel Player()
    {
        static auto instance = winrt::make<winrt::w_music::implementation::PlayerViewModel>();
        return instance;
    }

    winrt::w_music::LibraryViewModel LibraryVm()
    {
        static auto instance = winrt::make<winrt::w_music::implementation::LibraryViewModel>();
        return instance;
    }

    winrt::w_music::RecommendViewModel RecommendVm()
    {
        static auto instance = winrt::make<winrt::w_music::implementation::RecommendViewModel>();
        return instance;
    }
}
