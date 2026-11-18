// Add-on cars from tools/car_mod.py. Its cars.txt, in the mods folder, has a line for each: the pvehicle's key, the
// frontend's key, the hash of its CARNAME string, its name texture's hash, width, height and NV2A format, and the name.
// The stock car list a profile keeps (FEPlayerCarDB, at +0x414, 200 entries of 20 bytes, filled by 0x140890 when the
// profile is new) is saved with it: a car newer than the profile is added here, as 0x140890 adds them, and is saved with
// it from then on. The car select's name is a texture no front end pack has: GetTextureInfo answers its hash with a copy
// of a TextureInfo reading its name_<hash>.dxt (the PC tool's one car: car_name.dxt), as src/game/options_menu.cpp does its icons. A car taken out again (the app's
// ModInstaller lists it in cars_removed.txt) stays in the saves that kept it, its key naming nothing now: it is taken out
// of the list here, with the customization and career slots of its record.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game {

void SetOwnString(uint32_t hash, const std::string& text);  // prompts.cpp

namespace {

constexpr uint32_t kFeDatabase = 0x48EDD4, kProfile = 0x10, kPlayerCarDb = 0x414, kCurrentCar = 0xA8;
constexpr uint32_t kEntry = 0x14, kEntries = 0xC8, kFrontend = 4, kVehicle = 8, kEmpty = 0xFFFFFFFF;
constexpr uint32_t kRaceModes = 0x1C, kRaceModeSize = 0x18, kRaceModeCount = 0xB;  // FEDatabase's, 0x126130
constexpr uint32_t kSetUpEntry = 0x134070;  // thiscall: the car's type and class from its keys
// a record's slots, 0xFF none, each slot's first byte 0xFF when free (0x1408C0 to 0x1408F7)
constexpr uint32_t kCustomization = 0x10, kCareer = 0x11, kNoSlot = 0xFF;
constexpr uint32_t kCustomizations = 0x1134, kCustomizationSize = 0x198, kCareers = 0x8728, kCareerSize = 0x38;

constexpr uint32_t kCarTypes = 0x4D5800, kStockTypes = 84, kTypeSize = 0xD0, kTypeName = 0x10, kTypeHash = 0x50, kTypeId = 0x90;

constexpr uint32_t kTemplateTexture = 0xF37AF144;  // OPTIONS_ICON_AUDIO, a TextureInfo the front end always has

struct Car {
    uint32_t vehicle, frontend, name_texture, width, height, format;
};

const std::vector<Car>& Cars() {
    static const std::vector<Car> cars = [] {
        std::vector<Car> out;
        std::vector<uint8_t> file;
        if (!kernel::ReadGameFile("D:\\cars.txt", file)) return out;
        std::string text(file.begin(), file.end());
        for (size_t at = 0; at < text.size();) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at);
            at = end + 1;
            uint32_t vehicle, frontend, hash, texture, width, height, format;
            int name = 0;
            if (std::sscanf(line.c_str(), "%x %x %x %x %u %u %u %n", &vehicle, &frontend, &hash, &texture, &width, &height,
                            &format, &name) < 7 || !name)
                continue;
            out.push_back({vehicle, frontend, texture, width, height, format});
            SetOwnString(hash, line.substr(name));
        }
        return out;
    }();
    return cars;
}

// the car types, the game's 84 and an empty one (84 is the "no car" answer) and the add-ons, from car_types.txt
uint32_t CarTypes() {
    static const uint32_t types = [] {
        std::vector<uint8_t> file;
        uint32_t n = 0;
        if (kernel::ReadGameFile("D:\\car_types.txt", file)) {
            file.push_back(0);
            std::sscanf(reinterpret_cast<const char*>(file.data()), "%u", &n);
        }
        return n > kStockTypes ? n : kStockTypes;
    }();
    return types;
}

// the type of a car's name's hash, kStockTypes if none (0x244FE0)
uint32_t TypeOfName(uint32_t hash) {
    uint32_t table = kernel::R32(kCarTypes);
    for (uint32_t i = 0, n = CarTypes(); i < n; i++) {
        if (i == kStockTypes) continue;
        uint32_t h = 0xFFFFFFFF;
        const auto* name = static_cast<const uint8_t*>(kernel::HostPtr(table + i * kTypeSize + kTypeName));
        while (*name) h = h * 33 + *name++;
        if (h == hash) return kernel::R32(table + i * kTypeSize + kTypeId);
    }
    return kStockTypes;
}

const std::vector<uint32_t>& RemovedCars() {
    static const std::vector<uint32_t> keys = [] {
        std::vector<uint32_t> out;
        std::vector<uint8_t> file;
        if (!kernel::ReadGameFile("D:\\cars_removed.txt", file)) return out;
        std::string text(file.begin(), file.end());
        for (size_t at = 0; at < text.size();) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            uint32_t key;
            if (std::sscanf(text.substr(at, end - at).c_str(), "%x", &key) == 1) out.push_back(key);
            at = end + 1;
        }
        return out;
    }();
    return keys;
}

// The Agnes Tachyon flipbook (see reference-addon/flipbook_port_hook_investigation.md):
// the add-on carries two baked running poses as part solids, pose 0 in AGNES_BASE_*
// (part slot 0) and pose 1 in AGNES_KIT00_BODY_* (part slot 23). The game's world car
// list at 0x4D5ED0 is a circular doubly-linked list (sentinel = itself); each node holds
// the render-object pointer at +0x318 and the live world position (x, y, z floats) at
// +0x320/+0x324/+0x328. A render object's part table is at +0xB94, 76 slots x 5 LODs,
// entry = +slot*20 + lod*4, and bit 0 of the stored model pointer hides the part
// (the renderer skips it: test al, 1 at 0x255C40). The render object's type record is
// at +0x94, its name inline at +0x10 (bhash("AGNES") = 0x026b540d).
constexpr uint32_t kWorldCarList = 0x4D5ED0;
constexpr uint32_t kNodeNext = 0x00, kNodeRenderObj = 0x318;
constexpr uint32_t kNodePosX = 0x320, kNodePosY = 0x324, kNodePosZ = 0x328;
constexpr uint32_t kRenderObjTypeRecord = 0x94;
constexpr uint32_t kPartTable = 0xB94, kPartSlotStride = 20, kPartLods = 5;
constexpr uint32_t kPoseSlotA = 0, kPoseSlotB = 23;  // BASE, _KIT00_BODY
constexpr uint32_t kAgnesTypeHash = 0x026b540d;      // bhash("AGNES")
constexpr float kFlipbookStride = 0.5f;              // metres of travel per pose step

