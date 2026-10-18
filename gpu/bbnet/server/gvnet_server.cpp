// SPDX-License-Identifier: GPL-2.0-or-later
// gvnet-server: a small shadNet server for playing two (or more) instances of the game against or
// with each other on one computer or a LAN: the online modes' matchmaking (NpMatching2 rooms) and
// the players' addresses for their peer-to-peer connection (the UDP "STUN" port). It speaks the
// protocol of the online module's shadNet client (shadps4/shadnet/client.cpp, shadnet.proto).
//
// What a public server does and this one does not: accounts (any name signs in, no password),
// friends lists kept anywhere (everyone signed in is everyone's friend), scores (every score
// request answers "not found"), the WebAPI. Rooms, members and their data live in memory.
//
// The room logic follows Wozzardman/shadnet-p2p's cmd_matching.cpp (GPL-2.0-or-later): member ids
// (slot << 4 | counter), room events (0x1101 joined, 0x1102 left, 0x1103 kicked out, 0x1106 room
// data), owner succession, slot counts and the search filters.
//
//   gvnet-server [--port 31313] [--bind 127.0.0.1]   (UDP on port + 1)

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "shadnet.pb.h"

namespace {

constexpr uint32_t kProtocolVersion = 1;
constexpr size_t kHeaderSize = 15;
constexpr uint32_t kMaxPacket = 0x800000;

enum PacketType : uint8_t { Request = 0, Reply = 1, Notification = 2, ServerInfo = 3 };
enum Command : uint16_t {
    Login = 0, Terminate = 1, Create = 2, Delete = 3, AddFriend = 8, RemoveFriend = 9,
    AddBlock = 10, RemoveBlock = 11, GetServerFeatures = 12, SetClientVersion = 13,
    GetBoardInfos = 30, GetScoreGameDataByAccId = 38, GetToken = 39, SetAppearOffline = 40,
    ContextStart = 100, CreateRoom = 101, JoinRoom = 102, LeaveRoom = 103, SearchRoom = 104,
    RequestSignalingInfos = 105, ContextStop = 106, SetUserInfo = 107, SetRoomDataInternal = 108,
    SetRoomDataExternal = 109, KickoutRoomMember = 110, GetWorldInfoList = 111,
    GetRoomDataExternalList = 112, GetUserInfoList = 113, GetRoomMemberDataExternalList = 114,
    SendRoomMessage = 115,
};
enum Notify : uint16_t { FriendNew = 6, FriendStatus = 8, RoomEvent = 10, RoomMessage = 11 };
enum Error : uint8_t {
    NoError = 0, Malformed = 1, Invalid = 2, InvalidInput = 3, RoomMissing = 14,
    RoomAlreadyJoined = 15, RoomFull = 16, Unauthorized = 23, NotFound = 26, Blocked = 27,
};

constexpr uint32_t kEventMemberJoined = 0x1101, kEventMemberLeft = 0x1102,
                   kEventKickedOut = 0x1103, kEventRoomDataInternal = 0x1106;
constexpr uint32_t kCauseLeave = 1, kCauseKickout = 2, kCauseServer = 4;
constexpr uint32_t kRoomFlagFull = 0x20000000, kRoomFlagHidden = 0x10000000,
                   kRoomFlagNatRestriction = 0x04000000, kMemberFlagOwner = 0x80000000;
constexpr uint16_t kIntAttr1 = 0x4C, kIntAttr8 = 0x53, kSearchBinAttr = 0x54, kExtBinAttr1 = 0x55,
                   kExtBinAttr2 = 0x56, kIntBinAttr1 = 0x57, kIntBinAttr2 = 0x58;
constexpr int32_t kKickedOutStatus = static_cast<int32_t>(0xFF000019u);

bool g_verbose = false;
#define LOG(...) do { std::printf("gvnet: " __VA_ARGS__); std::printf("\n"); std::fflush(stdout); } while (0)
#define VLOG(...) do { if (g_verbose) LOG(__VA_ARGS__); } while (0)

uint64_t NowUsec() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

struct BinSlot {
    bool set = false;
    uint16_t attr_id = 0;
    std::string data;
    uint64_t update_date = 0;
    uint16_t update_member = 0;
};

struct Member {
    std::string npid;
    uint64_t user_id = 0;
    uint16_t member_id = 0;
    uint8_t team_id = 0, group_id = 0;
    uint32_t flag_attr = 0;
    uint64_t join_date = 0;
    std::string addr;
    uint16_t port = 0;
    BinSlot bin;
};

struct Group {
    uint8_t group_id = 0;
    uint32_t slots = 0, members = 0;
    bool password = false;
    std::optional<std::string> label;
};

struct Room {
    uint64_t room_id = 0;
    uint16_t max_slot = 1;
    uint32_t server_id = 1, world_id = 1, lobby_id = 0, flags = 0;
    uint64_t passwd_mask = 0;
    uint32_t public_slots = 0, private_slots = 0, open_public = 0, open_private = 0;
    uint16_t owner = 0, counter = 0;
    std::map<uint16_t, Member> members;
    std::vector<Group> groups;
    BinSlot search_int[8]; // value in data as 4 bytes
    uint32_t search_int_value[8]{};
    BinSlot search_bin, ext_bin[2], int_bin[2];
    std::vector<std::string> allowed, blocked;

