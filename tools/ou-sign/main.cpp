// ou-sign: release signing for OpenUnderworld update manifests (Ed25519, libsodium).
// Keys live outside the repository, by default in $XDG_CONFIG_HOME/openunderworld-signing.
// Ceremony and threat model: docs/UPDATES.md.
#include "config/Config.hpp"
#include "update/Update.hpp"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using ou::update::Bytes;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  ou-sign keygen   [--dir D]           create release.key (secret, 0600) + release.pub.hex\n"
                 "  ou-sign pubkey   [--dir D]           print the public key (hex)\n"
                 "  ou-sign manifest --version V --base-url https://... [--channel C] [--min-version V]\n"
                 "                   [--changelog FILE] DIR > manifest.json\n"
                 "  ou-sign sign     [--dir D] MANIFEST  write MANIFEST.sig (64 raw bytes), then verify it\n"
                 "  ou-sign verify   [--pubkey HEX] MANIFEST [SIG]   default key: the one compiled in\n"
                 "default D: $XDG_CONFIG_HOME/openunderworld-signing\n");
    return 2;
}

Bytes readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + p.string());
    return Bytes(std::istreambuf_iterator<char>(f), {});
}

void writeAll(const fs::path& p, const void* data, size_t n, fs::perms mode) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), std::streamsize(n));
    if (!f.flush()) throw std::runtime_error("cannot write " + p.string());
    fs::permissions(p, mode, fs::perm_options::replace);
}

std::string hex(const unsigned char* d, size_t n) {
    std::string s(2 * n + 1, '\0');
    sodium_bin2hex(s.data(), s.size(), d, n);
    s.pop_back();
    return s;
}

// The secret key must never land in a git work tree.
void refuseGitTree(fs::path d) {
    for (d = fs::weakly_canonical(fs::absolute(d)); !d.empty(); d = d.parent_path()) {
        if (fs::exists(d / ".git")) throw std::runtime_error("refusing key directory inside git work tree " + d.string());
        if (d == d.root_path()) break;
    }
}

std::array<unsigned char, crypto_sign_SECRETKEYBYTES> loadSecret(const fs::path& dir) {
    Bytes b = readAll(dir / "release.key");
    if (b.size() != crypto_sign_SECRETKEYBYTES) throw std::runtime_error("release.key has wrong size");
    std::array<unsigned char, crypto_sign_SECRETKEYBYTES> sk;
    std::copy(b.begin(), b.end(), sk.begin());
    sodium_memzero(b.data(), b.size());
    return sk;
}

} // namespace

