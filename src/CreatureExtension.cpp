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

#include "CreatureExtension.hpp"
#include "VirtualPath.hpp"
#include "events/Event.hpp"
#include "game/io/Io.hpp"
#include "offsets/engine/Io.hpp"
#include "offsets/game/DB2.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// VPathPopulateGlobal (and the whole virtual-model-serving mechanism it belongs to) lives under
// wxl::scripts::equipextension -- historically named after its first consumer, but it was always a
// generic (path, id) -> baked-bytes mechanism, not anything weapon-specific. This script reuses it
// as-is via its current namespace rather than doing an unrequested rename/move; worth revisiting as
// a cleanup if a third consumer ever shows up and the naming gets more confusing still.
using wxl::scripts::equipextension::VPathPopulateGlobal;

namespace wxl::scripts::creatureextension
{
    namespace ev  = wxl::events;
    namespace db2 = wxl::offsets::game::db2;

    // ─── Logging ────────────────────────────────────────────────────────────────
    // Same opt-in-file-log shape as EquipExtension's EquipLog/EquipLogEnabled, kept separate (own
    // env var, own log file) so enabling one doesn't spam the other feature's log.
    static bool CreatureLogEnabled() noexcept
    {
        static int enabled = []() noexcept -> int {
#pragma warning(suppress: 4996)
            const char* env = std::getenv("WXL_CREATURE_LOG");
            if (env && *env && *env != '0' && *env != 'n' && *env != 'N')
                return 1;

#pragma warning(suppress: 4996)
            FILE* flag = std::fopen("WarcraftXL_creature.log.enable", "rb");
            if (!flag) return 0;
            std::fclose(flag);
            return 1;
        }();
        return enabled != 0;
    }

    static void CreatureLog(const char* fmt, ...) noexcept
    {
        if (!CreatureLogEnabled()) return;
#pragma warning(suppress: 4996)
        FILE* f = std::fopen("WarcraftXL_creature.log", "a");
        if (!f) return;
        va_list ap; va_start(ap, fmt);
        std::vfprintf(f, fmt, ap);
        va_end(ap);
        std::fputc('\n', f);
        std::fclose(f);
    }

    // ─── CSV helpers ────────────────────────────────────────────────────────────
    // Deliberately duplicated (not shared) from EquipExtension.cpp's equivalents: those are file-
    // local `static` functions with internal linkage, not exposed anywhere a second translation
    // unit could reach them. Small and self-contained enough that duplicating is simpler than
    // introducing a shared header just for this; worth factoring out if a third sidecar-driven
    // script shows up and this becomes a third copy.

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
    // WXLCreatureTextures.csv columns: DisplayID, TextureType, TexturePath. Multiple rows per
    // DisplayID are expected -- one per baked texture layer (e.g. skin + extra glow/emissive).
    // TextureType is whatever numeric texture-unit type PatchTargetedMaterialTextures in
    // VirtualPath.cpp expects (same convention as the weapon material sidecar's TextureType column).
    // TexturePath is a literal archive-relative BLP path -- unlike the item sidecars, there is no
    // race/gender/folder-inference step here; creature texture variants don't follow that
    // convention, so the sidecar author is expected to give the real path directly.
    struct SidecarCreatureTextureEntry
    {
        uint32_t textureType = static_cast<uint32_t>(-1);
        char     texturePath[264] = {};
    };

    static bool g_sidecarLoaded = false;
    static std::unordered_map<uint32_t, std::vector<SidecarCreatureTextureEntry>> g_sidecarCreatureTextures;

    // WXLCreatureModels.csv columns: DisplayID, CreatureModelPath. One row per DisplayID --
    // CreatureModelPath is the real archive path of the .mdx that displayId natively resolves to
    // (author-supplied, e.g. pulled from a DBC/SQL dump), same "known-good pair, no in-memory
    // guesswork" convention as EquipExtension's WXLItemEntryDisplay.csv. This is what makes eager
    // preregistration possible for creatures: without it, the only way to learn a displayId's real
    // model path is the native CreatureDisplayInfo -> CreatureModelData lookup chain, which isn't a
    // real callable function (see DB2.hpp's kCaptureDisplayId/kResolveMerge comments) -- walking its
    // id tables by hand at module-load time is fragile against whatever state the client's own DB2
    // storage happens to be in that early, and empirically has taken the client down outright rather
    // than failing safely inside the __try/__except guards. Only displayIds listed in this file are
    // eligible for eager preregistration; anything else still falls back to the existing reactive
    // path in OnCreatureModelResolve, same as before this file existed.
    static std::unordered_map<uint32_t, std::string> g_sidecarCreatureModelPath;