    uint16_t AllocMemberId() {
        uint16_t slot = 1;
        for (; slot <= 64; ++slot) {
            bool taken = false;
            for (const auto& [id, m] : members) taken = taken || (id >> 4) == slot;
            if (!taken) break;
        }
        counter = static_cast<uint16_t>((counter + 1) & 0xF);
        return static_cast<uint16_t>((slot << 4) | counter);
    }
    uint64_t JoinedMask() const {
        uint64_t mask = 0;
        for (const auto& [id, m] : members) {
            const uint16_t slot = id >> 4;
            if (slot > 0 && slot <= 64) mask |= 1ull << (slot - 1);
        }
        return mask;
    }
    Member* ByNpid(const std::string& npid) {
        for (auto& [id, m] : members) if (m.npid == npid) return &m;
        return nullptr;
    }
    void Recount() {
        const uint64_t valid = max_slot >= 64 ? ~0ull : (1ull << max_slot) - 1;
        uint64_t priv = passwd_mask & valid;
        if (!priv) {
            uint16_t base = 0;
            for (const auto& g : groups) {
                const uint32_t n = g.slots ? g.slots : max_slot;
                for (uint32_t i = 0; i < n && base < max_slot; ++i, ++base)
                    if (g.password) priv |= 1ull << base;
            }
            priv &= valid;
        }
        const uint64_t pub = valid & ~priv, joined = JoinedMask() & valid;
        public_slots = __builtin_popcountll(pub);
        private_slots = __builtin_popcountll(priv);
        open_public = __builtin_popcountll(pub & ~joined);
        open_private = __builtin_popcountll(priv & ~joined);
    }
};

struct Client {
    int fd = -1;
    std::string in;
    bool authed = false;
    std::string npid, title;
    uint64_t user_id = 0;
    uint32_t ctx_id = 0;
    uint64_t room_id = 0;
    std::string peer_ip;
};

std::map<int, Client> g_clients;                           // by socket
std::map<std::pair<std::string, uint64_t>, Room> g_rooms;  // (title, room id)
std::map<std::string, std::pair<std::string, uint16_t>> g_udp; // npid -> observed UDP endpoint
std::map<std::pair<std::string, std::string>, std::vector<BinSlot>> g_user_info;
std::map<std::string, uint64_t> g_user_ids;
uint64_t g_next_room = 1, g_next_user = 1;

// ---- packets ----

void PutLE(std::string& out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<char>(v >> (8 * i)));
}
uint64_t GetLE(const char* p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
    return v;
}
std::string Packet(PacketType type, uint16_t cmd, uint64_t id, const std::string& payload) {
    std::string out;
    out.push_back(static_cast<char>(type));
    PutLE(out, cmd, 2);
    PutLE(out, kHeaderSize + payload.size(), 4);
    PutLE(out, id, 8);
    return out + payload;
}
template <typename T>
std::string Blob(const T& msg) {
    const std::string s = msg.SerializeAsString();
    std::string out;
    PutLE(out, s.size(), 4);
    return out + s;
}
void SendRaw(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return; // the reader notices the closed socket
        sent += static_cast<size_t>(n);
    }
}
template <typename T>
bool Decode(T& msg, const std::string& payload) {
    if (payload.size() < 4) return false;
    const uint32_t len = static_cast<uint32_t>(GetLE(payload.data(), 4));
    return payload.size() >= 4 + len && msg.ParseFromArray(payload.data() + 4, static_cast<int>(len));
}

Client* ClientByNpid(const std::string& npid) {
    for (auto& [fd, c] : g_clients) if (c.authed && c.npid == npid) return &c;
    return nullptr;
}

// ---- room data in the protocol's shape ----

void FillBin(shadnet::MatchingBinAttr* a, const BinSlot& s) {
    a->set_attr_id(s.attr_id);
    a->set_data(s.data);
    a->set_update_date(s.update_date);
    a->set_update_member_id(s.update_member);
}
void FillGroup(shadnet::MatchingRoomGroup* g, const Group& s) {
    g->set_group_id(s.group_id);
    g->set_has_passwd(s.password);
    g->set_has_label(s.label.has_value());
    if (s.label) g->set_label(*s.label);
    g->set_slot_count(s.slots);
    g->set_num_members(s.members);
}
shadnet::MatchingRoomMemberData MemberData(const Member& m) {
    shadnet::MatchingRoomMemberData pb;
    pb.set_npid(m.npid);
    pb.set_member_id(m.member_id);
    pb.set_team_id(m.team_id);
    pb.set_is_owner((m.flag_attr & kMemberFlagOwner) != 0);
    pb.set_join_date(m.join_date);
    pb.set_flag_attr(m.flag_attr);
    pb.set_addr(m.addr);
    pb.set_port(m.port);
    pb.set_group_id(m.group_id);
    pb.set_account_id(m.user_id);
    pb.set_platform(3);
    if (m.bin.set) FillBin(pb.add_bin_attrs_internal(), m.bin);
    return pb;
}
shadnet::MatchingRoomDataInternal RoomInternal(const Room& r) {
    shadnet::MatchingRoomDataInternal pb;
    pb.set_public_slots(r.public_slots);
    pb.set_private_slots(r.private_slots);
    pb.set_open_public_slots(r.open_public);
    pb.set_open_private_slots(r.open_private);
    pb.set_max_slot(r.max_slot);
    pb.set_server_id(r.server_id);
    pb.set_world_id(r.world_id);
    pb.set_lobby_id(r.lobby_id);
    pb.set_room_id(r.room_id);
    pb.set_passwd_slot_mask(r.passwd_mask);
    pb.set_joined_slot_mask(static_cast<uint32_t>(r.JoinedMask()));
    for (const auto& g : r.groups) FillGroup(pb.add_groups(), g);
    pb.set_flags(r.flags);
    for (const auto& s : r.int_bin) if (s.set) FillBin(pb.add_bin_attrs_internal(), s);
    return pb;
}
shadnet::CreateJoinRoomResponse CreateJoin(const Room& r, uint16_t me) {
    shadnet::CreateJoinRoomResponse pb;
    *pb.mutable_room_data() = RoomInternal(r);
    for (const auto& [id, m] : r.members) *pb.add_members() = MemberData(m);
    pb.set_me_member_id(me);
    pb.set_owner_member_id(r.owner);
    return pb;
}
bool Wanted(const google::protobuf::RepeatedField<uint32_t>& ids, uint16_t attr) {
    return ids.empty() || std::find(ids.begin(), ids.end(), attr) != ids.end();
}
shadnet::MatchingRoomDataExternal RoomExternal(const Room& r,
                                               const google::protobuf::RepeatedField<uint32_t>& ids) {
    shadnet::MatchingRoomDataExternal pb;
    pb.set_max_slot(r.max_slot);
    pb.set_cur_members(static_cast<uint32_t>(r.members.size()));
    pb.set_flags(r.flags);
    pb.set_server_id(r.server_id);
    pb.set_world_id(r.world_id);
    pb.set_lobby_id(r.lobby_id);
    pb.set_room_id(r.room_id);
    pb.set_passwd_slot_mask(r.passwd_mask);
    pb.set_joined_slot_mask(r.JoinedMask());
    pb.set_public_slots(r.public_slots);
    pb.set_private_slots(r.private_slots);
    pb.set_open_public_slots(r.open_public);
    pb.set_open_private_slots(r.open_private);
    for (const auto& g : r.groups) FillGroup(pb.add_groups(), g);
    for (int i = 0; i < 8; ++i) {
        if (!r.search_int[i].set || !Wanted(ids, r.search_int[i].attr_id)) continue;
        auto* a = pb.add_external_search_int_attrs();
        a->set_attr_id(r.search_int[i].attr_id);
        a->set_attr_value(r.search_int_value[i]);
    }
    if (r.search_bin.set && Wanted(ids, r.search_bin.attr_id))
        FillBin(pb.add_external_search_bin_attrs(), r.search_bin);
    for (const auto& s : r.ext_bin)
        if (s.set && Wanted(ids, s.attr_id)) FillBin(pb.add_external_bin_attrs(), s);
    const auto owner = r.members.find(r.owner);
    if (owner != r.members.end()) {
        pb.set_owner_npid(owner->second.npid);
        pb.set_owner_account_id(owner->second.user_id);
        pb.set_owner_platform(3);
    }
    return pb;
}

