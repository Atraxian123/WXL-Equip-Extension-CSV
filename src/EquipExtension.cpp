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

#include "EquipExtension.hpp"
#include "VirtualPath.hpp"
#include "core/Logger.hpp"
#include "events/Event.hpp"
#include "game/Binding.hpp"
#include "game/io/Io.hpp"
#include "game/m2/M2.hpp"
#include "game/unit/Unit.hpp"
#include "offsets/engine/Io.hpp"
#include "offsets/game/DB2.hpp"
#include "offsets/game/M2.hpp"
#include "offsets/game/Unit.hpp"
#include "runtime/GameHooks.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::scripts::equipextension
{
    namespace ev   = wxl::events;
    namespace m2   = wxl::offsets::game::m2;
    namespace db2  = wxl::offsets::game::db2;
    namespace unit = wxl::offsets::game::unit;
    namespace gm2  = wxl::game::m2;
    using wxl::game::Native;

    // ─── Data types ──────────────────────────────────────────────────────────────

    // List of skinSectionIds to show. count == 0 means "show all" (no filter applied).
    // WoW geoset IDs are two-part values (e.g. 1301 = group 13, option 01).
    struct GeosetFilter
    {
        uint16_t ids[16];
        uint32_t count;
    };

    // Maps collection bone index → character bone index (0xFF = unmatched).
    // Built by 3-pass matching: (1a) BoneNameCRC, (1b) key_bone_id raw scan, (1c) BoneIndicesByID LUT,
    //  then (2) parent-chain propagation.
    // Retail collection M2s (full-body chest pieces etc.) routinely have 60-100+ bones;
    // 48 was too small and caused the early-exit, leaving count=0 for all collection models.
    struct BoneRemap
    {
        uint16_t count;
        uint8_t  collToChar[256];


    };

    struct SidecarModelEntry
    {
        uint32_t modelSlot    = static_cast<uint32_t>(-1);
        uint32_t attachId     = static_cast<uint32_t>(-1);
        uint32_t modelFlags   = 0x2 | 0x4; // exact path unless the sidecar asks for race/gender suffixes
        uint32_t textureFlags = 0;
        char     folder[32]   = {};
        char     model[264]   = {};
        char     texture[264] = {};
        GeosetFilter geoFilter = {};
    };

    struct SidecarMaterialEntry
    {
        uint32_t modelIndex  = static_cast<uint32_t>(-1);
        uint32_t modelColumn = static_cast<uint32_t>(-1);
        uint32_t textureType = static_cast<uint32_t>(-1);
        char     folder[32]  = {};
        char     model[264]  = {};
        char     texture[264] = {};
        char     targetMode[32] = {}; // only "Hide" is consulted now, to skip a row entirely
    };

    // One attachment entry per attached M2. Keyed by CharModelObject pointer in g_attached.
    struct AttachEntry
    {
        uint32_t    equipSlot = static_cast<uint32_t>(-1); // internal model slot (0-10)
        uint32_t    attachId  = static_cast<uint32_t>(-1); // WoW attachment point id
        void*       subObj    = nullptr; // cmo->sceneNode stamped at last rebuild
        void*       renderCtx = nullptr; // live render context; null = evicted/dead
        char        keyBuf[264] = {};        // real M2 path (used for matching/dedup)
        char        mangledKeyBuf[264] = {}; // virtual _wxl_ path for GetRenderCtx (collection only)
        char        texBuf[264] = {};    // BLP path for BindTexSlot on re-attach
        char        matTexBuf[2048] = {}; // TextureType-keyed material texture patches for virtual M2 bytes
        GeosetFilter geoFilter  = {};
        BoneRemap    boneRemap  = {};
        uint32_t    mergeKey = 0; // non-zero keeps logical semicolon collection models separate
        bool        perFrameLogged    = false; // suppress repeated per-frame noise after first log
        bool        charSweepApplied = false; // set when character-model PerFrame sweep first copies bones
        bool        bbpLogDone       = false; // suppress OnBuildBonePalette remap dump after first fire
        uint16_t    texAnimProbeFrames = 0; // short targeted UV-animation diagnostic
        uint32_t    texAnimProbeFirst[4] = {};
        uint8_t     texAnimProbeChanged = 0;											
    };

    // ─── Weapon attachment scaffold ─────────────────────────────────────────────
    //
    // Weapons have no wrapper 'cmo' (see OnItemSlotChange / hkSlotDispatch) -- OnWeaponVisualChange
    // hands us unit->model, which is already equal to cmo->sceneNode ('subObj' in the armor path,
    // confirmed by direct comparison against a live OnItemSlotChange hit on the same character).
    // So this map is keyed by subObj directly, one step ahead of g_attached's cmo key, and its
    // matching logic (see the OnM2PerFrameUpdate-equivalent sweep, still TODO) compares subObj
    // against renderCtx directly -- no cmo->sceneNode indirection needed, we already start there.
    struct WeaponAttachEntry
    {
        uint32_t slot      = static_cast<uint32_t>(-1); // 0=mainhand, 1=offhand, 2=ranged
        uint32_t itemEntry = 0;                          // raw item entry id (0 = slot empty)
        void*    subObj    = nullptr;                    // == unit->model; stamped at last update
        void*    owner28   = nullptr;                    // *(subObj + kOffSceneNodeOwner); "real CMO"
        void*    renderCtx = nullptr;                     // TODO: set once a real model path is loaded
    };

    // subObj (== unit->model) -> weapon attach state for that character/unit.
    // std::array, not a raw C-array: unordered_map's value type needs to be movable/assignable
    // for operator[]/insert, which a raw T[3] is not.
    static std::unordered_map<void*, std::array<WeaponAttachEntry, 3>> g_weaponAttached; // [0]=main [1]=off [2]=ranged

    // ─── File-scope state ─────────────────────────────────────────────────────────

    // cmo (CharModelObject*) → attached M2 entries for that character
    static std::unordered_map<void*, std::vector<AttachEntry>> g_attached;

    static bool g_sidecarLoaded = false;
    static std::unordered_map<uint32_t, std::vector<SidecarModelEntry>> g_sidecarModels;
    static std::unordered_map<uint32_t, std::vector<SidecarMaterialEntry>> g_sidecarMaterials;

    // Optional itemEntry -> ItemDisplayInfo displayId override table, loaded from
    // WXLItemEntryDisplay.csv. Exists because the client's own in-memory Item.dbc storage layout
    // (db2::item) is NOT reliably parseable from outside the client (see LookupItemDisplayId) --
    // this table lets a mapper supply known-correct entry->displayId pairs directly (e.g. pulled
    // from their own item database/SQL dump) instead of depending on that guesswork. Checked first;
    // the raw-memory lookup is only a fallback for entries not listed here.
    static std::unordered_map<uint32_t, uint32_t> g_sidecarItemDisplay;

    // Optional ItemDisplayInfo displayId -> model folder override, from WXLItemEntryDisplay.csv's
    // optional third column (e.g. "Weapon", "Shield"). Consulted by PatchWeaponModelByDisplayId
    // ahead of its own Weapon/Shield guesses -- see that function and LoadItemDisplaySidecarFile's
    // doc comments. Keyed by displayId (not itemEntry): the folder is a property of the model file
    // a displayId points at, so two itemEntries sharing a displayId share its folder too.
    static std::unordered_map<uint32_t, std::string> g_sidecarModelFolder;

    // ItemDisplayInfo displayId -> virtual model path per column (0=Model1, 1=Model2), populated by
    // PatchWeaponModelByDisplayId right after a successful VPathPopulateGlobal registration. Consumed
    // by EquipExtension::OnItemDisplayLookup (subscribed to GameHooks' OnItemDisplayLookup) to
    // substitute Model1/Model2 for every future native lookup of that displayId -- see the doc
    // comment on ItemDisplayLookupArgs in Event.hpp. Process-lifetime, same as g_globalOverrides in
    // VirtualPath.cpp: once a displayId's column has a virtual path it stays for the session.
    struct WeaponVPathOverride { std::string vModel[2]; };
    static std::unordered_map<uint32_t, WeaponVPathOverride> g_weaponVPaths;

    // Set to the cmo being processed in RebuildAllModels Phase1. OnM2SkinFinalize uses
    // this to apply the filter during a synchronous load that fires kFinalizeSkin inside
    // GetRenderCtx, before Phase1 has set entry.renderCtx.
    static void* g_currentRebuildCmo = nullptr;

    // ─── Slot config ──────────────────────────────────────────────────────────────

    struct SlotConfig
    {
        const char* folder;
        uint32_t    defAttach1;
        uint32_t    defAttach2;
        bool        defUseRace;
        bool        defUseGender;
    };

    // Indexed by internal model slot (0-13), matching sub_4f2640's first arg.
    //
    // Slots 11-13 (weapons) are new: unlike armor slots, the client already natively
    // attaches the weapon's own ModelName_1/2 at these attach points, so these entries
    // mainly matter for (a) sidecar-driven material patches on the native weapon model
    // and (b) any *additional* attached M2 the sidecar/DBC asks for. Default attach IDs
    // follow the classic ItemVisual convention: 1=HandRight (mainhand), 2=HandLeft
    // (offhand/shield), 3/4=ElbowRight/Left, dual-purposed as the ranged weapon + quiver
    // attach points. Verify against WXL_EQUIP_LOG output for your client build; the
    // upstream WarcraftXL core hook must also dispatch OnItemSlotChange/OnItemSlotClear
    // for these WoW equip slots (15/16/17) with a matching modelSlot value for any of
    // this to fire at all -- see the caveat in the PR/README notes.
    static const SlotConfig kSlotConfig[14] = {
        { "Head",     11,  55, true,  true  }, // 0  HEAD
        { "Shoulder",  6,   5, false, false }, // 1  SHOULDER (Model1=left/6, Model2=right/5)
        { "Shirt",    34,  34, false, false }, // 2  SHIRT
        { "Chest",    34,  34, false, false }, // 3  CHEST
        { "Waist",    53,  53, false, false }, // 4  WAIST
        { "Leg",       9,  10, false, false }, // 5  LEGS
        { "Foot",     47,  48, false, false }, // 6  FEET
        { "Bracer",    3,   4, false, false }, // 7  WRIST
        { "Glove",     1,   2, false, false }, // 8  HAND
        { "Cape",     12,  12, false, false }, // 9  BACK
        { "Tabard",   34,  34, false, false }, // 10 TABARD
        { "Weapon",    1,   1, false, false }, // 11 MAINHAND (HandRight_ItemVisual1)
        { "OffHand",    2,   2, false, false }, // 12 OFFHAND  (HandLeft_ItemVisual2, also shields)
        { "Ranged",    3,   4, false, false }, // 13 RANGED   (bow/gun=3, quiver/ammo=4)
    };
    constexpr uint32_t kCollectionAttach = 19;
    constexpr uint32_t kFlagForceModelRaceGender = 0x80;

    // WoW C++ equip slot (0-18) → internal model slot. UINT32_MAX = not handled here.
    static const uint32_t kEquipToModelSlot[19] = {
        0,                       // 0  HEAD
        static_cast<uint32_t>(-1), // 1  NECK
        1,                       // 2  SHOULDER
        2,                       // 3  SHIRT
        3,                       // 4  CHEST
        4,                       // 5  WAIST
        5,                       // 6  LEGS
        6,                       // 7  FEET
        7,                       // 8  WRIST
        8,                       // 9  HAND
        static_cast<uint32_t>(-1), // 10 deferred (FINGER1)
        static_cast<uint32_t>(-1), // 11 deferred (FINGER2)
        static_cast<uint32_t>(-1), // 12 deferred (TRINKET1)
        static_cast<uint32_t>(-1), // 13 deferred (TRINKET2)
        9,                       // 14 BACK
        15,                      // 15 MAINHAND
        16,                      // 16 OFFHAND
        17,                      // 17 RANGED
        10,                      // 18 TABARD
    };

    // ─── Debug logging ───────────────────────────────────────────────────────────

    static bool EquipLogEnabled() noexcept
    {
        static int enabled = []() noexcept -> int {
#pragma warning(suppress: 4996)
            const char* env = std::getenv("WXL_EQUIP_LOG");
            if (env && *env && *env != '0' && *env != 'n' && *env != 'N')
                return 1;

#pragma warning(suppress: 4996)
            FILE* flag = std::fopen("WarcraftXL_equip.log.enable", "rb");
            if (!flag) return 0;
            std::fclose(flag);
            return 1;
        }();
        return enabled != 0;
    }

    // Verbose equip diagnostics are opt-in. They touch very hot model/skin/per-frame paths, and opening
    // + flushing the log for every line can dominate frame time on HD-equipment-heavy scenes.
    static void EquipLog(const char* fmt, ...) noexcept
    {
        if (!EquipLogEnabled()) return;
#pragma warning(suppress: 4996)
        FILE* f = std::fopen("WarcraftXL_equip.log", "a");
        if (!f) return;
        va_list ap; va_start(ap, fmt);
        std::vfprintf(f, fmt, ap);
        va_end(ap);
        std::fputc('\n', f);
        std::fclose(f);
    }

    static bool SameAttachModelGroup(const AttachEntry& a, const AttachEntry& b) noexcept
    {
        const bool aFiltered = a.geoFilter.count > 0;
        const bool bFiltered = b.geoFilter.count > 0;
        return a.mergeKey == b.mergeKey &&
               a.attachId == b.attachId &&
               aFiltered == bFiltered &&
               std::strcmp(a.keyBuf, b.keyBuf) == 0 &&
               std::strcmp(a.texBuf, b.texBuf) == 0 &&
               std::strcmp(a.matTexBuf, b.matTexBuf) == 0;
    }

    // ─── SEH helpers (no C++ objects — safe to use __try/__except) ───────────────

    static uint32_t GuardedReadU32(const void* ptr) noexcept
    {
        uint32_t v = 0;
        __try { v = *static_cast<const uint32_t*>(ptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return v;
    }

    static void* GuardedReadPtr(const void* addr) noexcept
    {
        void* v = nullptr;
        __try { v = *static_cast<void* const*>(addr); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return v;
    }

    static uint32_t GuardedMatrixHash(const void* matrix) noexcept
    {
        if (!matrix) return 0;
        uint32_t hash = 2166136261u;
        __try
        {
            const auto* words = static_cast<const uint32_t*>(matrix);
            for (uint32_t i = 0; i < 16; ++i)
                hash = (hash ^ words[i]) * 16777619u;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
													  
													 
        {
            return 0;
																						
								  
																									   
							   
												   
        }
        return hash;
    }

    static void ProbeRaidWarriorTextureAnimation(AttachEntry& entry, void* renderCtx) noexcept
    {
        if (entry.texAnimProbeFrames >= 1200 ||
            !std::strstr(entry.keyBuf, "raidwarriorprogenitor_d_01")) return;

        void* evaluated = GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) + 0xAC);
        void* matrices  = GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) + 0xB0);
        const uint32_t hashes[4] = {
            GuardedMatrixHash(evaluated),
            GuardedMatrixHash(evaluated ? reinterpret_cast<uint8_t*>(evaluated) + 0x40 : nullptr),
            GuardedMatrixHash(matrices),
            GuardedMatrixHash(matrices ? reinterpret_cast<uint8_t*>(matrices) + 0x40 : nullptr)
        };

        if (entry.texAnimProbeFrames == 0)
            std::memcpy(entry.texAnimProbeFirst, hashes, sizeof(hashes));
        else
            for (uint32_t i = 0; i < 4; ++i)
                if (hashes[i] && hashes[i] != entry.texAnimProbeFirst[i])
                    entry.texAnimProbeChanged |= static_cast<uint8_t>(1u << i);
																													
	 

        ++entry.texAnimProbeFrames;
        if (entry.texAnimProbeFrames == 1 || entry.texAnimProbeFrames == 30 ||
            entry.texAnimProbeFrames == 90 || entry.texAnimProbeFrames == 300 ||
            entry.texAnimProbeFrames == 600 || entry.texAnimProbeFrames == 900 ||
            entry.texAnimProbeFrames == 1200)
        {
            EquipLog("  TexAnimProbe frame=%u key='%s' eval=[%08X %08X] out=[%08X %08X] changed=0x%X",
                     static_cast<uint32_t>(entry.texAnimProbeFrames), entry.keyBuf,
                     hashes[0], hashes[1], hashes[2], hashes[3],
                     static_cast<uint32_t>(entry.texAnimProbeChanged));
																							   
											
        }
														  
    }

    static bool CopyRemappedBonesGuarded(uint8_t* dstBuf,
                                         const uint8_t* srcBuf,
                                         const BoneRemap& remap) noexcept
																	 
    {
							   
		 
									   
						
			 
							  
																										
			 
						
		 
																					   
        if (!dstBuf || !srcBuf || remap.count == 0 || remap.count > 256)
																
															  
																													
																												 
            return false;
        __try
        {
            for (uint32_t bi = 0; bi < remap.count; ++bi)
            {
                uint8_t ci = remap.collToChar[bi];
                if (ci == 0xFF) continue;

                std::memcpy(dstBuf + bi * m2::kBonePaletteStride,
                            srcBuf + ci * m2::kBonePaletteStride,
                            m2::kBonePaletteStride);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return true;
    }

    // SEH-only wrapper: no C++ objects in this function, __try is safe.
    // Catches access violations from garbage/dangling hash-table buckets — see RebuildAllModels.
    // owner28 = *(subObj + kOffSceneNodeOwner): the actual CharModelObject that owns the
    // M2 hash table — NOT the slot-dispatch 'cmo' from hkSlotDispatch, which is a different
    // (wrapper) object whose +4 hash-table pointer is always null.
    static void* SafeGetRenderCtx(void* owner28, const char* keyBuf) noexcept
    {
        void* result = nullptr;
        __try { result = gm2::GetRenderCtx(owner28, const_cast<char*>(keyBuf)); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return result;
    }

    // SEH wrapper for BuildBoneRemap's pointer walks.
    // BoneRemap is a POD struct (trivially destructible), so __try is safe inside this function.
    static BoneRemap BuildBoneRemapGuarded(void* collRc, void* charRc) noexcept
    {
        BoneRemap r = {};
        std::memset(r.collToChar, 0xFF, sizeof(r.collToChar));
        __try
        {
            auto* collBytes = reinterpret_cast<uint8_t*>(collRc);
            auto* charBytes = reinterpret_cast<uint8_t*>(charRc);

            void* collM2  = *reinterpret_cast<void**>(collBytes + m2::kOffInstModel);
            void* charM2  = *reinterpret_cast<void**>(charBytes + m2::kOffInstModel);
            if (!collM2 || !charM2) return r;


            // M2AnimData = raw M2 file buffer = M2FileHeader (at m2_inst+0x150)
            auto* collHdr = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(collM2) + m2::kOffModelHeader);
            auto* charHdr = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(charM2) + m2::kOffModelHeader);
            if (!collHdr || !charHdr) return r;

            uint32_t collN    = *reinterpret_cast<uint32_t*>(collHdr + m2::kOffHdrBoneCount);
            uint8_t* collBase = *reinterpret_cast<uint8_t**>(collHdr + m2::kOffHdrBoneArray);

            uint32_t charN    = *reinterpret_cast<uint32_t*>(charHdr + m2::kOffHdrBoneCount);
            uint8_t* charBase = *reinterpret_cast<uint8_t**>(charHdr + m2::kOffHdrBoneArray);

            uint32_t charLutN = *reinterpret_cast<uint32_t*>(charHdr + m2::kOffHdrBoneIdxLutCount);
            int16_t* charLut  = *reinterpret_cast<int16_t**>(charHdr + m2::kOffHdrBoneIdxLutPtr);

          //  EquipLog("  BuildBoneRemap: collN=%u charN=%u collBase=%p charBase=%p charLut=%p",
          //           collN, charN, collBase, charBase, charLut);
          //  if (!collBase || collN == 0 || collN > 256 || charN == 0 || charN > 256) return r;
            r.count = static_cast<uint16_t>(collN);

            // Pass 1a: bone name CRC match for remaining unmatched bones
            if (charBase)
            {
                for (uint32_t i = 0; i < collN; ++i)
                {
                    if (r.collToChar[i] != 0xFF) continue;
                    uint32_t collCrc = *reinterpret_cast<uint32_t*>(collBase + i * m2::kBoneStride + m2::kOffBoneNameCrc);
                    if (collCrc == 0) continue;
                    for (uint32_t j = 0; j < charN && j < 255; ++j)
                    {
                        uint32_t charCrc = *reinterpret_cast<uint32_t*>(charBase + j * m2::kBoneStride + m2::kOffBoneNameCrc);
                        if (charCrc == collCrc) { r.collToChar[i] = static_cast<uint8_t>(j); break; }
                    }
                }
            }


            // Pass 1b: match by key_bone_id scan of char bone array
            if (charBase)
            {
                for (uint32_t i = 0; i < collN; ++i)
                {
                    int32_t key = *reinterpret_cast<int32_t*>(collBase + i * m2::kBoneStride + m2::kOffBoneKeyId);
                    if (key < 0) continue;
                    for (uint32_t j = 0; j < charN && j < 255; ++j)
                    {
                        if (*reinterpret_cast<int32_t*>(charBase + j * m2::kBoneStride + m2::kOffBoneKeyId) == key)
                        {
                            r.collToChar[i] = static_cast<uint8_t>(j);
                            break;
                        }
                    }
                }
            }

            // Pass 1c: BoneIndicesByID LUT for unmatched bones
            if (charLut)
            {
                for (uint32_t i = 0; i < collN; ++i)
                {
                    if (r.collToChar[i] != 0xFF) continue;
                    int32_t key = *reinterpret_cast<int32_t*>(collBase + i * m2::kBoneStride + m2::kOffBoneKeyId);
                    if (key < 0 || static_cast<uint32_t>(key) >= charLutN) continue;
                    int16_t ci = charLut[key];
                    if (ci >= 0 && static_cast<uint32_t>(ci) < charN && ci < 255)
                        r.collToChar[i] = static_cast<uint8_t>(ci);
                }
            }

            // Accessory chains in paper-doll collection models can be deeper than eight bones.
            for (uint32_t pass = 0; pass < collN; ++pass)
            {
                bool changed = false;
                for (uint32_t i = 0; i < collN; ++i)
                {
                    if (r.collToChar[i] != 0xFF) continue;
                    int16_t parent = *reinterpret_cast<int16_t*>(collBase + i * m2::kBoneStride + m2::kOffBoneParent);
                    if (parent < 0 || static_cast<uint32_t>(parent) >= collN) continue;
                    if (r.collToChar[parent] != 0xFF)
                    {
                        r.collToChar[i] = r.collToChar[parent];
                        changed = true;
                    }
                }
                if (!changed) break;
            }

            // If no bone was matched at all, signal "not ready" by resetting count to 0.
            // This happens when charBase is null (character M2 not yet parsed, e.g. initFlags=0x40).
            // count=0 causes all callers' retry guards to re-fire until the character is ready.
            {
                uint32_t matched = 0;
                for (uint32_t bi = 0; bi < r.count; ++bi)
                    if (r.collToChar[bi] != 0xFF) ++matched;
                if (matched == 0) r.count = 0;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return r;
    }


    // ─── Parse helpers ────────────────────────────────────────────────────────────

    // flags: 0x1=hide geoset, 0x2=no gender, 0x4=no race,
    //        0x8=append race to tex, 0x10=append gender to tex,
    //        0x20=model in subfolder, 0x40=new _Hu_F suffix,
    //        0x80=force model race/gender suffix

    static GeosetFilter ParseGeosetFilter(const char* spec)
    {
        GeosetFilter f = {};
        if (!spec || !*spec) return f;
        const char* p = spec;
        while (*p && f.count < 16)
        {
            uint32_t v = 0;
            while (*p >= '0' && *p <= '9') v = v * 10 + static_cast<uint32_t>(*p++ - '0');
            f.ids[f.count++] = static_cast<uint16_t>(v);
            if (*p == ',') p++;
            else break;
        }
        return f;
    }

    static bool CopyListPart(const char* list, uint32_t index, char* out, size_t outSz)
    {
        if (outSz == 0) return false;
        out[0] = '\0';
        if (!list) return false;

        const char* part = list;
        uint32_t cur = 0;
        for (;;)
        {
            const char* end = part;
            while (*end && *end != ';') ++end;

            if (cur == index)
            {
                while (part < end && (*part == ' ' || *part == '\t')) ++part;
                while (end > part && (end[-1] == ' ' || end[-1] == '\t')) --end;
                size_t len = static_cast<size_t>(end - part);
                if (len >= outSz) len = outSz - 1;
                if (len) std::memcpy(out, part, len);
                out[len] = '\0';
                return true;
            }

            if (!*end) break;
            part = end + 1;
            ++cur;
        }
        return false;
    }

    static void CopyString(char* out, size_t outSz, const char* value) noexcept
    {
        if (!out || outSz == 0) return;
        out[0] = '\0';
        if (!value) return;
        std::strncpy(out, value, outSz - 1);
        out[outSz - 1] = '\0';
    }

    static std::string TrimCopy(std::string value)
    {
        size_t first = 0;
        while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) ++first;
        size_t last = value.size();
        while (last > first)
        {
            const char c = value[last - 1];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            --last;
        }
        return value.substr(first, last - first);
    }

    static std::string NormalizeCsvName(const std::string& value)
    {
        std::string out;
        out.reserve(value.size());
        for (char c : value)
        {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out.push_back(c);
        }
        return out;
    }

    static std::vector<std::string> ParseCsvLine(const char* line)
    {
        std::vector<std::string> fields;
        std::string field;
        bool quoted = false;
        for (const char* p = line; *p; ++p)
        {
            char c = *p;
            if (c == '\r' || c == '\n')
            {
                if (!quoted) break;
            }
            if (quoted)
            {
                if (c == '"')
                {
                    if (p[1] == '"') { field.push_back('"'); ++p; }
                    else quoted = false;
                }
                else field.push_back(c);
            }
            else
            {
                if (c == '"') quoted = true;
                else if (c == ',') { fields.push_back(TrimCopy(field)); field.clear(); }
                else field.push_back(c);
            }
        }
        fields.push_back(TrimCopy(field));
        return fields;
    }

    static int FindCsvColumn(const std::vector<std::string>& header, const char* name)
    {
        const std::string wanted = NormalizeCsvName(name);
        for (size_t i = 0; i < header.size(); ++i)
            if (NormalizeCsvName(header[i]) == wanted)
                return static_cast<int>(i);
        return -1;
    }

    static const char* CsvField(const std::vector<std::string>& row, int column) noexcept
    {
        if (column < 0 || static_cast<size_t>(column) >= row.size()) return "";
        return row[static_cast<size_t>(column)].c_str();
    }

    static bool ParseU32(const char* text, uint32_t* out) noexcept
    {
        if (!text || !*text || !out) return false;
        char* end = nullptr;
        unsigned long value = std::strtoul(text, &end, 10);
        if (end == text) return false;
        *out = static_cast<uint32_t>(value);
        return true;
    }

    static bool StartsWithCI(const char* s, const char* prefix) noexcept;
    static bool ContainsCI(const char* s, const char* needle) noexcept;

    static uint32_t ParseModelColumn(const char* value) noexcept
    {
        if (!value || !*value) return static_cast<uint32_t>(-1);
        uint32_t numeric = 0;
        if (ParseU32(value, &numeric))
        {
            if (numeric == 0 || numeric == 1) return numeric;
            if (numeric == 2) return 1;
            return static_cast<uint32_t>(-1);
        }

        if (ContainsCI(value, "ModelName_1") || ContainsCI(value, "ModelTexture_1") ||
            ContainsCI(value, "Model_1") || ContainsCI(value, "Texture_1"))
            return 0;
        if (ContainsCI(value, "ModelName_2") || ContainsCI(value, "ModelTexture_2") ||
            ContainsCI(value, "Model_2") || ContainsCI(value, "Texture_2"))
            return 1;
        return static_cast<uint32_t>(-1);
    }

    static void NormalizedStemKey(const char* value, char* out, size_t outSz) noexcept
    {
        if (!out || outSz == 0) return;
        out[0] = '\0';
        if (!value) return;

        const char* base = value;
        for (const char* p = value; *p; ++p)
            if (*p == '\\' || *p == '/') base = p + 1;

        const char* end = base + std::strlen(base);
        for (const char* p = base; *p; ++p)
        {
            if (*p == ':' || *p == '.') { end = p; break; }
        }

        size_t n = 0;
        for (const char* p = base; p < end && n + 1 < outSz; ++p)
        {
            char c = *p;
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
                out[n++] = c;
        }
        out[n] = '\0';
    }

    static bool ModelStemMatches(const char* sidecarModel, const char* model) noexcept
    {
        if (!sidecarModel || !*sidecarModel) return true;
        char a[264], b[264];
        NormalizedStemKey(sidecarModel, a, sizeof(a));
        NormalizedStemKey(model, b, sizeof(b));
        return a[0] && b[0] && std::strcmp(a, b) == 0;
    }

    static uint32_t ParseSidecarSlot(const char* slot) noexcept
    {
        if (!slot || !*slot) return static_cast<uint32_t>(-1);
        uint32_t numeric = 0;
        if (ParseU32(slot, &numeric) && numeric < 14) return numeric;

        if (StartsWithCI(slot, "Head")) return 0;
        if (StartsWithCI(slot, "Shoulder")) return 1;
        if (StartsWithCI(slot, "Shirt")) return 2;
        if (StartsWithCI(slot, "Chest") || StartsWithCI(slot, "Robe")) return 3;
        if (StartsWithCI(slot, "Waist") || StartsWithCI(slot, "Belt")) return 4;
        if (StartsWithCI(slot, "Leg") || StartsWithCI(slot, "Pant")) return 5;
        if (StartsWithCI(slot, "Foot") || StartsWithCI(slot, "Feet") || StartsWithCI(slot, "Boot")) return 6;
        if (StartsWithCI(slot, "Bracer") || StartsWithCI(slot, "Wrist")) return 7;
        if (StartsWithCI(slot, "Glove") || StartsWithCI(slot, "Hand")) return 8;
        if (StartsWithCI(slot, "Cape") || StartsWithCI(slot, "Back") || StartsWithCI(slot, "Cloak")) return 9;
        if (StartsWithCI(slot, "Tabard")) return 10;
        // Weapon slots. Bare "Weapon" is intentionally left unmapped (returns -1, i.e.
        // "any slot") since a display ID is normally only ever equipped in one weapon
        // slot anyway; use the specific names below when a display can appear in more
        // than one hand (e.g. a shield display that could be Offhand vs. Ranged).
        if (StartsWithCI(slot, "MainHand") || StartsWithCI(slot, "Main")) return 15;
        if (StartsWithCI(slot, "OffHand") || StartsWithCI(slot, "Off") || StartsWithCI(slot, "Shield")) return 16;
        if (StartsWithCI(slot, "Ranged") || StartsWithCI(slot, "Bow") || StartsWithCI(slot, "Gun")
            || StartsWithCI(slot, "Quiver") || StartsWithCI(slot, "Ammo")) return 17;
        return static_cast<uint32_t>(-1);
    }

    static uint32_t ModelFlagsForSuffixPolicy(const char* policy) noexcept
    {
        if (policy && (StartsWithCI(policy, "DBC") || StartsWithCI(policy, "Slot"))) return 0xffffffffu;
        if (!policy || !*policy || StartsWithCI(policy, "Exact")) return 0x2 | 0x4;
        if (StartsWithCI(policy, "None")) return 0x2 | 0x4;
        if (StartsWithCI(policy, "Retail")) return 0x40;
        if (StartsWithCI(policy, "New")) return 0x40;
        if (StartsWithCI(policy, "Legacy")) return 0;
        if (StartsWithCI(policy, "Race")) return 0;
        return 0x2 | 0x4;
    }

    static uint32_t TextureFlagsForPolicy(const char* policy) noexcept
    {
        if (policy && (StartsWithCI(policy, "DBC") || StartsWithCI(policy, "Slot"))) return 0xffffffffu;
        if (!policy || !*policy || StartsWithCI(policy, "Exact")) return 0;
        if (StartsWithCI(policy, "None")) return 0;
        if (StartsWithCI(policy, "Retail")) return 0x8 | 0x10 | 0x40;
        if (StartsWithCI(policy, "New")) return 0x8 | 0x10 | 0x40;
        if (StartsWithCI(policy, "Legacy")) return 0x8 | 0x10;
        if (StartsWithCI(policy, "Race")) return 0x8 | 0x10;
        return 0;
    }

    static void ParseAttachField(const char* start, const char* end,
                                  uint32_t* leftOut, uint32_t* rightOut)
    {
        uint32_t a = 0;
        const char* p = start;
        while (p < end && *p >= '0' && *p <= '9') a = a * 10 + static_cast<uint32_t>(*p++ - '0');
        *leftOut = a;
        if (p < end && *p == '|')
        {
            p++;
            uint32_t b = 0;
            while (p < end && *p >= '0' && *p <= '9') b = b * 10 + static_cast<uint32_t>(*p++ - '0');
            *rightOut = b;
        }
        else { *rightOut = a; }
    }

    // Icon2 format: "<attach1>:<attach2>:<flags>:<customfolder>" — each field optional.
    // Caller sets attach/flag defaults; only fields present in the string override them.
    // Guard: only call when icon2str starts with ':' or a digit.
    static void ParseIcon2(const char* icon2str,
                            uint32_t* attachA_l, uint32_t* attachA_r,
                            uint32_t* attachB_l, uint32_t* attachB_r,
                            uint32_t* flags,
                            char* customFolder, size_t customFolderSz)
    {
        *flags = 0;
        if (customFolder) customFolder[0] = '\0';
        if (!icon2str || !*icon2str) return;

        const char* f2 = icon2str;
        while (*f2 && *f2 != ':') f2++;
        if (icon2str != f2) ParseAttachField(icon2str, f2, attachA_l, attachA_r);
        if (*f2 != ':') return;

        const char* f3 = f2 + 1;
        while (*f3 && *f3 != ':') f3++;
        if (f2 + 1 != f3) ParseAttachField(f2 + 1, f3, attachB_l, attachB_r);
        if (*f3 != ':') return;

        const char* f4 = f3 + 1;
        while (*f4 && *f4 != ':') f4++;
        if (f3 + 1 != f4)
        {
            uint32_t v = 0;
            for (const char* p = f3 + 1; p < f4; p++) v = v * 10 + static_cast<uint32_t>(*p - '0');
            *flags = v;
        }
        if (*f4 != ':') return;

        if (customFolder && customFolderSz > 1)
        {
            size_t len = std::strlen(f4 + 1);
            if (len >= customFolderSz) len = customFolderSz - 1;
            std::memcpy(customFolder, f4 + 1, len);
            customFolder[len] = '\0';
        }
    }

    static bool Icon2FieldHasValue(const char* icon2str, uint32_t fieldIndex)
    {
        if (!icon2str) return false;
        const char* start = icon2str;
        for (uint32_t i = 0; i < fieldIndex; ++i)
        {
            while (*start && *start != ':') ++start;
            if (*start != ':') return false;
            ++start;
        }
        const char* end = start;
        while (*end && *end != ':') ++end;
        return end > start;
    }

    static bool StartsWithCI(const char* s, const char* prefix) noexcept
    {
        if (!s || !prefix) return false;
        while (*prefix)
        {
            char a = *s++;
            char b = *prefix++;
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) return false;
        }
        return true;
    }

    static bool ContainsCI(const char* s, const char* needle) noexcept
    {
        if (!s || !needle || !*needle) return false;
        for (const char* p = s; *p; ++p)
        {
            const char* a = p;
            const char* b = needle;
            while (*a && *b)
            {
                char ca = *a++;
                char cb = *b++;
                if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
                if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
                if (ca != cb) break;
            }
            if (!*b) return true;
        }
        return false;
    }

    static bool IsKnownObjectComponentFolder(const char* folder, size_t len) noexcept
    {
        static const char* kFolders[] = {
            "Head", "Shoulder", "Shirt", "Chest", "Waist", "Leg", "Foot",
            "Bracer", "Glove", "Cape", "Tabard", "Collections", "Weapon",
        };

        for (const char* known : kFolders)
        {
            size_t knownLen = std::strlen(known);
            if (knownLen != len) continue;

            bool match = true;
            for (size_t i = 0; i < len; ++i)
            {
                char a = folder[i];
                char b = known[i];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
                if (a != b)
                {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    }

    static const char* InferObjectComponentFolder(const char* name, bool isCollection,
                                                  const char* customFolder,
                                                  const char* slotFolder) noexcept
    {
        if (customFolder && *customFolder) return customFolder;
        if (isCollection) return "Collections";
        if (StartsWithCI(name, "lshoulder_") || StartsWithCI(name, "rshoulder_") ||
            StartsWithCI(name, "shoulder_") || ContainsCI(name, "_shoulder_l") ||
            ContainsCI(name, "_shoulder_r"))
            return "Shoulder";
        if (StartsWithCI(name, "cape_")) return "Cape";
        if (StartsWithCI(name, "tabard_")) return "Tabard";
        return slotFolder;
    }

    static uint32_t InferObjectComponentAttach(const char* name, bool isCollection,
                                               bool explicitAttach,
                                               uint32_t attach) noexcept
    {
        if (explicitAttach || isCollection) return attach;
        if (StartsWithCI(name, "lshoulder_") || ContainsCI(name, "_shoulder_l")) return 6;
        if (StartsWithCI(name, "rshoulder_") || ContainsCI(name, "_shoulder_r")) return 5;
        if (StartsWithCI(name, "collections_"))
        {
            if (ContainsCI(name, "_shoulder_l")) return 6;
            if (ContainsCI(name, "_shoulder_r")) return 5;
            if (StartsWithCI(name, "collections_belt_") || ContainsCI(name, "_belt")) return 53;
        }
        if (StartsWithCI(name, "cape_")) return 12;
        if (StartsWithCI(name, "tabard_")) return 34;
        return attach;
    }

    static bool SlotAllowsNormalObjectModel(uint32_t modelSlot) noexcept
    {
        switch (modelSlot)
        {
            case 0:  // Head
            case 1:  // Shoulder
            case 4:  // Waist / 3D belts
            case 9:  // Back/cape
            case 10: // Tabard
            case 15: // MainHand
            case 16: // OffHand
            case 17: // Ranged
                return true;
            default:
                return false;
        }
    }

    static bool IsCollectionObjectPath(const char* path) noexcept
    {
        return ContainsCI(path, "objectcomponents\\collections\\") ||
               ContainsCI(path, "objectcomponents/collections/");
    }

    static bool IsCollectionGloveBodyModel(uint32_t modelSlot, const char* stem) noexcept
    {
        return modelSlot == 8 && stem && ContainsCI(stem, "_glove");
    }

    static bool NeedsVirtualModel(const AttachEntry& e) noexcept
    {
        return e.matTexBuf[0] ||
               e.geoFilter.count > 0 ||
               (e.texBuf[0] && IsCollectionObjectPath(e.keyBuf));
    }

    static bool IsCollectionEntry(const AttachEntry& e) noexcept
    {
        return e.geoFilter.count > 0 || IsCollectionObjectPath(e.keyBuf);
    }

    static bool ShouldDetachDefaultAttachPoints(const AttachEntry& e) noexcept
    {
        if (e.equipSlot >= 11) return false;
        if (e.equipSlot == 8 && e.attachId == kCollectionAttach) return false;
        return true;
    }

    static void AnalyzeModelList(const char* list, bool* hasCollection, bool* hasNormal)
    {
        char part[264] = {};
        for (uint32_t idx = 0; idx < 16; ++idx)
        {
            if (!CopyListPart(list, idx, part, sizeof(part))) break;
            if (!part[0]) continue;
            if (std::strchr(part, ':')) *hasCollection = true;
            else                        *hasNormal = true;
        }
    }

    static bool ReadSidecarLines(const char* path, std::vector<std::string>& lines)
    {
        lines.clear();
        if (!path || !*path) return false;

        if (FILE* f = std::fopen(path, "rb"))
        {
            char line[4096];
            while (std::fgets(line, sizeof(line), f))
                lines.emplace_back(line);
            std::fclose(f);
            return !lines.empty();
        }

        namespace io    = wxl::game::io;
        namespace iooff = wxl::offsets::engine::io;

        void* handle = nullptr;
        if (!io::FileOpen(path, iooff::kOpenWholeFile, &handle) || !handle)
            return false;

        uint32_t sizeHigh = 0;
        const uint32_t size = io::FileSize(handle, &sizeHigh);
        std::string bytes;
        bool ok = false;
        if (size > 0 && sizeHigh == 0)
        {
            bytes.resize(size);
            uint32_t got = 0;
            io::FileRead(handle, &bytes[0], size, &got);
            ok = (got == size);
        }
        io::FileClose(handle);
        if (!ok) return false;

        size_t start = 0;
        for (size_t i = 0; i <= bytes.size(); ++i)
        {
            if (i != bytes.size() && bytes[i] != '\n') continue;
            std::string line = bytes.substr(start, i - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            start = i + 1;
        }
        return !lines.empty();
    }

    static void LoadSidecarFile(const char* path)
    {
        std::vector<std::string> lines;
        if (!ReadSidecarLines(path, lines)) return;

        const std::vector<std::string> header = ParseCsvLine(lines[0].c_str());
        const int cDisplay = FindCsvColumn(header, "DisplayID");
        const int cSlot = FindCsvColumn(header, "Slot");
        int cModel = FindCsvColumn(header, "Model");
        if (cModel < 0) cModel = FindCsvColumn(header, "ModelStem");
        if (cModel < 0) cModel = FindCsvColumn(header, "ModelName");
        int cTexture = FindCsvColumn(header, "Texture");
        if (cTexture < 0) cTexture = FindCsvColumn(header, "TextureStem");
        if (cTexture < 0) cTexture = FindCsvColumn(header, "ModelTexture");
        const int cFolder = FindCsvColumn(header, "Folder");
        const int cGeosets = FindCsvColumn(header, "Geosets");
        const int cAttach = FindCsvColumn(header, "Attach");
        const int cSuffixPolicy = FindCsvColumn(header, "SuffixPolicy");
        const int cTexturePolicy = FindCsvColumn(header, "TexturePolicy");
        const int cFlags = FindCsvColumn(header, "Flags");

        if (cDisplay < 0 || cModel < 0)
        {
            EquipLog("sidecar '%s': missing DisplayID or Model column", path);
            return;
        }

        uint32_t loaded = 0;
        for (size_t lineIndex = 1; lineIndex < lines.size(); ++lineIndex)
        {
            if (lines[lineIndex].empty()) continue;
            const std::vector<std::string> row = ParseCsvLine(lines[lineIndex].c_str());
            uint32_t displayId = 0;
            if (!ParseU32(CsvField(row, cDisplay), &displayId) || displayId == 0) continue;

            SidecarModelEntry e = {};
            e.modelSlot = ParseSidecarSlot(CsvField(row, cSlot));
            e.modelFlags = ModelFlagsForSuffixPolicy(CsvField(row, cSuffixPolicy));
            e.textureFlags = TextureFlagsForPolicy(CsvField(row, cTexturePolicy));

            uint32_t attach = static_cast<uint32_t>(-1);
            if (ParseU32(CsvField(row, cAttach), &attach)) e.attachId = attach;

            uint32_t extraFlags = 0;
            if (ParseU32(CsvField(row, cFlags), &extraFlags)) e.modelFlags |= extraFlags;

            CopyString(e.folder, sizeof(e.folder), CsvField(row, cFolder));
            CopyString(e.model, sizeof(e.model), CsvField(row, cModel));
            CopyString(e.texture, sizeof(e.texture), CsvField(row, cTexture));

            char* colon = std::strchr(e.model, ':');
            if (colon)
            {
                *colon = '\0';
                e.geoFilter = ParseGeosetFilter(colon + 1);
            }
            const char* geosets = CsvField(row, cGeosets);
            if (geosets && *geosets) e.geoFilter = ParseGeosetFilter(geosets);
            if (!e.model[0]) continue;

            g_sidecarModels[displayId].push_back(e);
            ++loaded;
        }

        if (loaded)
            EquipLog("sidecar loaded '%s' rows=%u", path, loaded);
    }

    static void LoadMaterialSidecarFile(const char* path)
    {
        std::vector<std::string> lines;
        if (!ReadSidecarLines(path, lines)) return;

        const std::vector<std::string> header = ParseCsvLine(lines[0].c_str());
        const int cDisplay = FindCsvColumn(header, "DisplayID");
        const int cModelIndex = FindCsvColumn(header, "ModelIndex");
        const int cModelColumn = FindCsvColumn(header, "ModelColumn");
        const int cModel = FindCsvColumn(header, "Model");
        const int cTextureType = FindCsvColumn(header, "TextureType");
        const int cFolder = FindCsvColumn(header, "Folder");
        const int cTexture = FindCsvColumn(header, "Texture");
        const int cTargetMode = FindCsvColumn(header, "TargetMode");
        // NOTE: Layer, SkinSectionIDs, BatchIndexes, TargetSkinSectionIDs, and TargetBatchIndexes
        // are intentionally not looked up here -- texture-value patching is TextureType-keyed only
        // (see PatchTargetedMaterialTextures in VirtualPath.cpp), and the hide-by-batch machinery
        // those columns used to feed has been removed along with them. MaterialResourcesID and
        // FileDataID were never read by this loader either; all seven of those columns can be
        // dropped from the CSV entirely.

        if (cDisplay < 0 || cTexture < 0)
        {
            EquipLog("material sidecar '%s': missing DisplayID or Texture column", path);
            return;
        }

        uint32_t loaded = 0;
        for (size_t lineIndex = 1; lineIndex < lines.size(); ++lineIndex)
        {
            if (lines[lineIndex].empty()) continue;
            const std::vector<std::string> row = ParseCsvLine(lines[lineIndex].c_str());
            uint32_t displayId = 0;
            if (!ParseU32(CsvField(row, cDisplay), &displayId) || displayId == 0) continue;

            SidecarMaterialEntry e = {};
            ParseU32(CsvField(row, cModelIndex), &e.modelIndex);
            e.modelColumn = ParseModelColumn(CsvField(row, cModelColumn));
            if (e.modelColumn == static_cast<uint32_t>(-1))
                e.modelColumn = ParseModelColumn(CsvField(row, cModelIndex));
            if (cTextureType >= 0)
                ParseU32(CsvField(row, cTextureType), &e.textureType);

            CopyString(e.folder, sizeof(e.folder), CsvField(row, cFolder));
            CopyString(e.model, sizeof(e.model), CsvField(row, cModel));
            CopyString(e.texture, sizeof(e.texture), CsvField(row, cTexture));
            CopyString(e.targetMode, sizeof(e.targetMode), CsvField(row, cTargetMode));
            if (!e.texture[0]) continue;

            g_sidecarMaterials[displayId].push_back(e);
            ++loaded;
        }

        if (loaded)
            EquipLog("material sidecar loaded '%s' rows=%u", path, loaded);
    }

    // WXLItemEntryDisplay.csv columns: ItemEntry, DisplayID, Folder (optional). ItemEntry/DisplayID
    // are both required; malformed or duplicate rows for the same ItemEntry keep the first value
    // seen and log the collision. Folder is the model's real Item\ObjectComponents\<Folder>\
    // subdirectory (e.g. "Weapon", "Shield") -- when given, PatchWeaponModelByDisplayId tries it
    // before falling back to its own Weapon/Shield guesses, so a mapper who already knows a
    // display's folder (shields especially, since they don't live under \Weapon\ like everything
    // else this sidecar is normally used for) can just say so instead of relying on the guess
    // succeeding. Blank/missing Folder cells, or an entirely absent column (older two-column
    // sidecar files keep working unchanged), just skip straight to those guesses.
    static void LoadItemDisplaySidecarFile(const char* path)
    {
        std::vector<std::string> lines;
        if (!ReadSidecarLines(path, lines)) return;

        const std::vector<std::string> header = ParseCsvLine(lines[0].c_str());
        const int cEntry = FindCsvColumn(header, "ItemEntry");
        int cDisplay = FindCsvColumn(header, "DisplayID");
        if (cDisplay < 0) cDisplay = FindCsvColumn(header, "DisplayId");
        const int cFolder = FindCsvColumn(header, "Folder");
        if (cEntry < 0 || cDisplay < 0)
        {
            EquipLog("item-display sidecar '%s': missing ItemEntry or DisplayID column", path);
            return;
        }

        uint32_t loaded = 0, loadedFolders = 0;
        for (size_t lineIndex = 1; lineIndex < lines.size(); ++lineIndex)
        {
            if (lines[lineIndex].empty()) continue;
            const std::vector<std::string> row = ParseCsvLine(lines[lineIndex].c_str());
            uint32_t itemEntry = 0, displayId = 0;
            if (!ParseU32(CsvField(row, cEntry), &itemEntry) || itemEntry == 0) continue;
            if (!ParseU32(CsvField(row, cDisplay), &displayId) || displayId == 0) continue;

            auto ins = g_sidecarItemDisplay.emplace(itemEntry, displayId);
            if (!ins.second && ins.first->second != displayId)
            {
                EquipLog("item-display sidecar '%s': itemEntry=%u already mapped to displayId=%u, "
                         "ignoring duplicate row displayId=%u", path, itemEntry, ins.first->second, displayId);
                continue;
            }
            ++loaded;

            if (cFolder >= 0)
            {
                const char* folder = CsvField(row, cFolder);
                if (folder && *folder)
                {
                    auto fins = g_sidecarModelFolder.emplace(displayId, folder);
                    if (!fins.second && fins.first->second != folder)
                    {
                        EquipLog("item-display sidecar '%s': displayId=%u already has folder '%s', "
                                 "ignoring conflicting row folder '%s'", path, displayId,
                                 fins.first->second.c_str(), folder);
                    }
                    else if (fins.second)
                    {
                        ++loadedFolders;
                    }
                }
            }
        }

        if (loaded)
            EquipLog("item-display sidecar loaded '%s' rows=%u (folders=%u)", path, loaded, loadedFolders);
    }

    // Forward decl: full definition sits near OnWeaponVisualChange (needs PatchWeaponModelByDisplayId,
    // itself defined there), but LoadSidecarModels needs to call it as soon as the sidecar tables are
    // populated, and LoadSidecarModels is defined earlier in this file.
    static void PreregisterSidecarWeapons();

    static void LoadSidecarModels()
    {
        if (g_sidecarLoaded) return;
        g_sidecarLoaded = true;

        LoadSidecarFile("WXLItemDisplayModels.csv");
        LoadSidecarFile("DBFilesClient\\WXLItemDisplayModels.csv");
        LoadMaterialSidecarFile("WXLItemDisplayModelMaterials.csv");
        LoadMaterialSidecarFile("DBFilesClient\\WXLItemDisplayModelMaterials.csv");
        LoadItemDisplaySidecarFile("WXLItemEntryDisplay.csv");
        LoadItemDisplaySidecarFile("DBFilesClient\\WXLItemEntryDisplay.csv");

        WIN32_FIND_DATAA fd = {};
        HANDLE h = FindFirstFileA("Data\\*.MPQ", &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::string path = "Data\\";
                path += fd.cFileName;
                path += "\\DBFilesClient\\WXLItemDisplayModels.csv";
                LoadSidecarFile(path.c_str());
                path = "Data\\";
                path += fd.cFileName;
                path += "\\DBFilesClient\\WXLItemDisplayModelMaterials.csv";
                LoadMaterialSidecarFile(path.c_str());
                path = "Data\\";
                path += fd.cFileName;
                path += "\\DBFilesClient\\WXLItemEntryDisplay.csv";
                LoadItemDisplaySidecarFile(path.c_str());
            }
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }

        EquipLog("sidecar table ready: displays=%zu materialDisplays=%zu itemEntryDisplays=%zu",
                 g_sidecarModels.size(), g_sidecarMaterials.size(), g_sidecarItemDisplay.size());

        PreregisterSidecarWeapons();
    }

    // ─── Path builders ────────────────────────────────────────────────────────────

    // Builds the virtual M2 path: "Item\ObjectComponents\<folder>\[<stem>\]<stem>_<race>[_]<gender>.mdx"
    // Folder priority: customFolder > collection/cape stem inference > slotFolder.
    // flags: 0x2=no gender, 0x4=no race, 0x20=subfolder, 0x40=new _Hu_F underscore between race+gender.
    static void BuildSlotPath(char* buf, const char* modelName,
                               const char* raceCode, const char* genderStr,
                               uint32_t flags, const char* slotFolder, bool isCollection,
                               const char* customFolder = nullptr)
    {
        bool appendRace   = !(flags & 0x4);
        bool appendGender = !(flags & 0x2);
        bool subFolder    = (flags & 0x20) != 0;
        bool newSuffix    = (flags & 0x40) != 0;

        const char* folder = InferObjectComponentFolder(modelName, isCollection, customFolder, slotFolder);

        char* p = buf;
        for (const char* s = "Item\\ObjectComponents\\"; *s; ) *p++ = *s++;
        for (const char* s = folder; *s; )                     *p++ = *s++;
        *p++ = '\\';

        const char* lastDot = nullptr;
        for (const char* s = modelName; *s; s++) if (*s == '.') lastDot = s;
        size_t stemLen = lastDot ? static_cast<size_t>(lastDot - modelName) : std::strlen(modelName);

        if (subFolder)
        {
            for (size_t i = 0; i < stemLen; ++i) *p++ = modelName[i];
            *p++ = '\\';
        }
        for (size_t i = 0; i < stemLen; ++i) *p++ = modelName[i];

        if (!appendRace && !appendGender)
        {
            *p++ = '.'; *p++ = 'm'; *p++ = 'd'; *p++ = 'x'; *p = '\0';
            return;
        }

        *p++ = '_';
        if (appendRace)   for (const char* s = raceCode;  *s; ) *p++ = *s++;
        if (newSuffix && appendRace && appendGender) *p++ = '_';
        if (appendGender) for (const char* s = genderStr; *s; ) *p++ = *s++;
        *p++ = '.'; *p++ = 'm'; *p++ = 'd'; *p++ = 'x'; *p = '\0';
    }

    // Builds the virtual BLP texture path.
    // flags: 0x8=append race, 0x10=append gender, 0x20=subfolder, 0x40=new underscore.
    // modelStem: when 0x20 set, names the subfolder after the model stem (not the texture name).
    static void BuildTexPath(char* buf, size_t bufSz, const char* texName,
                              const char* raceCode, const char* genderStr,
                              uint32_t flags, const char* slotFolder, bool isCollection,
                              const char* customFolder = nullptr, const char* modelStem = nullptr)
    {
        bool appendRace   = (flags & 0x8) != 0;
        bool appendGender = (flags & 0x10) != 0;
        bool subFolder    = (flags & 0x20) != 0;
        bool newSuffix    = (flags & 0x40) != 0;

        char explicitFolder[32] = {};
        const char* texStem = texName;
        for (const char* s = texName; *s; ++s)
        {
            if (*s != '\\' && *s != '/') continue;
            size_t folderLen = static_cast<size_t>(s - texName);
            if (folderLen > 0 && folderLen < sizeof(explicitFolder)
                && IsKnownObjectComponentFolder(texName, folderLen))
            {
                std::memcpy(explicitFolder, texName, folderLen);
                explicitFolder[folderLen] = '\0';
                texStem = s + 1;
            }
            break;
        }

        const char* subBase = (modelStem && *modelStem) ? modelStem : texStem;
        const char* folder = InferObjectComponentFolder(subBase, isCollection, customFolder, slotFolder);
        if (explicitFolder[0]) folder = explicitFolder;
        size_t subBaseLen = std::strlen(subBase);
        for (const char* s = subBase; *s; s++)
            if (*s == '.') { subBaseLen = static_cast<size_t>(s - subBase); break; }

        char tmp[264];
        char* p = tmp;
        for (const char* s = "Item\\ObjectComponents\\"; *s; ) *p++ = *s++;
        for (const char* s = folder; *s; )                     *p++ = *s++;
        *p++ = '\\';
        if (subFolder)
        {
            for (size_t i = 0; i < subBaseLen; ++i) *p++ = subBase[i];
            *p++ = '\\';
        }
        for (const char* s = texStem; *s; ) *p++ = *s++;
        if (appendRace || appendGender)
        {
            *p++ = '_';
            if (appendRace)   for (const char* s = raceCode;  *s; ) *p++ = *s++;
            if (newSuffix && appendRace && appendGender) *p++ = '_';
            if (appendGender) for (const char* s = genderStr; *s; ) *p++ = *s++;
        }
        *p++ = '.'; *p++ = 'b'; *p++ = 'l'; *p++ = 'p'; *p = '\0';
        std::strncpy(buf, tmp, bufSz - 1);
        buf[bufSz - 1] = '\0';
    }

    static bool MaterialEntryMatches(const SidecarMaterialEntry& e,
                                     uint32_t modelColumn,
                                     uint32_t partIndex,
                                     const char* modelName) noexcept
    {
        const bool hasModelName = e.model[0] != '\0';
        if (hasModelName && !ModelStemMatches(e.model, modelName)) return false;
        if (!hasModelName &&
            e.modelColumn != static_cast<uint32_t>(-1) &&
            e.modelColumn != modelColumn)
            return false;
        if (hasModelName &&
            modelColumn != static_cast<uint32_t>(-1) &&
            e.modelColumn != static_cast<uint32_t>(-1) &&
            e.modelColumn != modelColumn)
            return false;

        if (!hasModelName && e.modelColumn == static_cast<uint32_t>(-1) &&
            e.modelIndex != static_cast<uint32_t>(-1))
        {
            const bool columnKnown = modelColumn != static_cast<uint32_t>(-1);
            const bool columnMatch = columnKnown &&
                (e.modelIndex == modelColumn || e.modelIndex == modelColumn + 1);
            const bool partMatch = e.modelIndex == partIndex || e.modelIndex == partIndex + 1;
            if (!columnMatch && !partMatch)
                return false;
        }

        return e.texture[0] != '\0';
    }

    // Armor-side texture-value-baking loop. This is the counterpart to
    // BuildMaterialPatchSpecWeaponFallback below: bakes each matching sidecar material row's
    // Texture into every texture-unit record on the model declaring that row's TextureType, via
    // PatchTargetedMaterialTextures's TextureType-only matching in VirtualPath.cpp. The CSV's
    // Layer, SkinSectionIDs, BatchIndexes, TargetSkinSectionIDs, and TargetBatchIndexes columns are
    // no longer read at all (see LoadMaterialSidecarFile / SidecarMaterialEntry) -- there is no
    // hide/edgefade/targeted-batch overlay any more, for either armor or weapons. A row with
    // TargetMode=Hide (or a __hide__/hide texture value) is simply skipped, since there is no
    // longer any data source to say which batches such a row would apply to.
    static void BuildMaterialPatchSpecArmor(char* out, size_t outSz,
                                            uint32_t displayId,
                                            uint32_t modelColumn,
                                            uint32_t partIndex,
                                            const char* modelName,
                                            const char* raceCode,
                                            const char* genderStr,
                                            const char* slotFolder,
                                            bool isCollection,
                                            const char* customFolder,
                                            const char* modelStem)
    {
        if (!out || outSz == 0) return;
        out[0] = '\0';

        auto it = g_sidecarMaterials.find(displayId);
        if (it == g_sidecarMaterials.end()) return;

        size_t used = 0;
        for (const SidecarMaterialEntry& m : it->second)
        {
            if (!MaterialEntryMatches(m, modelColumn, partIndex, modelName)) continue;
            if (m.textureType == static_cast<uint32_t>(-1)) continue; // no type, nothing to key off
            if (!m.texture[0]) continue;
            if (ContainsCI(m.targetMode, "Hide") ||
                StartsWithCI(m.texture, "__hide__") || StartsWithCI(m.texture, "hide"))
                continue; // hide is no longer supported; skip rather than emit a dead entry

            char texPath[264] = {};
            const char* folder = m.folder[0] ? m.folder : customFolder;
            BuildTexPath(texPath, sizeof(texPath), m.texture, raceCode, genderStr, 0,
                         slotFolder, isCollection, folder, modelStem);
            if (!texPath[0]) continue;

            char item[768];
            int n = std::snprintf(item, sizeof(item), "%s%u=%s",
                                  used ? "|" : "", m.textureType, texPath);
            if (n <= 0) continue;
            const size_t len = static_cast<size_t>(n);
            if (len >= sizeof(item) || used + len >= outSz) break;
            std::memcpy(out + used, item, len);
            used += len;
            out[used] = '\0';
        }
    }

static void BuildMaterialPatchSpecWeaponFallback(char* out, size_t outSz,
                                                 uint32_t displayId,
                                                 uint32_t modelColumn,
                                                 uint32_t partIndex,
                                                 const char* modelName,
                                                 const char* raceCode,
                                                 const char* genderStr,
                                                 const char* slotFolder,
                                                 bool isCollection,
                                                 const char* customFolder,
                                                 const char* modelStem)
{
    if (!out || outSz == 0) return;
    out[0] = '\0';
    auto it = g_sidecarMaterials.find(displayId);
    if (it == g_sidecarMaterials.end()) return;

    // normalize stem
    std::string normStem;
    if (modelStem && *modelStem) {
        normStem = modelStem;
        auto p = normStem.find_last_of("\\/");
        if (p != std::string::npos) normStem = normStem.substr(p + 1);
        p = normStem.find_last_of('.');
        if (p != std::string::npos) normStem = normStem.substr(0, p);
    }

    size_t used = 0;
    for (const SidecarMaterialEntry& m : it->second)
    {
        // prefer explicit match, but allow stem match
        bool matMatch = MaterialEntryMatches(m, modelColumn, partIndex, modelName);
        bool stemMatch = false;
        if (m.model[0] && !normStem.empty()) {
            std::string mstem = m.model;
            auto p = mstem.find_last_of("\\/");
            if (p != std::string::npos) mstem = mstem.substr(p + 1);
            p = mstem.find_last_of('.');
            if (p != std::string::npos) mstem = mstem.substr(0, p);
            stemMatch = (_stricmp(mstem.c_str(), normStem.c_str()) == 0);
        }
        if (!matMatch && !stemMatch) continue;
        if (!m.texture[0]) continue;
        if (m.textureType == static_cast<uint32_t>(-1)) continue; // no type, nothing to key off

        if (ContainsCI(m.targetMode, "Hide") ||
            StartsWithCI(m.texture, "__hide__") || StartsWithCI(m.texture, "hide"))
            continue; // hide is no longer supported; skip rather than emit a dead entry

        // build tex path using normalized stem
        char texPath[264] = {};
        const char* folder = m.folder[0] ? m.folder : customFolder;
        BuildTexPath(texPath, sizeof(texPath), m.texture, raceCode, genderStr, 0,
                     slotFolder, isCollection, folder, normStem.c_str());
        if (!texPath[0]) continue;

        char item[768];
        int n = std::snprintf(item, sizeof(item), "%s%u=%s",
                              used ? "|" : "", m.textureType, texPath);
        if (n <= 0) continue;
        const size_t len = static_cast<size_t>(n);
        if (len >= sizeof(item) || used + len >= outSz) break;
        std::memcpy(out + used, item, len);
        used += len;
        out[used] = '\0';

        EquipLog("equip-extension: weapon fallback matched display=%u model='%s' texture='%s' textureType=%u",
                 displayId, m.model, m.texture, m.textureType);
    }
}


    // ─── Geoset filter ────────────────────────────────────────────────────────────

    // Zeros rawTri (skin->indices) for submeshes whose skinSectionId is not in the filter.
    // Must run before kFinalizeSkin builds the D3D index buffer — the GPU IB is static after
    // that point. Called from OnM2SkinFinalize (the primary path) and Phase3 (fallback for
    // the rare sync-load case where the model is already available at equip time).
    static void ApplyRawTriFilter(gm2::M2SkinProfile* skin, const GeosetFilter& filter) noexcept
    {
        if (!skin || filter.count == 0) return;
        __try
        {
            if (!skin->indices || !skin->submeshes || skin->submeshCount == 0) return;
            EquipLog("  ApplyRawTriFilter: submeshCount=%u indexCount=%u filter=[%u %u %u %u](n=%u)",
                     skin->submeshCount, skin->indexCount,
                     filter.count > 0 ? filter.ids[0] : 0,
                     filter.count > 1 ? filter.ids[1] : 0,
                     filter.count > 2 ? filter.ids[2] : 0,
                     filter.count > 3 ? filter.ids[3] : 0,
                     filter.count);
            for (uint32_t si = 0; si < skin->submeshCount; ++si)
            {
                uint16_t secId = skin->submeshes[si].skinSectionId;
                uint16_t lvl   = skin->submeshes[si].level;
                uint16_t start16 = skin->submeshes[si].indexStart;
                uint16_t count16 = skin->submeshes[si].indexCount;
                bool visible = false;
                for (uint32_t fi = 0; fi < filter.count; ++fi)
                    if (filter.ids[fi] == secId) { visible = true; break; }
                EquipLog("    submesh[%u]: skinSectionId=%u level=%u indexStart=%u indexCount=%u -> %s",
                         si, (uint32_t)secId, (uint32_t)lvl, (uint32_t)start16, (uint32_t)count16,
                         visible ? "KEEP" : "ZERO");
                if (!visible)
                {
                    uint32_t count = count16;
                    if (count == 0) continue;

                    uint32_t start = (static_cast<uint32_t>(lvl) << 16) | start16;
                    if (start > skin->indexCount || count > skin->indexCount - start)
                    {
                        start = start16;
                        if (start > skin->indexCount || count > skin->indexCount - start) continue;
                    }

                    std::memset(skin->indices + start, 0, count * sizeof(uint16_t));
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ─── Detach / rebuild ─────────────────────────────────────────────────────────

    // Erases all entries for (cmo, equipSlot) and issues the minimal set of DetachSlot calls
    // (deduplicating by attach_id to avoid double-detach corrupting the scene-graph linked list).
    static void DetachSlotEntries(void* cmo, uint32_t equipSlot)
    {
        auto it = g_attached.find(cmo);
        if (it == g_attached.end()) return;

        // Current live sub_obj (read fresh; may differ from stored sub_obj after re-login).
        void* curSubObj = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoSceneNode);

        struct DetachPair { void* subObj; uint32_t attachId; };
        DetachPair pairs[8]; uint32_t nPairs = 0;
        auto addPair = [&](void* so, uint32_t aid)
        {
            if (!so || aid == static_cast<uint32_t>(-1) || nPairs >= 8) return;
            for (uint32_t k = 0; k < nPairs; ++k)
                if (pairs[k].subObj == so && pairs[k].attachId == aid) return;
            pairs[nPairs++] = { so, aid };
        };

        auto& vec = it->second;
        for (size_t i = 0; i < vec.size(); )
        {
            AttachEntry& e = vec[i];
            if (e.equipSlot != equipSlot) { ++i; continue; }
            if (e.renderCtx)
                addPair(curSubObj ? curSubObj : e.subObj, e.attachId);
            vec.erase(vec.begin() + static_cast<ptrdiff_t>(i));
        }

        for (uint32_t k = 0; k < nPairs; ++k)
            gm2::DetachSlot(pairs[k].subObj, pairs[k].attachId);

        if (vec.empty()) g_attached.erase(it);
    }

    // Detaches all unique attach_ids in g_attached[cmo], then re-attaches each unique
    // (keyBuf, attachId, texBuf) combination via GetRenderCtx → (BindTexSlot) → AttachToScene.
    // render_ctx values in entries are rebuilt transient each time; called on every equip/unequip.
    static void RebuildAllModels(void* cmo)
    {
        auto it = g_attached.find(cmo);
        if (it == g_attached.end() || it->second.empty()) return;
        auto& entries = it->second;

        void* subObj = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoSceneNode);
        if (!subObj) { EquipLog("  RebuildAllModels: subObj null, bail"); return; }

        // GetRenderCtx (sub_81f8f0) must be called with the scene-node's owner (owner28), NOT
        // with our slot-dispatch 'cmo'. The slot-dispatch ECX is a wrapper object whose +4
        // hash-table pointer is always null. The real CMO — the object that owns the M2 hash
        // table — is the scene node's back-pointer at kOffSceneNodeOwner (+0x28). This matches
        // how vanilla's sub_4eaa70 calls sub_81f8f0: it dereferences *(sub_obj+0x28) first.
        // If owner28 is null the CMO is not yet fully initialised; bail so OnM2PerFrameUpdate
        // can retry via the pending-attach sweep.
        void* owner28 = GuardedReadPtr(reinterpret_cast<uint8_t*>(subObj) + m2::kOffSceneNodeOwner);
        if (!owner28) { EquipLog("  RebuildAllModels: owner28 null, bail (deferred)"); return; }

        EquipLog("  RebuildAllModels: %zu entries, subObj=0x%p owner28=0x%p",
                 entries.size(), subObj, owner28);

 for (auto& e : entries) { e.renderCtx = nullptr; e.subObj = subObj; e.perFrameLogged = false; }
//        // Reset all state tied to the previous render context.
//        for (auto& e : entries)
//        {
//            e.renderCtx = nullptr;
//            e.subObj = subObj;
//            e.boneRemap = {};
//            e.perFrameLogged = false;
//            e.charSweepApplied = false;
//            e.bbpLogDone = false;
//            e.cloneBbpLogDone = false;
//        }
        // Pre-phase: build virtual paths for collection entries and populate the serve table.
        // Each unique (keyBuf, attachId, texBuf) collection group gets one merged geoset filter and one
        // virtual key encoding (cmo × model × geosets × texture). GetRenderCtx with this key
        // guarantees a cache miss on every distinct filter/texture combination, forcing a fresh
        // async load and an OnM2SkinFinalize fire with the correct merged filter.
        for (size_t i = 0; i < entries.size(); ++i)
        {
            AttachEntry& e = entries[i];
            if (!NeedsVirtualModel(e)) continue;
            // Skip if a sibling already processed this (keyBuf, attachId, texBuf) group.
            bool done = false;
            for (size_t j = 0; j < i; ++j)
            {
                AttachEntry& prev = entries[j];
                if (NeedsVirtualModel(prev) && SameAttachModelGroup(prev, e)) { done = true; break; }
            }
            if (done) continue;

            // Merge geoset IDs within this (keyBuf, attachId, texBuf) group.
            // texBuf is fixed per group — e.texBuf is the canonical texture for this virtual model.
            uint16_t mergedIds[16]; uint32_t mergedCount = 0;
            for (auto& e2 : entries)
            {
                if (!SameAttachModelGroup(e2, e)) continue;
                for (uint32_t fi = 0; fi < e2.geoFilter.count && mergedCount < 16; ++fi)
                {
                    bool dup = false;
                    for (uint32_t fj = 0; fj < mergedCount; ++fj)
                        if (mergedIds[fj] == e2.geoFilter.ids[fi]) { dup = true; break; }
                    if (!dup) mergedIds[mergedCount++] = e2.geoFilter.ids[fi];
                }
            }

            // Build the virtual key and ensure bytes are in the serve table.
            char mangled[264];
            const size_t mangledLen =
                VPathBuildKey(mangled, sizeof(mangled), cmo, e.keyBuf, mergedIds, mergedCount,
                              e.texBuf, e.mergeKey, e.matTexBuf);
            const bool vpathReady = mangledLen != 0 &&
                VPathPopulate(cmo, e.keyBuf, mergedIds, mergedCount, e.texBuf, e.mergeKey, e.matTexBuf);
            EquipLog("  VPath: '%s' tex='%s' mat='%s' -> '%s' (merged=%u mergeKey=0x%X)",
                     e.keyBuf, e.texBuf, e.matTexBuf, vpathReady ? mangled : "(disabled)",
                     mergedCount, e.mergeKey);
            if (!vpathReady)
                continue;

            // Propagate the mangled key to all entries in this (keyBuf, attachId, texBuf) group.
            for (auto& e2 : entries)
            {
                if (NeedsVirtualModel(e2) && SameAttachModelGroup(e2, e))
                    std::memcpy(e2.mangledKeyBuf, mangled, sizeof(e2.mangledKeyBuf));
            }
        }

        // Phase 1 — GetRenderCtx before DetachSlot.
        // hkSlotDispatch (GameHooks.cpp) runs vanilla first: it calls GetRenderCtx + AttachToScene
        // + ReleaseRenderCtx, leaving refcount=1 held by the scene. If we called DetachSlot first
        // the scene would release its ref (refcount→0, node freed), leaving a dangling pointer in
        // the hash bucket. Subsequent GetRenderCtx reads that dangling value and crashes at
        // sub_81c390+0x170 (mov eax,[esi+0x14c]) where esi=0x206 (freed-heap garbage).
        // Getting our ref first keeps the node alive through the detach step.
        //
        // g_currentRebuildCmo enables OnM2SkinFinalize to apply the geoset filter during the
        // synchronous load that fires kFinalizeSkin inside GetRenderCtx for virtual-path entries
        // (in-process byte serving has no IPC delay, so the model loads and finalizes before
        // GetRenderCtx returns — before entry.renderCtx is set). Cleared after Phase1.
        g_currentRebuildCmo = cmo;
        for (size_t i = 0; i < entries.size(); ++i)
        {
            AttachEntry& e = entries[i];
            if (e.renderCtx) continue;  // already set by a sibling

            const char* p1Key = e.mangledKeyBuf[0] ? e.mangledKeyBuf : e.keyBuf;
            void* rctx = SafeGetRenderCtx(owner28, p1Key);
            EquipLog("  Phase1 GetRenderCtx key='%s' -> 0x%p", p1Key, rctx);
            if (!rctx) continue;

            // Propagate to all siblings sharing (keyBuf, attachId, texBuf).
            for (size_t j = i; j < entries.size(); ++j)
            {
                AttachEntry& e2 = entries[j];
                if (!e2.renderCtx && SameAttachModelGroup(e2, e))
                    e2.renderCtx = rctx;
            }
        }
        g_currentRebuildCmo = nullptr;

        // Phase 2 — DetachSlot all unique attach_ids (dedup avoids double-detach crash in sub_827560).
        // Also detach the slot config's vanilla default attach points: g_origSlotDispatch (which runs
        // before our event) calls sub_4eaa70 which attaches to defAttach1/defAttach2. Without removing
        // these, the vanilla model shows at its default position alongside our custom attachments.
        uint32_t detached[22] = {}; uint32_t nDetached = 0;
        auto detachOnce = [&](uint32_t aid) {
            if (aid == static_cast<uint32_t>(-1) || nDetached >= 22) return;
            for (uint32_t i = 0; i < nDetached; ++i) if (detached[i] == aid) return;
            gm2::DetachSlot(subObj, aid);
            detached[nDetached++] = aid;
        };
        for (auto& e : entries)
        {
            detachOnce(e.attachId);
            if (ShouldDetachDefaultAttachPoints(e))
            {
                detachOnce(kSlotConfig[e.equipSlot].defAttach1);
                detachOnce(kSlotConfig[e.equipSlot].defAttach2);
            }
        }

        // Phase 3 — Bind texture, AttachToScene, geoset filter, release ref.
        // Process each unique (keyBuf, attachId, texBuf) once; siblings share the same render_ctx.
        for (size_t i = 0; i < entries.size(); ++i)
        {
            AttachEntry& e = entries[i];
            if (!e.renderCtx)
            {
                // renderCtx is null for two reasons:
                //   (a) Collection entry evicted by Phase 2.5 (model was loaded): re-acquire fresh rctx.
                //   (b) Non-collection entry whose Phase1 GetRenderCtx failed: skip.
                // Loading collection entries are NOT nulled by Phase 2.5; they reach here with a
                // non-null renderCtx (Phase1's rctx) and fall through to the attach below.
                if (!NeedsVirtualModel(e)) continue;
                const char* p3Key = e.mangledKeyBuf[0] ? e.mangledKeyBuf : e.keyBuf;
                void* rctx = SafeGetRenderCtx(owner28, p3Key);
                EquipLog("  Phase3[%zu] collection re-acquire key='%s' -> 0x%p", i, p3Key, rctx);
                if (!rctx) continue;
                e.renderCtx = rctx;
                for (size_t j = i + 1; j < entries.size(); ++j)
                {
                    AttachEntry& e2 = entries[j];
                    if (!e2.renderCtx && NeedsVirtualModel(e2) && SameAttachModelGroup(e2, e))
                        e2.renderCtx = rctx;
                }
            }

            // Skip siblings — only the first occurrence of each (keyBuf, attachId, texBuf) does the attach.
            bool alreadyAttached = false;
            for (size_t j = 0; j < i; ++j)
            {
                AttachEntry& prev = entries[j];
                if (prev.renderCtx && SameAttachModelGroup(prev, e))
                { alreadyAttached = true; break; }
            }
            if (alreadyAttached) continue;

            void* rctx = e.renderCtx;
            bool isCollection = IsCollectionEntry(e);

            if (e.texBuf[0])
            {
                void* tex = gm2::LoadResource(e.texBuf, 0);
                if (!tex)
                {
                    EquipLog("  Phase3[%zu] texture load failed '%s', skip attach", i, e.texBuf);
                    for (auto& e2 : entries)
                        if (SameAttachModelGroup(e2, e)) e2.renderCtx = nullptr;
                    gm2::ReleaseRenderCtx(rctx);
                    continue;
                }
                else
                {
                    gm2::BindTexSlot(rctx, tex);
                    // Do not release here. TextureCreate returns a handle that the render context
                    // continues to use after BindTexSlot; releasing immediately can recycle it and
                    // leave the attached M2 sampling the client's missing-texture green.
                }
            }

            // subObj->initFlags & 1: when set (character fully initialised), sub_831630 checks the
            // BoneIndicesByID LUT; if the attach point is absent and zero2==0 it exits silently.
            // Passing forceAttach=true (zero2=1) bypasses the early-exit for non-standard points
            // (e.g. attach_id=19) that collection M2s use.
            uint32_t subInitFlags = GuardedReadU32(reinterpret_cast<uint8_t*>(subObj) + m2::kOffInstInitFlags);
            EquipLog("  Phase3[%zu] attach=%u rctx=0x%p subObj_initFlags=0x%X isCollection=%d",
                     i, e.attachId, rctx, subInitFlags, (int)isCollection);
            gm2::AttachToScene(rctx, subObj, e.attachId, isCollection);

//            void* nativeParent = NativeParentGuarded(rctx);
            BoneRemap remap = BuildBoneRemapGuarded(rctx, subObj);
//            EquipLog("  Phase3[%zu] boneRemap.count=%u requestedParent=0x%p nativeParent=0x%p",
//                     i, (uint32_t)remap.count, subObj, nativeParent);
            EquipLog("  Phase3[%zu] boneRemap.count=%u", i, (uint32_t)remap.count);
            // Propagate boneRemap to siblings sharing (keyBuf, attachId, texBuf).
            for (auto& e2 : entries)
            {
                if (!SameAttachModelGroup(e2, e)) continue;
                if (remap.count > 0) e2.boneRemap = remap;
            }

            // Apply merged geoset filter for entries sharing this (keyBuf, attachId, texBuf).
            if (isCollection)
            {
                GeosetFilter merged = {};
                for (auto& e2 : entries)
                {
                    if (!SameAttachModelGroup(e2, e)) continue;
                    for (uint32_t fi = 0; fi < e2.geoFilter.count && merged.count < 16; ++fi)
                    {
                        bool dup = false;
                        for (uint32_t fj = 0; fj < merged.count; ++fj)
                            if (merged.ids[fj] == e2.geoFilter.ids[fi]) { dup = true; break; }
                        if (!dup) merged.ids[merged.count++] = e2.geoFilter.ids[fi];
                    }
                }
                EquipLog("  Phase3[%zu] geoFilter merged.count=%u ids=[%u %u %u %u]",
                         i, merged.count,
                         merged.count > 0 ? merged.ids[0] : 0,
                         merged.count > 1 ? merged.ids[1] : 0,
                         merged.count > 2 ? merged.ids[2] : 0,
                         merged.count > 3 ? merged.ids[3] : 0);
                void* mdl = GuardedReadPtr(reinterpret_cast<uint8_t*>(rctx) + m2::kOffInstModel);
                auto* skin = mdl ? gm2::Skin(mdl) : nullptr;
                if (skin)
                {
                    // The GPU IB was already built by kFinalizeSkin during Phase1's synchronous
                    // GetRenderCtx load. OnM2SkinFinalize (fallback path via g_currentRebuildCmo)
                    // applied the merged filter before the IB was written. Do not call
                    // gm2::FinalizeSkin here: it calls kFinalizeSkin (0x837A40) directly,
                    // bypassing sub_838490's post-call buffer-fill step. That creates a new empty
                    // D3D buffer that never gets filled, making the model invisible.
                    EquipLog("  Phase3[%zu] skin present: IB already built by synchronous load, skip re-finalize", i);
                }
                else
                {
                    EquipLog("  Phase3[%zu] skin null: OnM2SkinFinalize will apply filter async", i);
                }
            }

            // Release our Phase-1 ref. The scene holds a ref from AttachToScene, so rctx stays
            // alive. e.renderCtx continues to point to it and is used by OnM2PerFrameUpdate.
            gm2::ReleaseRenderCtx(rctx);
        }
    }

    // Best-effort native fallback only -- see the sidecar-first note on LookupItemDisplayId below.
    // Tries db2::item::kIdTable as a raw record* table indexed by (id - minId), same convention as
    // db2::map / db2::chrraces in this same file, not a funneled accessor like itemdisplayinfo has.
    //
    // Both interpretations tried so far (pointer-array indirection, then flat/contiguous inline
    // records at kOffDisplayInfoId=0x14) have failed their own self-check in the field: reading back
    // record[0] does not equal itemEntry, and the failure is a genuine mismatched value (not a guard
    // fault), so the record IS pointing at readable memory -- just not at the row we think, or the
    // client's compacted per-item cache doesn't redundantly store the ID at field 0 the way a raw
    // DBC row would (in which case this self-check can't validate the row at all, correct offset or
    // not). Diagnosing this further needs a live memory correlation against a known-good (itemEntry,
    // displayId) pair -- when WXL_EQUIP_LOG is enabled, this dumps a hex window around the computed
    // candidate address so that can be done by inspection instead of more blind guessing.
    static uint32_t LookupItemDisplayIdNative(uint32_t itemEntry)
    {
        int32_t minId = static_cast<int32_t>(GuardedReadU32(reinterpret_cast<void*>(db2::item::kMinId)));
        int32_t maxId = static_cast<int32_t>(GuardedReadU32(reinterpret_cast<void*>(db2::item::kMaxId)));
        if (static_cast<int32_t>(itemEntry) < minId || static_cast<int32_t>(itemEntry) > maxId)
        {
            EquipLog("  LookupItemDisplayIdNative: entry=%u out of range [%d,%d] "
                     "(min/maxId themselves may be stale for this build)", itemEntry, minId, maxId);
            return 0;
        }

        uint8_t* record = reinterpret_cast<uint8_t*>(db2::item::kIdTable)
                         + (itemEntry - static_cast<uint32_t>(minId)) * db2::item::kRecordSize;

        uint32_t selfId = GuardedReadU32(record + 0x00);
        if (selfId != itemEntry)
        {
            EquipLog("  LookupItemDisplayIdNative: entry=%u SELF-CHECK MISMATCH record[0]=%u -- "
                     "record pointer/indexing is wrong, not just the DisplayInfoID offset", itemEntry, selfId);
            if (EquipLogEnabled())
            {
                // Dump 16 aligned uint32 words (-16..+48 bytes around the candidate record) so a
                // known-good (itemEntry, displayId) pair can be located by eye. Each word is read
                // independently guarded, so one bad word can't blank out the rest of the dump.
                char hex[256] = {};
                char* p = hex;
                char* end = hex + sizeof(hex) - 4;
                for (int base = -16; base < 48 && p < end; base += 4)
                {
                    uint32_t word = GuardedReadU32(record + base);
                    for (int b = 0; b < 4 && p < end; ++b)
                    {
                        uint8_t byteVal = static_cast<uint8_t>((word >> (b * 8)) & 0xFF);
                        p += std::snprintf(p, 4, "%02X ", byteVal);
                    }
                }
                EquipLog("  LookupItemDisplayIdNative: hex[-16..+48) around record=0x%p (LE words): %s",
                         record, hex);
            }
            return 0;
        }

        constexpr size_t kOffDisplayInfoId = 0x14; // unverified -- see comment above
        uint32_t displayId = GuardedReadU32(record + kOffDisplayInfoId);
        EquipLog("  LookupItemDisplayIdNative: entry=%u -> displayId=%u", itemEntry, displayId);
        return displayId;
    }

    // Resolves a raw item entry id to its ItemDisplayInfo display id.
    //
    // Checks the WXLItemEntryDisplay.csv sidecar override table first -- populate it with known-good
    // (ItemEntry, DisplayID) pairs from your own item database/SQL dump and this is a guaranteed-correct
    // O(1) lookup that never touches guesswork about the client's in-memory Item.dbc layout. Falls back
    // to LookupItemDisplayIdNative only for entries not listed there.
    static uint32_t LookupItemDisplayId(uint32_t itemEntry)
    {
        LoadSidecarModels(); // no-op after the first call
        auto it = g_sidecarItemDisplay.find(itemEntry);
        if (it != g_sidecarItemDisplay.end())
        {
            EquipLog("  LookupItemDisplayId: entry=%u -> displayId=%u (sidecar override)",
                     itemEntry, it->second);
            return it->second;
        }
        return LookupItemDisplayIdNative(itemEntry);
    }

    // Resolves displayId -> ItemDisplayInfo -> weapon model/texture patches and registers them via
    // VPathPopulateGlobal. owner28 is optional: pass a live "real CMO" pointer (from a real equip
    // event) to resolve forced race/gender weapon variants (Icon2 flag 0x80); pass nullptr for
    // pre-registration ahead of char-select (see PreregisterSidecarWeapons below), when no character
    // object exists yet to read ChrRaces/gender off of -- forced race/gender records are skipped in
    // that case and get patched later, reactively, on the wearer's first real equip instead.
    static void PatchWeaponModelByDisplayId(uint32_t displayId, uint32_t itemEntryForLog, void* owner28)
    {
        LoadSidecarModels(); // no-op after the first call

        alignas(4) uint8_t dispBuf[db2::itemdisplayinfo::kRecordSize] = {};
        // Must call the un-hooked trampoline, not the (now live-detoured) kLookup address directly --
        // hkItemDisplayLookup's OnItemDisplayLookup subscriber below is what substitutes Model1/Model2
        // with our virtual paths, and this call needs the TRUE native fields to decide what those
        // virtual paths should even be. See ItemDisplayInfoLookupNative's doc comment in GameHooks.hpp.
        uint32_t ok = wxl::runtime::game::ItemDisplayInfoLookupNative(displayId, dispBuf);
        if (!ok)
        {
            EquipLog("  weapon patch: ItemDisplayInfo lookup FAILED (itemEntry=%u displayId=%u)",
                     itemEntryForLog, displayId);
            return;
        }

        const char* modelName1 = *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffModel1);
        const char* modelName2 = *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffModel2);
        const char* texName1   = *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffTex1);
        const char* texName2   = *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffTex2);

        EquipLog("  weapon model resolved: itemEntry=%u displayId=%u model1='%s' model2='%s' tex1='%s' tex2='%s'",
                 itemEntryForLog, displayId,
                 modelName1 ? modelName1 : "(null)", modelName2 ? modelName2 : "(null)",
                 texName1 ? texName1 : "(null)", texName2 ? texName2 : "(null)");

        const char* icon2str = *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffIcon2);
        uint32_t icon2flags = 0;
        char customFolder[64] = {};
        if (icon2str && reinterpret_cast<uintptr_t>(icon2str) > 0x10000
            && *icon2str && (*icon2str == ':' || (*icon2str >= '0' && *icon2str <= '9')))
        {
            uint32_t dummyA_l = 0, dummyA_r = 0, dummyB_l = 0, dummyB_r = 0;
            ParseIcon2(icon2str, &dummyA_l, &dummyA_r, &dummyB_l, &dummyB_r,
                       &icon2flags, customFolder, sizeof(customFolder));
        }

        // Weapon models are not race/gender suffixed by default (matches kSlotConfig's Weapon rows,
        // defUseRace=defUseGender=false); Icon2 flag 0x80 can still force it for an oddball retail
        // export that does need per-race/gender weapon variants.
        const uint32_t pathFlags = (icon2flags & kFlagForceModelRaceGender) ? 0u : (0x2u | 0x4u);
        const bool needsRaceGender = (pathFlags & (0x2u | 0x4u)) != (0x2u | 0x4u);

        // Only resolve ChrRaces (via owner28, the same "real CMO" pointer the armor path calls cmo)
        // when a forced-race/gender weapon path could actually need it AND a live character is
        // available -- pre-registration (owner28 == nullptr) can never satisfy this, by design.
        const char* raceCode  = "";
        const char* genderStr = "";
        if (needsRaceGender && owner28)
        {
            uint32_t raceId = GuardedReadU32(reinterpret_cast<uint8_t*>(owner28) + m2::kOffCmoRace);
            uint32_t low    = *reinterpret_cast<uint32_t*>(db2::chrraces::kMinId);
            uint32_t high   = *reinterpret_cast<uint32_t*>(db2::chrraces::kMaxId);
            if (raceId >= low && raceId <= high)
            {
                uint8_t* idTable = *reinterpret_cast<uint8_t**>(db2::chrraces::kIdTable);
                void* chrRec = *reinterpret_cast<void**>(idTable + (raceId - low) * sizeof(void*));
                if (chrRec)
                    raceCode = *reinterpret_cast<const char**>(
                        reinterpret_cast<uint8_t*>(chrRec) + db2::chrraces::kOffRecordPrefix);
            }
            uint32_t genderIdx = GuardedReadU32(reinterpret_cast<uint8_t*>(owner28) + m2::kOffCmoGender);
            genderStr = *reinterpret_cast<const char**>(db2::genderstrings::kTable + genderIdx * sizeof(void*));
            if (!raceCode) raceCode = "";
            if (!genderStr) genderStr = "";
        }

        // Sidecar Folder column (WXLItemEntryDisplay.csv) takes priority when given for this
        // displayId -- see LoadItemDisplaySidecarFile's doc comment -- but still falls back to the
        // Weapon/Shield guesses below if it's missing, blank, or turns out not to be readable, so a
        // stale/incorrect sidecar folder degrades gracefully instead of hard-failing the model.
        const char* folderCandidates[3];
        uint32_t folderCandidateCount = 0;
        {
            auto folderIt = g_sidecarModelFolder.find(displayId);
            if (folderIt != g_sidecarModelFolder.end() && !folderIt->second.empty())
                folderCandidates[folderCandidateCount++] = folderIt->second.c_str();
            for (const char* def : { "Weapon", "Shield" })
            {
                bool dup = false;
                for (uint32_t i = 0; i < folderCandidateCount; ++i)
                    if (_stricmp(folderCandidates[i], def) == 0) { dup = true; break; }
                if (!dup) folderCandidates[folderCandidateCount++] = def;
            }
        }

        auto patchWeaponModel = [&](const char* modelName, const char* texName, uint32_t modelColumn)
        {
            if (!modelName || !*modelName) return;

            // Defensive ":geosets" strip -- collection-style suffixes aren't meaningful here since
            // we're patching the existing native model in place, not filtering attached geometry.
            char stem[264];
            const char* colon = std::strchr(modelName, ':');
            size_t len = colon ? static_cast<size_t>(colon - modelName) : std::strlen(modelName);
            if (len >= sizeof(stem)) len = sizeof(stem) - 1;
            std::memcpy(stem, modelName, len);
            stem[len] = '\0';
            if (!stem[0]) return;

            if (needsRaceGender && (!*raceCode || !*genderStr))
            {
                EquipLog("  weapon patch: model=%u '%s' -- forced race/gender path (Icon2 flag 0x80) "
                         "requested but race/gender could not be resolved (%s), skipping",
                         modelColumn, stem, owner28 ? "lookup failed" : "no character yet -- "
                         "will patch reactively on first real equip");
                return;
            }

            for (uint32_t fi = 0; fi < folderCandidateCount; ++fi)
            {
                const char* folder = folderCandidates[fi];
                char modelPath[264];
                BuildSlotPath(modelPath, stem, raceCode, genderStr, pathFlags, folder, false,
                              customFolder[0] ? customFolder : nullptr);

                // Base ModelTexture_N -- baked directly into the model's own texture-unit table
                // (OBJECT_SKIN promoted to HARDCODED, same as VPathPopulate does for a freshly-attached
                // armor piece) rather than bound at runtime. Vanilla 3.3.5a only auto-binds
                // ModelTexture_1/2 natively for Head/Shoulder; for weapons/shields nothing else does
                // it, which is why an otherwise-correctly-resolved weapon/shield renders with a
                // blank/white texture. Baking it into the served bytes means the very first load
                // already has the right texture -- no per-frame render-context matching or BindTexSlot
                // call needed. WEAPON_BLADE records are never touched by this bake -- only an explicit
                // sidecar TextureType=3 row (see LoadMaterialSidecarFile) can hardcode one; a
                // weapon/shield with an unclaimed WEAPON_BLADE slot renders blank until that row
                // exists, by design.
                char texPath[264] = {};
                if (texName && *texName)
                    BuildTexPath(texPath, sizeof(texPath), texName, raceCode, genderStr,
                                 pathFlags, folder, false,
                                 customFolder[0] ? customFolder : nullptr, stem);

                // Sidecar material patch (WXLItemDisplayModelMaterials.csv) -- extra layers beyond
                // ModelTexture_1/2 (glow, tint, edgefade...). Independent of texPath above; either or
                // both can be empty and VPathPopulateGlobal just skips whichever step has nothing to do.
                char matTexSpec[2048] = {};
                BuildMaterialPatchSpecWeaponFallback(matTexSpec, sizeof(matTexSpec), displayId, modelColumn, 0, stem,
                                       raceCode, genderStr, folder, false,
                                       customFolder[0] ? customFolder : nullptr, stem);
                if (!matTexSpec[0])
                    EquipLog("  weapon patch: model=%u '%s' folder='%s' -- no material sidecar rows",
                             modelColumn, stem, folder);

                if (!texPath[0] && !matTexSpec[0])
                {
                    EquipLog("  weapon patch: model=%u '%s' folder='%s' -- nothing to bake (no "
                             "ModelTexture_N, no material sidecar rows)", modelColumn, stem, folder);
                    continue; // a different folder's sidecar rows could still match; keep trying
                }

                // VPathPopulateGlobal is a no-op (returns true, doesn't re-register) if this exact
                // (path, displayId) pair was already registered by an earlier call -- e.g.
                // PreregisterSidecarWeapons() already did this at startup, and this is a later
                // reactive call for the same displayId. Cheap and safe to call from both the eager
                // and reactive paths without tracking which one won. It also returns false if the real
                // file can't be read under this folder -- expected for every candidate that isn't the
                // right one, not just an I/O fluke, so a miss here just means "try the next folder",
                // not "give up". The bytes are now keyed by a virtual path (modelPath mangled with
                // displayId, returned in vModelPath below) rather than modelPath itself, so distinct
                // displayIds sharing modelPath no longer clobber each other's patch -- but nothing will
                // intercept a load of modelPath unmodified any more; whatever ultimately resolves the
                // weapon's model needs to be pointed at vModelPath instead.
                char vModelPath[280] = {};
                bool registered = VPathPopulateGlobal(modelPath, displayId, texPath[0] ? texPath : nullptr,
                                                       matTexSpec[0] ? matTexSpec : nullptr,
                                                       vModelPath, sizeof(vModelPath));
                EquipLog("  weapon patch: model=%u path='%s' vpath='%s' tex='%s' spec='%s' registered=%d",
                         modelColumn, modelPath, vModelPath, texPath, matTexSpec, registered ? 1 : 0);

                if (!registered)
                    continue; // wrong folder for this displayId -- try the next candidate

                // Only publish the substitution once the bytes are actually behind vModelPath -- a
                // failed registration must not redirect ItemDisplayInfo at a path nothing will ever
                // serve. modelColumn is 0 (Model1) or 1 (Model2), matching WeaponVPathOverride::vModel's
                // index.
                //
                // IMPORTANT: Model1/Model2 for WEAPONS AND SHIELDS hold a bare filename natively
                // (unlike Head/Shoulder/etc., whose native fields are already full archive paths) --
                // the client's own loader prepends "Item\ObjectComponents\<folder>\" itself before
                // opening the file, using whichever folder actually matches the item (Weapon vs
                // Shield) -- not necessarily the one we tried first. vModelPath is the FULL virtual
                // path under the folder that just proved correct (needed as-is for the
                // VPathPopulateGlobal/g_globalOverrides key, which matches what the client requests
                // *after* re-adding that same prefix), so only the basename goes into the DB row --
                // writing the full path here made the client double up the directory and miss the
                // override entirely (rendered as the default missing-model cube).
                if (vModelPath[0] && modelColumn < 2)
                {
                    const char* base = vModelPath;
                    for (const char* p = vModelPath; *p; ++p)
                        if (*p == '\\' || *p == '/') base = p + 1;
                    g_weaponVPaths[displayId].vModel[modelColumn] = base;
                }
                return; // found the right folder for this model -- done
            }

            char triedFolders[128] = {};
            for (uint32_t fi = 0; fi < folderCandidateCount; ++fi)
            {
                if (fi) std::strncat(triedFolders, ", ", sizeof(triedFolders) - std::strlen(triedFolders) - 1);
                std::strncat(triedFolders, folderCandidates[fi], sizeof(triedFolders) - std::strlen(triedFolders) - 1);
            }
            EquipLog("  weapon patch: model=%u '%s' -- not found under any candidate folder (%s)",
                     modelColumn, stem, triedFolders);
        };

        patchWeaponModel(modelName1, texName1, 0);
        patchWeaponModel(modelName2, texName2, 1);

        // TODO: attaching an *additional* M2 to a weapon (as opposed to patching the one the client
        // already loads) still needs the same three pieces the old comment here described: a real
        // attach-point RE pass for weapon-specific AT_ ids, SafeGetRenderCtx(owner28, path), and the
        // BindTexSlot/AttachToScene sequence the armor path uses. Out of scope for the baked-texture
        // and material-patch features above, neither of which need it.
    }

    // Walks every itemEntry -> displayId mapping known at sidecar-load time (WXLItemEntryDisplay.csv)
    // and registers its weapon patch immediately, before any character -- including the char-select
    // preview model -- has a chance to load it first.
    //
    // Why this exists: VPathPopulateGlobal is explicitly process-lifetime and path-keyed, not
    // per-character (see its doc comment in VirtualPath.hpp), so it was always meant to support being
    // populated ahead of time rather than only reactively. The reactive-only path (OnWeaponVisualChange
    // registering on first equip) loses the race against the client's own char-select preview load for
    // whatever weapon a character logs in already wielding -- that preview happens before the
    // character ever exists as an object our hooks can see, so no reactive event can possibly fire in
    // time. Pre-registering everything the sidecar table knows about at startup closes that gap
    // entirely: by the time char-select even renders a model, every known custom weapon's override is
    // already sitting in g_globalOverrides.
    static void PreregisterSidecarWeapons()
    {
        if (g_sidecarItemDisplay.empty()) return;
        EquipLog("weapon preregister: patching %zu sidecar-known weapon(s) ahead of char-select",
                 g_sidecarItemDisplay.size());
        for (const auto& [itemEntry, displayId] : g_sidecarItemDisplay)
            PatchWeaponModelByDisplayId(displayId, itemEntry, nullptr);
    }

    // ─── Event handler implementations ───────────────────────────────────────────

    // Tracks weapon-visual field changes in g_weaponAttached and, once the item entry resolves to a
    // real ItemDisplayInfo record, registers a baked-texture/material patch for the weapon model the
    // client itself already loads/attaches natively, via a single VPathPopulateGlobal call that can
    // do either or both of:
    //   1. Base ModelTexture_1/2 -- baked directly into the model's own texture-unit table
    //      (OBJECT_SKIN promoted to HARDCODED) the same way VPathPopulate does for a
    //      freshly-attached armor piece. Vanilla 3.3.5a only auto-binds ModelTexture_1/2 natively for
    //      Head/Shoulder; for weapons nothing else does it, which is why an otherwise-correctly-
    //      resolved weapon renders with a blank/white texture. WEAPON_BLADE records are left
    //      untouched by this bake -- only an explicit sidecar TextureType=3 row can hardcode one.
    //   2. Sidecar material patching (WXLItemDisplayModelMaterials.csv) -- extra layers beyond
    //      ModelTexture_1/2 (glow, tint, etc.).
    // Both are keyed on the model's own real path, not a per-wearer mangled name, so the first
    // registration for a given displayId covers every subsequent wearer for the rest of the session;
    // there's nothing left to do at render time (no PerFrame render-context matching, no BindTexSlot
    // call) since the served bytes already have the texture baked in on the very first load.
    // owner28 ("the real CMO", same object RebuildAllModels derives from cmo for armor) is resolved
    // here when available, with a PerFrame retry sweep below for the case where the scene node's
    // owner back-pointer isn't initialised yet at the time this fires.
    void EquipExtension::OnWeaponVisualChange(const ev::WeaponVisualChangeArgs& a)
    {
        static const char* kSlotNames[3] = { "mainhand", "offhand", "ranged" };
        if (a.slot >= 3) return; // defensive; core only emits 0/1/2
        const char* slotName = kSlotNames[a.slot];

        // wxl::game::unit::Model() dereferences unit->model unguarded. a.unit is derived generically
        // (any object writing field 0x139/0x13B/0x13D, not verified safe for every possible object
        // type in the game, only proven correct for our own player object) -- read it guarded instead.
        void* subObj = a.unit
            ? GuardedReadPtr(reinterpret_cast<uint8_t*>(a.unit) + unit::kUnitModelField)
            : nullptr;
        if (!subObj)
        {
            EquipLog("*** OnWeaponVisualChange slot=%u(%s) itemEntry=%u unit=0x%p -- null model, skip",
                     a.slot, slotName, a.itemEntry, a.unit);
            return;
        }

        void* owner28 = GuardedReadPtr(reinterpret_cast<uint8_t*>(subObj) + m2::kOffSceneNodeOwner);

        auto& entry = g_weaponAttached[subObj][a.slot];
        entry.slot      = a.slot;
        entry.itemEntry = a.itemEntry;
        entry.subObj    = subObj;
        entry.owner28   = owner28;
        entry.renderCtx = nullptr;

        EquipLog("*** OnWeaponVisualChange slot=%u(%s) itemEntry=%u unit=0x%p subObj=0x%p owner28=0x%p",
                 a.slot, slotName, a.itemEntry, a.unit, subObj, owner28);

        if (a.itemEntry == 0)
            return; // slot cleared; nothing to resolve

        uint32_t displayId = LookupItemDisplayId(a.itemEntry);
        if (displayId == 0)
            return;

        PatchWeaponModelByDisplayId(displayId, a.itemEntry, owner28);

        // TODO: attaching an *additional* M2 to a weapon (as opposed to patching the one the client
        // already loads) still needs the same three pieces the old comment here described: a real
        // attach-point RE pass for weapon-specific AT_ ids, SafeGetRenderCtx(owner28, path), and the
        // BindTexSlot/AttachToScene sequence the armor path uses. Out of scope for the baked-texture
        // and material-patch features above, neither of which need it.
    }

    // Fires for every successful native ItemDisplayInfo lookup (GameHooks' hkItemDisplayLookup),
    // including the client's own model loader -- this is what actually gets the client to fetch our
    // rewritten weapon models instead of the real ones. Substitutes Model1/Model2 in place when
    // PatchWeaponModelByDisplayId has already registered a virtual path for this displayId/column;
    // a miss here is the overwhelmingly common case (every non-weapon item, and any weapon slot with
    // nothing to bake) and costs one hash lookup.
    void EquipExtension::OnItemDisplayLookup(const ev::ItemDisplayLookupArgs& a)
    {
        const auto it = g_weaponVPaths.find(a.displayId);
        if (it == g_weaponVPaths.end()) return;

        auto* rec = static_cast<uint8_t*>(a.record);
        if (!it->second.vModel[0].empty())
            *reinterpret_cast<const char**>(rec + db2::itemdisplayinfo::kOffModel1) = it->second.vModel[0].c_str();
        if (!it->second.vModel[1].empty())
            *reinterpret_cast<const char**>(rec + db2::itemdisplayinfo::kOffModel2) = it->second.vModel[1].c_str();
    }

    // See the doc comment on the declaration in EquipExtension.hpp for why this exists alongside
    // the older OnItemSlotChange/OnWeaponVisualChange/LookupItemDisplayId trigger points.
    void EquipExtension::OnModelLoadPre(const ev::ModelLoadArgs&)
    {
        LoadSidecarModels(); // no-op after the first call; runs PreregisterSidecarWeapons at its end
    }

    void EquipExtension::OnItemSlotChange(const ev::ItemSlotChangeArgs& a)
    {
        if (a.modelSlot >= 14) return;
        const SlotConfig& cfg = kSlotConfig[a.modelSlot];

        void* cmo = a.charModelObj;
        void* subObj = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoSceneNode);

        // Unequip path: display_id == 0 when the slot is being cleared via the equip hook.
        uint32_t displayId = 0;
        if (a.itemDataPtr)
            displayId = GuardedReadU32(a.itemDataPtr);

        EquipLog("--- OnItemSlotChange cmo=0x%p slot=%u idp=0x%p displayId=%u subObj=0x%p",
                 cmo, a.modelSlot, a.itemDataPtr, displayId, subObj);

        if (displayId == 0)
        {
            EquipLog("  unequip: detach+rebuild");
            DetachSlotEntries(cmo, a.modelSlot);
            if (subObj) RebuildAllModels(cmo);
            return;
        }

        // Look up the ItemDisplayInfo record. Uses the native trampoline, not the live-hooked
        // kLookup address, for the same reason as PatchWeaponModelByDisplayId above -- this needs
        // the true native fields, not whatever a weapon-slot override may have substituted for some
        // other display id.
        alignas(4) uint8_t dispBuf[db2::itemdisplayinfo::kRecordSize] = {};
        uint32_t ok = wxl::runtime::game::ItemDisplayInfoLookupNative(displayId, dispBuf);
        const char* modelName1 = nullptr;
        const char* modelName2 = nullptr;
        const char* texName1   = nullptr;
        const char* texName2   = nullptr;
        const char* icon2str   = nullptr;
        if (ok)
        {
            modelName1 = *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffModel1);
            modelName2 = *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffModel2);
            texName1   = *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffTex1);
            texName2   = *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffTex2);
            icon2str   = *reinterpret_cast<const char**>(dispBuf + db2::itemdisplayinfo::kOffIcon2);
            EquipLog("  DBC ok: model1='%s' model2='%s' tex1='%s' tex2='%s' icon2ptr=0x%p icon2='%s'",
                     modelName1 ? modelName1 : "(null)", modelName2 ? modelName2 : "(null)",
                     texName1 ? texName1 : "(null)", texName2 ? texName2 : "(null)",
                     static_cast<const void*>(icon2str),
                     (icon2str && reinterpret_cast<uintptr_t>(icon2str) > 0x10000 && *icon2str)
                         ? icon2str : "(empty/invalid)");
        }
        else
        {
            EquipLog("  DBC lookup FAILED (displayId=%u not found)", displayId);
            return;
        }

        // Parse Icon2 attach config (start with defaults from slot config).
        uint32_t attachA_l = cfg.defAttach1, attachA_r = cfg.defAttach1;
        uint32_t attachB_l = cfg.defAttach2, attachB_r = cfg.defAttach2;
        uint32_t icon2flags = 0;
        char customFolder[64] = {};
        bool icon2AttachAExplicit = false;
        bool icon2AttachBExplicit = false;
        // Guard pointer range before dereferencing: a raw integer in g_disp+0x18 could be a
        // small non-null value (GeosetGroup integer) that would AV if dereferenced directly.
        if (icon2str && reinterpret_cast<uintptr_t>(icon2str) > 0x10000
            && *icon2str && (*icon2str == ':' || (*icon2str >= '0' && *icon2str <= '9')))
        {
            icon2AttachAExplicit = Icon2FieldHasValue(icon2str, 0);
            icon2AttachBExplicit = Icon2FieldHasValue(icon2str, 1);
            ParseIcon2(icon2str, &attachA_l, &attachA_r, &attachB_l, &attachB_r,
                       &icon2flags, customFolder, sizeof(customFolder));
        }

        // Use left-side attach values (0x80 sided-slot selection not yet implemented).
        uint32_t attachA = attachA_l;
        uint32_t attachB = attachB_l;

        EquipLog("  attachA=%u attachB=%u icon2flags=0x%X folder='%s'",
                 attachA, attachB, icon2flags, customFolder[0] ? customFolder : "(default)");

        // ChrRaces lookup for race code and gender string.
        uint32_t raceId = GuardedReadU32(reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoRace);
        uint32_t low    = *reinterpret_cast<uint32_t*>(db2::chrraces::kMinId);
        uint32_t high   = *reinterpret_cast<uint32_t*>(db2::chrraces::kMaxId);
        if (raceId < low || raceId > high)
        {
            EquipLog("  raceId=%u out of range [%u,%u], bail", raceId, low, high);
            return;
        }
        uint8_t* idTable = *reinterpret_cast<uint8_t**>(db2::chrraces::kIdTable);
        void*    chrRec  = *reinterpret_cast<void**>(idTable + (raceId - low) * sizeof(void*));
        if (!chrRec)
        {
            EquipLog("  chrRec null for raceId=%u, bail", raceId);
            return;
        }

        const char* raceCode  = *reinterpret_cast<const char**>(reinterpret_cast<uint8_t*>(chrRec) + db2::chrraces::kOffRecordPrefix);
        uint32_t    genderIdx = GuardedReadU32(reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoGender);
        const char* genderStr = *reinterpret_cast<const char**>(db2::genderstrings::kTable + genderIdx * sizeof(void*));
        if (!raceCode || !genderStr)
        {
            EquipLog("  raceCode or genderStr null (raceCode=0x%p genderStr=0x%p), bail",
                     static_cast<const void*>(raceCode), static_cast<const void*>(genderStr));
            return;
        }

        EquipLog("  race='%s' gender='%s'", raceCode, genderStr);

        if (!subObj)
        {
            EquipLog("  subObj null, bail");
            return;
        }

        // Remove stale entries for this model slot before adding new ones.
        DetachSlotEntries(cmo, a.modelSlot);

        // Helper: strip optional ":N[,M]" geoset suffix from the model name field.
        auto splitModelName = [](const char* name, char* stemOut, size_t stemSz, GeosetFilter* filterOut)
        {
            *filterOut = {};
            if (!name || !*name) { stemOut[0] = '\0'; return; }
            const char* colon = std::strchr(name, ':');
            if (colon)
            {
                size_t len = static_cast<size_t>(colon - name);
                if (len >= stemSz) len = stemSz - 1;
                std::memcpy(stemOut, name, len);
                stemOut[len] = '\0';
                *filterOut = ParseGeosetFilter(colon + 1);
            }
            else { std::strncpy(stemOut, name, stemSz - 1); stemOut[stemSz - 1] = '\0'; }
        };

        bool rowHasCollection = false;
        bool rowHasNormal = false;
        AnalyzeModelList(modelName1, &rowHasCollection, &rowHasNormal);
        AnalyzeModelList(modelName2, &rowHasCollection, &rowHasNormal);
        const bool mixedCollectionRow = rowHasCollection && rowHasNormal;

        // Effective path flags: collection models always use race+gender; normal models use slot defaults.
        // Icon2 flags 0x2/0x4 can still suppress race/gender regardless.
        auto effectiveFlags = [&](bool isCollection) -> uint32_t
        {
            uint32_t ef = icon2flags;
            if (isCollection && !(ef & 0x40))
            {
                const bool appendsRace = (ef & 0x4) == 0;
                const bool appendsGender = (ef & 0x2) == 0;
                if (appendsRace && appendsGender)
                    ef |= 0x40;
            }
            if (!isCollection)
            {
                const bool forceModelRaceGender =
                    (ef & kFlagForceModelRaceGender) || ((ef & 0x40) && !mixedCollectionRow);
                if (!forceModelRaceGender)
                {
                    if (!cfg.defUseRace)   ef |= 0x4;
                    if (!cfg.defUseGender) ef |= 0x2;
                }
            }
            return ef;
        };

        auto attachForEntry = [](bool isCollection, bool mixedRow, uint32_t columnAttach,
                                 uint32_t defaultAttach, bool explicitAttach) -> uint32_t
        {
            if (mixedRow) return isCollection ? kCollectionAttach : defaultAttach;
            if (isCollection && !explicitAttach) return kCollectionAttach;
            return columnAttach;
        };

        auto addModelList = [&](const char* label, const char* modelList, const char* texList,
                                uint32_t columnAttach, uint32_t defaultAttach, bool explicitAttach)
        {
            bool anyModel = false;
            for (uint32_t idx = 0; idx < 16; ++idx)
            {
                char modelPart[264] = {};
                if (!CopyListPart(modelList, idx, modelPart, sizeof(modelPart))) break;
                if (!modelPart[0]) continue;
                anyModel = true;

                char texPart[264] = {};
                if (!CopyListPart(texList, idx, texPart, sizeof(texPart)))
                    CopyListPart(texList, 0, texPart, sizeof(texPart));

                char stem[264]; GeosetFilter geo;
                splitModelName(modelPart, stem, sizeof(stem), &geo);
                if (!stem[0])
                {
                    EquipLog("  %s[%u]: empty stem after split, skip", label, idx);
                    continue;
                }

                const bool isCollection = geo.count > 0;
                if (!isCollection && !SlotAllowsNormalObjectModel(a.modelSlot))
                {
                    EquipLog("  %s[%u]: normal model '%s' ignored for texture-only slot %u",
                             label, idx, stem, a.modelSlot);
                    continue;
                }

                const uint32_t ef = effectiveFlags(isCollection);
                uint32_t attach = attachForEntry(isCollection, mixedCollectionRow, columnAttach,
                                                 defaultAttach, explicitAttach);
                attach = InferObjectComponentAttach(stem, isCollection, explicitAttach, attach);
                if (isCollection && IsCollectionGloveBodyModel(a.modelSlot, stem))
                    attach = kCollectionAttach;
                if (attach == static_cast<uint32_t>(-1))
                {
                    EquipLog("  %s[%u]: attach==-1, skip", label, idx);
                    continue;
                }

                char modelPath[264];
                BuildSlotPath(modelPath, stem, raceCode, genderStr, ef, cfg.folder,
                              isCollection, customFolder);

                char texPath[264] = {};
                if (texPart[0])
                    BuildTexPath(texPath, sizeof(texPath), texPart, raceCode, genderStr,
                                 ef, cfg.folder, isCollection, customFolder, stem);

                char matTexSpec[2048] = {};
                const uint32_t modelColumn = (label && label[1] == '1') ? 0u : 1u;
                BuildMaterialPatchSpecArmor(matTexSpec, sizeof(matTexSpec), displayId,
                                            modelColumn, idx, stem, raceCode, genderStr,
                                            cfg.folder, isCollection, customFolder, stem);

                EquipLog("  %s[%u]: attach=%u path='%s' tex='%s' mat='%s' geoCount=%u",
                         label, idx, attach, modelPath, texPath[0] ? texPath : "(none)",
                         matTexSpec, geo.count);

                AttachEntry e = {};
                e.equipSlot = a.modelSlot;
                e.attachId  = attach;
                e.subObj    = subObj;
                std::memcpy(e.keyBuf, modelPath, sizeof(e.keyBuf));
                std::memcpy(e.texBuf, texPath,   sizeof(e.texBuf));
                std::memcpy(e.matTexBuf, matTexSpec, sizeof(e.matTexBuf));
                e.geoFilter = geo;
                if (isCollection && mixedCollectionRow)
                    e.mergeKey = ((a.modelSlot + 1) << 16) |
                                 ((label && label[1] == '1' ? 1u : 2u) << 8) |
                                 (idx & 0xFFu);
                if (e.mergeKey)
                    EquipLog("  %s[%u]: isolated collection mergeKey=0x%X", label, idx, e.mergeKey);
                g_attached[cmo].push_back(e);
            }

            if (!anyModel) EquipLog("  %s: no model name, skip", label);
        };

        auto addSidecarModels = [&]() -> bool
        {
            LoadSidecarModels();
            auto it = g_sidecarModels.find(displayId);
            if (it == g_sidecarModels.end() || it->second.empty()) return false;

            bool added = false;
            uint32_t sidecarIndex = 0;
            for (const SidecarModelEntry& sc : it->second)
            {
                if (sc.modelSlot != static_cast<uint32_t>(-1) && sc.modelSlot != a.modelSlot)
                    continue;

                uint32_t attach = sc.attachId;
                if (attach == static_cast<uint32_t>(-1))
                    attach = cfg.defAttach1;
                if (attach == static_cast<uint32_t>(-1))
                    continue;

                const bool isCollectionPath =
                    (sc.folder[0] && StartsWithCI(sc.folder, "Collections")) ||
                    sc.geoFilter.count > 0 ||
                    StartsWithCI(sc.model, "collections_");
                if (isCollectionPath && IsCollectionGloveBodyModel(a.modelSlot, sc.model))
                    attach = kCollectionAttach;
                if (!isCollectionPath && !SlotAllowsNormalObjectModel(a.modelSlot))
                {
                    EquipLog("  SC[%u]: normal model '%s' ignored for texture-only slot %u",
                             sidecarIndex, sc.model, a.modelSlot);
                    ++sidecarIndex;
                    continue;
                }
                const uint32_t modelFlags =
                    (sc.modelFlags == 0xffffffffu) ? effectiveFlags(isCollectionPath) : sc.modelFlags;
                const uint32_t textureFlags =
                    (sc.textureFlags == 0xffffffffu) ? effectiveFlags(isCollectionPath) : sc.textureFlags;

                char modelPath[264];
                BuildSlotPath(modelPath, sc.model, raceCode, genderStr, modelFlags, cfg.folder,
                              isCollectionPath, sc.folder[0] ? sc.folder : nullptr);

                char texPath[264] = {};
                if (sc.texture[0])
                    BuildTexPath(texPath, sizeof(texPath), sc.texture, raceCode, genderStr,
                                 textureFlags, cfg.folder, isCollectionPath,
                                 sc.folder[0] ? sc.folder : nullptr, sc.model);

                char matTexSpec[2048] = {};
                BuildMaterialPatchSpecArmor(matTexSpec, sizeof(matTexSpec), displayId,
                                            static_cast<uint32_t>(-1), sidecarIndex, sc.model,
                                            raceCode, genderStr, cfg.folder, isCollectionPath,
                                            sc.folder[0] ? sc.folder : nullptr, sc.model);

                EquipLog("  SC[%u]: attach=%u path='%s' tex='%s' mat='%s' geoCount=%u slot=%u",
                         sidecarIndex, attach, modelPath, texPath[0] ? texPath : "(none)",
                         matTexSpec, sc.geoFilter.count, sc.modelSlot);

                AttachEntry e = {};
                e.equipSlot = a.modelSlot;
                e.attachId  = attach;
                e.subObj    = subObj;
                std::memcpy(e.keyBuf, modelPath, sizeof(e.keyBuf));
                std::memcpy(e.texBuf, texPath,   sizeof(e.texBuf));
                std::memcpy(e.matTexBuf, matTexSpec, sizeof(e.matTexBuf));
                e.geoFilter = sc.geoFilter;
                g_attached[cmo].push_back(e);
                added = true;
                ++sidecarIndex;
            }

            if (added)
                EquipLog("  sidecar used for displayId=%u, DBC model lists skipped", displayId);
            return added;
        };

        if (!addSidecarModels())
        {
            addModelList("M1", modelName1, texName1, attachA, cfg.defAttach1, icon2AttachAExplicit);
            addModelList("M2", modelName2, texName2, attachB, cfg.defAttach2, icon2AttachBExplicit);
        }

        EquipLog("  calling RebuildAllModels");
        RebuildAllModels(cmo);
    }

    void EquipExtension::OnItemSlotClear(const ev::ItemSlotClearArgs& a)
    {
        if (a.equipSlotWow >= 19) return;
        uint32_t modelSlot = kEquipToModelSlot[a.equipSlotWow];
        if (modelSlot == static_cast<uint32_t>(-1)) return;

        void* cmo = a.charModelObj;

        DetachSlotEntries(cmo, modelSlot);

        void* subObj = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoSceneNode);
        if (subObj) RebuildAllModels(cmo);
    }

    void EquipExtension::OnM2PerFrameUpdate(const ev::M2PerFrameUpdateArgs& a)
    {
        void* renderCtx = a.renderCtx;

        // Weapon owner28 retry sweep: OnWeaponVisualChange can fire before the character's scene
        // node has an initialised owner back-pointer (owner28 == null), same timing window
        // RebuildAllModels already defers around for armor via this same PerFrame tick. Runs
        // independent of g_attached below -- must not be skipped by g_attached's own early-return.
        if (!g_weaponAttached.empty())
        {
            for (auto& [subObj, entries] : g_weaponAttached)
            {
                for (auto& e : entries)
                {
                    if (e.itemEntry == 0 || e.owner28) continue; // empty slot, or already resolved
                    void* owner28 = GuardedReadPtr(reinterpret_cast<uint8_t*>(subObj) + m2::kOffSceneNodeOwner);
                    if (owner28)
                    {
                        e.owner28 = owner28;
                        EquipLog("  weapon owner28 resolved (deferred): slot=%u itemEntry=%u subObj=0x%p owner28=0x%p",
                                 e.slot, e.itemEntry, subObj, owner28);
                    }
                }
            }
        }

        if (g_attached.empty()) return;

        // Purge dead CMOs: cmo->sceneNode is zeroed when WoW frees the character model object.
        // Without purging, a stale entry can mistakenly match a reused CMO address.
        for (auto it = g_attached.begin(); it != g_attached.end(); )
        {
            void* sub = GuardedReadPtr(reinterpret_cast<uint8_t*>(it->first) + m2::kOffCmoSceneNode);
            if (!sub)
            {
                void* deadCmo = it->first;
                VPathEvictCmo(deadCmo);
                it = g_attached.erase(it);
            }
            else      ++it;
        }
        if (g_attached.empty()) return;

        // Character-model PerFrame sweep: fires when the character's sceneNode is the current
        // renderCtx. Two responsibilities:
        //   1. Pending-attach rebuild (entries with null renderCtx).
        //   2. Per-frame bone copy driven from the character model itself.
        // Driving bone copy here guarantees it runs every frame regardless of whether the
        // collection model's own PerFrame has fired yet, avoiding the race where collBuf is
        // null on the collection model's first PerFrame and the entry's renderCtx changes
        // before collBuf becomes non-null.
        for (auto& [cmo, entries] : g_attached)
        {
            void* sceneNode = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoSceneNode);
            if (sceneNode != renderCtx) continue;

            bool hasPending = false;
            for (const auto& e : entries)
                if (!e.renderCtx) { hasPending = true; break; }
            if (hasPending) { RebuildAllModels(cmo); return; }

            // charBuf: character bone palette. Valid when the character model is rendering.
            auto* charBuf = reinterpret_cast<uint8_t*>(
                GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) + m2::kOffInstBonePalette));
            if (charBuf)
            {
                for (auto& entry : entries)
                {
                    if (!IsCollectionEntry(entry)) continue;
                    if (!entry.renderCtx || entry.boneRemap.count == 0) continue;
                    auto* collBuf = reinterpret_cast<uint8_t*>(
                        GuardedReadPtr(reinterpret_cast<uint8_t*>(entry.renderCtx) + m2::kOffInstBonePalette));
                    if (!collBuf) continue;
                    if (!entry.charSweepApplied)
                    {
                        uint32_t charInitF = GuardedReadU32(
                            reinterpret_cast<uint8_t*>(renderCtx) + m2::kOffInstInitFlags);
                        uint32_t collInitF = GuardedReadU32(
                            reinterpret_cast<uint8_t*>(entry.renderCtx) + m2::kOffInstInitFlags);
                        EquipLog("  CharSweep(bone): rctx=0x%p charInitFlags=0x%X collBuf=0x%p collInitFlags=0x%X -> APPLYING",
                                 entry.renderCtx, charInitF, static_cast<void*>(collBuf), collInitF);
                        entry.charSweepApplied = true;
                    }
                    const BoneRemap& remap = entry.boneRemap;
																							  
																						
                    if (!CopyRemappedBonesGuarded(collBuf, charBuf, remap))
																					 
                    {
                        EquipLog("  CharSweep(bone): guarded copy failed rctx=0x%p count=%u",
                                 entry.renderCtx, (uint32_t)remap.count);
														  
													 
						 
																		 
                        entry.renderCtx = nullptr;
												   

                    }
                }
            }
            return;
        }

        for (auto& [cmo, entries] : g_attached)
        {
            for (auto& entry : entries)
            {
                if (entry.renderCtx != renderCtx) continue;

                // Liveness check: DetachSlot zeroes the model pointer (render_ctx+0x2c) without
                // freeing the render_ctx allocation. If evicted (by vanilla slot handlers sharing
                // the same attach point), re-attach immediately.
                void* m2Live = GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) + m2::kOffInstModel);
                if (!m2Live)
                {
                    // Use owner28 (scene-node's back-pointer to its owning CMO) for GetRenderCtx,
                    // not the slot-dispatch 'cmo'. See RebuildAllModels for the full explanation.
                    void* owner28 = GuardedReadPtr(reinterpret_cast<uint8_t*>(entry.subObj) + m2::kOffSceneNodeOwner);
                    if (owner28 && entry.keyBuf[0])
                    {
                        // SafeGetRenderCtx guards against dangling hash-table buckets (same
                        // root cause as the RebuildAllModels equip crash — see that function).
                        const char* pfKey = entry.mangledKeyBuf[0] ? entry.mangledKeyBuf : entry.keyBuf;
                        void* rctx = SafeGetRenderCtx(owner28, pfKey);
                        if (rctx)
                        {
                            if (entry.texBuf[0])
                            {
                                void* tex = gm2::LoadResource(entry.texBuf, 0);
                                if (!tex)
                                {
                                    EquipLog("  PerFrame reattach texture load failed '%s', skip", entry.texBuf);
                                    for (auto& e2 : entries)
                                        if (e2.renderCtx == renderCtx) e2.renderCtx = nullptr;
                                    gm2::ReleaseRenderCtx(rctx);
                                    continue;
                                }
                                else
                                {
                                    gm2::BindTexSlot(rctx, tex);
                                    // Keep the texture handle alive for the attached render context.
                                }
                            }
                            gm2::AttachToScene(rctx, entry.subObj, entry.attachId, IsCollectionEntry(entry));
                            for (auto& e2 : entries)
                                if (e2.renderCtx == renderCtx) e2.renderCtx = rctx;
                            gm2::ReleaseRenderCtx(rctx);
                        }
                        else
                        {
                            for (auto& e2 : entries)
                                if (e2.renderCtx == renderCtx) e2.renderCtx = nullptr;
                        }
                    }
                    else
                    {
                        for (auto& e2 : entries)
                            if (e2.renderCtx == renderCtx) e2.renderCtx = nullptr;
                    }
                    return;
                }

                // Bone matrix copy via 3-pass remap table.
                // charCtx is the character's render_ctx (= cmo->sceneNode = the outer frame call).
                // The collection M2's bone buffer is overwritten with corresponding char matrices.
                if (!IsCollectionEntry(entry)) return;
                if (entry.boneRemap.count == 0) return;													   
                void* charCtx = GuardedReadPtr(reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoSceneNode);
                if (charCtx && charCtx != renderCtx)
                {
                    BoneRemap& remap = entry.boneRemap;
                    // Retry if: (a) never built (count=0), or (b) all entries are 0xFF.
                    // Case (b) occurs when Phase3 runs while charBase is uninitialized:
                    // the function throws inside a pass, __except catches it and returns
                    // count=N/all-0xFF. Phase3's propagation doesn't overwrite it on
                    // subsequent rebuilds because the count != 0 guard prevents updates.
                    bool remapBroken = (remap.count == 0);
                    if (!remapBroken && remap.count > 0)
                    {
                        bool allFF = true;
                        for (uint32_t bi = 0; allFF && bi < remap.count; ++bi)
                            allFF = (remap.collToChar[bi] == 0xFF);
                        remapBroken = allFF;
                    }
                    if (remapBroken)
                    {
                        BoneRemap tryRemap = BuildBoneRemapGuarded(renderCtx, charCtx);
                        if (tryRemap.count > 0)
                        {
                            // Only adopt if at least one bone was matched; otherwise charBase
                            // is still unavailable and we'd just pin the same broken remap.
                            bool hasMatch = false;
                            for (uint32_t bi = 0; !hasMatch && bi < tryRemap.count; ++bi)
                                hasMatch = (tryRemap.collToChar[bi] != 0xFF);
                            if (hasMatch)
                            {
                                remap = tryRemap;
                                // Propagate to ALL entries sharing this renderCtx — not just
                                // count==0 ones, since they may be stuck with the broken remap.
                                for (auto& e2 : entries)
                                    if (e2.renderCtx == renderCtx)
                                        e2.boneRemap = tryRemap;
                                uint32_t xff2 = 0;
                                for (uint32_t bi = 0; bi < tryRemap.count; ++bi)
                                    if (tryRemap.collToChar[bi] == 0xFF) ++xff2;
                                EquipLog("  PerFrame[retry-ok] rctx=0x%p count=%u xff=%u"
                                         " map0-7=[%u %u %u %u %u %u %u %u]",
                                         renderCtx, (uint32_t)tryRemap.count, xff2,
                                         tryRemap.count>0 ? (uint32_t)tryRemap.collToChar[0] : 255u,
                                         tryRemap.count>1 ? (uint32_t)tryRemap.collToChar[1] : 255u,
                                         tryRemap.count>2 ? (uint32_t)tryRemap.collToChar[2] : 255u,
                                         tryRemap.count>3 ? (uint32_t)tryRemap.collToChar[3] : 255u,
                                         tryRemap.count>4 ? (uint32_t)tryRemap.collToChar[4] : 255u,
                                         tryRemap.count>5 ? (uint32_t)tryRemap.collToChar[5] : 255u,
                                         tryRemap.count>6 ? (uint32_t)tryRemap.collToChar[6] : 255u,
                                         tryRemap.count>7 ? (uint32_t)tryRemap.collToChar[7] : 255u);
                            }
                        }
                    }

                    if (remap.count == 0)
                    {
                        entry.perFrameLogged = true;
                        return;
                    }

                    auto* charBuf = reinterpret_cast<uint8_t*>(
                        GuardedReadPtr(reinterpret_cast<uint8_t*>(charCtx) + m2::kOffInstBonePalette));
                    auto* collBuf = reinterpret_cast<uint8_t*>(
                        GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) + m2::kOffInstBonePalette));

                    if (!entry.perFrameLogged)
                    {
                        uint32_t collInitF = GuardedReadU32(reinterpret_cast<uint8_t*>(renderCtx) + m2::kOffInstInitFlags);
                        uint32_t xff = 0;
                        for (uint32_t bi = 0; bi < remap.count; ++bi)
                            if (remap.collToChar[bi] == 0xFF) ++xff;
                        EquipLog("  PerFrame[first] rctx=0x%p flags=0x%X remap.count=%u xff=%u"
                                 " map0-7=[%u %u %u %u %u %u %u %u]"
                                 " charBuf=0x%p collBuf=0x%p",
                                 renderCtx, collInitF, (uint32_t)remap.count, xff,
                                 remap.count>0 ? (uint32_t)remap.collToChar[0] : 255u,
                                 remap.count>1 ? (uint32_t)remap.collToChar[1] : 255u,
                                 remap.count>2 ? (uint32_t)remap.collToChar[2] : 255u,
                                 remap.count>3 ? (uint32_t)remap.collToChar[3] : 255u,
                                 remap.count>4 ? (uint32_t)remap.collToChar[4] : 255u,
                                 remap.count>5 ? (uint32_t)remap.collToChar[5] : 255u,
                                 remap.count>6 ? (uint32_t)remap.collToChar[6] : 255u,
                                 remap.count>7 ? (uint32_t)remap.collToChar[7] : 255u,
                                 static_cast<void*>(charBuf), static_cast<void*>(collBuf));
                    }

                    if (charBuf && collBuf)
                    {
																							
																						  
                        if (!CopyRemappedBonesGuarded(collBuf, charBuf, remap))
																				 
                        {
                            EquipLog("  PerFrame(copy): guarded copy failed rctx=0x%p count=%u",
                                     renderCtx, (uint32_t)remap.count);
														 
							 
																			 
                            entry.renderCtx = nullptr;
													   

                        }
                    }
                }

                entry.perFrameLogged = true;
                return;
            }
        }
    }

    void EquipExtension::OnM2SkinFinalize(const ev::M2SkinFinalizeArgs& a)
    {
        void* model = a.model;
        if (!model || g_attached.empty()) return;

        // Primary path: match by renderCtx → model pointer. Reliable for async loads where
        // entry.renderCtx is set by Phase1 before kFinalizeSkin fires.
        for (auto& [cmo, entries] : g_attached)
        {
            for (auto& entry : entries)
            {
                if (!entry.renderCtx || !IsCollectionEntry(entry)) continue;
                void* entryModel = GuardedReadPtr(
                    reinterpret_cast<uint8_t*>(entry.renderCtx) + m2::kOffInstModel);
                if (entryModel != model) continue;

                GeosetFilter merged = {};
                for (auto& e2 : entries)
                {
                    if (e2.renderCtx != entry.renderCtx) continue;
                    for (uint32_t fi = 0; fi < e2.geoFilter.count && merged.count < 16; ++fi)
                    {
                        bool dup = false;
                        for (uint32_t fj = 0; fj < merged.count; ++fj)
                            if (merged.ids[fj] == e2.geoFilter.ids[fi]) { dup = true; break; }
                        if (!dup) merged.ids[merged.count++] = e2.geoFilter.ids[fi];
                    }
                }


                    auto* skin = gm2::Skin(model);
                    EquipLog("  OnM2SkinFinalize(primary): model=0x%p skin=0x%p merged.count=%u ids=[%u %u %u %u]",
                             model, static_cast<void*>(skin), merged.count,
                             merged.count > 0 ? merged.ids[0] : 0,
                             merged.count > 1 ? merged.ids[1] : 0,
                             merged.count > 2 ? merged.ids[2] : 0,
                             merged.count > 3 ? merged.ids[3] : 0);
                    if (skin) ApplyRawTriFilter(skin, merged);


                // Bone remap: apply immediately on async load completion.
                // OnM2SkinFinalize is the earliest moment entry.renderCtx is valid and the
                // collection model is parsed. PerFrame retries if collBuf is still null here.
                void* charCtx = GuardedReadPtr(
                    reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoSceneNode);
                if (charCtx && charCtx != entry.renderCtx)
                {
                    if (entry.boneRemap.count == 0)
                    {
                        BoneRemap tryRemap = BuildBoneRemapGuarded(entry.renderCtx, charCtx);
                        if (tryRemap.count > 0)
                        {
                            for (auto& e2 : entries)
                                if (e2.renderCtx == entry.renderCtx) e2.boneRemap = tryRemap;
                        }
                    }
                    const BoneRemap& remap = entry.boneRemap;
                    if (remap.count > 0)
                    {
                        auto* charBuf = reinterpret_cast<uint8_t*>(GuardedReadPtr(
                            reinterpret_cast<uint8_t*>(charCtx) + m2::kOffInstBonePalette));
                        auto* collBuf = reinterpret_cast<uint8_t*>(GuardedReadPtr(
                            reinterpret_cast<uint8_t*>(entry.renderCtx) + m2::kOffInstBonePalette));
                        EquipLog("  OnM2SkinFinalize(bone): charCtx=0x%p charBuf=0x%p collBuf=0x%p count=%u -> %s",
                             charCtx, static_cast<void*>(charBuf), static_cast<void*>(collBuf),
                             (uint32_t)remap.count,
                             (charBuf && collBuf) ? "APPLIED" : "DEFERRED-to-PerFrame");
                    if (charBuf && collBuf)
                        {
																									  
																							  
                            if (!CopyRemappedBonesGuarded(collBuf, charBuf, remap))
																						   
                            {
                                EquipLog("  OnM2SkinFinalize(copy): guarded copy failed rctx=0x%p count=%u",
                                         entry.renderCtx, (uint32_t)remap.count);
																  
															 
								 
																				 
                                entry.renderCtx = nullptr;
														   
								 
                            }
                        }
                    }
                }
                return;
            }
        }

        // Fallback path: virtual-path models load SYNCHRONOUSLY inside Phase1's GetRenderCtx
        // (in-process VirtualProvide has no IPC delay), so kFinalizeSkin fires before Phase1
        // sets entry.renderCtx. Match by model path stem (kOffModelPathStem = inline char buf
        // at model+0x3C, set by kInit from the M2 file's internal name = the real path stem).
        if (!g_currentRebuildCmo) return;
        auto it2 = g_attached.find(g_currentRebuildCmo);
        if (it2 == g_attached.end()) return;

        __try
        {
            const char* modelStem = reinterpret_cast<const char*>(
                reinterpret_cast<uint8_t*>(model) + m2::kOffModelPathStem);
            // Sanity-check: first char must be printable ASCII (path starts with 'I' for
            // Item\... paths). Garbage pointers or uninitialised memory fail this check.
            if (!modelStem || static_cast<unsigned char>(modelStem[0]) < 0x20
                           || static_cast<unsigned char>(modelStem[0]) > 0x7E)
                return;

            EquipLog("  OnM2SkinFinalize(fallback) cmo=0x%p modelStem='%.80s'",
                     g_currentRebuildCmo, modelStem);

            GeosetFilter merged = {};
            bool found = false;
            for (auto& entry : it2->second)
            {
                if (entry.geoFilter.count == 0 || entry.renderCtx) continue;
                // modelStem (model+0x3C) may be a full path or just a filename stem, and may
                // differ in case from keyBuf (DBC uses mixed case; host files are lowercase).
                // Extract filename part from both sides, strip any extension, compare ignoring case.
                const char* msBase = modelStem;
                for (const char* p = modelStem; *p; ++p)
                    if (*p == '\\' || *p == '/') msBase = p + 1;
                const char* msDot = nullptr;
                for (const char* p = msBase; *p; ++p) if (*p == '.') msDot = p;
                size_t msLen = msDot ? static_cast<size_t>(msDot - msBase) : std::strlen(msBase);

                const char* fileBase = entry.keyBuf;
                for (const char* p = entry.keyBuf; *p; ++p)
                    if (*p == '\\' || *p == '/') fileBase = p + 1;
                const char* lastDot = nullptr;
                for (const char* p = fileBase; *p; ++p) if (*p == '.') lastDot = p;
                size_t stemLen = lastDot ? static_cast<size_t>(lastDot - fileBase)
                                         : std::strlen(fileBase);
                if (msLen != stemLen) continue;
                if (_strnicmp(msBase, fileBase, stemLen) != 0) continue;

                found = true;
                for (uint32_t fi = 0; fi < entry.geoFilter.count && merged.count < 16; ++fi)
                {
                    bool dup = false;
                    for (uint32_t fj = 0; fj < merged.count; ++fj)
                        if (merged.ids[fj] == entry.geoFilter.ids[fi]) { dup = true; break; }
                    if (!dup) merged.ids[merged.count++] = entry.geoFilter.ids[fi];
                }
            }

            if (found)
            {
                auto* skin = gm2::Skin(model);
                EquipLog("  OnM2SkinFinalize(fallback): skin=0x%p merged.count=%u ids=[%u %u %u %u]",
                         static_cast<void*>(skin), merged.count,
                         merged.count > 0 ? merged.ids[0] : 0,
                         merged.count > 1 ? merged.ids[1] : 0,
                         merged.count > 2 ? merged.ids[2] : 0,
                         merged.count > 3 ? merged.ids[3] : 0);
                if (skin) ApplyRawTriFilter(skin, merged);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ─── OnBuildBonePalette ───────────────────────────────────────────────────────
    // Re-apply the character remap after the engine's native palette build. ModelFrame clones do not fire
    // equip-slot events, but preserve the shared collection/character model pair, so handle those too.
    void EquipExtension::OnBuildBonePalette(const ev::BuildBonePaletteArgs& a)
   {
        if (g_attached.empty()) return;
        void* renderCtx = a.renderCtx;

        // Fast reject: only collection models need manual bone driving.
        uint32_t initF = GuardedReadU32(
            reinterpret_cast<uint8_t*>(renderCtx) + m2::kOffInstInitFlags);
        if (!(initF & 0x40000)) return;
										

        for (auto& [cmo, entries] : g_attached)
        {
																									
            for (auto& entry : entries)
            {
                if (entry.renderCtx != renderCtx) continue;
                if (entry.geoFilter.count == 0) continue;
				 
																							  
                if (entry.boneRemap.count == 0) continue;
                void* charRctx = GuardedReadPtr(
                    reinterpret_cast<uint8_t*>(cmo) + m2::kOffCmoSceneNode);
                if (!charRctx) continue;
                auto* charBuf = reinterpret_cast<uint8_t*>(
                    GuardedReadPtr(reinterpret_cast<uint8_t*>(charRctx) + m2::kOffInstBonePalette));
                if (!charBuf) continue;
                auto* collBuf = reinterpret_cast<uint8_t*>(
                    GuardedReadPtr(reinterpret_cast<uint8_t*>(renderCtx) + m2::kOffInstBonePalette));
                if (!collBuf) continue;
                const BoneRemap& remap = entry.boneRemap;

																										   
																			 
													   
																   

                if (!entry.bbpLogDone)
                {
                    uint32_t xff = 0;
                    for (uint32_t bi = 0; bi < remap.count; ++bi)
                        if (remap.collToChar[bi] == 0xFF) ++xff;
                    float m00 = *reinterpret_cast<const float*>(charBuf); // [0,0] of char bone 0
                    EquipLog("  OnBBP[first] collRctx=0x%p initF=0x%X count=%u xff=%u"
                             " map0-7=[%u %u %u %u %u %u %u %u] charBuf[0][0]=%.4f",
                             renderCtx, initF, (uint32_t)remap.count, xff,
                             remap.count>0 ? (uint32_t)remap.collToChar[0] : 255u,
                             remap.count>1 ? (uint32_t)remap.collToChar[1] : 255u,
                             remap.count>2 ? (uint32_t)remap.collToChar[2] : 255u,
                             remap.count>3 ? (uint32_t)remap.collToChar[3] : 255u,
                             remap.count>4 ? (uint32_t)remap.collToChar[4] : 255u,
                             remap.count>5 ? (uint32_t)remap.collToChar[5] : 255u,
                             remap.count>6 ? (uint32_t)remap.collToChar[6] : 255u,
                             remap.count>7 ? (uint32_t)remap.collToChar[7] : 255u,
                             m00);
                    entry.bbpLogDone = true;
                }

														 
																		  
																					   
																		  
																					  
												 
                if (!CopyRemappedBonesGuarded(collBuf, charBuf, remap))
																							 
																			
                {
                    EquipLog("  OnBBP(copy): guarded copy failed rctx=0x%p count=%u",
                             renderCtx, (uint32_t)remap.count);
												 
					 
																	 
											   
                    entry.renderCtx = nullptr;
					 
                }
                return; // renderCtx is unique per (keyBuf,attachId,texBuf) group; one copy suffices
					   
            }
        }

														
														   
																  
																					
																  
																			  
										 
																				   
																						
																	   
												   
    }
    // ─── Constructor ──────────────────────────────────────────────────────────────

    EquipExtension::EquipExtension()
    {
        if (GetFileAttributesA("WarcraftXL_equip-extension.disable") != INVALID_FILE_ATTRIBUTES)
        {
            WLOG_WARN("equip-extension: disabled by client flag");
            return;
        }
        on<&EquipExtension::OnItemSlotChange>(ev::Event::OnItemSlotChange);
        on<&EquipExtension::OnItemSlotClear>(ev::Event::OnItemSlotClear);
        on<&EquipExtension::OnM2SkinFinalize>(ev::Event::OnM2SkinFinalize);
        on<&EquipExtension::OnM2PerFrameUpdate>(ev::Event::OnM2PerFrameUpdate);
        on<&EquipExtension::OnBuildBonePalette>(ev::Event::OnBuildBonePalette);
        on<&EquipExtension::OnWeaponVisualChange>(ev::Event::OnWeaponVisualChange);
        on<&EquipExtension::OnItemDisplayLookup>(ev::Event::OnItemDisplayLookup);
        on<&EquipExtension::OnModelLoadPre>(ev::Event::OnModelLoadPre);
    }

    // Self-registration: file-scope instance binds handlers at DLL load via EventScript ctor.
    EquipExtension g_equipExtension;
}