// The type name's bhash (as TypeOfName computes it), or 0 when the render
// object has no readable type record.
uint32_t TypeHash(uint32_t render) {
    if (!render || !mem::IsCommitted(render + kRenderObjTypeRecord)) return 0;
    uint32_t type_rec = kernel::R32(render + kRenderObjTypeRecord);
    if (!type_rec || !mem::IsCommitted(type_rec + kTypeName)) return 0;
    // bhash the type name, as TypeOfName does; "AGNES" -> 0x026b540d
    uint32_t h = 0xFFFFFFFF;
    for (const auto* p = static_cast<const uint8_t*>(kernel::HostPtr(type_rec + kTypeName)); *p; ++p)
        h = h * 33 + *p;
    return h;
}

// The type name as a short string (up to 15 chars), for the diagnostic log.
void TypeName(uint32_t render, char* out, size_t n) {
    out[0] = 0;
    if (!render || !mem::IsCommitted(render + kRenderObjTypeRecord)) return;
    uint32_t type_rec = kernel::R32(render + kRenderObjTypeRecord);
    if (!type_rec || !mem::IsCommitted(type_rec + kTypeName)) return;
    const auto* p = static_cast<const uint8_t*>(kernel::HostPtr(type_rec + kTypeName));
    size_t i = 0;
    for (; i + 1 < n && p[i]; ++i) out[i] = static_cast<char>(p[i]);
    out[i] = 0;
}

bool IsAgnesRenderObj(uint32_t render) {
    uint32_t h = TypeHash(render);
    return h != 0 && h == kAgnesTypeHash;
}

// While racing, alternate the two baked running poses as the car travels, one pose
// step per kFlipbookStride metres. Runs once per presented frame from ModCarsFrame;
// all state lives in port-side statics, no guest memory is used. In the front end /
// garage (not racing) the state resets, so a race always starts on pose 0.
void FlipbookFrame() {
    static float prev_x = 0, prev_y = 0, prev_z = 0;
    static bool have_prev = false;
    static double distance = 0;
    static int pose = -1;  // -1 = no pose applied yet
    if (!Racing()) {
        have_prev = false;
        distance = 0;
        pose = -1;
        return;
    }
    if (!mem::IsCommitted(kWorldCarList)) return;
    uint32_t head = kWorldCarList;
    uint32_t node = kernel::R32(head);
    bool found = false;
    // Diagnostic (temporary): log the first 3 racing frames' walks, so the log
    // shows whether the list is walked, how many nodes it has, and what type
    // names the cars actually carry at runtime.
    static int diag_frames = 0;
    bool diag = diag_frames < 3;
    if (diag) ++diag_frames;
    int nodes = 0;
    for (int guard = 0; node != head && node != 0 && guard < 64; ++guard) {
        if (!mem::IsCommitted(node) || !mem::IsCommitted(node + kNodePosZ)) break;
        uint32_t render = kernel::R32(node + kNodeRenderObj);
        uint32_t next = kernel::R32(node + kNodeNext);
        ++nodes;
        if (diag) {
            char name[16];
            TypeName(render, name, sizeof(name));
            LOGR("flipbook dbg: node %08X render %08X type '%s' hash %08X", node, render, name, TypeHash(render));
        }
        if (render && IsAgnesRenderObj(render)) {
            found = true;
            float x = 0, y = 0, z = 0;
            std::memcpy(&x, kernel::HostPtr(node + kNodePosX), 4);
            std::memcpy(&y, kernel::HostPtr(node + kNodePosY), 4);
            std::memcpy(&z, kernel::HostPtr(node + kNodePosZ), 4);
            if (have_prev) {
                float dx = x - prev_x, dy = y - prev_y, dz = z - prev_z;
                double step = std::sqrt(double(dx) * dx + double(dy) * dy + double(dz) * dz);
                if (step < 10.0) distance += step;  // ignore teleports / resets
            }
            prev_x = x;
            prev_y = y;
            prev_z = z;
            have_prev = true;
            int new_pose = int(distance / kFlipbookStride) % 2;
            if (new_pose != pose) {
                if (pose == -1) LOGR("flipbook: Agnes pose flipbook active (render object %08X)", render);
                pose = new_pose;
                for (uint32_t l = 0; l < kPartLods; ++l) {
                    uint32_t e_a = render + kPartTable + kPoseSlotA * kPartSlotStride + l * 4;
                    uint32_t e_b = render + kPartTable + kPoseSlotB * kPartSlotStride + l * 4;
                    if (!mem::IsCommitted(e_a) || !mem::IsCommitted(e_b)) continue;
                    uint32_t v_a = kernel::R32(e_a), v_b = kernel::R32(e_b);
                    uint32_t n_a = (pose == 0) ? (v_a & ~1u) : (v_a | 1u);
                    uint32_t n_b = (pose == 0) ? (v_b | 1u) : (v_b & ~1u);
                    if (n_a != v_a) kernel::W32(e_a, n_a);
                    if (n_b != v_b) kernel::W32(e_b, n_b);
                }
            }
        }
        node = next;
    }
    if (diag) LOGR("flipbook dbg: walk nodes=%d found=%d", nodes, found ? 1 : 0);
    if (!found) {
        have_prev = false;
        distance = 0;
        pose = -1;
    }
}