// Room events to every member but one (their own matching context in each).
void NotifyMembers(const Room& r, shadnet::NotifyRoomEvent ev, const std::string& except = {}) {
    for (const auto& [id, m] : r.members) {
        if (m.npid == except) continue;
        Client* c = ClientByNpid(m.npid);
        if (!c || !c->ctx_id) continue;
        ev.set_ctx_id(c->ctx_id);
        SendRaw(c->fd, Packet(Notification, RoomEvent, 0, Blob(ev)));
    }
}
void MemberEvent(const Room& r, uint32_t event, uint32_t cause, const Member& m,
                 const std::string& except = {}) {
    shadnet::NotifyRoomEvent ev;
    ev.set_room_id(r.room_id);
    ev.set_event(event);
    ev.set_event_cause(cause);
    *ev.mutable_member() = MemberData(m);
    NotifyMembers(r, ev, except);
}

void SelfAddr(const Client& c, std::string& addr, uint16_t& port) {
    const auto it = g_udp.find(c.npid);
    if (it != g_udp.end()) { addr = it->second.first; port = it->second.second; }
    else { addr = c.peer_ip; port = 0; }
}

void ApplyBin(Room& r, const shadnet::MatchingBinAttr& a, uint16_t member, uint64_t date) {
    const uint16_t id = static_cast<uint16_t>(a.attr_id());
    BinSlot* s = id == kIntBinAttr1 ? &r.int_bin[0] : id == kIntBinAttr2 ? &r.int_bin[1]
               : id == kExtBinAttr1 ? &r.ext_bin[0] : id == kExtBinAttr2 ? &r.ext_bin[1]
               : id == kSearchBinAttr ? &r.search_bin : nullptr;
    if (!s) return;
    s->set = true; s->attr_id = id; s->data = a.data(); s->update_date = date; s->update_member = member;
}
void ApplyInt(Room& r, const shadnet::MatchingIntAttr& a) {
    const uint16_t id = static_cast<uint16_t>(a.attr_id());
    if (id < kIntAttr1 || id > kIntAttr8) return;
    r.search_int[id - kIntAttr1].set = true;
    r.search_int[id - kIntAttr1].attr_id = id;
    r.search_int_value[id - kIntAttr1] = a.attr_value();
}

// Leaves a room (request or disconnect): the owner passes to the next member, an empty room goes.
void DoLeave(Client& c, uint64_t room_id, uint32_t cause) {
    auto it = g_rooms.find({c.title, room_id});
    if (it == g_rooms.end()) return;
    Room& r = it->second;
    Member* me = r.ByNpid(c.npid);
    if (!me) return;
    Member left = *me;
    const bool owner = left.member_id == r.owner;
    r.members.erase(left.member_id);
    if (c.room_id == room_id) c.room_id = 0;
    if (r.members.empty()) {
        LOG("room %llu closed", static_cast<unsigned long long>(room_id));
        g_rooms.erase(it);
        return;
    }
    r.Recount();
    r.flags &= ~kRoomFlagFull;
    if (owner) {
        r.owner = r.members.begin()->first;
        r.members.begin()->second.flag_attr |= kMemberFlagOwner;
    }
    LOG("%s left room %llu", c.npid.c_str(), static_cast<unsigned long long>(room_id));
    MemberEvent(r, kEventMemberLeft, cause, left);
}

// ---- commands ----

