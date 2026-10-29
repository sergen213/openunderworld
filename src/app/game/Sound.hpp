// ou-game sound events (docs/formats/AUDIO.md §9): SoundPlex graphs started by unit sfxmarkers (perimeter
// enter/exit), by script "sound" natives and by Lara drowning; voice-line subtitles on the two-line ticker;
// music-variable changes (gamebase natives) logged for the Mus! sequencer.
#pragma once

#include "audio/SoundPlex.hpp"
#include "fx/Fx.hpp"
#include "game/Game.hpp"
#include "locale/Locale.hpp"
#include "ui/Ui.hpp"

#include <array>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace ou::app {

class GameSound {
public:
    GameSound(const cdc::BigfileSet& bigfile, audio::Mixer& mixer); // loads lara.drm + generalbank.drm graphs
    void attach(game::World& world); // script "sound" native hook; call once after the world exists
    void tick(game::World& world);   // after World::tick: units, sfxmarkers, drowning, scheduled parts, subtitles
    const locale::SubtitleTicker& subtitles() const { return ticker_; }
    // Help on demand (PDA Hint / Answer): plays vo\lara\sc_<level>_<NN>_hint|answer for the unit script's current
    // "hint_NN" condition (script::Runtime::helpCondition); false (and a log line) when there is none.
    bool playHelp(const game::World& world, bool answer);
    // A sound record of an animation / one-shot event list (fx::System::sound; AUDIO.md §9.7) at a world position.
    void addObject(const fx::ObjectFx& fx); // its DTPs join the bank, its Waves the mixer (once; animSound does it too)
    void animSound(const fx::ObjectFx& fx, const fx::Event& e, glm::vec3 position, uint8_t material, uint64_t tick);

private:
    struct UnitSfx {
        std::shared_ptr<const cdc::Drm> drm;
        std::vector<audio::SfxMarker> markers;
        audio::SfxMarkerTriggers triggers;
        std::map<std::pair<size_t, size_t>, audio::SoundPlayer::Handle> handles; // (marker, sound)
        std::map<std::string, std::vector<uint32_t>> barks; // enemy chatter DTPs by category ("vo\\enemy\\merc\\b_M11_ADV_g_01": ADV)
    };
    void syncUnits(const game::World& world);
    const cdc::BigfileSet& bf_;
    audio::Mixer& mixer_;
    audio::PlexBank bank_;
    audio::SoundPlayer player_;
    std::map<std::string, std::shared_ptr<const audio::Mul>> streams_;
    std::map<std::string, UnitSfx> units_;
    std::map<std::pair<int32_t, uint32_t>, audio::SoundPlayer::Handle> scriptHandles_; // (owner, member)
    std::map<std::tuple<std::string, uint32_t, uint32_t>, std::unique_ptr<audio::Plex>> scriptPlex_; // inline graphs
    uint64_t unitsVersion_ = ~0ull;
    audio::SoundPlayer::Handle drown_ = 0;
    audio::SoundPlayer::Handle engine_ = 0; // the bike's engine graph while ridden
    float bikeSpeed_ = 0;
    std::array<uint8_t, 256> lastVars_{};
    locale::SubtitleTicker ticker_;
    std::map<std::string, uint32_t> helpLines_; // generalbank stream path -> its DTP
    audio::SoundPlayer::Handle help_ = 0;
    std::string lastCondition_;
    std::map<std::tuple<const cdc::Drm*, uint32_t, uint32_t>, std::unique_ptr<audio::Plex>> eventPlex_; // kind-0 graphs
    std::set<const cdc::Drm*> eventDrms_; // object DRMs whose DTPs / Waves joined the bank and the mixer
    std::mt19937 rng_{0x5B28}; // kind-3 weighted picks (fixed seed: replays repeat)
};

// The two-line subtitle ticker (cutscenes and gameplay voice lines), drawn by ui::Hud::subtitles.
void drawSubtitles(ui::Canvas& canvas, const locale::SubtitleTicker& ticker);

} // namespace ou::app