// ---- Agnes Tachyon vertex-cache animation (supersedes the flipbook) ----
// Data: D:\agnes_anims.bin, format AGNV v1 (see reference-addon/agnes_anims.bin).
//   header 64 B: "AGNV", version u32=1, num_clips u32, vertex_count u32, reserved
//   per-clip dir (28 B): name[16], frames u32, cycle_seconds f32, data_offset u32
//   data: frames * vertex_count * 3 f32 positions per clip (NFS/Xbox coords).
// Clips (30 frames each, 12941 verts): Walk 1.667 s, Jog 1.25 s, Run 1.0 s,
// Sprint 0.806 s. Speed bands (km/h): 0-15 Walk, 15-30 Jog, 30-70 Run, 70+
// Sprint. Constant playback per band (no speed scaling); crossfade on band
// change; frame interpolation so 30 fps keyframes play smoothly at 60 fps.
// The BODY model (part slot 23) is animated; BASE (slot 0) is hidden. The BODY
// solid's first vertex normal is tagged (0.1337, 0, 0.0) to confirm the buffer.
// Vertex buffers: renderObj -> part table +0xB94 -> model (slot 23, lod 0, bit 0
// masked) -> P=[model+0xC] -> modelData=[P] -> groups=[modelData+0xC] ->
// group=groups+g*96 -> vbuf=[group+0x30], 32 B/vertex (pos/uv/normal). The port
// re-uploads guest vertex data every draw, so writing the guest buffer here
// shows up the same frame. UVs are left untouched (only pos and normal are
// written); normals are set flat (0,0,1), the look Larzy prefers.
// Only LOD 0 is animated (the close-up LOD used in races and the garage);
// lower LODs keep the static pose.
constexpr uint32_t kAgnesBodySlot = 23;
constexpr uint32_t kAgnesBaseSlot = 0;
constexpr uint32_t kAgnesModelDef = 0xC;        // [model+0xC] -> model definition
constexpr uint32_t kModelDataGroups = 0xC;      // [modelData+0xC] -> groups array
constexpr uint32_t kModelDataGroupCount = 0x8;  // u16 at [modelData+0x8]
constexpr uint32_t kModelDataIndices = 0x10;    // [modelData+0x10] -> u16 indices
constexpr uint32_t kGroupRecordSize = 96;
constexpr uint32_t kGroupIndexCount = 0x20;     // u16 at [group+0x20]
constexpr uint32_t kGroupIndexStart = 0x2c;     // u32 at [group+0x2c]
constexpr uint32_t kGroupVertexData = 0x30;     // [group+0x30] -> vertex buffer
constexpr float kAgnesTagX = 0.1337f;           // magic normal.x (car index 0)
constexpr uint32_t kAgnesMaxGroups = 8;
constexpr float kAgnesCrossfadeSecs = 0.2f;     // seconds to blend between clips
constexpr float kAgnesFlatNy = 0.0f, kAgnesFlatNz = 1.0f;  // flat normal (0,0,1)

struct AgnesClipInfo {
    char name[16];
    uint32_t frames;
    float cycle;
    uint32_t data_off;
};

// The loaded animation file (AGNV v1), kept for the process lifetime.
struct AgnesAnims {
    std::vector<uint8_t> file;
    uint32_t verts = 0;
    AgnesClipInfo clips[4];
    int nclips = 0;
    bool loaded = false;
    bool tried = false;
};

const AgnesAnims& LoadAgnesAnims() {
    static AgnesAnims a;
    if (a.tried) return a;
    a.tried = true;
    std::vector<uint8_t> f;
    if (!kernel::ReadGameFile("D:\\agnes_anims.bin", f)) {
        LOGR("agnes anim: D:\\agnes_anims.bin not found");
        return a;
    }
    if (f.size() < 64 || std::memcmp(f.data(), "AGNV", 4) != 0) {
        LOGR("agnes anim: bad agnes_anims.bin header");
        return a;
    }
    uint32_t ver = 0, ncl = 0, nv = 0;
    std::memcpy(&ver, f.data() + 4, 4);
    std::memcpy(&ncl, f.data() + 8, 4);
    std::memcpy(&nv, f.data() + 12, 4);
    if (ver != 1 || ncl == 0 || ncl > 4 || nv == 0) {
        LOGR("agnes anim: bad agnes_anims.bin (ver %u clips %u verts %u)", ver, ncl, nv);
        return a;
    }
    a.file = std::move(f);
    a.verts = nv;
    a.nclips = (int)ncl;
    for (int i = 0; i < a.nclips; ++i) {
        const uint8_t* e = a.file.data() + 64 + i * 28;
        std::memcpy(a.clips[i].name, e, 16);
        a.clips[i].name[15] = 0;
        std::memcpy(&a.clips[i].frames, e + 16, 4);
        std::memcpy(&a.clips[i].cycle, e + 20, 4);
        std::memcpy(&a.clips[i].data_off, e + 24, 4);
        if (a.clips[i].frames == 0 || a.clips[i].cycle <= 0) {
            LOGR("agnes anim: bad clip %d", i);
            a.loaded = false;
            return a;
        }
    }
    a.loaded = true;
    LOGR("agnes anim: loaded agnes_anims.bin (%u clips, %u verts)", ncl, nv);
    return a;
}

// Per-group vertex buffers + remap for the resolved BODY model (LOD 0).
struct AgnesModel {
    bool resolved = false;
    uint32_t render = 0;
    uint32_t groupCount = 0;
    uint32_t vbuf[kAgnesMaxGroups] = {};
    // invRemap[g][local] = global vertex index (local order = index-encounter order)
    std::vector<uint32_t> invRemap[kAgnesMaxGroups];
};

// Diagnostic (temporary): log each ResolveAgnesModel failure step once, so the
// log shows exactly which link of the vertex-buffer chain breaks.
void ResolveFailLog(int step, uint32_t render) {
    static bool logged[16] = {};
    if (step >= 0 && step < 16 && !logged[step]) {
        logged[step] = true;
        LOGR("agnes dbg: resolve fail step %d render %08X", step, render);
    }
}