Error CmdLogin(Client& c, const std::string& in, std::string& out) {
    shadnet::LoginRequest req;
    if (!Decode(req, in) || req.npid().empty()) return Malformed;
    if (Client* old = ClientByNpid(req.npid()); old && old->fd != c.fd) {
        LOG("%s signs in again: the earlier connection is closed", req.npid().c_str());
        if (old->room_id) DoLeave(*old, old->room_id, kCauseLeave);
        old->authed = false;
        ::shutdown(old->fd, SHUT_RDWR);
    }
    c.npid = req.npid();
    c.title = req.title_id();
    auto id = g_user_ids.find(c.npid);
    if (id == g_user_ids.end()) id = g_user_ids.emplace(c.npid, 0x1000 + g_next_user++).first;
    c.user_id = id->second;
    c.authed = true;
    shadnet::LoginReply rep;
    rep.set_user_id(c.user_id);
    for (auto& [fd, other] : g_clients) {
        if (!other.authed || other.fd == c.fd) continue;
        auto* f = rep.add_friends();
        f->set_npid(other.npid);
        f->set_online(true);
        shadnet::NotifyFriendNew n;
        n.set_npid(c.npid);
        n.set_online(true);
        SendRaw(other.fd, Packet(Notification, FriendNew, 0, Blob(n)));
    }
    LOG("%s signed in (%s, user %llu)", c.npid.c_str(), c.title.c_str(),
        static_cast<unsigned long long>(c.user_id));
    out = Blob(rep);
    return NoError;
}

Error CmdCreateRoom(Client& c, const std::string& in, std::string& out) {
    shadnet::CreateRoomRequest req;
    if (!Decode(req, in)) return Malformed;
    if (c.room_id) DoLeave(c, c.room_id, kCauseLeave);
    Room r;
    r.room_id = g_next_room++;
    r.max_slot = static_cast<uint16_t>(std::max<uint32_t>(1, req.max_slots()));
    r.world_id = req.world_id() ? req.world_id() : 1;
    r.lobby_id = req.lobby_id();
    r.flags = req.flags() & ~kRoomFlagFull;
    for (const auto& s : req.allowed_online_ids()) r.allowed.push_back(s);
    for (const auto& s : req.blocked_online_ids()) r.blocked.push_back(s);
    if (req.has_passwd_slot_mask()) r.passwd_mask = req.passwd_slot_mask();
    const uint32_t groups = req.group_configs_size() ? req.group_configs_size() : req.group_config_count();
    for (uint32_t g = 0; g < groups; ++g) {
        Group grp;
        grp.group_id = static_cast<uint8_t>(g + 1);
        if (g < static_cast<uint32_t>(req.group_configs_size())) {
            const auto& cfg = req.group_configs(static_cast<int>(g));
            grp.slots = cfg.slot_count();
            grp.password = cfg.has_passwd();
            if (cfg.has_label()) grp.label = cfg.label();
        } else {
            grp.password = req.room_password_present();
            if (!req.join_group_label().empty()) grp.label = req.join_group_label();
        }
        r.groups.push_back(grp);
    }
    for (const auto& a : req.external_search_int_attrs()) ApplyInt(r, a);
    Member owner;
    owner.npid = c.npid;
    owner.user_id = c.user_id;
    SelfAddr(c, owner.addr, owner.port);
    owner.join_date = NowUsec();
    owner.team_id = static_cast<uint8_t>(req.team_id());
    owner.flag_attr = kMemberFlagOwner;
    if (!r.groups.empty()) {
        Group* pick = &r.groups.front();
        for (auto& g : r.groups) if (g.label && *g.label == req.join_group_label() && !req.join_group_label().empty()) pick = &g;
        owner.group_id = pick->group_id;
        pick->members++;
    }
    if (req.member_bin_attrs_size()) {
        owner.bin.set = true;
        owner.bin.attr_id = static_cast<uint16_t>(req.member_bin_attrs(0).attr_id());
        owner.bin.data = req.member_bin_attrs(0).data();
        owner.bin.update_date = owner.join_date;
    }
    owner.member_id = r.AllocMemberId();
    r.owner = owner.member_id;
    r.members[owner.member_id] = owner;
    for (const auto& a : req.internal_bin_attrs()) ApplyBin(r, a, owner.member_id, owner.join_date);
    for (const auto& a : req.external_search_bin_attrs()) ApplyBin(r, a, owner.member_id, owner.join_date);
    for (const auto& a : req.external_bin_attrs()) ApplyBin(r, a, owner.member_id, owner.join_date);
    r.Recount();
    c.room_id = r.room_id;
    shadnet::CreateRoomReply rep;
    rep.set_room_id(r.room_id);
    rep.set_server_id(r.server_id);
    rep.set_world_id(r.world_id);
    rep.set_lobby_id(r.lobby_id);
    rep.set_member_id(owner.member_id);
    rep.set_max_slots(r.max_slot);
    rep.set_flags(r.flags);
    rep.set_cur_members(1);
    *rep.mutable_details() = CreateJoin(r, owner.member_id);
    out = Blob(rep);
    LOG("%s created room %llu (%u slots, world %u, flags %#x, %s:%u)", c.npid.c_str(),
        static_cast<unsigned long long>(r.room_id), r.max_slot, r.world_id, r.flags,
        owner.addr.c_str(), owner.port);
    g_rooms[{c.title, r.room_id}] = std::move(r);
    return NoError;
}