int main(int argc, char** argv) {
    if (sodium_init() < 0 || argc < 2) return usage();
    std::string cmd = argv[1];
    fs::path dir = ou::config::configHome() / "openunderworld-signing";
    std::string pubHex, version, channel = "stable", baseUrl, minVersion, changelogFile;
    std::vector<std::string> pos;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        try {
            if (a == "--dir") dir = val();
            else if (a == "--pubkey") pubHex = val();
            else if (a == "--version") version = val();
            else if (a == "--channel") channel = val();
            else if (a == "--base-url") baseUrl = val();
            else if (a == "--min-version") minVersion = val();
            else if (a == "--changelog") changelogFile = val();
            else if (a.starts_with("--")) return usage();
            else pos.push_back(a);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-sign: %s\n", e.what());
            return 2;
        }
    }
    try {
        if (cmd == "keygen") {
            refuseGitTree(dir);
            if (fs::exists(dir / "release.key")) throw std::runtime_error((dir / "release.key").string() + " exists; not overwriting");
            fs::create_directories(dir);
            fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace);
            unsigned char pk[crypto_sign_PUBLICKEYBYTES], sk[crypto_sign_SECRETKEYBYTES];
            crypto_sign_keypair(pk, sk);
            writeAll(dir / "release.key", sk, sizeof sk, fs::perms::owner_read | fs::perms::owner_write);
            sodium_memzero(sk, sizeof sk);
            std::string h = hex(pk, sizeof pk) + "\n";
            writeAll(dir / "release.pub.hex", h.data(), h.size(), fs::perms(0644));
            std::printf("%s", h.c_str());
            std::fprintf(stderr, "ou-sign: wrote %s (secret) and release.pub.hex\n", (dir / "release.key").c_str());
        } else if (cmd == "pubkey") {
            auto sk = loadSecret(dir);
            unsigned char pk[crypto_sign_PUBLICKEYBYTES];
            crypto_sign_ed25519_sk_to_pk(pk, sk.data());
            sodium_memzero(sk.data(), sk.size());
            std::printf("%s\n", hex(pk, sizeof pk).c_str());
        } else if (cmd == "manifest") {
            if (pos.size() != 1 || version.empty() || baseUrl.empty()) return usage();
            if (!baseUrl.ends_with('/')) baseUrl += '/';
            fs::path root = pos[0];
            nlohmann::json files = nlohmann::json::array();
            std::vector<fs::path> paths;
            for (const auto& e : fs::recursive_directory_iterator(root))
                if (e.is_regular_file() && !e.is_symlink()) paths.push_back(e.path());
            std::sort(paths.begin(), paths.end());
            for (const auto& p : paths) {
                Bytes b = readAll(p);
                std::string rel = p.lexically_relative(root).generic_string();
                bool exec = (fs::status(p).permissions() & fs::perms::owner_exec) != fs::perms::none;
                nlohmann::json f = {{"path", rel}, {"size", b.size()}, {"sha256", ou::update::sha256Hex(b)}, {"url", baseUrl + rel}};
                if (exec) f["executable"] = true;
                files.push_back(f);
            }
            std::string changelog;
            if (!changelogFile.empty()) { Bytes c = readAll(changelogFile); changelog.assign(c.begin(), c.end()); }
            nlohmann::json m = {{"version", version}, {"channel", channel}, {"min_version", minVersion}, {"changelog", changelog}, {"files", files}};
            std::string out = m.dump(2) + "\n";
            ou::update::parseManifest(out); // same validation the client applies (paths, URLs, no game data)
            std::cout << out;
        } else if (cmd == "sign") {
            if (pos.size() != 1) return usage();
            Bytes msg = readAll(pos[0]);
            ou::update::parseManifest({reinterpret_cast<const char*>(msg.data()), msg.size()});
            auto sk = loadSecret(dir);
            unsigned char sig[crypto_sign_BYTES], pk[crypto_sign_PUBLICKEYBYTES];
            crypto_sign_detached(sig, nullptr, msg.data(), msg.size(), sk.data());
            crypto_sign_ed25519_sk_to_pk(pk, sk.data());
            sodium_memzero(sk.data(), sk.size());
            ou::update::PublicKey key;
            std::copy(pk, pk + sizeof pk, key.begin());
            if (!ou::update::verifySignature(msg, {sig, sizeof sig}, key)) throw std::runtime_error("self-verify failed");
            writeAll(pos[0] + ".sig", sig, sizeof sig, fs::perms(0644));
            std::fprintf(stderr, "ou-sign: wrote %s.sig (key %s)\n", pos[0].c_str(), hex(pk, sizeof pk).c_str());
            auto compiled = ou::update::releasePublicKey();
            if (!compiled || *compiled != key)
                std::fprintf(stderr, "ou-sign: WARNING: this key is not the release key compiled into this build\n");
        } else if (cmd == "verify") {
            if (pos.empty() || pos.size() > 2) return usage();
            auto key = pubHex.empty() ? ou::update::releasePublicKey() : ou::update::parsePublicKeyHex(pubHex);
            if (!key) throw std::runtime_error("no public key (none compiled in and no valid --pubkey)");
            Bytes msg = readAll(pos[0]), sig = readAll(pos.size() == 2 ? pos[1] : pos[0] + ".sig");
            auto m = ou::update::verifyAndParse(msg, sig, *key);
            std::printf("OK: version %s, channel %s, %zu files\n", m.version.c_str(), m.channel.c_str(), m.files.size());
        } else {
            return usage();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ou-sign: %s\n", e.what());
        return 1;
    }
    return 0;
}