// Resolve the BODY model's vertex buffers for an Agnes render object.
// Validates with the (0.1337, 0, 0) tag, then caches the pointers + remap.
bool ResolveAgnesModel(uint32_t render, AgnesModel& m) {
    if (m.resolved && m.render == render) return true;
    m = AgnesModel();
    m.render = render;
    const AgnesAnims& anims = LoadAgnesAnims();
    if (!anims.loaded) { ResolveFailLog(1, render); return false; }
    uint32_t e = render + kPartTable + kAgnesBodySlot * kPartSlotStride + 0 * 4;
    if (!mem::IsCommitted(e)) { ResolveFailLog(2, render); return false; }
    uint32_t model = kernel::R32(e) & ~1u;
    if (!model || !mem::IsCommitted(model + kAgnesModelDef)) { ResolveFailLog(3, render); return false; }
    uint32_t p = kernel::R32(model + kAgnesModelDef);
    uint32_t modelData = 0;
    if (p && mem::IsCommitted(p)) modelData = kernel::R32(p);
    if (!modelData && mem::IsCommitted(model)) modelData = kernel::R32(model);  // fallback: [model+0]
    if (!modelData || !mem::IsCommitted(modelData + kModelDataGroupCount)) { ResolveFailLog(4, render); return false; }
    uint32_t groups = kernel::R32(modelData + kModelDataGroups);
    uint32_t indexData = kernel::R32(modelData + kModelDataIndices);
    if (!groups || !indexData) { ResolveFailLog(5, render); return false; }
    uint16_t gc = 0;
    std::memcpy(&gc, kernel::HostPtr(modelData + kModelDataGroupCount), 2);
    if (gc == 0 || gc > kAgnesMaxGroups) { ResolveFailLog(6, render); return false; }
    // group headers first (vbuf, index count/start), and the index span
    uint32_t vb[kAgnesMaxGroups] = {};
    uint32_t istart[kAgnesMaxGroups] = {};
    uint32_t icount[kAgnesMaxGroups] = {};
    uint32_t idxSpan = 0;
    for (uint32_t g = 0; g < gc; ++g) {
        uint32_t grp = groups + g * kGroupRecordSize;
        if (!mem::IsCommitted(grp + kGroupVertexData + 4)) { ResolveFailLog(7, render); return false; }
        vb[g] = kernel::R32(grp + kGroupVertexData);
        uint16_t ic = 0;
        uint32_t st = 0;
        std::memcpy(&ic, kernel::HostPtr(grp + kGroupIndexCount), 2);
        std::memcpy(&st, kernel::HostPtr(grp + kGroupIndexStart), 4);
        if (!vb[g] || ic == 0) { ResolveFailLog(8, render); return false; }
        icount[g] = ic;
        istart[g] = st;
        if (st + ic > idxSpan) idxSpan = st + ic;
    }
    if (idxSpan == 0 || !mem::IsCommitted(indexData + idxSpan * 2 - 1)) { ResolveFailLog(9, render); return false; }
    // validate the tag: group 0 vertex 0 normal == (0.1337, 0, 0)
    float n0[3];
    std::memcpy(n0, static_cast<const uint8_t*>(kernel::HostPtr(vb[0])) + 0x14, 12);
    if (std::fabs(n0[0] - kAgnesTagX) > 0.0021f || std::fabs(n0[1]) > 0.0021f || std::fabs(n0[2]) > 0.0021f) {
        static bool tagFailLogged = false;
        if (!tagFailLogged) {
            tagFailLogged = true;
            LOGR("agnes anim: BODY tag check failed (normal %.4f %.4f %.4f) - is the tagged flipbook installed?",
                 n0[0], n0[1], n0[2]);
        }
        return false;
    }
    // copy the index buffer, then build the remap (index-encounter order)
    std::vector<uint16_t> indices(idxSpan);
    std::memcpy(indices.data(), kernel::HostPtr(indexData), idxSpan * 2);
    std::vector<uint32_t> remap(anims.verts, 0xFFFFFFFFu);
    for (uint32_t g = 0; g < gc; ++g) {
        m.vbuf[g] = vb[g];
        m.invRemap[g].reserve(4096);
        for (uint32_t k = 0; k < icount[g]; ++k) {
            uint32_t gi = indices[istart[g] + k];
            if (gi >= anims.verts) continue;
            if (remap[gi] == 0xFFFFFFFFu) {
                remap[gi] = (g << 24) | (uint32_t)m.invRemap[g].size();
                m.invRemap[g].push_back(gi);
            }
        }
    }
    for (uint32_t v = 0; v < anims.verts; ++v) {
        if (remap[v] == 0xFFFFFFFFu) {
            LOGR("agnes anim: vertex %u not covered by any group", v);
            return false;
        }
    }
    m.groupCount = gc;
    m.resolved = true;
    LOGR("agnes anim: BODY resolved (render %08X, %u groups, %u verts)", render, gc, anims.verts);
    return true;
}

// Speed band -> clip index: 0 Walk, 1 Jog, 2 Run, 3 Sprint.
int AgnesClipForSpeed(float kmh) {
    if (kmh < 15.0f) return 0;
    if (kmh < 30.0f) return 1;
    if (kmh < 70.0f) return 2;
    return 3;
}

float AgnesSmoothstep(float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    return t * t * (3 - 2 * t);
}