Error CmdJoinRoom(Client& c, const std::string& in, std::string& out) {
    shadnet::JoinRoomRequest req;
    if (!Decode(req, in)) return Malformed;
    auto it = g_rooms.find({c.title, req.room_id()});
    if (it == g_rooms.end()) return RoomMissing;
    Room& r = it->second;
    if (r.members.size() >= r.max_slot) return RoomFull;
    if (r.ByNpid(c.npid)) return RoomAlreadyJoined;
    if ((!r.allowed.empty() && std::find(r.allowed.begin(), r.allowed.end(), c.npid) == r.allowed.end()) ||
        std::find(r.blocked.begin(), r.blocked.end(), c.npid) != r.blocked.end())
        return Blocked;
    if (c.room_id && c.room_id != r.room_id) DoLeave(c, c.room_id, kCauseLeave);
    Member m;
    m.npid = c.npid;
    m.user_id = c.user_id;
    SelfAddr(c, m.addr, m.port);
    m.join_date = NowUsec();
    m.team_id = static_cast<uint8_t>(req.team_id());
    if (req.member_bin_attrs_size()) {
        m.bin.set = true;
        m.bin.attr_id = static_cast<uint16_t>(req.member_bin_attrs(0).attr_id());
        m.bin.data = req.member_bin_attrs(0).data();
        m.bin.update_date = m.join_date;
    }
    m.member_id = r.AllocMemberId();
    r.members[m.member_id] = m;
    r.Recount();
    if (r.members.size() >= r.max_slot) r.flags |= kRoomFlagFull;
    c.room_id = r.room_id;
    shadnet::JoinRoomReply rep;
    rep.set_room_id(r.room_id);
    rep.set_member_id(m.member_id);
    rep.set_max_slots(r.max_slot);
    rep.set_flags(r.flags);
    rep.set_cur_members(static_cast<uint32_t>(r.members.size()));
    *rep.mutable_details() = CreateJoin(r, m.member_id);
    out = Blob(rep);
    LOG("%s joined room %llu as member %#x (%s:%u)", c.npid.c_str(),
        static_cast<unsigned long long>(r.room_id), m.member_id, m.addr.c_str(), m.port);
    MemberEvent(r, kEventMemberJoined, kCauseServer, m, c.npid);
    return NoError;
}

Error CmdLeaveRoom(Client& c, const std::string& in, std::string& out) {
    shadnet::LeaveRoomRequest req;
    if (!Decode(req, in)) return Malformed;
    auto it = g_rooms.find({c.title, req.room_id()});
    if (it == g_rooms.end() || !it->second.ByNpid(c.npid)) return RoomMissing;
    DoLeave(c, req.room_id(), kCauseLeave);
    shadnet::LeaveRoomReply rep;
    rep.set_room_id(req.room_id());
    out = Blob(rep);
    return NoError;
}

Error CmdKickout(Client& c, const std::string& in, std::string& out) {
    shadnet::KickoutRoomMemberRequest req;
    if (!Decode(req, in)) return Malformed;
    auto it = g_rooms.find({c.title, req.room_id()});
    if (it == g_rooms.end()) return RoomMissing;
    Room& r = it->second;
    Member* me = r.ByNpid(c.npid);
    if (!me) return RoomMissing;
    if (me->member_id != r.owner) return Unauthorized;
    const auto target = r.members.find(static_cast<uint16_t>(req.target_member_id()));
    if (target == r.members.end() || target->first == me->member_id) return NotFound;
    Member gone = target->second;
    if (req.block_kick_flag()) r.blocked.push_back(gone.npid);
    r.members.erase(target);
    r.Recount();
    r.flags &= ~kRoomFlagFull;
    if (Client* t = ClientByNpid(gone.npid)) {
        if (t->room_id == r.room_id) t->room_id = 0;
        shadnet::NotifyRoomEvent ev;
        ev.set_ctx_id(t->ctx_id);
        ev.set_room_id(r.room_id);
        ev.set_event(kEventKickedOut);
        ev.set_event_cause(kCauseKickout);
        ev.set_error_code(kKickedOutStatus);
        SendRaw(t->fd, Packet(Notification, RoomEvent, 0, Blob(ev)));
    }
    MemberEvent(r, kEventMemberLeft, kCauseKickout, gone, gone.npid);
    shadnet::KickoutRoomMemberReply rep;
    rep.set_room_id(r.room_id);
    out = Blob(rep);
    return NoError;
}

bool IntFilter(uint32_t op, uint32_t room, uint32_t value) {
    switch (op) {
    case 1: return room == value;
    case 2: return room != value;
    case 3: return room < value;
    case 4: return room <= value;
    case 5: return room > value;
    case 6: return room >= value;
    default: return false;
    }
}
bool Matches(const Room& r, const shadnet::SearchRoomRequest& req) {
    if (r.flags & kRoomFlagHidden) return false;
    const uint32_t filter = req.flag_filter() & ~kRoomFlagNatRestriction;
    if ((r.flags & filter) != (req.flag_attr() & ~kRoomFlagNatRestriction)) return false;
    for (const auto& f : req.int_filters()) {
        const uint16_t id = static_cast<uint16_t>(f.attr_id());
        if (!id) continue;
        if (id < kIntAttr1 || id > kIntAttr8) return false;
        if (!IntFilter(f.op(), r.search_int_value[id - kIntAttr1], f.attr_value())) return false;
    }
    for (const auto& f : req.bin_filters()) {
        if (!f.attr_id()) continue;
        if (f.attr_id() != kSearchBinAttr) return false;
        const bool eq = r.search_bin.data == f.data();
        if ((f.op() == 1 && !eq) || (f.op() == 2 && eq)) return false;
    }
    return true;
}

Error CmdSearchRoom(Client& c, const std::string& in, std::string& out) {
    shadnet::SearchRoomRequest req;
    if (!Decode(req, in)) return Malformed;
    const uint32_t start = std::max<uint32_t>(1, req.range_filter_start());
    uint32_t max = req.range_filter_max();
    if (!max || max > 20) max = 20;
    std::vector<const Room*> found;
    for (const auto& [key, r] : g_rooms) {
        if (key.first != c.title) continue;
        if (req.world_id() && r.world_id != req.world_id()) continue;
        if (!req.world_id() && req.lobby_id() && r.lobby_id != req.lobby_id()) continue;
        if (Matches(r, req)) found.push_back(&r);
    }
    shadnet::SearchRoomReply rep;
    for (uint32_t i = start - 1, n = 0; i < found.size() && n < max; ++i, ++n)
        *rep.add_rooms() = RoomExternal(*found[i], req.attr_ids());
    rep.set_range_start(start);
    rep.set_range_total(static_cast<uint32_t>(found.size()));
    rep.set_range_result(static_cast<uint32_t>(rep.rooms_size()));
    out = Blob(rep);
    LOG("%s searched world %u: %zu room(s)", c.npid.c_str(), req.world_id(), found.size());
    return NoError;
}

