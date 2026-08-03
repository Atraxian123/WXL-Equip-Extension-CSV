// wxl-equip-extension: client-side virtual M2 path table.
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

#include <cstddef>
#include <cstdint>

namespace wxl::scripts::equipextension
{
    /**
     * @brief Builds the virtual .mdx key for a collection M2 into out[outSz].
     *
     * Format: <stem>_wxl_<sorted_geosets>_tex_<texbasename>[_grp<HEX>]_cmo<HEX>.mdx
     * geoIds are sorted ascending internally; caller need not sort them.
     * @param out         destination buffer
     * @param outSz       destination buffer size in bytes
     * @param cmo         CharModelObject pointer, used as the character identifier
     * @param realMdxPath real archive path of the collection .mdx
     * @param geoIds      geoset IDs included in the filter (unsorted)
     * @param geoCount    number of IDs
     * @param texPath     full BLP path for the texture slot (may be empty)
     * @param variantKey  optional logical-model discriminator for entries that must not share cache keys
     * @return number of characters written (excluding null), or 0 on truncation
     */
    size_t VPathBuildKey(char* out, size_t outSz, void* cmo,
                         const char* realMdxPath,
                         const uint16_t* geoIds, uint32_t geoCount,
                         const char* texPath,
                         uint32_t variantKey = 0,
                         const char* materialPatchSpec = nullptr);

    /**
     * @brief Ensures the virtual .mdx and .skin bytes are in the client serve table.
     *
     * Reads the real .mdx and its 00.skin from the archive on first call for this key;
     * subsequent calls with the same key are no-ops. Registers both paths under cmo for
     * cleanup via VPathEvictCmo.
     * @param cmo         CharModelObject pointer
     * @param realMdxPath real archive path of the collection .mdx
     * @param geoIds      geoset IDs (unsorted)
     * @param geoCount    number of IDs
     * @param texPath     full BLP path for the texture slot (may be empty)
     * @param variantKey  optional logical-model discriminator for entries that must not share cache keys
     */
    bool VPathPopulate(void* cmo, const char* realMdxPath,
                       const uint16_t* geoIds, uint32_t geoCount,
                       const char* texPath,
                       uint32_t variantKey = 0,
                       const char* materialPatchSpec = nullptr);

    /**
     * @brief Removes all virtual table entries owned by cmo.
     *
     * Call when the cmo's sceneNode goes null (character evicted from the scene).
     * @param cmo  CharModelObject pointer to evict
     */
    void VPathEvictCmo(void* cmo);

    /**
     * @brief Registers a texture/material-patched version of a model under a per-itemDisplayId
     *        VIRTUAL archive path (real path with "_<itemDisplayId>" inserted before the
     *        extension), so every future load of that virtual path -- from any character, any
     *        client, any owner -- is transparently served the patched bytes.
     *
     * Unlike VPathPopulate, this is NOT scoped to a cmo and NOT keyed by a mangled "_wxl_" name.
     * It IS still keyed by itemDisplayId, though: two different ItemDisplayInfo rows that happen
     * to point at the same underlying model file (e.g. shared weapon geometry with a
     * displayId-specific texture/material patch) each get their own distinct virtual path and
     * therefore their own table entry, rather than colliding on one shared key the way registering
     * under the bare real path would. The patch itself is still treated as a property of the
     * model file -- not of the wearer -- for a given displayId; it is not scoped to a cmo, e.g. a
     * weapon's base ModelTexture_1/2 (vanilla only auto-binds that natively for Head/Shoulder;
     * everywhere else, including weapons, something has to bake or bind it) and/or a weapon's
     * extra glow/emissive layers. There is no per-cmo eviction for these entries: once registered,
     * the override stays for the life of the process, and the first (texPath, materialPatchSpec)
     * pair registered for a given (path, itemDisplayId) wins for the rest of the session -- later
     * calls for the same pair with different arguments are no-ops (see g_globalOverrides).
     *
     * Because the served bytes now live under a virtual path rather than the model's own real
     * name, nothing will transparently intercept the native loader's request for the original
     * path any more -- the caller is responsible for getting that virtual path in front of the
     * loader instead (e.g. by pointing the resolved ItemDisplayInfo model field at it).
     * outVirtualPath, if non-null, receives that path so the caller can do so.
     *
     * No per-batch texture-slot override -- callers that need that should still go through
     * VPathPopulate on a per-attach basis. A geoset filter IS supported here (geoIds/geoCount),
     * unlike the original version of this function -- it behaves the same as VPathPopulate's own
     * geoIds/geoCount: submesh sections whose sectionId isn't in the list get their indices zeroed
     * out of the .skin bytes, geoCount == 0 means "no filter, keep every section". Like texPath and
     * materialPatchSpec, it's part of the (path, itemDisplayId) pair's first-registration-wins bake --
     * not re-appliable later with different ids for an already-registered pair.
     *
     * @param realMdxPath        real archive path of the .mdx (as ItemDisplayInfo/sidecar would give it)
     * @param itemDisplayId      ItemDisplayInfo display id this patch belongs to; mangled into the
     *                           virtual path so distinct displayIds sharing a model file don't
     *                           collide on the same table entry
     * @param texPath            full BLP path to bake into the model's replaceable/hardcoded texture
     *                           slots (OBJECT_SKIN records only, promoted to hardcoded, same as
     *                           VPathPopulate's PatchReplaceableTextureTypes step -- WEAPON_BLADE is
     *                           never touched by this and needs an explicit sidecar TextureType=3
     *                           row instead) -- pass nullptr/empty to skip this and only apply
     *                           materialPatchSpec
     * @param materialPatchSpec  batch-scoped material texture patch spec (see BuildMaterialPatchSpec) --
     *                           pass nullptr/empty to skip this and only bake texPath
     * @param geoIds             skinSectionIds to keep; every other section's indices are zeroed out of
     *                           the .skin bytes -- pass nullptr/0 (with geoCount) to skip this and keep
     *                           every section as-is
     * @param geoCount           number of entries in geoIds
     * @param outVirtualPath     optional destination buffer that receives the virtual path the
     *                           patched bytes were registered under (e.g. "...\\model_12345.m2")
     * @param outVirtualPathSz   size in bytes of outVirtualPath
     * @return true if the override was registered (or was already registered for this path/id pair)
     */
    bool VPathPopulateGlobal(const char* realMdxPath, uint32_t itemDisplayId,
                             const char* texPath, const char* materialPatchSpec,
                             const uint16_t* geoIds = nullptr, uint32_t geoCount = 0,
                             char* outVirtualPath = nullptr, size_t outVirtualPathSz = 0);
}