// Per-frame Agnes animation. Called from ModCarsFrame every presented frame.
// Hides BASE (slot 0), shows + animates BODY (slot 23, LOD 0). State lives in
// port-side statics; the model pointers + remap are cached after first resolve.
void AgnesAnimateFrame() {
    const AgnesAnims& anims = LoadAgnesAnims();
    static AgnesModel model;
    static double phase = 0;        // current clip phase [0,1)
    static double prevPhase = 0;    // previous clip phase (crossfade source)
    static int clip = 2;            // current clip (start on Run)
    static int prevClip = -1;       // clip being faded out, -1 = none
    static double crossfadeT = 1.0;  // 1 = no crossfade in progress
    static float prevX = 0, prevY = 0, prevZ = 0;
    static bool havePrev = false;
    static double lastTime = 0;
    static float smoothKmh = 0;
    static bool activeLogged = false;
    if (!Racing()) {
        havePrev = false;
        lastTime = 0;
        smoothKmh = 0;
        return;
    }
    if (!anims.loaded) return;
    if (!mem::IsCommitted(kWorldCarList)) return;
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    double dt = lastTime > 0 ? now - lastTime : 1.0 / 60.0;
    lastTime = now;
    if (dt < 1.0 / 240.0) dt = 1.0 / 240.0;
    if (dt > 1.0 / 10.0) dt = 1.0 / 10.0;
    // Diagnostic (temporary): sample the world-list walk once per second while
    // racing, so the log shows whether the list is populated and what type
    // names the cars actually carry at runtime.
    {
        static double lastWalkDiag = 0;
        if (now - lastWalkDiag >= 1.0) {
            lastWalkDiag = now;
            uint32_t headDbg = kernel::R32(kWorldCarList);
            LOGR("agnes dbg: walk head=%08X", headDbg);
            int nodesDbg = 0;
            uint32_t nd = headDbg;
            for (int g = 0; nd != kWorldCarList && nd != 0 && g < 64; ++g) {
                if (!mem::IsCommitted(nd) || !mem::IsCommitted(nd + kNodePosZ)) break;
                uint32_t rd = kernel::R32(nd + kNodeRenderObj);
                uint32_t nx = kernel::R32(nd + kNodeNext);
                ++nodesDbg;
                char nm[16];
                TypeName(rd, nm, sizeof(nm));
                LOGR("agnes dbg: node %d render %08X type '%s' hash %08X", nodesDbg, rd, nm, TypeHash(rd));
                nd = nx;
            }
            LOGR("agnes dbg: walk nodes=%d", nodesDbg);
        }
    }
    uint32_t head = kWorldCarList;
    uint32_t node = kernel::R32(head);
    uint32_t agnesRender = 0;
    float x = 0, y = 0, z = 0;
    for (int guard = 0; node != head && node != 0 && guard < 64; ++guard) {
        if (!mem::IsCommitted(node) || !mem::IsCommitted(node + kNodePosZ)) break;
        uint32_t render = kernel::R32(node + kNodeRenderObj);
        uint32_t next = kernel::R32(node + kNodeNext);
        if (render && IsAgnesRenderObj(render)) {
            agnesRender = render;
            std::memcpy(&x, kernel::HostPtr(node + kNodePosX), 4);
            std::memcpy(&y, kernel::HostPtr(node + kNodePosY), 4);
            std::memcpy(&z, kernel::HostPtr(node + kNodePosZ), 4);
            break;
        }
        node = next;
    }
    if (!agnesRender) {
        havePrev = false;
        return;
    }
    // Hide BASE, show BODY (only writes when the bit must change).
    for (uint32_t l = 0; l < kPartLods; ++l) {
        uint32_t e_base = agnesRender + kPartTable + kAgnesBaseSlot * kPartSlotStride + l * 4;
        uint32_t e_body = agnesRender + kPartTable + kAgnesBodySlot * kPartSlotStride + l * 4;
        if (!mem::IsCommitted(e_base) || !mem::IsCommitted(e_body)) continue;
        uint32_t v_base = kernel::R32(e_base), v_body = kernel::R32(e_body);
        uint32_t n_base = v_base | 1u, n_body = v_body & ~1u;
        if (n_base != v_base) kernel::W32(e_base, n_base);
        if (n_body != v_body) kernel::W32(e_body, n_body);
    }
    if (!ResolveAgnesModel(agnesRender, model)) return;
    if (!activeLogged) {
        activeLogged = true;
        LOGR("agnes anim: Agnes animation active (render object %08X)", agnesRender);
    }
    // Speed from the position delta (m/s -> km/h), smoothed.
    float kmh = 0;
    if (havePrev) {
        float dx = x - prevX, dy = y - prevY, dz = z - prevZ;
        double step = std::sqrt(double(dx) * dx + double(dy) * dy + double(dz) * dz);
        if (step < 10.0) kmh = float(step / dt * 3.6);
    }
    prevX = x;
    prevY = y;
    prevZ = z;
    havePrev = true;
    smoothKmh = smoothKmh * 0.7f + kmh * 0.3f;
    // Select the clip by band; start a crossfade on change.
    int want = AgnesClipForSpeed(smoothKmh);
    if (want != clip) {
        prevClip = clip;
        prevPhase = phase;
        clip = want;
        phase = 0;
        crossfadeT = 0;
    }
    // Advance the current clip's phase (constant playback per band).
    const AgnesClipInfo& cur = anims.clips[clip];
    phase += dt / cur.cycle;
    if (phase >= 1.0) phase -= 1.0;
    if (prevClip >= 0) {
        const AgnesClipInfo& pc = anims.clips[prevClip];
        prevPhase += dt / pc.cycle;
        if (prevPhase >= 1.0) prevPhase -= 1.0;
        crossfadeT += dt / kAgnesCrossfadeSecs;
        if (crossfadeT >= 1.0) {
            prevClip = -1;
            crossfadeT = 1.0;
        }
    }
    float blend = prevClip >= 0 ? AgnesSmoothstep((float)crossfadeT) : 1.0f;
    // Interpolated frame indices for the current (and previous) clip.
    auto framePair = [](const AgnesClipInfo& c, double ph, int& f0, int& f1, float& t) {
        double ff = ph * c.frames;
        int i0 = (int)ff;
        if (i0 >= (int)c.frames) i0 = (int)c.frames - 1;
        f0 = i0;
        f1 = (i0 + 1) % (int)c.frames;
        t = (float)(ff - i0);
    };
    int f0, f1;
    float t;
    framePair(cur, phase, f0, f1, t);
    int pf0 = 0, pf1 = 0;
    float pt = 0;
    if (prevClip >= 0) framePair(anims.clips[prevClip], prevPhase, pf0, pf1, pt);
    const uint8_t* base = anims.file.data();
    const float* curA = reinterpret_cast<const float*>(base + cur.data_off + (size_t)f0 * anims.verts * 3);
    const float* curB = reinterpret_cast<const float*>(base + cur.data_off + (size_t)f1 * anims.verts * 3);
    const float* prevA = nullptr;
    const float* prevB = nullptr;
    if (prevClip >= 0) {
        const AgnesClipInfo& pc = anims.clips[prevClip];
        prevA = reinterpret_cast<const float*>(base + pc.data_off + (size_t)pf0 * anims.verts * 3);
        prevB = reinterpret_cast<const float*>(base + pc.data_off + (size_t)pf1 * anims.verts * 3);
    }
    // Write every vertex: interpolated position + flat normal (0,0,1); UV untouched.
    for (uint32_t g = 0; g < model.groupCount; ++g) {
        uint8_t* vb = static_cast<uint8_t*>(kernel::HostPtr(model.vbuf[g]));
        for (uint32_t local = 0, n = (uint32_t)model.invRemap[g].size(); local < n; ++local) {
            uint32_t gv = model.invRemap[g][local];
            const float* a = curA + (size_t)gv * 3;
            const float* b = curB + (size_t)gv * 3;
            float px = a[0] + (b[0] - a[0]) * t;
            float py = a[1] + (b[1] - a[1]) * t;
            float pz = a[2] + (b[2] - a[2]) * t;
            if (prevClip >= 0) {
                const float* pa = prevA + (size_t)gv * 3;
                const float* pb = prevB + (size_t)gv * 3;
                float ox = pa[0] + (pb[0] - pa[0]) * pt;
                float oy = pa[1] + (pb[1] - pa[1]) * pt;
                float oz = pa[2] + (pb[2] - pa[2]) * pt;
                px = ox + (px - ox) * blend;
                py = oy + (py - oy) * blend;
                pz = oz + (pz - oz) * blend;
            }
            uint8_t* dst = vb + (size_t)local * 32;
            std::memcpy(dst + 0, &px, 4);
            std::memcpy(dst + 4, &py, 4);
            std::memcpy(dst + 8, &pz, 4);
            static const float flatN[3] = {0.0f, 0.0f, 1.0f};
            std::memcpy(dst + 0x14, flatN, 12);
        }
    }
}

}  // namespace

