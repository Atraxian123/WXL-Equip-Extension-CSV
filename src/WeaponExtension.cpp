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

#include "WeaponExtension.hpp"
#include "VirtualPath.hpp"
#include "events/Event.hpp"
#include "game/io/Io.hpp"
#include "offsets/engine/Io.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// VPathPopulateGlobal (and the whole virtual-model-serving mechanism it belongs to) lives under
// wxl::scripts::equipextension -- historically named after its first consumer, but it was always a
// generic (path, id) -> baked-bytes mechanism, not anything armor-specific. This script reuses it
// as-is via its current namespace, same as CreatureExtension.cpp does, rather than doing an
// unrequested rename/move.
using wxl::scripts::equipextension::VPathPopulateGlobal;
using wxl::scripts::equipextension::VPathRegisterLazyResolver;
using wxl::scripts::equipextension::VPathDecodeGlobalVirtualKey;
using wxl::scripts::equipextension::WxlIniGetBool;

namespace wxl::scripts::weaponextension
{
    namespace ev = wxl::events;

    // ─── Logging ────────────────────────────────────────────────────────────────
    // Same opt-in-file-log shape as CreatureExtension's CreatureLog/EquipExtension's EquipLog, kept
    // separate (own env var, own log file) so enabling one doesn't spam the others.
    static bool WeaponLogEnabled() noexcept
    {
        static int enabled = []() noexcept -> int {
#pragma warning(suppress: 4996)
            const char* env = std::getenv("WXL_WEAPON_LOG");
            if (env && *env && *env != '0' && *env != 'n' && *env != 'N')
                return 1;

#pragma warning(suppress: 4996)
            FILE* flag = std::fopen("WarcraftXL_weapon.log.enable", "rb");
            if (!flag) return 0;
            std::fclose(flag);
            return 1;
        }();
        return enabled != 0;
    }

    static void WeaponLog(const char* fmt, ...) noexcept
    {
        if (!WeaponLogEnabled()) return;
#pragma warning(suppress: 4996)
        FILE* f = std::fopen("WarcraftXL_weapon.log", "a");
        if (!f) return;
        va_list ap; va_start(ap, fmt);
        std::vfprintf(f, fmt, ap);
        va_end(ap);
        std::fputc('\n', f);
        std::fclose(f);
    }

    // ─── CSV helpers ────────────────────────────────────────────────────────────
    // Deliberately duplicated (not shared) from CreatureExtension.cpp/EquipExtension.cpp's
    // equivalents -- see CreatureExtension.cpp's identical comment on this same duplication.

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

    // ─── Sidecar table ──────────────────────────────────────────────────────────
    // WXLWeaponTextures.csv columns: DisplayID, ModelColumn, TextureType, TexturePath. Multiple rows
    // per (DisplayID, ModelColumn) are expected -- one per baked texture layer (e.g. base skin +
    // extra glow/emissive), same convention as WXLCreatureTextures.csv. ModelColumn is 0 (Model1) or
    // 1 (Model2) -- matches ItemDisplayInfo's own Model1/Model2 pairing, since the two columns are
    // independent real model files that each need their own texture-unit patch spec. TextureType is
    // whatever numeric texture-unit type PatchTargetedMaterialTextures in VirtualPath.cpp expects.
    // TexturePath is a literal archive-relative BLP path -- no race/gender inference, same as
    // creature texture rows.
    struct SidecarWeaponTextureEntry
    {
        uint32_t modelColumn = static_cast<uint32_t>(-1);
        uint32_t textureType = static_cast<uint32_t>(-1);
        char     texturePath[264] = {};
    };

    static bool g_sidecarLoaded = false;
    static std::unordered_map<uint32_t, std::vector<SidecarWeaponTextureEntry>> g_sidecarWeaponTextures;

