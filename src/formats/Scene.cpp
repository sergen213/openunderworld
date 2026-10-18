#include "formats/Formats.hpp"

#include "anim/Anim.hpp"

#include <cstring>
#include <deque>
#include <map>
#include <set>

namespace ou::formats {
namespace {

using Drms = std::deque<std::pair<std::string, cdc::Drm>>; // deque: references stay valid on growth

const cdc::Drm* loadInto(Drms& drms, const cdc::BigfileSet& bigfile, const std::string& path) {
    auto bytes = bigfile.read(path);
    if (!bytes) return nullptr;
    return &drms.emplace_back(path, cdc::parseDrm(std::move(*bytes))).second;
}

void warnUnusedSections(scene::SceneData& sc, const std::string& path, const cdc::Drm& drm) {
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
void loadTextures(scene::SceneData& sc, const Drms& drms) {
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
}

// Copy of `meshes` with `matrix` as model transform.
void place(scene::SceneData& sc, const std::vector<scene::MeshData>& meshes, const float matrix[16], const std::string& tag) {
    for (const auto& m : meshes) {
        auto& p = sc.meshes.emplace_back(m);
        std::copy(matrix, matrix + 16, p.transform);
        p.origin += tag;
    }
}

} // namespace

scene::SceneData loadDrmScene(const cdc::BigfileSet& bigfile, std::string_view drmPath, bool withDependencies) {
    scene::SceneData sc;
    Drms drms;
    if (!loadInto(drms, bigfile, std::string(drmPath))) throw cdc::FormatError("not in bigfile: " + std::string(drmPath));
    if (withDependencies) {
        const auto deps = drms.front().second.dependencies;
        for (const auto& d : deps) {
            const std::string path = "pc-w\\" + d + ".drm";
            try {
                if (!loadInto(drms, bigfile, path)) sc.warnings.push_back("dependency not in bigfile: " + path);
            } catch (const cdc::FormatError& e) {
                sc.warnings.push_back(path + ": " + e.what());
            }
        }
    }
    for (const auto& [path, drm] : drms) {
        // Skinned models: link each mesh to the skeleton deforming its render model (docs/formats/ANIMATION.md).
        const size_t base = sc.skeletons.size();
        for (auto& s : anim::decodeSkeletons(drm, sc.warnings)) sc.skeletons.push_back(std::move(s));
        for (auto& [id, meshes] : decodeRenderResources(drm, sc.warnings, path))
            for (auto& m : meshes) {
                for (size_t k = base; k < sc.skeletons.size(); ++k)
                    if (!m.skin.empty() && sc.skeletons[k].renderModelId == id) m.skeleton = int32_t(k);
                sc.meshes.push_back(std::move(m));
            }
        warnUnusedSections(sc, path, drm);
    }
    loadTextures(sc, drms);
    return sc;
}

scene::SceneData loadLevelScene(const cdc::BigfileSet& bigfile, std::string_view unitPath) {
    scene::SceneData sc;
    Drms drms;
    const std::string unitName(unitPath);
    const cdc::Drm* unit = loadInto(drms, bigfile, unitName);
    if (!unit) throw cdc::FormatError("not in bigfile: " + unitName);
    const Unit u = parseUnit(*unit);

    // Level geometry: the unit plus its stream-group DRMs. Terrain and cell meshes are in world
    // space; models referenced by IMF DTPs are placed per IMF record instead.
    std::vector<const cdc::Drm*> levelDrms{unit};
    for (const auto& g : u.streamGroups) {
        const std::string path = "pc-w\\streamgroups\\" + g + ".drm";
        try {
            if (const auto* d = loadInto(drms, bigfile, path)) levelDrms.push_back(d);
            else sc.warnings.push_back("stream group not in bigfile: " + path);
        } catch (const cdc::FormatError& e) {
            sc.warnings.push_back(path + ": " + e.what());
        }
    }
    std::map<uint32_t, uint32_t> imfModel; // DTP id -> render model id (DTP +4, observed)
    for (const auto& p : u.imfs) imfModel.emplace(p.dtpId, 0);
    std::map<uint32_t, std::vector<scene::MeshData>> levelModels; // render resource id -> meshes
    for (size_t i = 0; i < levelDrms.size(); ++i) {
        const auto& drm = *levelDrms[i];
        for (const auto& s : drm.sections)
            if (s.type == cdc::SectionType::DtpData && imfModel.contains(s.id) && s.data.size() >= 8)
                std::memcpy(&imfModel[s.id], s.data.data() + 4, 4);
        for (auto& [id, meshes] : decodeRenderResources(drm, sc.warnings, drms[i].first))
            levelModels.emplace(id, std::move(meshes));
        warnUnusedSections(sc, drms[i].first, drm);
    }
    std::set<uint32_t> placedModels;
    for (const auto& [dtp, model] : imfModel) placedModels.insert(model);
    for (auto& [id, meshes] : levelModels)
        if (!placedModels.contains(id))
            for (const auto& m : meshes) sc.meshes.push_back(m);
    std::map<uint32_t, size_t> unresolvedImf;
    for (const auto& p : u.imfs) {
        const auto it = levelModels.find(imfModel[p.dtpId]);
        if (it == levelModels.end()) { ++unresolvedImf[p.dtpId]; continue; }
        place(sc, it->second, p.matrix, " @imf " + p.name);
    }
    for (const auto& [dtp, n] : unresolvedImf)
        sc.warnings.push_back(unitName + ": " + std::to_string(n) + " IMF placement(s) of DTP " + std::to_string(dtp) +
                              " without a decoded render model");

    // Intros: object id -> objectlist.txt name -> pc-w\<name>.drm -> Object model 0.
    const auto listBytes = bigfile.read("\\tr8\\pc-w\\objectlist.txt");
    if (!listBytes) sc.warnings.push_back("\\tr8\\pc-w\\objectlist.txt not in bigfile: intros not placed");
    const auto objectNames = listBytes ? parseObjectList({reinterpret_cast<const char*>(listBytes->data()), listBytes->size()})
                                       : std::map<uint16_t, std::string>{};
    struct ObjectMeshes { std::string name; std::vector<scene::MeshData> meshes; size_t intros = 0; std::string problem; };
    std::map<uint16_t, ObjectMeshes> objects;
    for (const auto& in : u.intros) {
        if (!listBytes || in.objectId == kMarkerObjectId) continue;
        auto [it, fresh] = objects.try_emplace(in.objectId);
        auto& o = it->second;
        ++o.intros;
        if (fresh) {
            const auto n = objectNames.find(in.objectId);
            if (n == objectNames.end()) {
                o.problem = "object id not in objectlist.txt";
            } else {
                o.name = n->second;
                const std::string path = "pc-w\\" + o.name + ".drm";
                try {
                    const auto* d = loadInto(drms, bigfile, path);
                    if (!d) {
                        o.problem = path + " not in bigfile";
                    } else {
                        uint32_t model = 0;
                        for (const uint32_t id : objectRenderModels(*d))
                            if ((model = id)) break;
                        auto decoded = decodeRenderResources(*d, sc.warnings, path);
                        if (model && !decoded[model].empty()) o.meshes = std::move(decoded[model]);
                        else o.problem = model ? "model has no drawable batches" : "object has no render model";
                    }
                } catch (const cdc::FormatError& e) {
                    o.problem = path + ": " + e.what();
                }
            }
        }
        float m[16];
        introMatrix(in, m);
        place(sc, o.meshes, m, " @intro " + std::to_string(in.uniqueId));
    }
    for (const auto& [id, o] : objects)
        if (!o.problem.empty())
            sc.warnings.push_back(unitName + ": object " + std::to_string(id) + (o.name.empty() ? "" : " (" + o.name + ")") +
                                  ", " + std::to_string(o.intros) + " intro(s) not drawn: " + o.problem);

    loadTextures(sc, drms);
    return sc;
}

} // namespace ou::formats