Error CmdRoomExternalList(Client& c, const std::string& in, std::string& out) {
    shadnet::GetRoomDataExternalListRequest req;
    if (!Decode(req, in)) return Malformed;
    shadnet::GetRoomDataExternalListReply rep;
    for (const auto id : req.room_ids()) {
        const auto it = g_rooms.find({c.title, id});
        if (it != g_rooms.end()) *rep.add_rooms() = RoomExternal(it->second, req.attr_ids());
    }
    out = Blob(rep);
    return NoError;
}

Error CmdMemberExternalList(Client& c, const std::string& in, std::string& out) {
    shadnet::GetRoomMemberDataExternalListRequest req;
    if (!Decode(req, in)) return Malformed;
    const auto it = g_rooms.find({c.title, req.room_id()});
    if (it == g_rooms.end()) return NotFound;
    shadnet::GetRoomMemberDataExternalListReply rep;
    for (const auto& [id, m] : it->second.members) {
        auto* d = rep.add_members();
        d->set_npid(m.npid);
        d->set_member_id(id);
        d->set_account_id(m.user_id);
        d->set_platform(3);
        d->set_join_date(m.join_date);
        d->set_role(id == it->second.owner ? 2 : 1);
    }
    out = Blob(rep);
    return NoError;
}

Error CmdSetUserInfo(Client& c, const std::string& in, std::string& out) {
    shadnet::SetUserInfoRequest req;
    if (!Decode(req, in)) return Malformed;
    std::vector<BinSlot> attrs;
    for (const auto& a : req.user_bin_attrs()) {
        BinSlot s;
        s.set = true; s.attr_id = static_cast<uint16_t>(a.attr_id()); s.data = a.data();
        attrs.push_back(std::move(s));
    }
    g_user_info[{c.title, c.npid}] = std::move(attrs);
    out = Blob(shadnet::SetUserInfoReply{});
    return NoError;
}

Error CmdGetUserInfoList(Client& c, const std::string& in, std::string& out) {
    shadnet::GetUserInfoListRequest req;
    if (!Decode(req, in)) return Malformed;
    shadnet::GetUserInfoListReply rep;
    for (const auto& npid : req.npids()) {
        auto* u = rep.add_users();
        u->set_npid(npid);
        if (const auto id = g_user_ids.find(npid); id != g_user_ids.end()) {
            u->set_account_id(id->second);
            u->set_platform(3);
        }
        const auto info = g_user_info.find({c.title, npid});
        if (info == g_user_info.end()) continue;
        for (const auto& s : info->second) {
            if (!Wanted(req.attr_ids(), s.attr_id)) continue;
            auto* a = u->add_user_bin_attrs();
            a->set_attr_id(s.attr_id);
            a->set_data(s.data);
        }
    }
    out = Blob(rep);
    return NoError;
}

Error CmdSignalingInfos(Client& c, const std::string& in, std::string& out) {
    shadnet::RequestSignalingInfosRequest req;
    if (!Decode(req, in)) return Malformed;
    std::string ip;
    uint16_t port = 0, member = 0;
    if (const auto it = g_udp.find(req.target_npid()); it != g_udp.end()) {
        ip = it->second.first;
        port = it->second.second;
    }
    for (auto& [key, r] : g_rooms) {
        if (key.first != c.title) continue;
        if (const Member* m = r.ByNpid(req.target_npid())) {
            member = m->member_id;
            if (ip.empty()) { ip = m->addr; port = m->port; }
            break;
        }
    }
    if (ip.empty()) return NotFound;
    shadnet::RequestSignalingInfosReply rep;
    rep.set_target_npid(req.target_npid());
    rep.set_target_ip(ip);
    rep.set_target_port(port);
    rep.set_target_member_id(member);
    out = Blob(rep);
    VLOG("%s asked for %s's address: %s:%u", c.npid.c_str(), req.target_npid().c_str(), ip.c_str(), port);
    return NoError;
}

Error CmdSendRoomMessage(Client& c, const std::string& in, std::string& out) {
    shadnet::SendRoomMessageRequest req;
    if (!Decode(req, in)) return Malformed;
    const auto it = g_rooms.find({c.title, req.room_id()});
    if (it == g_rooms.end()) return RoomMissing;
    Room& r = it->second;
    const Member* src = r.ByNpid(c.npid);
    if (!src) return RoomMissing;
    std::vector<const Member*> targets;
    const auto add = [&](uint32_t id) {
        const auto m = r.members.find(static_cast<uint16_t>(id));
        if (m != r.members.end() && m->second.npid != c.npid) targets.push_back(&m->second);
    };
    if (req.cast_type() == 1) for (const auto& [id, m] : r.members) add(id);
    else if (req.cast_type() == 2 && req.dst_member_ids_size()) add(req.dst_member_ids(0));
    else if (req.cast_type() == 3) for (const auto id : req.dst_member_ids()) add(id);
    else return InvalidInput;
    shadnet::NotifyRoomMessage n;
    n.set_room_id(r.room_id);
    n.set_src_member_id(src->member_id);
    n.set_event(0x2102);
    n.set_cast_type(req.cast_type());
    n.set_src_npid(src->npid);
    n.set_src_account_id(src->user_id);
    n.set_src_platform(3);
    n.set_msg(req.msg());
    for (const Member* t : targets) n.add_dst_member_ids(t->member_id);
    for (const Member* t : targets) {
        Client* tc = ClientByNpid(t->npid);
        if (!tc || !tc->ctx_id) continue;
        n.set_ctx_id(tc->ctx_id);
        SendRaw(tc->fd, Packet(Notification, RoomMessage, 0, Blob(n)));
    }
    shadnet::SendRoomMessageReply rep;
    rep.set_room_id(r.room_id);
    out = Blob(rep);
    return NoError;
}

