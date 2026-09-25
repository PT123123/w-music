#include "pch.h"

#include "Views/RecommendPage.h"
#include "Views/RecommendPage.g.cpp"

#include "Services/Services.h"
#include "ViewModels/RecommendViewModel.h"

using namespace winrt;
using namespace Windows::Foundation;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace winrt::w_music::implementation
{
    RecommendPage::RecommendPage()
    {
        InitializeComponent();

        RefreshButton().Click([](auto&&, auto&&) { wm::app::RecommendVm().RefreshFeedAsync(); });
        ShuffleButton().Click([](auto&&, auto&&) { wm::app::RecommendVm().ShuffleFeedAsync(); });
        SimilarButton().Click([](auto&&, auto&&) { wm::app::RecommendVm().LoadSimilarNowAsync(); });
        AnalyzeButton().Click([](auto&&, auto&&) { wm::app::RecommendVm().AnalyzeLibraryAsync(); });
        ResetButton().Click([](auto&&, auto&&) { wm::app::RecommendVm().ResetTasteAsync(); });
        TextQueryButton().Click([this](auto&&, auto&&) { SubmitTextQuery(); });
        RecommendList().ItemClick({ this, &RecommendPage::OnItemClick });
    }

    winrt::w_music::RecommendViewModel RecommendPage::ViewModel() const
    {
        return wm::app::RecommendVm();
    }

    void RecommendPage::OnLoaded(IInspectable const&, RoutedEventArgs const&)
    {
        m_propertyToken = ViewModel().PropertyChanged({ this, &RecommendPage::OnViewModelPropertyChanged });
        RebuildChips();
        UpdateChipStyles();
        // The view model initializes once per app run (guard inside); later
        // visits to this page just show the current state.
        wm::app::RecommendVm().InitializeAsync();
    }

    void RecommendPage::OnUnloaded(IInspectable const&, RoutedEventArgs const&)
    {
        ViewModel().PropertyChanged(m_propertyToken);
    }

    void RecommendPage::OnViewModelPropertyChanged(IInspectable const&,
                                                   Microsoft::UI::Xaml::Data::PropertyChangedEventArgs const& args)
    {
        if (args.PropertyName() == L"SelectedCategoryId")
        {
            UpdateChipStyles();
        }
        else if (args.PropertyName() == L"Categories")
        {
            // The engine re-lists categories after a library analysis: the
            // discovered ones are a function of the library.
            RebuildChips();
            UpdateChipStyles();
        }
    }

    Button RecommendPage::MakeChip(winrt::w_music::CategoryItem const& category) const
    {
        Button chip;
        chip.Tag(category);
        chip.Padding(ThicknessHelper::FromLengths(14, 6, 14, 6));
        chip.CornerRadius(CornerRadiusHelper::FromUniformRadius(15));
        chip.Margin(ThicknessHelper::FromLengths(0, 0, 8, 8));
        chip.FontSize(13);

        std::wstring label{ category.Label() };
        if (!category.SupportText().empty())
        {
            label += L" · " + std::wstring{ category.SupportText() };
        }
        if (category.HasNote())
        {
            ToolTipService::SetToolTip(chip, box_value(hstring{ category.Note() }));
        }
        // Content is a plain string: the label is short and the chip needs no
        // template of its own.
        chip.Content(box_value(hstring{ label }));

        auto const categoryCopy = category;
        chip.Click([categoryCopy](IInspectable const&, RoutedEventArgs const&)
            { wm::app::RecommendVm().SelectCategoryAsync(categoryCopy); });
        return chip;
    }

    void RecommendPage::RebuildChips()
    {
        auto const& panelPreset = PresetChips().Children();
        auto const& panelAuto = AutoChips().Children();
        panelPreset.Clear();
        panelAuto.Clear();
        int discovered = 0;
        for (auto const& category : ViewModel().Categories())
        {
            if (category.IsDiscovered())
            {
                panelAuto.Append(MakeChip(category));
                ++discovered;
            }
            else
            {
                panelPreset.Append(MakeChip(category));
            }
        }
        // Keep the row when the engine explained why it produced none -- the
        // "too small to cluster honestly" line is the useful half of a failed
        // discovery, not something to collapse away.
        bool const hasCaption = !std::wstring{ ViewModel().DiscoveryText() }.empty();
        AutoCategorySection().Visibility(discovered > 0 || hasCaption
            ? Visibility::Visible
            : Visibility::Collapsed);
    }

    void RecommendPage::SubmitTextQuery()
    {
        wm::app::RecommendVm().SearchByTextAsync(TextQueryBox().Text());
    }

    void RecommendPage::OnTextQueryKeyDown(IInspectable const&, Input::KeyRoutedEventArgs const& args)
    {
        if (args.Key() == Windows::System::VirtualKey::Enter)
        {
            SubmitTextQuery();
            args.Handled(true);
        }
    }

    void RecommendPage::OnLikeClick(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (auto item = ItemOf(sender).try_as<winrt::w_music::RecommendItem>())
        {
            wm::app::RecommendVm().LikeItem(item);
        }
    }

    void RecommendPage::OnDislikeClick(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (auto item = ItemOf(sender).try_as<winrt::w_music::RecommendItem>())
        {
            wm::app::RecommendVm().DislikeItem(item);
        }
    }

    void RecommendPage::OnItemClick(IInspectable const&, ItemClickEventArgs const& args)
    {
        if (auto item = args.ClickedItem().try_as<winrt::w_music::RecommendItem>())
        {
            wm::app::RecommendVm().PlayItem(item);
        }
    }

    winrt::Windows::Foundation::IInspectable RecommendPage::ItemOf(IInspectable const& sender)
    {
        // Buttons inside a DataTemplate carry the bound row as DataContext.
        if (auto element = sender.try_as<FrameworkElement>())
        {
            return element.DataContext();
        }
        return nullptr;
    }

    void RecommendPage::UpdateChipStyles()
    {
        std::wstring const selected{ ViewModel().SelectedCategoryId() };

        // WmAccentBrush lives in the app-level theme dictionaries (not the
        // root resources); grab the shared instance so theme changes keep
        // applying to the chips as well.
        Media::Brush accentBrush{ nullptr };
        auto resources = Application::Current().Resources();
        for (wchar_t const* key : { L"Dark", L"Light" })
        {
            auto const dictionaryKey = box_value(hstring{ key });
            if (!resources.ThemeDictionaries().HasKey(dictionaryKey))
            {
                continue;
            }
            auto dictionary = resources.ThemeDictionaries().Lookup(dictionaryKey).try_as<ResourceDictionary>();
            if (dictionary == nullptr)
            {
                continue;
            }
            auto const brushKey = box_value(hstring{ L"WmAccentBrush" });
            if (dictionary.HasKey(brushKey))
            {
                accentBrush = dictionary.Lookup(brushKey).try_as<Media::SolidColorBrush>();
            }
            if (accentBrush != nullptr)
            {
                break;
            }
        }

        // 本地 null 会压掉样式里的默认前景色，文字会变成"看不见"——所以未选中
        // 态也给显式画刷（与发现页搜索框同一档玻璃感），不用 nullptr 回退。
        Media::Brush const idleBackground = Media::SolidColorBrush{ Windows::UI::Color{ 0x26, 0xFF, 0xFF, 0xFF } };
        Media::Brush const idleForeground = Media::SolidColorBrush{ Windows::UI::Colors::White() };

        auto styleChip = [&](Button const& button)
        {
            auto const category = button.Tag().try_as<winrt::w_music::CategoryItem>();
            if (category != nullptr && std::wstring{ category.Id() } == selected && accentBrush != nullptr)
            {
                button.Background(accentBrush);
                button.Foreground(Media::SolidColorBrush{ Windows::UI::Colors::White() });
            }
            else
            {
                button.Background(idleBackground);
                button.Foreground(idleForeground);
            }
        };

        for (auto const& child : PresetChips().Children())
        {
            if (auto button = child.try_as<Button>())
            {
                styleChip(button);
            }
        }
        for (auto const& child : AutoChips().Children())
        {
            if (auto button = child.try_as<Button>())
            {
                styleChip(button);
            }
        }
    }
}
