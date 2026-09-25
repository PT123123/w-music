#pragma once

#include "CategoryItem.g.h"

namespace winrt::w_music::implementation
{
    /// One category of the recommendation engine. The engine now has three
    /// sources behind one list (configs/categories.yaml presets, `auto-*`
    /// clusters discovered in the current library, and the free-text entry
    /// that reuses them), and every entry carries its own provenance so the
    /// UI can show a name without hiding where it came from.
    struct CategoryItem : CategoryItemT<CategoryItem>
    {
        CategoryItem() = default;

        hstring Id() const noexcept { return m_id; }
        void Id(hstring const& value) noexcept { m_id = value; }

        hstring Label() const noexcept { return m_label; }
        void Label(hstring const& value) noexcept { m_label = value; }

        hstring Source() const noexcept { return m_source; }
        void Source(hstring const& value) noexcept { m_source = value; }

        bool IsDiscovered() const noexcept { return m_source == L"discovered"; }

        hstring Note() const noexcept { return m_note; }
        void Note(hstring const& value) noexcept { m_note = value; }

        bool HasNote() const noexcept { return !m_note.empty(); }

        int32_t Support() const noexcept { return m_support; }
        void Support(int32_t value) noexcept { m_support = value; }

        hstring SupportText() const noexcept { return m_supportText; }
        void SupportText(hstring const& value) noexcept { m_supportText = value; }

    private:
        hstring m_id;
        hstring m_label;
        hstring m_source{ L"preset" };
        hstring m_note;
        int32_t m_support = 0;
        hstring m_supportText;
    };
} // namespace winrt::w_music::implementation

namespace winrt::w_music::factory_implementation
{
    struct CategoryItem : CategoryItemT<CategoryItem, implementation::CategoryItem>
    {
    };
}
