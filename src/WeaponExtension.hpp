// wxl-weapon-extension: sidecar-driven weapon model/texture baking for WarcraftXL.
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

namespace wxl::scripts::weaponextension
{
    class WeaponExtension : public wxl::events::EventScript
    {
    public:
        WeaponExtension();

    private:
        // Not model-load-related at all -- reused purely as the earliest possible, unconditional
        // trigger to kick the one-time sidecar load off. Identical role, and identical reasoning, to
        // CreatureExtension::OnModelLoadPre (see that class's doc comment for the full explanation of
        // why OnModelLoadPre and not something equip/world-specific): OnModelLoadPre fires for ANY
        // model init anywhere in the client, including the login/char-select glue scene's own preview
        // models -- weapons held by a char-select preview character render there too, well before the
        // client ever enters the actual game world or fires a single equip-slot event.
        //
        // This is WeaponExtension's ONLY job: kick off sidecar loading, which itself decides (via
        // WXLExtendedEquipment.ini's [EagerPreload] Weapons key) whether to also eagerly bake every
        // sidecar-known displayId's Model1/Model2 virtual .m2 bytes into the process-lifetime override
        // table ahead of time. Either way, VirtualPath.cpp's VirtualProvide falls back to a lazy
        // resolver (WeaponLazyResolve, registered once from this class's constructor) on any displayId
        // that wasn't already baked -- decoding the exact virtual name the loader just missed on and
        // baking it on the spot, before that miss is ever handed back.
        //
        // Unlike EquipExtension's older weapon path, there is deliberately no equip-time hook here at
        // all: ItemDisplayInfo's Model1/Model2 fields are patched at the data level (outside this
        // module, in the same spirit as CreatureModelData::ModelName) to already name the correct
        // per-displayId virtual path directly -- e.g. "Weapon\Woodcutteraxe\Woodcutteraxe_53211.mdx"
        // instead of "Weapon\Woodcutteraxe\Woodcutteraxe.mdx" -- so the native loader always just asks
        // for the right file on its own, eager or lazy, exactly like a creature's ModelName field does.
        // This only works because weapons have no per-wearer race/gender model variants to resolve at
        // equip time (unlike the Icon2 flag 0x80 case the old EquipExtension weapon path had to
        // account for) -- there is truly one fixed model per displayId, so a static DBC edit is enough
        // and no reactive substitution hook is needed.
        void OnModelLoadPre(const wxl::events::ModelLoadArgs& a);
    };
}