    // WXLWeaponModels.csv columns: DisplayID, Model1Path, Model2Path. One row per DisplayID --
    // Model1Path/Model2Path are the real archive paths of the .mdx files that displayId's
    // ItemDisplayInfo record natively resolves to for its two model slots (author-supplied, e.g.
    // pulled from a DBC/SQL dump), same "known-good pair, no in-memory guesswork" convention as
    // WXLCreatureModels.csv. Either column may be left blank if that displayId's ItemDisplayInfo
    // record only uses one slot (the overwhelmingly common case -- Model2 is normally empty even in
    // retail data). This is what makes eager preregistration possible: only displayIds listed here
    // are ever eligible for baking, eager or lazy -- there is no reactive ItemDisplayInfo-lookup
    // fallback in this module (unlike EquipExtension's older weapon path); a displayId not listed
    // here simply isn't patched.
    struct SidecarWeaponModelPaths
    {
        std::string path[2]; // [0]=Model1Path, [1]=Model2Path; empty string means "not used"
    };
    static std::unordered_map<uint32_t, SidecarWeaponModelPaths> g_sidecarWeaponModelPath;

    static void LoadWeaponTextureSidecarFile(const char* path)
    {
        std::vector<std::string> lines;
        if (!ReadSidecarLines(path, lines)) return;

        const std::vector<std::string> header = ParseCsvLine(lines[0].c_str());
        const int cDisplay     = FindCsvColumn(header, "DisplayID");
        const int cModelColumn = FindCsvColumn(header, "ModelColumn");
        const int cTextureType = FindCsvColumn(header, "TextureType");
        const int cTexturePath = FindCsvColumn(header, "TexturePath");

        if (cDisplay < 0 || cModelColumn < 0 || cTextureType < 0 || cTexturePath < 0)
        {
            WeaponLog("weapon texture sidecar '%s': missing DisplayID, ModelColumn, TextureType, or "
                      "TexturePath column", path);
            return;
        }

        uint32_t loaded = 0;
        for (size_t lineIndex = 1; lineIndex < lines.size(); ++lineIndex)
        {
            if (lines[lineIndex].empty()) continue;
            const std::vector<std::string> row = ParseCsvLine(lines[lineIndex].c_str());

            uint32_t displayId = 0;
            if (!ParseU32(CsvField(row, cDisplay), &displayId) || displayId == 0) continue;

            SidecarWeaponTextureEntry e = {};
            if (!ParseU32(CsvField(row, cModelColumn), &e.modelColumn) || e.modelColumn > 1) continue;
            if (!ParseU32(CsvField(row, cTextureType), &e.textureType)) continue;
            CopyString(e.texturePath, sizeof(e.texturePath), CsvField(row, cTexturePath));
            if (!e.texturePath[0]) continue;

            // Dedup by (displayId, modelColumn, textureType), first-seen wins -- same reasoning as
            // WXLCreatureTextures.csv's own dedup (guards against the same physical CSV being
            // reachable, and therefore re-parsed, via more than one of LoadWeaponSidecar's search
            // paths).
            auto& rows = g_sidecarWeaponTextures[displayId];
            bool duplicate = false;
            for (const SidecarWeaponTextureEntry& existing : rows)
            {
                if (existing.modelColumn == e.modelColumn && existing.textureType == e.textureType)
                {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            rows.push_back(e);
            ++loaded;
        }

        if (loaded)
            WeaponLog("weapon texture sidecar loaded '%s' rows=%u", path, loaded);
    }

    static void LoadWeaponModelPathSidecarFile(const char* path)
    {
        std::vector<std::string> lines;
        if (!ReadSidecarLines(path, lines)) return;

        const std::vector<std::string> header = ParseCsvLine(lines[0].c_str());
        const int cDisplay = FindCsvColumn(header, "DisplayID");
        const int cModel1  = FindCsvColumn(header, "Model1Path");
        const int cModel2  = FindCsvColumn(header, "Model2Path"); // optional -- absent is fine

        if (cDisplay < 0 || cModel1 < 0)
        {
            WeaponLog("weapon model-path sidecar '%s': missing DisplayID or Model1Path column", path);
            return;
        }

        uint32_t loaded = 0;
        for (size_t lineIndex = 1; lineIndex < lines.size(); ++lineIndex)
        {
            if (lines[lineIndex].empty()) continue;
            const std::vector<std::string> row = ParseCsvLine(lines[lineIndex].c_str());

            uint32_t displayId = 0;
            if (!ParseU32(CsvField(row, cDisplay), &displayId) || displayId == 0) continue;

            char model1[264] = {};
            CopyString(model1, sizeof(model1), CsvField(row, cModel1));
            char model2[264] = {};
            if (cModel2 >= 0) CopyString(model2, sizeof(model2), CsvField(row, cModel2));
            if (!model1[0] && !model2[0]) continue; // nothing usable on this row

            // First-seen wins, same dedup convention as the texture sidecar.
            if (g_sidecarWeaponModelPath.find(displayId) != g_sidecarWeaponModelPath.end())
                continue;

            SidecarWeaponModelPaths& entry = g_sidecarWeaponModelPath[displayId];
            entry.path[0] = model1;
            entry.path[1] = model2;
            ++loaded;
        }

        if (loaded)
            WeaponLog("weapon model-path sidecar loaded '%s' rows=%u", path, loaded);
    }

    // Forward decl: full definition sits after this, but must be called from the end of
    // LoadWeaponSidecar below -- same shape as CreatureExtension's PreregisterSidecarCreatures.
    static void PreregisterSidecarWeapons();

    // Same search pattern as CreatureExtension's LoadCreatureSidecar / EquipExtension's
    // LoadSidecarModels: bare filename, DBFilesClient\ beside it, and DBFilesClient\ inside every
    // mounted Data\*.MPQ directory (patch overrides).
    static void LoadWeaponSidecar()
    {
        if (g_sidecarLoaded) return;
        g_sidecarLoaded = true;

        LoadWeaponTextureSidecarFile("WXLWeaponTextures.csv");
        LoadWeaponTextureSidecarFile("DBFilesClient\\WXLWeaponTextures.csv");
        LoadWeaponModelPathSidecarFile("WXLWeaponModels.csv");
        LoadWeaponModelPathSidecarFile("DBFilesClient\\WXLWeaponModels.csv");

        WIN32_FIND_DATAA fd = {};
        HANDLE h = FindFirstFileA("Data\\*.MPQ", &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::string base = "Data\\";
                base += fd.cFileName;
                base += "\\DBFilesClient\\";
                LoadWeaponTextureSidecarFile((base + "WXLWeaponTextures.csv").c_str());
                LoadWeaponModelPathSidecarFile((base + "WXLWeaponModels.csv").c_str());
            }
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }

        WeaponLog("weapon sidecar table ready: displays=%zu textures, %zu model-paths",
                  g_sidecarWeaponTextures.size(), g_sidecarWeaponModelPath.size());

        // Deliberately called here, at the end of the CSV load, NOT from WeaponExtension's
        // constructor -- same static-init-timing reason as CreatureExtension::LoadCreatureSidecar
        // (the constructor runs at DLL static-init time, before the client's own file-I/O subsystems
        // are guaranteed up; this only ever runs lazily, the first time OnModelLoadPre fires).
        //
        // Default true (matches CreatureExtension's own default) if the ini/section/key is missing --
        // see WxlIniGetBool's doc comment. Turning this off relies entirely on WeaponLazyResolve (see
        // its own doc comment, and its registration in the constructor below) to bake anything at
        // all -- there is no separate reactive per-equip hook for weapon models to fall back on in
        // this module, by design (see WeaponExtension.hpp's doc comment on why that's safe here).
        if (WxlIniGetBool("EagerPreload", "Weapons", true))
            PreregisterSidecarWeapons();
    }

    // Builds a "TextureType=TexturePath|TextureType=TexturePath|..." spec for (displayId, modelColumn),
    // in the exact format PatchTargetedMaterialTextures (VirtualPath.cpp) expects -- same format
    // CreatureExtension's BuildCreatureMaterialPatchSpec produces, just filtered to rows for this one
    // model column instead of every column at once (Model1 and Model2 are separate real files with
    // separate texture-unit tables, so each needs its own spec baked via its own VPathPopulateGlobal
    // call).
    static void BuildWeaponMaterialPatchSpec(char* out, size_t outSz, uint32_t displayId, uint32_t modelColumn)
    {
        if (!out || outSz == 0) return;
        out[0] = '\0';

        auto it = g_sidecarWeaponTextures.find(displayId);
        if (it == g_sidecarWeaponTextures.end()) return;

        size_t used = 0;
        for (const SidecarWeaponTextureEntry& e : it->second)
        {
            if (e.modelColumn != modelColumn) continue;
            if (e.textureType == static_cast<uint32_t>(-1) || !e.texturePath[0]) continue;

            char item[300];
            int n = std::snprintf(item, sizeof(item), "%s%u=%s",
                                  used ? "|" : "", e.textureType, e.texturePath);
            if (n <= 0) continue;
            const size_t len = static_cast<size_t>(n);
            if (len >= sizeof(item) || used + len >= outSz) break;
            std::memcpy(out + used, item, len);
            used += len;
            out[used] = '\0';
        }
    }

    // Bakes displayId's Model1 and/or Model2 patched bytes into the process-lifetime override table,
    // for whichever columns the sidecar tables know a real path for (WXLWeaponModels.csv) and have
    // something to actually bake (WXLWeaponTextures.csv rows for that column -- unlike creatures,
    // there is no geoset filter here at all: weapons don't need one, see VPathPopulateGlobal's own
    // call below passing nullptr/0). Shared by both PreregisterSidecarWeapons (eager, walks every
    // known displayId at startup) and WeaponLazyResolve (lazy, one displayId at a time, on demand) so
    // the bake logic only exists once. Assumes the sidecar tables are already populated -- both
    // current callers route through LoadWeaponSidecar first.
    //
    // Returns true if at least one column was registered. A displayId whose sidecar row has a model
    // path but no texture rows for that column still isn't baked for that column -- same "nothing to
    // bake" rule CreatureExtension's BakeCreatureDisplay follows -- since VPathPopulateGlobal with
    // both texPath and materialPatchSpec null wouldn't be registering a patch, just re-serving the
    // model unmodified under a new name for no reason.
    static bool BakeWeaponDisplay(uint32_t displayId)
    {
        auto pathIt = g_sidecarWeaponModelPath.find(displayId);
        if (pathIt == g_sidecarWeaponModelPath.end()) return false;

        bool anyRegistered = false;
        for (uint32_t col = 0; col < 2; ++col)
        {
            const std::string& realPath = pathIt->second.path[col];
            if (realPath.empty()) continue;

            char matSpec[2048] = {};
            BuildWeaponMaterialPatchSpec(matSpec, sizeof(matSpec), displayId, col);
            if (!matSpec[0])
            {
                WeaponLog("  bake: display=%u col=%u real='%s' -- no texture sidecar rows, skipping",
                          displayId, col, realPath.c_str());
                continue; // model path known, but nothing to bake for this column
            }

            // No texPath (only materialPatchSpec) -- weapon texture rows are all TextureType-keyed,
            // same convention as creature texture rows. No geoset filter -- weapons only.
            // evictable=false (explicit, even though it's also the default): per
            // WXLExtendedEquipment.ini's [Memory] section, weapon overrides are never evictable --
            // only CreatureExtension.cpp's BakeCreatureDisplay opts in to the MaxCreatureCacheMB LRU
            // cap. See VPathPopulateGlobal's doc comment in VirtualPath.hpp for why.
            char vModelPath[280] = {};
            bool registered = VPathPopulateGlobal(realPath.c_str(), displayId, nullptr, matSpec,
                                                   nullptr, 0, vModelPath, sizeof(vModelPath),
                                                   /*evictable=*/false);
            WeaponLog("  bake: display=%u col=%u real='%s' vpath='%s' spec='%s' registered=%d",
                      displayId, col, realPath.c_str(), vModelPath, matSpec, registered ? 1 : 0);
            anyRegistered |= (registered && vModelPath[0]);
        }
        return anyRegistered;
    }

    // Walks every displayId listed in WXLWeaponModels.csv and bakes its patched bytes into the
    // process-lifetime override table under the exact virtual .m2 name ItemDisplayInfo's Model1/
    // Model2 fields already name for that displayId (patched at the data level, outside this module --
    // see WeaponExtension.hpp's OnModelLoadPre doc comment). There is no race to lose here: the
    // native loader always asks for that virtual name directly, on every equip/relog/char-select
    // preview, so this only needs to run once, sometime before the first such request -- it doesn't
    // matter which weapon asks first.
    //
    // Gated behind WXLExtendedEquipment.ini's [EagerPreload] Weapons key (default true) -- see its
    // call site's comment for why turning it off, in this module, needs WeaponLazyResolve alongside
    // it. Deliberately sidecar-only either way: only displayIds with an explicit WXLWeaponModels.csv
    // row are ever eligible, eager or lazy.
    static void PreregisterSidecarWeapons()
    {
        // No LoadWeaponSidecar() call here -- this function is only ever invoked from the end of
        // LoadWeaponSidecar itself, so the sidecar tables below are already guaranteed populated.
        if (g_sidecarWeaponModelPath.empty()) return;

        WeaponLog("weapon preregister: %zu display(s) with a known model path, %zu with texture rows",
                  g_sidecarWeaponModelPath.size(), g_sidecarWeaponTextures.size());

        uint32_t registeredCount = 0;
        for (const auto& [displayId, paths] : g_sidecarWeaponModelPath)
        {
            if (BakeWeaponDisplay(displayId)) ++registeredCount;
        }

        WeaponLog("weapon preregister: done, %u display(s) registered ahead of first spawn",
                  registeredCount);
    }

    // Lazy on-demand counterpart to PreregisterSidecarWeapons, registered with VirtualPath.cpp via
    // VPathRegisterLazyResolver and invoked from VirtualProvide on a g_globalOverrides miss -- same
    // shape, same reasoning, as CreatureExtension.cpp's CreatureLazyResolve. Runs unconditionally
    // regardless of the [EagerPreload] Weapons ini setting: with eager preload on, this is a safety
    // net for any displayId that slips past the startup sweep; with it off, this is the ONLY path
    // that ever bakes a weapon override at all in this module (there is no separate reactive
    // per-equip hook here -- see WeaponExtension.hpp's doc comment for why that's fine for weapons).
    //
    // normVirtualPath is whatever VirtualProvide's own NormalizeRealPath produced from the raw loader
    // request -- already lowercase with a .m2 extension, the form VPathDecodeGlobalVirtualKey
    // expects. A successful decode only confirms the name has the right *shape* (see that function's
    // doc comment); the decoded displayId is only trusted once it's confirmed present in
    // g_sidecarWeaponModelPath -- the same table PreregisterSidecarWeapons itself walks -- and
    // BakeWeaponDisplay re-derives everything it bakes from that table (and g_sidecarWeaponTextures),
    // not from the decoded path, so a coincidental decode of an unrelated real archive path can't
    // smuggle in bytes for the wrong model.
    static bool WeaponLazyResolve(const char* normVirtualPath)
    {
        LoadWeaponSidecar(); // no-op after the first call; must run before the sidecar tables are read

        char decodedPath[264] = {};
        uint32_t displayId = 0;
        if (!VPathDecodeGlobalVirtualKey(normVirtualPath, decodedPath, sizeof(decodedPath), &displayId))
            return false;

        if (g_sidecarWeaponModelPath.find(displayId) == g_sidecarWeaponModelPath.end())
            return false; // not a displayId this module knows about

        WeaponLog("  WeaponLazyResolve: miss '%s' decoded displayId=%u -- baking now",
                  normVirtualPath, displayId);
        return BakeWeaponDisplay(displayId);
    }

    WeaponExtension::WeaponExtension()
    {
        // Deliberately does NOT call LoadWeaponSidecar/PreregisterSidecarWeapons here -- this
        // constructor runs on the file-scope global at DLL static-init time (DllMain), before the
        // client's own archive/file-I/O subsystems are guaranteed up.
        on<&WeaponExtension::OnModelLoadPre>(ev::Event::OnModelLoadPre);

        // Cheap (one vector push_back, no I/O) so it's safe here regardless of engine subsystem init
        // order -- see VPathRegisterLazyResolver's doc comment in VirtualPath.hpp. Registered
        // unconditionally, independent of the [EagerPreload] Weapons ini setting, so lazy baking
        // works whether or not eager preload is turned on.
        VPathRegisterLazyResolver(&WeaponLazyResolve);
    }

    // See the doc comment on the declaration in WeaponExtension.hpp for why OnModelLoadPre and not
    // an equip-time hook.
    void WeaponExtension::OnModelLoadPre(const ev::ModelLoadArgs&)
    {
        static bool kicked = false;
        if (kicked) return;
        kicked = true;
        LoadWeaponSidecar(); // no-op after the first call; runs PreregisterSidecarWeapons at its end
    }

    // Self-registration: file-scope instance binds handlers at DLL load via EventScript ctor.
    WeaponExtension g_weaponExtension;
}