// GetTextureInfo's question, on the game thread: our TextureInfo for the name texture's hash, else 0
uint32_t ModCarTexture(X86Context& ctx, uint32_t hash) {
    static std::map<uint32_t, uint32_t> made;
    for (const Car& car : Cars()) {
        if (!car.name_texture || car.name_texture != hash) continue;
        if (uint32_t info = made[hash]) return info;
        constexpr uint32_t kInfoBytes = 0x80, kPlatBytes = 0x40, kD3D = 0x10, kInfoHash = 0x24, kInfoSize = 0x40;
        std::vector<uint8_t> pixels;
        char file[32];
        std::snprintf(file, sizeof file, "D:\\name_%08x.dxt", hash);
        if (!kernel::ReadGameFile(file, pixels) && !kernel::ReadGameFile("D:\\car_name.dxt", pixels)) return 0;
        uint32_t tmpl = kernel::CallGuest(ctx, 0xC5900, {kTemplateTexture, 0, 0});
        if (!tmpl || !kernel::R32(tmpl)) return 0;
        uint32_t info = mem::AllocSystem(kInfoBytes), plat = mem::AllocSystem(kPlatBytes);
        std::memcpy(kernel::HostPtr(info), kernel::HostPtr(tmpl), kInfoBytes);
        std::memcpy(kernel::HostPtr(plat), kernel::HostPtr(kernel::R32(tmpl)), kPlatBytes);
        kernel::W32(info, plat);
        kernel::W32(info + kInfoHash, hash);
        kernel::W32(info + kInfoSize, car.width | car.height << 16);
        uint32_t data = mem::AllocContiguous(uint32_t(pixels.size()), 0, 0xFFFFFFFF, 0x1000);
        std::memcpy(kernel::HostPtr(data), pixels.data(), pixels.size());
        auto log2 = [](uint32_t v) {
            uint32_t n = 0;
            while ((1u << n) < v) n++;
            return n;
        };
        uint32_t d3d = plat + kD3D;
        kernel::W32(d3d + 4, mem::PhysicalFromVirtual(data));
        kernel::W32(d3d + 8, 0);
        kernel::W32(d3d + 12, (kernel::R32(d3d + 12) & 0xFF) | car.format << 8 | 1u << 16 | log2(car.width) << 20 |
                                  log2(car.height) << 24);
        kernel::W32(d3d + 16, 0);
        made[hash] = info;
        return info;
    }
    return 0;
}

uint32_t ListedHandle(uint32_t table, uint32_t handle);

