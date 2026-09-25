#pragma once

#include "RecommendPage.g.h"

namespace winrt::w_music::implementation
{
    struct RecommendPage : RecommendPageT<RecommendPage>
    {
        RecommendPage();

        winrt::w_music::RecommendViewModel ViewModel() const;

        // XAML 里直接挂的处理器必须 public：生成的 RecommendPageT 基类
        // (Connect/显式实例化) 够不到子类的 private 成员（见 MainWindow.h）。
        void OnLoaded(winrt::Windows::Foundation::IInspectable const& sender,
                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnUnloaded(winrt::Windows::Foundation::IInspectable const& sender,
                        winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnTextQueryKeyDown(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
        void OnLikeClick(winrt::Windows::Foundation::IInspectable const& sender,
                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnDislikeClick(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

    private:
        void OnViewModelPropertyChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                        winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs const& args);
        void OnItemClick(winrt::Windows::Foundation::IInspectable const& sender,
                         winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& args);
        /// Category chips are built in code, not bound: the engine's list grows
        /// past one row (presets + discovered), so each row needs its own
        /// wrapping panel and the chips carry provenance the template cannot
        /// express.
        void RebuildChips();
        /// Accent-fills the chips of the selected category.
        void UpdateChipStyles();
        /// Sends whatever is typed in the free-text box to the engine.
        void SubmitTextQuery();
        /// One chip for |category|; its label shows the member count for
        /// discovered categories and its tooltip carries the engine's note.
        winrt::Microsoft::UI::Xaml::Controls::Button MakeChip(winrt::w_music::CategoryItem const& category) const;
        /// The bound row object of a template button (RecommendItem).
        static winrt::Windows::Foundation::IInspectable ItemOf(winrt::Windows::Foundation::IInspectable const& sender);

        winrt::event_token m_propertyToken{};
    };
}

namespace winrt::w_music::factory_implementation
{
    struct RecommendPage : RecommendPageT<RecommendPage, implementation::RecommendPage>
    {
    };
}
