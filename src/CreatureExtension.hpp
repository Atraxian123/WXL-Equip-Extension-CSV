// wxl-creature-extension: sidecar-driven creature retexturing for WarcraftXL.
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

namespace wxl::scripts::creatureextension
{
    class CreatureExtension : public wxl::events::EventScript
    {
    public:
        CreatureExtension();

    private:
        void OnCreatureModelResolve(const wxl::events::CreatureModelResolveArgs& a);

        // Not a creature-related handler at all -- used purely as a trigger to kick the one-time
        // sidecar load + eager preload off as early as possible, decoupled from any specific
        // creature's own resolve. OnItemSlotChange is the proven-early choice: it's confirmed to be
        // the very first event in the log -- firing for the head slot before any weapon-specific
        // event exists, ahead of any full RebuildAllModels/OnM2SkinFinalize/PerFrame cycle -- well
        // before any creature could possibly need to resolve. No item/equip-related work happens
        // here; the args are ignored.
        void OnItemSlotChange(const wxl::events::ItemSlotChangeArgs& a);
    };
}