// game thread, at each present
void ModCarsFrame(X86Context& ctx) {
    const std::vector<Car>& cars = Cars();
    const std::vector<uint32_t>& removed = RemovedCars();
    {  // Diagnostic (temporary): prove whether the early return below fires.
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOGR("agnes dbg: ModCarsFrame cars=%u removed=%u", (unsigned)cars.size(), (unsigned)removed.size());
        }
    }
    if (cars.empty() && removed.empty()) return;
    // The Agnes vertex-cache animation runs next: it needs no profile, and it
    // must run every frame (not just on the 30-frame car-list sync below).
    // (The flipbook pose switcher is superseded by the animation.)
    AgnesAnimateFrame();
    uint32_t db = kernel::R32(kFeDatabase);
    uint32_t profile = db && mem::IsCommitted(db + kProfile) ? kernel::R32(db + kProfile) : 0;
    if (!profile || !mem::IsCommitted(profile + kPlayerCarDb + kEntries * kEntry - 1)) return;
    uint32_t table = profile + kPlayerCarDb;
    // filled: 0x140890 empties every handle, then adds the default car first and gives it the handle 0x12345678
    // (0x140905), then the stock cars. Older code took a first handle of 0 for filled and added no car to such a profile
    if (kernel::R32(table) == kEmpty || !kernel::R32(table + kVehicle)) return;
    for (uint32_t key : removed) {
        for (uint32_t i = 0; i < kEntries; i++) {
            uint32_t entry = table + i * kEntry;
            if (kernel::R32(entry) == kEmpty || kernel::R32(entry + kVehicle) != key) continue;
            auto byte = [](uint32_t va) { return static_cast<uint8_t*>(kernel::HostPtr(va)); };
            uint8_t custom = *byte(entry + kCustomization), career = *byte(entry + kCareer);
            if (custom != kNoSlot) *byte(table + kCustomizations + custom * kCustomizationSize) = kNoSlot;
            if (career != kNoSlot) *byte(table + kCareers + career * kCareerSize) = kNoSlot;
            kernel::W32(entry, kEmpty);
        }
    }
    // the career's car (CareerSettings::CurrentCar, a record's handle, set at boot from 0x132A90 at 0x1571F6) gone with
    // the add-on: another of the career's cars instead, so the career goes on with what it has
    uint32_t current = kernel::R32(profile + kCurrentCar);
    bool found = false;
    uint32_t career_car = kEmpty;
    for (uint32_t i = 0; i < kEntries; i++) {
        uint32_t entry = table + i * kEntry, handle = kernel::R32(entry);
        if (handle == kEmpty) continue;
        if (handle == current) found = true;
        if (career_car == kEmpty && *static_cast<uint8_t*>(kernel::HostPtr(entry + kCareer)) != kNoSlot) career_car = handle;
    }
    // no other career car: the list's (0x12F4E0 and the race's start 0x213380 read the record with no check)
    if (!found && current != kEmpty) {
        uint32_t pick = career_car != kEmpty ? career_car : ListedHandle(table, current);
        if (pick != current) {
            LOGR("mods: the career's car %u is gone with its add-on; the career goes on in %u", current, pick);
            kernel::W32(profile + kCurrentCar, pick);
        }
    }
    // the race modes' settings (24 byte records at +0x1C, 0x126130; 0xB of them) keep their car's handle at +0x10
    if (mem::IsCommitted(db + kRaceModes + kRaceModeCount * kRaceModeSize - 1))
        for (uint32_t i = 0; i < kRaceModeCount; i++) {
            uint32_t slot = db + kRaceModes + i * kRaceModeSize + 0x10, handle = kernel::R32(slot);
            if (handle == kEmpty) continue;
            uint32_t listed = ListedHandle(table, handle);
            if (listed != handle) kernel::W32(slot, listed);
        }
    static uint32_t frames = 0;
    if (++frames % 30) return;
    for (const Car& car : cars) {
        uint32_t free_slot = kEmpty;
        bool listed = false;
        for (uint32_t i = 0; i < kEntries && !listed; i++) {
            uint32_t entry = table + i * kEntry;
            if (kernel::R32(entry) == kEmpty) {
                if (free_slot == kEmpty) free_slot = i;
            } else if (kernel::R32(entry + kVehicle) == car.vehicle) {
                listed = true;
            }
        }
        if (listed || free_slot == kEmpty) continue;
        LOGR("mods: %08X added to the profile's cars", car.vehicle);
        uint32_t entry = table + free_slot * kEntry;
        kernel::W32(entry, free_slot);
        kernel::W32(entry + kFrontend, car.frontend);
        kernel::W32(entry + kVehicle, car.vehicle);
        uint32_t saved[] = {ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esi, ctx.edi, ctx.ebp};
        ctx.ecx = entry;
        kernel::CallGuest(ctx, kSetUpEntry, {});
        ctx.eax = saved[0], ctx.ecx = saved[1], ctx.edx = saved[2], ctx.ebx = saved[3], ctx.esi = saved[4],
        ctx.edi = saved[5], ctx.ebp = saved[6];
    }
}

// A stock car for an AI racer that would drive an add-on: their lower LODs, which an opponent is mostly seen in, are the
// converter's thinning of a PC model and often torn, so opponents keep to the game's own cars. Taken from the profile's
// stock list, an add-on's key or a removed one's never; a different one for each add-on asked in a row
uint32_t StockStandIn(uint32_t vehicle) {
    auto addon = [](uint32_t key) {
        for (const Car& car : Cars())
            if (car.vehicle == key) return true;
        for (uint32_t removed : RemovedCars())
            if (removed == key) return true;
        return false;
    };
    if (!vehicle || !addon(vehicle)) return 0;
    uint32_t db = kernel::R32(kFeDatabase);
    uint32_t profile = db && mem::IsCommitted(db + kProfile) ? kernel::R32(db + kProfile) : 0;
    if (!profile || !mem::IsCommitted(profile + kPlayerCarDb + kEntries * kEntry - 1)) return 0;
    std::vector<uint32_t> stock;
    for (uint32_t i = 0; i < kEntries; i++) {
        uint32_t entry = profile + kPlayerCarDb + i * kEntry, key = kernel::R32(entry + kVehicle);
        if (kernel::R32(entry) == kEmpty || !key || addon(key)) continue;
        if (std::find(stock.begin(), stock.end(), key) == stock.end()) stock.push_back(key);
    }
    if (stock.empty()) return 0;
    static uint32_t turn = 0;
    uint32_t pick = stock[(vehicle + turn++ * 7) % stock.size()];
    LOGR("mods: an AI racer's add-on %08X drives the stock %08X instead", vehicle, pick);
    return pick;
}

// A handle the list (this, the profile's +0x414) has, for one it has not: the career's car, else the first listed
uint32_t ListedHandle(uint32_t table, uint32_t handle) {
    if (!mem::IsCommitted(table + kEntries * kEntry - 1)) return handle;
    uint32_t first = kEmpty;
    for (uint32_t i = 0; i < kEntries; i++) {
        uint32_t h = kernel::R32(table + i * kEntry);
        if (h == handle) return handle;
        if (first == kEmpty && h != kEmpty) first = h;
    }
    uint32_t current = kernel::R32(table - kPlayerCarDb + kCurrentCar), pick = first;
    for (uint32_t i = 0; i < kEntries && current != kEmpty; i++)
        if (kernel::R32(table + i * kEntry) == current) pick = current;
    if (pick == kEmpty) return handle;
    LOGR("mods: car %u is not in the profile's list (an add-on taken out); %u instead", handle, pick);
    return pick;
}

}  // namespace game

