#pragma once
// A binary request/response server for the bot interface (py/polyenv.py is the client). One
// process hosts many matches ("slots") and serves one connection at a time. Every request and
// response is a frame: u32 length of what follows, then u8 op (request) or u8 status (response,
// 0 ok, 1 error followed by a message). All integers are little-endian.
//
//   op 1 NEW      u32 slot, spec                          -> status block
//   op 2 STEP     u32 n, n x (u32 slot, i32 legal index)  -> n x (u8 ok, status block)
//   op 3 OBSERVE  u32 n, n x (u32 slot, u8 viewer)        -> n x observation block (viewer 0: current player)
//   op 4 BOT      u32 n, n x u32 slot                     -> n x i32 index of the built-in AI's choice, or -1
//   op 5 SAVE     u32 slot                                -> u32 size, GameState bytes
//   op 6 LOAD     u32 slot, spec, u32 size, bytes          -> status block
//   op 7 DUMP     u32 slot                                -> the full GameState as JSON
//   op 8 INFO                                             -> versions and observation sizes, legal-list version
//   op 9 FREE     u32 slot
//   op 10 OWNERS  u32 slot                                -> u16 w, u16 h, u8 owner[w*h], u8 capitalOf[w*h]
//                 (the true state, not an observation: for training targets and checks only)
//   op 11 REPLAY  u32 slot, spec, u32 size, start blob, u32 size, final blob
//                                                         -> u32 commands left, status block
//                 (blobs as the game's server sends them: i32 version, then the GameState)
//   op 12 RSTEP   u32 slot                                -> u8 player, i32 label, i16 type, a, b, arg,
//                                                            u8 ok, u32 commands left, status block
//                 (plays the next recorded command; label as env::Match::replayStep)
//   op 13 LOADCUR u32 slot, spec, u32 size, i32 version + state -> status block: loads a state saved
//                 in an older game version, then marks it as the current version, so play goes on
//                 under the current rules (human midgame starts, py/human_starts.py)
//   op 14 DETERM  u32 src slot, u32 dst slot, u8 viewer, u64 seed -> status block of dst: dst becomes a
//                 fog-safe sample of src as `viewer` (0: src's current player) sees it (determinize.hpp)
//
//   spec          i32 tribe0, i32 bot0, i32 tribe1, i32 bot1, i32 mode, i32 mapPreset, i32 mapSize, u64 seed
//   status block  u8 current player, u8 ended, u8 winner, u32 turn, u16 n, n x (i16 type, a, b, arg)
//   observation   u16 width, u16 height, i16 tiles[w*h*T_COUNT], i32 globals[G_COUNT], u8 techs[64]

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <stdexcept>

#include "determinize.hpp"
#include "env.hpp"

