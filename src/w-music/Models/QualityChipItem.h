#pragma once

#include "QualityChipItem.g.h"

namespace winrt::w_music::implementation
{
    /// One download tier of a 无损站 row (母带 / 环绕 / 无损 chip).
    struct QualityChipItem : QualityChipItemT<QualityChipItem>
    {
        QualityChipItem() = default;

        hstring Key() const noexcept { return m_key; }
        void Key(hstring const& value) noexcept { m_key = value; }

        hstring Label() const noexcept { return m_label; }
        void Label(hstring const& value) noexcept { m_label = value; }

    private:
        hstring m_key;
        hstring m_label;
    };
} // namespace winrt::w_music::implementation

namespace winrt::w_music::factory_implementation
{
    struct QualityChipItem : QualityChipItemT<QualityChipItem, implementation::QualityChipItem>
    {
    };
}
