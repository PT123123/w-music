#pragma once

#include "App.g.h"

namespace winrt::w_music::implementation
{
    struct App : AppT<App>
    {
        App();

        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const& args);

    private:
        Microsoft::UI::Xaml::Window m_window{ nullptr };
    };
}

// No factory_implementation::App: App is deliberately not activatable (see
// App.idl). The generated wWinMain constructs it directly with
// winrt::make<implementation::App>(), so no activation factory is involved and
// cppwinrt emits no AppT factory template to specialise.