Error CmdSetRoomDataInternal(Client& c, const std::string& in, std::string& out) {
    shadnet::SetRoomDataInternalRequest req;
    if (!Decode(req, in)) return Malformed;
    const auto it = g_rooms.find({c.title, req.room_id()});
    if (it == g_rooms.end()) return RoomMissing;
    Room& r = it->second;
    const uint32_t filter = req.flag_filter() & ~kRoomFlagFull;
    r.flags = (r.flags & ~filter) | (req.flag_attr() & filter);
    if (req.has_passwd_mask()) { r.passwd_mask = req.passwd_slot_mask(); r.Recount(); }
    const Member* setter = r.ByNpid(c.npid);
    const uint64_t now = NowUsec();
    shadnet::NotifyRoomEvent ev;
    ev.set_room_id(r.room_id);
    ev.set_event(kEventRoomDataInternal);
    ev.set_event_cause(kCauseServer);
    ev.set_flags(r.flags);
    if (req.has_passwd_mask()) { ev.set_has_passwd_mask(true); ev.set_passwd_slot_mask(req.passwd_slot_mask()); }
    for (const auto& a : req.bin_attrs()) {
        const uint16_t id = static_cast<uint16_t>(a.attr_id());
        if (id != kIntBinAttr1 && id != kIntBinAttr2) continue;
        ApplyBin(r, a, setter ? setter->member_id : 0, now);
        FillBin(ev.add_bin_attrs(), r.int_bin[id - kIntBinAttr1]);
    }
    NotifyMembers(r, ev, c.npid);
    shadnet::SetRoomDataInternalReply rep;
    rep.set_room_id(r.room_id);
    out = Blob(rep);
    return NoError;
}

Error CmdSetRoomDataExternal(Client& c, const std::string& in, std::string& out) {
    shadnet::SetRoomDataExternalRequest req;
    if (!Decode(req, in)) return Malformed;
    const auto it = g_rooms.find({c.title, req.room_id()});
    if (it == g_rooms.end()) return RoomMissing;
    Room& r = it->second;
    for (const auto& a : req.search_int_attrs()) ApplyInt(r, a);
    for (const auto& a : req.search_bin_attrs()) if (a.attr_id() == kSearchBinAttr) ApplyBin(r, a, 0, 0);
    for (const auto& a : req.ext_bin_attrs())
        if (a.attr_id() == kExtBinAttr1 || a.attr_id() == kExtBinAttr2) ApplyBin(r, a, 0, 0);
    shadnet::SetRoomDataExternalReply rep;
    rep.set_room_id(r.room_id);
    out = Blob(rep);
    return NoError;
}

Error CmdWorldInfoList(Client& c, const std::string& in, std::string& out) {
    shadnet::GetWorldInfoListRequest req;
    if (!Decode(req, in)) return Malformed;
    // Worlds 1..n (GVNET_WORLDS, default 10), plus any a room is in. GUNDAM VERSUS keeps a table of
    // ten worlds and indexes it by world id - 1: with fewer it read an empty entry and threw
    // std::out_of_range (an uncaught C++ exception) while logging in.
    static const uint32_t world_count = [] {
        const char* env = std::getenv("GVNET_WORLDS");
        return env ? std::max(1, std::atoi(env)) : 10;
    }();
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> worlds;
    for (uint32_t w = 1; w <= world_count; ++w) worlds[w] = {0, 0};
    for (const auto& [key, r] : g_rooms) {
        if (key.first != c.title) continue;
        auto& w = worlds[r.world_id];
        ++w.first;
        w.second += static_cast<uint32_t>(r.members.size());
    }
    shadnet::GetWorldInfoListReply rep;
    LOG("%s asked for the worlds of server %u: %zu", c.npid.c_str(), req.server_id(), worlds.size());
    for (const auto& [id, counts] : worlds) {
        auto* w = rep.add_worlds();
        w->set_world_id(id);
        w->set_rooms_num(counts.first);
        w->set_room_members_num(counts.second);
    }
    out = Blob(rep);
    return NoError;
}

void Handle(Client& c, uint16_t cmd, uint64_t id, const std::string& in) {
    std::string body;
    Error err = NoError;
    if (cmd == Terminate) { ::shutdown(c.fd, SHUT_RDWR); return; }
    if (!c.authed && cmd != Login && cmd != Create) {
        err = Unauthorized;
    } else switch (cmd) {
    case Login: err = CmdLogin(c, in, body); break;
    case GetToken: {
        shadnet::GetTokenReply rep;
        rep.set_token("gvnet-" + c.npid);
        rep.set_user_id(c.user_id);
        rep.set_npid(c.npid);
        body = Blob(rep);
        break;
    }
    case GetServerFeatures: {
        shadnet::ServerFeaturesReply rep;
        rep.set_matching2_enabled(true);
        body = Blob(rep);
        break;
    }
    case SetClientVersion: case SetAppearOffline: case AddFriend: case RemoveFriend:
    case AddBlock: case RemoveBlock: break;
    case ContextStart: {
        shadnet::ContextStartRequest req;
        if (!Decode(req, in)) { err = Malformed; break; }
        c.ctx_id = req.ctx_id();
        LOG("%s started matching context %u", c.npid.c_str(), c.ctx_id);
        break;
    }
    case ContextStop:
        if (c.room_id) DoLeave(c, c.room_id, kCauseLeave);
        c.ctx_id = 0;
        break;
    case CreateRoom: err = CmdCreateRoom(c, in, body); break;
    case JoinRoom: err = CmdJoinRoom(c, in, body); break;
    case LeaveRoom: err = CmdLeaveRoom(c, in, body); break;
    case SearchRoom: err = CmdSearchRoom(c, in, body); break;
    case RequestSignalingInfos: err = CmdSignalingInfos(c, in, body); break;
    case SetUserInfo: err = CmdSetUserInfo(c, in, body); break;
    case SetRoomDataInternal: err = CmdSetRoomDataInternal(c, in, body); break;
    case SetRoomDataExternal: err = CmdSetRoomDataExternal(c, in, body); break;
    case KickoutRoomMember: err = CmdKickout(c, in, body); break;
    case GetWorldInfoList: err = CmdWorldInfoList(c, in, body); break;
    case GetRoomDataExternalList: err = CmdRoomExternalList(c, in, body); break;
    case GetUserInfoList: err = CmdGetUserInfoList(c, in, body); break;
    case GetRoomMemberDataExternalList: err = CmdMemberExternalList(c, in, body); break;
    case SendRoomMessage: err = CmdSendRoomMessage(c, in, body); break;
    default:
        err = cmd >= GetBoardInfos && cmd <= GetScoreGameDataByAccId ? NotFound : Invalid;
        VLOG("%s: command %u answered %u", c.npid.c_str(), cmd, err);
        break;
    }
    if (err != NoError && err != NotFound)
        LOG("%s: command %u failed with error %u", c.npid.c_str(), cmd, err);
    std::string payload(1, static_cast<char>(err));
    if (err == NoError) payload += body;
    SendRaw(c.fd, Packet(Reply, cmd, id, payload));
}