namespace wire {

struct Reader {
    const std::string& s;
    size_t p = 0;
    template <class T>
    T get() {
        if (p + sizeof(T) > s.size()) throw std::runtime_error("request too short");
        T v;
        std::memcpy(&v, s.data() + p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    std::string bytes(size_t n) {
        if (p + n > s.size()) throw std::runtime_error("request too short");
        std::string v = s.substr(p, n);
        p += n;
        return v;
    }
};

struct Writer {
    std::string s;
    template <class T>
    void put(T v) { s.append(reinterpret_cast<const char*>(&v), sizeof(T)); }
    void raw(const void* d, size_t n) { s.append(static_cast<const char*>(d), n); }
};

inline env::Spec readSpec(Reader& r) {
    env::Spec s;
    for (auto& seat : s.seats) {
        seat.tribe = r.get<int32_t>();
        seat.bot = r.get<int32_t>();
    }
    s.mode = r.get<int32_t>();
    s.mapPreset = r.get<int32_t>();
    s.mapSize = r.get<int32_t>();
    s.seed = r.get<uint64_t>();
    return s;
}

inline void status(Writer& w, env::Match& m) {
    w.put<uint8_t>(m.currentPlayer());
    w.put<uint8_t>(m.ended());
    w.put<uint8_t>(m.winner());
    w.put<uint32_t>(m.turn());
    w.put<uint16_t>(uint16_t(m.legal.size()));
    for (const env::Cmd& c : m.legal) w.raw(&c, sizeof c);
}

inline bool readAll(int fd, void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    while (n) {
        ssize_t k = read(fd, p, n);
        if (k <= 0) return false;
        p += k;
        n -= size_t(k);
    }
    return true;
}

inline bool writeAll(int fd, const std::string& s) {
    for (size_t o = 0; o < s.size();) {
        ssize_t k = write(fd, s.data() + o, s.size() - o);
        if (k <= 0) return false;
        o += size_t(k);
    }
    return true;
}

struct Server {
    Game g;
    env::Api api{g};
    env::Determinizer determinizer{api};
    std::map<uint32_t, std::unique_ptr<env::Match>> slots;

    env::Match& slot(uint32_t id) {
        auto it = slots.find(id);
        if (it == slots.end()) throw std::runtime_error("no match in slot " + std::to_string(id));
        return *it->second;
    }

    env::Match& fresh(uint32_t id) {
        auto& m = slots[id];
        m = std::make_unique<env::Match>(api);
        return *m;
    }

    std::string handle(const std::string& req) {
        Reader r{req};
        Writer w;
        w.put<uint8_t>(0);
        uint8_t op = r.get<uint8_t>();
        if (op == 1) {
            uint32_t id = r.get<uint32_t>();
            env::Spec spec = readSpec(r);
            env::Match& m = fresh(id);
            m.create(spec);
            status(w, m);
        } else if (op == 2) {
            uint32_t n = r.get<uint32_t>();
            for (uint32_t k = 0; k < n; ++k) {
                env::Match& m = slot(r.get<uint32_t>());
                int32_t action = r.get<int32_t>();
                w.put<uint8_t>(m.step(action));
                status(w, m);
            }
        } else if (op == 3) {
            uint32_t n = r.get<uint32_t>();
            std::vector<int16_t> tiles;
            int32_t globals[env::G_COUNT];
            uint8_t techs[env::kTechSlots];
            for (uint32_t k = 0; k < n; ++k) {
                env::Match& m = slot(r.get<uint32_t>());
                uint8_t viewer = r.get<uint8_t>();
                if (!viewer) viewer = m.currentPlayer();
                tiles.resize(size_t(m.width) * m.height * env::T_COUNT);
                m.observe(viewer, tiles.data(), globals, techs);
                w.put<uint16_t>(uint16_t(m.width));
                w.put<uint16_t>(uint16_t(m.height));
                w.raw(tiles.data(), tiles.size() * sizeof(int16_t));
                w.raw(globals, sizeof globals);
                w.raw(techs, sizeof techs);
            }
        } else if (op == 4) {
            uint32_t n = r.get<uint32_t>();
            for (uint32_t k = 0; k < n; ++k) w.put<int32_t>(slot(r.get<uint32_t>()).botChoice());
        } else if (op == 5) {
            std::string bytes = slot(r.get<uint32_t>()).save();
            w.put<uint32_t>(uint32_t(bytes.size()));
            w.raw(bytes.data(), bytes.size());
        } else if (op == 6) {
            uint32_t id = r.get<uint32_t>();
            env::Spec spec = readSpec(r);
            std::string bytes = r.bytes(r.get<uint32_t>());
            env::Match& m = fresh(id);
            m.load(spec, bytes);
            status(w, m);
        } else if (op == 7) {
            Dumper d;
            d.object(slot(r.get<uint32_t>()).state(), 0);
            w.raw(d.j.s.data(), d.j.s.size());
        } else if (op == 8) {
            Class* versions = cls("GameLogicAssembly", "", "VersionManager");
            w.put<int32_t>(unbox<int>(call(method(versions, "get_GameVersion", 0), nullptr)));
            w.put<int32_t>(unbox<int>(call(method(versions, "get_GameLogicDataVersion", 0), nullptr)));
            w.put<int32_t>(env::kObsVersion);
            w.put<int32_t>(env::T_COUNT);
            w.put<int32_t>(env::G_COUNT);
            w.put<int32_t>(env::kTechSlots);
            w.put<int32_t>(env::kLegalVersion);
        } else if (op == 9) {
            slots.erase(r.get<uint32_t>());
        } else if (op == 10) {
            env::Match& m = slot(r.get<uint32_t>());
            std::vector<uint8_t> owner(size_t(m.width) * m.height), capital(owner.size());
            for (Object* t : m.tiles()) {
                int16_t i = m.index(env::at<env::Coord>(t, api.tCoords));
                owner[i] = env::at<uint8_t>(t, api.tOwner);
                capital[i] = env::at<uint8_t>(t, api.tCapitalOf);
            }
            w.put<uint16_t>(uint16_t(m.width));
            w.put<uint16_t>(uint16_t(m.height));
            w.raw(owner.data(), owner.size());
            w.raw(capital.data(), capital.size());
        } else if (op == 11) {
            uint32_t id = r.get<uint32_t>();
            env::Spec spec = readSpec(r);
            std::string start = r.bytes(r.get<uint32_t>());
            std::string final = r.bytes(r.get<uint32_t>());
            env::Match& m = fresh(id);
            m.loadReplay(spec, start, final);
            w.put<uint32_t>(uint32_t(m.replayCount - m.replayAt));
            status(w, m);
        } else if (op == 13) {
            uint32_t id = r.get<uint32_t>();
            env::Spec spec = readSpec(r);
            std::string bytes = r.bytes(r.get<uint32_t>());
            env::Match& m = fresh(id);
            m.loadAsCurrent(spec, bytes);
            status(w, m);
        } else if (op == 12) {
            env::Match& m = slot(r.get<uint32_t>());
            uint8_t player = 0;
            int32_t label = 0;
            env::Cmd recorded;
            bool ok = m.replayStep(&player, &label, &recorded);
            w.put<uint8_t>(player);
            w.put<int32_t>(label);
            w.raw(&recorded, sizeof recorded);
            w.put<uint8_t>(ok);
            w.put<uint32_t>(uint32_t(m.replayCount - m.replayAt));
            status(w, m);
        } else if (op == 14) {
            env::Match& src = slot(r.get<uint32_t>());
            uint32_t id = r.get<uint32_t>();
            uint8_t viewer = r.get<uint8_t>();
            uint64_t seed = r.get<uint64_t>();
            if (!viewer) viewer = src.currentPlayer();
            auto m = std::make_unique<env::Match>(api);
            determinizer.build(src, *m, viewer, seed);
            env::Match& dst = *(slots[id] = std::move(m));  // (src may be the same slot: replaced only now)
            status(w, dst);
        } else {
            throw std::runtime_error("unknown op " + std::to_string(op));
        }
        return w.s;
    }

    void run(int port) {
        int srv = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(srv, (sockaddr*)&addr, sizeof addr) || listen(srv, 4)) die("cannot listen on port " + std::to_string(port));
        std::printf("env listening on 127.0.0.1:%d\n", port);
        std::fflush(stdout);
        while (true) {
            int fd = accept(srv, nullptr, nullptr);
            if (fd < 0) continue;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            while (true) {
                uint32_t len = 0;
                if (!readAll(fd, &len, 4)) break;
                std::string req(len, '\0');
                if (!readAll(fd, req.data(), len)) break;
                std::string resp;
                try {
                    resp = handle(req);
                } catch (const std::exception& e) {
                    resp = std::string(1, '\1') + e.what();
                }
                uint32_t n = uint32_t(resp.size());
                if (!writeAll(fd, std::string(reinterpret_cast<char*>(&n), 4) + resp)) break;
            }
            close(fd);
        }
    }
};

}  // namespace wire
