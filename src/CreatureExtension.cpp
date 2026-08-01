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

    // Same search pattern as EquipExtension's LoadSidecarModels: bare filename, DBFilesClient\
    // beside it, and DBFilesClient\ inside every mounted Data\*.MPQ directory (patch overrides).
    static void LoadCreatureSidecar()
    {
        if (g_sidecarLoaded) return;
        g_sidecarLoaded = true;

        LoadCreatureTextureSidecarFile("WXLCreatureTextures.csv");
        LoadCreatureTextureSidecarFile("DBFilesClient\\WXLCreatureTextures.csv");

        WIN32_FIND_DATAA fd = {};
        HANDLE h = FindFirstFileA("Data\\*.MPQ", &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::string path = "Data\\";
                path += fd.cFileName;
                path += "\\DBFilesClient\\WXLCreatureTextures.csv";
                LoadCreatureTextureSidecarFile(path.c_str());
            }
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }

        CreatureLog("creature sidecar table ready: displays=%zu", g_sidecarCreatureTextures.size());
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

    CreatureExtension::CreatureExtension()
    {
        on<&CreatureExtension::OnCreatureModelResolve>(ev::Event::OnCreatureModelResolve);
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