// A car's ride built from its record (thiscall on the profile's car list: handle, ?, the ride; 0x140BE0). A handle the
// list has not, a removed add-on's that a race mode's settings kept (0x2ADA23, the settings' +0x10), found no record and
// read through 0 (0x13417B); it is given one the list has
X86_EXTERN_FUNC(__imp__sub_00140BE0);
X86_FUNC(sub_00140BE0) {
    kernel::W32(ctx.esp + 4, game::ListedHandle(ctx.ecx, kernel::R32(ctx.esp + 4)));
    __imp__sub_00140BE0(ctx, base);
}


// An AI racer's vehicle made (thiscall, the pvehicle key; class 3, DRIVER_RACER, 0x172970), from the race's list of
// opponents' cars (0x199024): an add-on's key is given a stock car's
X86_EXTERN_FUNC(__imp__sub_00172730);
X86_FUNC(sub_00172730) {
    if (uint32_t stock = game::StockStandIn(kernel::R32(ctx.esp + 4))) kernel::W32(ctx.esp + 4, stock);
    __imp__sub_00172730(ctx, base);
}

// Car types past the game's 84. The table is read from GLOBALB (0x25D279) and is longer for add-ons; these are the places
// that know its length: its two searches, and three that took 84 and up for no car. 0x2A33EA's loop, a list of their names, stays at 84:
// what reads it is not known. A guest function called from here keeps the registers the x86 conventions say it keeps.
X86_FUNC(sub_00244FE0) {  // thiscall (hash)
    ctx.eax = game::TypeOfName(kernel::R32(ctx.esp + 4));
    ctx.esp += 8;  // a ret pops its address too
}

X86_FUNC(sub_00245030) {  // cdecl (the name's other hash, +0x50): the type's record, 0 if none
    uint32_t hash = kernel::R32(ctx.esp + 4), table = kernel::R32(game::kCarTypes);
    ctx.eax = 0;
    for (uint32_t i = 0, n = game::CarTypes(); i < n && !ctx.eax; i++)
        if (i != game::kStockTypes && kernel::R32(table + i * game::kTypeSize + game::kTypeHash) == hash)
            ctx.eax = table + i * game::kTypeSize;
    ctx.esp += 4;
}

namespace {
bool RealType(uint32_t type) { return type != 0xFFFFFFFF && type != game::kStockTypes && type < game::CarTypes(); }

// 0x263F10 and 0x263FD0, cdecl (a): the car's two render connections, each made as new (size)(a, type, a's +4)
void MakeConnection(X86Context& ctx, uint32_t size, uint32_t ctor) {
    uint32_t a = kernel::R32(ctx.esp + 4), inner = kernel::R32(a + 4);
    uint32_t type = game::TypeOfName(kernel::R32(inner + 4));
    ctx.eax = 0;
    if (RealType(type)) {
        ctx.ecx = 0x4936E8;
        uint32_t mem = kernel::CallGuest(ctx, 0x168F10, {size, 0});
        ctx.eax = 0;
        if (mem) {
            ctx.ecx = mem;
            ctx.eax = kernel::CallGuest(ctx, ctor, {a, type, inner});
        }
    }
    ctx.esp += 4;
}
}  // namespace

X86_FUNC(sub_00263F10) { MakeConnection(ctx, 0x410, 0x262E10); }
X86_FUNC(sub_00263FD0) { MakeConnection(ctx, 0x1C0, 0x263300); }

X86_FUNC(sub_001C9AB0) {  // thiscall (a, flag, c), a car's model ID as this
    uint32_t self = ctx.ecx, a = kernel::R32(ctx.esp + 4), flag = kernel::R32(ctx.esp + 8), c = kernel::R32(ctx.esp + 12);
    kernel::W32(self + 8, 0);
    uint32_t name = kernel::R32(kernel::R32(a + 8) + 0x10 + 0xC);
    if (!name) name = 0x3CA680;
    uint32_t hash = 0xFFFFFFFF;
    for (const auto* p = static_cast<const uint8_t*>(kernel::HostPtr(name)); *p; p++) hash = hash * 33 + *p;
    uint32_t type = game::TypeOfName(hash);
    kernel::W32(self, type);
    if (RealType(type)) {
        uint32_t flags = 0;
        if (kernel::CallGuest(ctx, 0x24FFB0, {type}) & 0xFF) flags |= 4;
        uint32_t one = kernel::CallGuest(ctx, 0x20C360, {}) == 1;
        uint32_t model = kernel::CallGuest(ctx, 0x245380, {type, c, one});
        kernel::W32(self + 4, model);
        if (flag & 0xFF) flags |= 2;
        if (model) flags |= 1;
        kernel::W32(self + 8, flags);
    }
    ctx.eax = self;
    ctx.esp += 16;
}

// The main pool is a fixed 55 MB taken from the kernel at the first allocation (0x421BB8, read by 0x54340), the car pool
// 7550 KB in a race and 6300 KB in the front end out of it (0x433A00, 0x433A04), sized for the disc's cars on a 64 MB box:
// add-on cars ran out of both. With twice the memory (kernel/memory.h) both grow; the track streamer's block is taken with the
// car pool's size added (0x1C7DC3), so its share stays. For every player, add-ons or not, so all play on the one layout.
namespace game {
namespace {

constexpr uint32_t kMainPoolSize = 0x421BB8, kMainPoolExtra = 48u << 20;
constexpr uint32_t kCarPoolRace = 0x433A00, kCarPoolFrontEnd = 0x433A04, kCarPoolExtraKb = 32u << 10;

}  // namespace
}  // namespace game

X86_EXTERN_FUNC(__imp__sub_00054340);
X86_FUNC(sub_00054340) {
    static bool done = false;
    if (!done) {
        done = true;
        kernel::W32(game::kMainPoolSize, kernel::R32(game::kMainPoolSize) + game::kMainPoolExtra);
        kernel::W32(game::kCarPoolRace, kernel::R32(game::kCarPoolRace) + game::kCarPoolExtraKb);
        kernel::W32(game::kCarPoolFrontEnd, kernel::R32(game::kCarPoolFrontEnd) + game::kCarPoolExtraKb);
    }
    __imp__sub_00054340(ctx, base);
}