    // displayId -> baked virtual model path, process-lifetime once registered (same convention as
    // VirtualPath.cpp's g_globalOverrides / EquipExtension's g_weaponVPaths). Owns the string bytes
    // ModelName gets pointed at, since OnCreatureModelResolveMergeCaptured only hands us a stack
    // buffer via VPathPopulateGlobal's outVirtualPath and that pointer must outlive this call.
    static std::unordered_map<uint32_t, std::string> g_creatureVPaths;

    static void LoadCreatureTextureSidecarFile(const char* path)
    {
        std::vector<std::string> lines;
        if (!ReadSidecarLines(path, lines)) return;

        const std::vector<std::string> header = ParseCsvLine(lines[0].c_str());
        const int cDisplay     = FindCsvColumn(header, "DisplayID");
        const int cTextureType = FindCsvColumn(header, "TextureType");
        const int cTexturePath = FindCsvColumn(header, "TexturePath");

        if (cDisplay < 0 || cTextureType < 0 || cTexturePath < 0)
        {
            CreatureLog("creature texture sidecar '%s': missing DisplayID, TextureType, or "
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

            SidecarCreatureTextureEntry e = {};
            if (!ParseU32(CsvField(row, cTextureType), &e.textureType)) continue;
            CopyString(e.texturePath, sizeof(e.texturePath), CsvField(row, cTexturePath));
            if (!e.texturePath[0]) continue;

            // Dedup by (displayId, textureType), first-seen wins. LoadCreatureSidecar() checks
            // several search paths (bare filename, DBFilesClient\, every mounted patch MPQ's
            // DBFilesClient\) -- if the same physical CSV is reachable via more than one of those,
            // every row would otherwise be parsed once per path that resolves to it, silently
            // doubling (or worse) the material patch spec built from this table.
            auto& rows = g_sidecarCreatureTextures[displayId];
            bool duplicate = false;
            for (const SidecarCreatureTextureEntry& existing : rows)
            {
                if (existing.textureType == e.textureType) { duplicate = true; break; }
            }
            if (duplicate) continue;

            rows.push_back(e);
            ++loaded;
        }

        if (loaded)
            CreatureLog("creature texture sidecar loaded '%s' rows=%u", path, loaded);
    }

    static void LoadCreatureModelPathSidecarFile(const char* path)
    {
        std::vector<std::string> lines;
        if (!ReadSidecarLines(path, lines)) return;

        const std::vector<std::string> header = ParseCsvLine(lines[0].c_str());
        const int cDisplay = FindCsvColumn(header, "DisplayID");
        const int cModel   = FindCsvColumn(header, "CreatureModelPath");

        if (cDisplay < 0 || cModel < 0)
        {
            CreatureLog("creature model-path sidecar '%s': missing DisplayID or CreatureModelPath "
                        "column", path);
            return;
        }

        uint32_t loaded = 0;
        for (size_t lineIndex = 1; lineIndex < lines.size(); ++lineIndex)
        {
            if (lines[lineIndex].empty()) continue;
            const std::vector<std::string> row = ParseCsvLine(lines[lineIndex].c_str());

            uint32_t displayId = 0;
            if (!ParseU32(CsvField(row, cDisplay), &displayId) || displayId == 0) continue;

            char modelPath[264];
            CopyString(modelPath, sizeof(modelPath), CsvField(row, cModel));
            if (!modelPath[0]) continue;

            // First-seen wins, same dedup convention as the texture sidecar -- guards against the
            // same physical CSV being reachable (and therefore re-parsed) via more than one of
            // LoadCreatureSidecar's search paths.
            if (g_sidecarCreatureModelPath.find(displayId) != g_sidecarCreatureModelPath.end())
                continue;

            g_sidecarCreatureModelPath.emplace(displayId, modelPath);
            ++loaded;
        }

        if (loaded)
            CreatureLog("creature model-path sidecar loaded '%s' rows=%u", path, loaded);
    }

    // Forward decl: full definition sits after BuildCreatureMaterialPatchSpec (needs it), but must
    // be called from the end of LoadCreatureSidecar below -- same shape as EquipExtension's
    // LoadSidecarModels calling PreregisterSidecarWeapons at its own end.
    static void PreregisterSidecarCreatures();

    // Same search pattern as EquipExtension's LoadSidecarModels: bare filename, DBFilesClient\
    // beside it, and DBFilesClient\ inside every mounted Data\*.MPQ directory (patch overrides).
    static void LoadCreatureSidecar()
    {
        if (g_sidecarLoaded) return;
        g_sidecarLoaded = true;

        LoadCreatureTextureSidecarFile("WXLCreatureTextures.csv");
        LoadCreatureTextureSidecarFile("DBFilesClient\\WXLCreatureTextures.csv");
        LoadCreatureModelPathSidecarFile("WXLCreatureModels.csv");
        LoadCreatureModelPathSidecarFile("DBFilesClient\\WXLCreatureModels.csv");

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
                LoadCreatureTextureSidecarFile((base + "WXLCreatureTextures.csv").c_str());
                LoadCreatureModelPathSidecarFile((base + "WXLCreatureModels.csv").c_str());
            }
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }

        CreatureLog("creature sidecar table ready: displays=%zu textures, %zu model-paths",
                    g_sidecarCreatureTextures.size(), g_sidecarCreatureModelPath.size());

        // Deliberately called here, at the end of the CSV load, NOT from CreatureExtension's
        // constructor. The constructor runs on the file-scope global at DLL static-init time
        // (DllMain/DLL_PROCESS_ATTACH) -- before the client's own engine subsystems (archive
        // mounting, file I/O) are guaranteed to be up. LoadCreatureSidecar, by contrast, only ever
        // runs lazily, the first time OnCreatureModelResolve actually fires for a real creature --
        // i.e. only once the client is already deep into a live game session. Same reasoning as
        // EquipExtension: PreregisterSidecarWeapons is called from the end of LoadSidecarModels,
        // not from EquipExtension's constructor, for the same static-init-timing reason.
        PreregisterSidecarCreatures();
    }

    // Builds a "TextureType=TexturePath|TextureType=TexturePath|..." spec for displayId, in the
    // exact format PatchTargetedMaterialTextures (VirtualPath.cpp) expects -- same format
    // EquipExtension's weapon material sidecar produces, just without any modelColumn/stem matching
    // since creature texture rows aren't disambiguated by anything but DisplayID.
    static void BuildCreatureMaterialPatchSpec(char* out, size_t outSz, uint32_t displayId)
    {
        if (!out || outSz == 0) return;
        out[0] = '\0';

        auto it = g_sidecarCreatureTextures.find(displayId);
        if (it == g_sidecarCreatureTextures.end()) return;

        size_t used = 0;
        for (const SidecarCreatureTextureEntry& e : it->second)
        {
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

    // Walks every displayId listed in WXLCreatureModels.csv (with a matching WXLCreatureTextures.csv
    // entry) and registers its baked-texture patch immediately, before any creature -- including
    // one already spawned/visible the moment this module loads -- has a chance to resolve its model
    // natively first.
    //
    // Why this exists: VPathPopulateGlobal is explicitly process-lifetime and path-keyed, not tied
    // to any particular event firing (see its doc comment in VirtualPath.hpp), so it was always
    // meant to support being populated ahead of time rather than only reactively -- same rationale
    // as EquipExtension's PreregisterSidecarWeapons for weapons losing the race against the
    // char-select preview load. Here the race is against whatever creature resolves its model first
    // (e.g. one already in view on zone-in), which can happen before OnCreatureModelResolve's
    // reactive registration would otherwise catch it.
    //
    // Deliberately sidecar-only, unlike EquipExtension's weapon preregister: that one falls back to
    // a native ItemDisplayInfo lookup (a real funnel function, ItemDisplayInfoLookupNative) when an
    // itemEntry has no sidecar Folder override. Creatures have no equivalent safe native entry
    // point -- CreatureDisplayInfo/CreatureModelData resolution is inlined into kResolveFn, not a
    // callable function, and hand-walking those id tables directly at module-load time proved
    // unsafe in practice. So only displayIds with an explicit WXLCreatureModels.csv row are eligible
    // for eager registration; anything else still gets patched reactively, on first resolve, via
    // OnCreatureModelResolve exactly as before.
    static void PreregisterSidecarCreatures()
    {
        // No LoadCreatureSidecar() call here -- this function is only ever invoked from the end of
        // LoadCreatureSidecar itself (see its call site's comment), so the sidecar tables below are
        // already guaranteed populated by the time we get here.
        if (g_sidecarCreatureModelPath.empty()) return;

        CreatureLog("creature preregister: %zu display(s) with a known model path, %zu with texture rows",
                    g_sidecarCreatureModelPath.size(), g_sidecarCreatureTextures.size());

        uint32_t registeredCount = 0;
        for (const auto& [displayId, realModelPath] : g_sidecarCreatureModelPath)
        {
            char matSpec[2048] = {};
            BuildCreatureMaterialPatchSpec(matSpec, sizeof(matSpec), displayId);
            if (!matSpec[0])
            {
                // Model path known but no texture rows for this displayId -- nothing to bake, and
                // OnCreatureModelResolve already skips displayIds with no g_sidecarCreatureTextures
                // entry, so pre-registering here would just be a no-op VPathPopulateGlobal call.
                continue;
            }

            if (realModelPath.empty()) continue;

            // No texPath (only materialPatchSpec) -- same as the reactive path in
            // OnCreatureModelResolve; creature texture rows are all TextureType-keyed.
            char vModelPath[280] = {};
            bool registered = VPathPopulateGlobal(realModelPath.c_str(), displayId, nullptr, matSpec,
                                                   vModelPath, sizeof(vModelPath));
            CreatureLog("  preregister: display=%u real='%s' vpath='%s' spec='%s' registered=%d",
                        displayId, realModelPath.c_str(), vModelPath, matSpec, registered ? 1 : 0);
            if (!registered || !vModelPath[0]) continue;

            g_creatureVPaths[displayId] = vModelPath;
            ++registeredCount;
        }

        CreatureLog("creature preregister: done, %u display(s) registered ahead of first spawn",
                    registeredCount);
    }

    CreatureExtension::CreatureExtension()
    {
        // Deliberately does NOT call LoadCreatureSidecar/PreregisterSidecarCreatures here -- this
        // constructor runs on the file-scope global at DLL static-init time (DllMain), before the
        // client's own archive/file-I/O subsystems are guaranteed up.
        on<&CreatureExtension::OnCreatureModelResolve>(ev::Event::OnCreatureModelResolve);
        on<&CreatureExtension::OnItemSlotChange>(ev::Event::OnItemSlotChange);
    }

    // Not item/equip-related -- reused purely as an early, reliable trigger. OnItemSlotChange is
    // confirmed to be the very first event in the entire log: it fires for the head slot before any
    // weapon-specific event exists, well ahead of any full RebuildAllModels/OnM2SkinFinalize/
    // PerFrame cycle. OnWeaponVisualChange was the previous choice here, but it doesn't actually fire
    // until after that whole cycle has already run for the character's other equipment -- it was
    // never as early as it looked. OnItemSlotChange is.
    //
    // This decoupling mirrors weapons directly: registration (PreregisterSidecarWeapons) and the
    // actual native field substitution (OnItemDisplayLookup) are two SEPARATE hooks there, so
    // registration finishing early costs nothing and helps every later lookup. Doing the same thing
    // here -- kicking creature preload off an unrelated-but-early event -- gets creatures the same
    // property: by the time any creature's OnCreatureModelResolve fires for real, the preload is
    // long since finished and out of the way.
    void CreatureExtension::OnItemSlotChange(const ev::ItemSlotChangeArgs&)
    {
        static bool kicked = false;
        if (kicked) return;
        kicked = true;
        LoadCreatureSidecar(); // no-op after the first call; runs PreregisterSidecarCreatures at its end
    }

    // Fires for every CreatureModelData row the client resolves (see OnCreatureModelResolve's doc
    // comment in Event.hpp) -- covers every way a creature's model gets resolved, native model
    // loader included, since GameHooks' hook sits inside the one confirmed choke point for this.
    // displayId is the sidecar key (NOT modelId -- several displayIds commonly share one model, and
    // each needs its own independently baked texture set; see CreatureModelResolveArgs's doc
    // comment for why modelId can't do this job).
    void CreatureExtension::OnCreatureModelResolve(const ev::CreatureModelResolveArgs& a)
    {
        if (!a.record || a.displayId == 0) return;

        // Already baked and registered for this displayId in a prior call -- just re-point
        // ModelName at the stashed, process-lifetime string and stop. The overwhelming majority of
        // calls for a displayId with no sidecar entry at all never reach this map (see the sidecar
        // lookup below), so this early hit only helps repeat resolves of displayIds we DO care about.
        auto stashedIt = g_creatureVPaths.find(a.displayId);
        if (stashedIt != g_creatureVPaths.end())
        {
            *reinterpret_cast<const char**>(static_cast<uint8_t*>(a.record) + db2::creaturemodeldata::kOffModelName)
                = stashedIt->second.c_str();
            return;
        }

        LoadCreatureSidecar(); // no-op after the first call

        // Re-check: LoadCreatureSidecar (first call only) runs PreregisterSidecarCreatures at its
        // end, which may have just registered and stashed THIS exact displayId. In the steady state
        // (OnItemSlotChange already kicked the preload on an earlier, unrelated event) this is
        // always a no-op second lookup -- cheap insurance, not the primary fix; the primary fix is
        // that the preload no longer runs inside this call in the first place.
        stashedIt = g_creatureVPaths.find(a.displayId);
        if (stashedIt != g_creatureVPaths.end())
        {
            *reinterpret_cast<const char**>(static_cast<uint8_t*>(a.record) + db2::creaturemodeldata::kOffModelName)
                = stashedIt->second.c_str();
            return;
        }

        auto it = g_sidecarCreatureTextures.find(a.displayId);
        if (it == g_sidecarCreatureTextures.end()) return; // nothing configured; leave ModelName untouched

        char matSpec[2048] = {};
        BuildCreatureMaterialPatchSpec(matSpec, sizeof(matSpec), a.displayId);
        if (!matSpec[0]) return;

        const char* realModelName =
            *reinterpret_cast<const char**>(static_cast<uint8_t*>(a.record) + db2::creaturemodeldata::kOffModelName);
        if (!realModelName || !*realModelName) return;

        // No texPath (only materialPatchSpec) -- creature texture rows are all TextureType-keyed,
        // there's no separate "bake this one texture onto whatever OBJECT_SKIN slots are left"
        // fallback the way weapons use texPath for ModelTexture_1/2. modelId is passed through only
        // for logging; the virtual-path key itself is displayId, via VPathPopulateGlobal's own
        // itemDisplayId parameter (generic despite the name -- see VirtualPath.hpp).
        char vModelPath[280] = {};
        bool registered = VPathPopulateGlobal(realModelName, a.displayId, nullptr, matSpec,
                                               vModelPath, sizeof(vModelPath));
        CreatureLog("model resolve: display=%u modelId=%u real='%s' vpath='%s' spec='%s' registered=%d",
                    a.displayId, a.modelId, realModelName, vModelPath, matSpec, registered ? 1 : 0);
        if (!registered || !vModelPath[0]) return;

        auto& stashed = g_creatureVPaths[a.displayId];
        stashed = vModelPath;
        *reinterpret_cast<const char**>(static_cast<uint8_t*>(a.record) + db2::creaturemodeldata::kOffModelName)
            = stashed.c_str();
    }

    // Self-registration: file-scope instance binds handlers at DLL load via EventScript ctor.
    CreatureExtension g_creatureExtension;
}