void Disconnect(int fd) {
    auto it = g_clients.find(fd);
    if (it == g_clients.end()) return;
    Client& c = it->second;
    if (c.authed) {
        if (c.room_id) DoLeave(c, c.room_id, kCauseLeave);
        LOG("%s signed out", c.npid.c_str());
        for (auto& [ofd, other] : g_clients) {
            if (!other.authed || ofd == fd) continue;
            shadnet::NotifyFriendStatus n;
            n.set_npid(c.npid);
            n.set_online(false);
            n.set_timestamp(NowUsec() * 1000);
            SendRaw(ofd, Packet(Notification, FriendStatus, 0, Blob(n)));
        }
    }
    ::close(fd);
    g_clients.erase(it);
}

// The UDP port: a ping (cmd 1, online id, local address) behind the signaling vport header is
// answered with the address and port it came from, which the server keeps for the npid.
void HandleUdp(int sock) {
    char buf[2048];
    sockaddr_in from{};
    socklen_t len = sizeof(from);
    const ssize_t n = ::recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &len);
    if (n < 4 + 21 || std::memcmp(buf, "\xff\xff\xff\xff", 4) != 0 || buf[4] != 0x01) return;
    char id[17]{};
    std::memcpy(id, buf + 5, 16);
    const std::string npid(id);
    if (npid.empty()) return;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    const uint16_t port = ntohs(from.sin_port);
    auto& known = g_udp[npid];
    if (known.first != ip || known.second != port) LOG("%s's address: %s:%u", npid.c_str(), ip, port);
    known = {ip, port};
    char reply[4 + 6];
    std::memcpy(reply, "\xff\xff\xff\xff", 4);
    std::memcpy(reply + 4, &from.sin_addr.s_addr, 4);
    std::memcpy(reply + 8, &from.sin_port, 2);
    ::sendto(sock, reply, sizeof(reply), 0, reinterpret_cast<sockaddr*>(&from), len);
}

} // namespace

int main(int argc, char** argv) {
    uint16_t port = 31313;
    std::string bind_addr = "0.0.0.0";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--bind") && i + 1 < argc) bind_addr = argv[++i];
        else if (!std::strcmp(argv[i], "--verbose")) g_verbose = true;
        else { std::fprintf(stderr, "usage: %s [--port 31313] [--bind ADDRESS] [--verbose]\n", argv[0]); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) { std::fprintf(stderr, "bad address\n"); return 2; }
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) || ::listen(listener, 16)) {
        std::perror("gvnet: TCP");
        return 1;
    }
    const int udp = ::socket(AF_INET, SOCK_DGRAM, 0);
    addr.sin_port = htons(static_cast<uint16_t>(port + 1));
    if (::bind(udp, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))) { std::perror("gvnet: UDP"); return 1; }
    LOG("listening on %s, TCP %u and UDP %u", bind_addr.c_str(), port, port + 1);

    for (;;) {
        std::vector<pollfd> fds{{listener, POLLIN, 0}, {udp, POLLIN, 0}};
        for (const auto& [fd, c] : g_clients) fds.push_back({fd, POLLIN, 0});
        if (::poll(fds.data(), fds.size(), -1) < 0) continue;
        if (fds[0].revents & POLLIN) {
            sockaddr_in peer{};
            socklen_t len = sizeof(peer);
            const int fd = ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &len);
            if (fd >= 0) {
                ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                Client c;
                c.fd = fd;
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
                c.peer_ip = ip;
                g_clients[fd] = c;
                std::string version;
                PutLE(version, kProtocolVersion, 4);
                SendRaw(fd, Packet(ServerInfo, 0, 0, version));
            }
        }
        if (fds[1].revents & POLLIN) HandleUdp(udp);
        for (size_t i = 2; i < fds.size(); ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const int fd = fds[i].fd;
            char buf[65536];
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) { Disconnect(fd); continue; }
            auto it = g_clients.find(fd);
            if (it == g_clients.end()) continue;
            Client& c = it->second;
            c.in.append(buf, static_cast<size_t>(n));
            bool bad = false;
            while (c.in.size() >= kHeaderSize) {
                const uint32_t size = static_cast<uint32_t>(GetLE(c.in.data() + 3, 4));
                if (size < kHeaderSize || size > kMaxPacket) { bad = true; break; }
                if (c.in.size() < size) break;
                const uint16_t cmd = static_cast<uint16_t>(GetLE(c.in.data() + 1, 2));
                const uint64_t id = GetLE(c.in.data() + 7, 8);
                const std::string payload = c.in.substr(kHeaderSize, size - kHeaderSize);
                c.in.erase(0, size);
                Handle(c, cmd, id, payload);
                if (g_clients.find(fd) == g_clients.end()) break;
            }
            if (bad) Disconnect(fd);
        }
    }
}
