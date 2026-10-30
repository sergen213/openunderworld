#include "formats/Formats.hpp"

#include "anim/Anim.hpp"
#include "formats/Shader.hpp"

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
        if (s.type != cdc::SectionType::Texture && s.type != cdc::SectionType::Material && s.type != cdc::SectionType::ShaderLib &&
            s.type != cdc::SectionType::RenderResource)
            ++skipped[s.type];
    for (const auto& [t, n] : skipped)
        sc.warnings.push_back(path + ": " + std::to_string(n) + " " + std::string(cdc::sectionTypeName(t)) +
                              " section(s) not used by the scene loader");
}

const cdc::Section* findSection(const Drms& drms, cdc::SectionType type, uint32_t id, const std::string** from = nullptr) {
    for (const auto& [path, drm] : drms)
        for (const auto& s : drm.sections)
            if (s.type == type && s.id == id) {
                if (from) *from = &path;
                return &s;
            }
    return nullptr;
}

// Materials referenced by submeshes with their composite-pass programs (docs/formats/SHADERS.md): VS slot 0x20
// (composite, unskinned; the renderer feeds float normals with NormalScaleOffset (1, 0)) else 0xA0, PS slot 0
// (no PSSM receive, no fade variant). The first loaded DRM containing a section id wins.
void loadMaterials(scene::SceneData& sc, const Drms& drms) {
    std::set<uint32_t> wanted;
    for (const auto& m : sc.meshes)
        for (const auto& s : m.submeshes)
            if (s.materialId) wanted.insert(s.materialId);
    std::set<uint64_t> have;
    for (const auto& c : sc.shaders) have.insert(c.hash);
    auto program = [&](uint32_t libId, std::initializer_list<uint32_t> slots) -> uint64_t {
        const cdc::Section* lib = libId ? findSection(drms, cdc::SectionType::ShaderLib, libId) : nullptr;
        if (!lib) return 0;
        try {
            const auto parsed = d3d9::parseShaderLib(lib->data);
            for (const uint32_t slot : slots)
                if (const auto* e = parsed.find(slot)) {
                    if (have.insert(e->hash).second)
                        sc.shaders.push_back({e->hash, parsed.pixel, {e->bytecode.begin(), e->bytecode.end()}});
                    return e->hash;
                }
        } catch (const cdc::FormatError& e) {
            sc.warnings.push_back("shader lib " + std::to_string(libId) + ": " + e.what());
        }
        return 0;
    };
    // Weighted-skin VS (slot 0x21, else 0xA1) writing its COLOR0 output from c[a0 + 88] (Material::skinParams).
    auto skinParams = [&](uint32_t libId) {
        const cdc::Section* lib = libId ? findSection(drms, cdc::SectionType::ShaderLib, libId) : nullptr;
        try {
            const auto parsed = lib ? d3d9::parseShaderLib(lib->data) : d3d9::ShaderLib{};
            const auto* e = parsed.find(0x21);
            if (!e) e = parsed.find(0xA1);
            if (!e) return false;
            const auto p = d3d9::parse(e->bytecode);
            int color = -1;
            for (const auto& in : p.instructions)
                if (in.op == d3d9::Dcl && in.dst.type == d3d9::Reg::Output && in.usage == d3d9::Usage::Color && !in.usageIndex)
                    color = in.dst.num;
            for (const auto& in : p.instructions)
                if (in.hasDst && in.dst.type == d3d9::Reg::Output && in.dst.num == color)
                    for (const auto& s : in.src)
                        if (s.relative && s.num == 88) return true;
        } catch (const cdc::FormatError&) {
        }
        return false;
    };
    for (const uint32_t id : wanted) {
        const cdc::Section* sec = findSection(drms, cdc::SectionType::Material, id);
        if (!sec) continue;
        MaterialShaderLibs libs;
        auto m = decodeMaterial(*sec, &libs);
        m.vertexShader = program(libs.vertex, {0x20, 0xA0});
        m.skinParams = m.vertexShader && skinParams(libs.vertex);
        m.pixelShader = program(libs.pixel, {0});
        sc.materials.push_back(std::move(m));
    }
}

