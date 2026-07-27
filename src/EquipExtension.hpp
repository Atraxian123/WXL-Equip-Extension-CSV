// wxl-equip-extension: equipment slot extension features for WarcraftXL.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once
#include "events/EventScript.hpp"

namespace wxl::scripts::equipextension
{
    class EquipExtension : public wxl::events::EventScript
    {
    public:
        EquipExtension();

    private:
        void OnItemSlotChange(const wxl::events::ItemSlotChangeArgs& a);
        void OnItemSlotClear(const wxl::events::ItemSlotClearArgs& a);
        void OnM2SkinFinalize(const wxl::events::M2SkinFinalizeArgs& a);
        void OnM2PerFrameUpdate(const wxl::events::M2PerFrameUpdateArgs& a);
        void OnBuildBonePalette(const wxl::events::BuildBonePaletteArgs& a);
		void OnWeaponVisualChange(const wxl::events::WeaponVisualChangeArgs& a);	//ATRAXIAN
        void OnItemDisplayLookup(const wxl::events::ItemDisplayLookupArgs& a);
    };
}
