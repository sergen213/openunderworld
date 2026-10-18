#include "formats/Formats.hpp"

#include <map>
#include <set>

namespace ou::formats {

scene::SceneData loadDrmScene(const cdc::Bigfile& bigfile, std::string_view drmPath, bool withDependencies) {
    scene::SceneData sc;
    std::vector<std::pair<std::string, cdc::Drm>> drms;
    auto load = [&](const std::string& path) {
        const auto rec = bigfile.find(path);
        if (!rec) return false;
        drms.emplace_back(path, cdc::parseDrm(bigfile.read(*rec)));
        return true;
    };
    if (!load(std::string(drmPath))) throw cdc::FormatError("not in bigfile: " + std::string(drmPath));
    if (withDependencies) {
        const auto deps = drms.front().second.dependencies;
        for (const auto& d : deps) {
            const std::string path = "pc-w\\" + d + ".drm";
            try {
                if (!load(path)) sc.warnings.push_back("dependency not in bigfile: " + path);
            } catch (const cdc::FormatError& e) {
                sc.warnings.push_back(path + ": " + e.what());
            }
        }
    }

    for (const auto& [path, drm] : drms) {
        auto meshes = decodeModels(drm, sc.warnings, path);
        for (auto& m : meshes) sc.meshes.push_back(std::move(m));
        std::map<cdc::SectionType, int> skipped;
        for (const auto& s : drm.sections)
            if (s.type != cdc::SectionType::Texture && s.type != cdc::SectionType::Material &&
                s.type != cdc::SectionType::RenderResource)
                ++skipped[s.type];
        for (const auto& [t, n] : skipped)
            sc.warnings.push_back(path + ": " + std::to_string(n) + " " + std::string(cdc::sectionTypeName(t)) +
                                  " section(s) not used by the scene loader");
    }

    // Textures referenced by submeshes; the first loaded DRM containing an id wins.
    std::set<uint32_t> wanted;
    for (const auto& m : sc.meshes)
        for (const auto& s : m.submeshes)
            if (s.textureId) wanted.insert(s.textureId);
    for (const uint32_t id : wanted) {
        const cdc::Section* sec = nullptr;
        const std::string* from = nullptr;
        for (const auto& [path, drm] : drms) {
            for (const auto& s : drm.sections)
                if (s.type == cdc::SectionType::Texture && s.id == id) { sec = &s; from = &path; break; }
            if (sec) break;
        }
        if (!sec) {
            sc.warnings.push_back("texture " + std::to_string(id) + " not found in loaded DRMs");
            continue;
        }
        std::string why;
        auto t = decodeTexture(*sec, &why);
        if (!t) {
            sc.warnings.push_back(*from + "#texture " + std::to_string(id) + ": " + why);
            continue;
        }
        t->origin = *from + "#texture " + std::to_string(id);
        sc.textures.push_back(std::move(*t));
    }
    return sc;
}

} // namespace ou::formats