// Textures referenced by submeshes, materials and instances; the first loaded DRM containing an id wins.
void loadTextures(scene::SceneData& sc, const Drms& drms) {
    std::set<uint32_t> wanted;
    for (const auto& m : sc.meshes) {
        for (const auto& s : m.submeshes) {
            if (s.textureId) wanted.insert(s.textureId);
            for (const uint32_t t : s.instanceTextures)
                if (t) wanted.insert(t);
        }
    }
    for (const auto& m : sc.materials)
        for (const auto& t : m.samplers)
            if (t.textureId) wanted.insert(t.textureId);
    if (sc.environment.sunCookie) wanted.insert(sc.environment.sunCookie);
    for (const auto& t : sc.textures) wanted.erase(t.id);
    for (const uint32_t id : wanted) {
        const std::string* from = nullptr;
        const cdc::Section* sec = findSection(drms, cdc::SectionType::Texture, id, &from);
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
// `textures`: per-placement instance textures (IMF +0x4C..) replacing the primgroup defaults where non-zero.
void place(scene::SceneData& sc, const std::vector<scene::MeshData>& meshes, const float matrix[16], const std::string& tag,
           const uint32_t* textures = nullptr) {
    for (const auto& m : meshes) {
        auto& p = sc.meshes.emplace_back(m);
        std::copy(matrix, matrix + 16, p.transform);
        p.origin += tag;
        if (textures)
            for (auto& s : p.submeshes)
                for (int k = 0; k < 4; ++k)
                    if (textures[k]) s.instanceTextures[k] = textures[k];
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
    loadMaterials(sc, drms);
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
    sc.environment = u.environment;

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
            for (auto m : meshes) {
                if (std::ranges::find(u.exteriorTerrains, id) != u.exteriorTerrains.end())
                    for (auto& s : m.submeshes) s.exterior = true;
                sc.meshes.push_back(std::move(m));
            }
    std::map<uint32_t, size_t> unresolvedImf;
    for (const auto& p : u.imfs) {
        const auto it = levelModels.find(imfModel[p.dtpId]);
        if (it == levelModels.end()) { ++unresolvedImf[p.dtpId]; continue; }
        place(sc, it->second, p.matrix, " @imf " + p.name, p.textures);
    }
    for (const auto& [dtp, n] : unresolvedImf)
        sc.warnings.push_back(unitName + ": " + std::to_string(n) + " IMF placement(s) of DTP " + std::to_string(dtp) +
                              " without a decoded render model");

    // Intros: object id -> objectlist.txt name -> pc-w\<name>.drm -> Object model 0.
    const auto listBytes = bigfile.read("\\tr8\\pc-w\\objectlist.txt");
    if (!listBytes) sc.warnings.push_back("\\tr8\\pc-w\\objectlist.txt not in bigfile: intros not placed");
    const auto objectNames = listBytes ? parseObjectList({reinterpret_cast<const char*>(listBytes->data()), listBytes->size()})
                                       : std::map<uint16_t, std::string>{};
    struct ObjectMeshes { std::string name; std::vector<scene::MeshData> meshes; size_t intros = 0; std::string problem; bool hidden = false; };
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
                    } else if (objectFlags(*d) & (kObjectHiddenAtBirth | kObjectNoInstanceDraw)) {
                        o.hidden = true; // trigger/sound volumes, camera anchors, dispatchers, fx_* effect proxies, ...
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
        if (o.hidden) continue;
        float m[16];
        introMatrix(in, m);
        place(sc, o.meshes, m, " @intro " + std::to_string(in.uniqueId));
    }
    for (const auto& [id, o] : objects)
        if (!o.problem.empty())
            sc.warnings.push_back(unitName + ": object " + std::to_string(id) + (o.name.empty() ? "" : " (" + o.name + ")") +
                                  ", " + std::to_string(o.intros) + " intro(s) not drawn: " + o.problem);

    loadMaterials(sc, drms);
    loadTextures(sc, drms);
    return sc;
}

} // namespace ou::formats
