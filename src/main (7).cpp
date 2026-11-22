#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <array>
#include <set>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <functional>
#include <string>
#include <vector>

using namespace geode::prelude;

#include "Localization.hpp"

static constexpr float kBaseR = 7.f;    // 이미지 모서리 반지름 (게임 단위)
static constexpr float kGlowPad = 16.f; // 글로우 이미지가 바깥으로 퍼지는 거리
static constexpr float kCap = 16.f;     // 9-slice 최소 크기 (양쪽 모서리 합)
static constexpr float kPi = 3.14159265358979f;

static double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------
// 도우미
// ---------------------------------------------------------------
static float lum(ccColor3B c) {
    return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
}

static ccColor3B lerpC(ccColor3B a, ccColor3B b, float t) {
    t = std::clamp(t, 0.f, 1.f);
    return {
        static_cast<GLubyte>(a.r + (b.r - a.r) * t),
        static_cast<GLubyte>(a.g + (b.g - a.g) * t),
        static_cast<GLubyte>(a.b + (b.b - a.b) * t)
    };
}

static ccColor3B textFor(ccColor3B bg) {
    return lum(bg) > 140.f ? ccColor3B{28, 28, 34} : ccColor3B{255, 255, 255};
}

static int packC(ccColor3B c) {
    return (c.r << 16) | (c.g << 8) | c.b;
}

static ccColor3B unpackC(int v) {
    return {
        static_cast<GLubyte>((v >> 16) & 255),
        static_cast<GLubyte>((v >> 8) & 255),
        static_cast<GLubyte>(v & 255)
    };
}

static std::string fmtNum(float v) {
    char buf[32];
    if (std::fabs(v - std::round(v)) < 0.01f) std::snprintf(buf, sizeof buf, "%.0f", v);
    else std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}

static std::string numStr(double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    return buf;
}

static std::vector<std::string> splitStr(std::string const& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

static std::string cleanStr(std::string s) {
    for (auto& c : s) if (c == ',' || c == ';') c = '_';
    return s;
}

static void setOp(CCScale9Sprite* sp, float a) {
    sp->setOpacity(static_cast<GLubyte>(std::clamp(a, 0.f, 1.f) * 255.f));
}

// 크기와 배율: 내용 크기를 1/s로 두고 s배 키워서 모서리 반지름을 조절한다
static void fitSize(CCScale9Sprite* sp, float w, float h, float s) {
    sp->setContentSize({w / s, h / s});
    sp->setScale(s);
}

static void placeCenter(CCScale9Sprite* sp, float x, float y, float w, float h) {
    sp->setPosition({x + w / 2.f, y + h / 2.f});
}

// 둥근 사각형 꼭짓점 (반시계 방향)
static std::vector<CCPoint> roundedPoly(float x0, float y0, float x1, float y1, float r, int segs) {
    float w = x1 - x0, h = y1 - y0;
    r = std::max(0.5f, std::min(r, std::min(w, h) / 2.f - 0.25f));
    std::vector<CCPoint> v;
    v.reserve(4 * (segs + 1));
    auto arc = [&](float cx, float cy, float a0) {
        for (int i = 0; i <= segs; i++) {
            float a = a0 + (kPi / 2.f) * i / segs;
            v.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
        }
    };
    arc(x1 - r, y1 - r, 0.f);
    arc(x0 + r, y1 - r, kPi / 2.f);
    arc(x0 + r, y0 + r, kPi);
    arc(x1 - r, y0 + r, kPi * 1.5f);
    return v;
}

// CCDrawNode는 미리 곱한 알파를 쓴다
static ccColor4F premulC(ccColor3B c, float a) {
    a = std::clamp(a, 0.f, 1.f);
    return {c.r / 255.f * a, c.g / 255.f * a, c.b / 255.f * a, a};
}

static void dnRect(CCDrawNode* n, float x0, float y0, float x1, float y1, ccColor3B c, float a) {
    CCPoint v[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    auto col = premulC(c, a);
    n->drawPolygon(v, 4, col, 0.f, col);
}

static void dnRound(CCDrawNode* n, float x0, float y0, float x1, float y1, float r,
                    ccColor3B c, float a = 1.f, int segs = 4) {
    auto pts = roundedPoly(x0, y0, x1, y1, r, segs);
    auto col = premulC(c, a);
    n->drawPolygon(pts.data(), static_cast<unsigned int>(pts.size()), col, 0.f, col);
}

static void dnStroke(CCDrawNode* n, float x0, float y0, float x1, float y1, float r,
                     ccColor3B c, float width, int segs = 4) {
    auto pts = roundedPoly(x0, y0, x1, y1, r, segs);
    ccColor4F none = {0.f, 0.f, 0.f, 0.f};
    ccColor4F col = {c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f};
    n->drawPolygon(pts.data(), static_cast<unsigned int>(pts.size()), none, width, col);
}

static void dnStrokeA(CCDrawNode* n, float x0, float y0, float x1, float y1, float r,
                      ccColor3B c, float width, float a, int segs = 4) {
    auto pts = roundedPoly(x0, y0, x1, y1, r, segs);
    ccColor4F none = {0.f, 0.f, 0.f, 0.f};
    auto col = premulC(c, a);
    n->drawPolygon(pts.data(), static_cast<unsigned int>(pts.size()), none, width, col);
}

static CCLabelBMFont* mkLabel(CCNode* parent, std::string const& s, float scale,
                              ccColor3B c, CCPoint pos, float ax) {
    auto l = CCLabelBMFont::create(uiText(s).c_str(), koreanUI() ? "ViewerUI.fnt"_spr : "InterBold.fnt"_spr);
    if (!l) return nullptr;
    l->setScale(scale);
    l->setColor(c);
    l->setPosition(pos);
    l->setAnchorPoint({ax, 0.5f});
    parent->addChild(l);
    return l;
}

// ---------------------------------------------------------------
// 전체 설정 묶음
// ---------------------------------------------------------------
struct Look {
    float size = 44.f;
    float radius = 0.16f; // 박스 크기 대비 모서리 비율
    float gap = 6.f;
    GLubyte tileOpacity = 200;
    ccColor3B tile = {18, 18, 22};
    ccColor3B pressed = {226, 226, 232};
    ccColor3B border = {150, 150, 160};
    float borderW = 0.f;
    ccColor3B accentJump = {255, 255, 255};
    ccColor3B accentLeft = {255, 255, 255};
    ccColor3B accentRight = {255, 255, 255};
    ccColor3B gradEnd = {255, 140, 230};
    bool gradNotes = false;
    bool gradTile = false;
    std::string labelJump = "JUMP";
    std::string labelLeft = "<";
    std::string labelRight = ">";
    float noteSpeed = 260.f;
    float noteHeight = 90.f;
    float noteWidth = 1.0f;
    float noteMin = 18.f;
    float jumpWidth = 1.0f;
    float noteOpacity = 0.92f;
    float noteFade = 1.3f;
    float noteRadius = 0.14f;      // 노트 폭 대비 모서리 비율
    float noteBorder = 0.f;
    ccColor3B noteBorderColor = {255, 255, 255};
    float glowStrength = 1.f;
    float glowSize = 1.f;
    bool glowCustom = false;
    ccColor3B glowColor = {255, 60, 70};
    bool viewerVisible = true;
    bool timeline = true;
    bool counter = true;
    bool kps = true;
    bool total = true;
    bool glow = true;
    bool noteGlow = true;
    bool resetOnRestart = false;
    int keyCode = 0;
    int labelFont = 0;
    float labelSize = 10.f;
    int labelColor = -1;
    bool noteAutoY = false;
    int countArea = 0;
    int countDirection = 0;
    int countAlign = 1;
    float countGap = 3.f;
    bool countFill = true;
    int countColor = -1;
    float countOutline = 0.f;
    ccColor3B countOutlineColor = {0, 0, 0};
    int countFont = 0;
    float countSize = 7.f;
    bool countMotion = false;
    int countMotionType = 0;
    float countMotionAmount = 25.f;
    float countMotionTime = 0.22f;
    float posX = 85.f;
    float posY = 10.f;
};

static ccColor3B accentFor(Look const& l, int button) {
    if (button == 2) return l.accentLeft;
    if (button == 3) return l.accentRight;
    return l.accentJump;
}

static std::string shortLabel(std::string s) {
    if (s.size() > 6) s.resize(6);
    return s;
}

static Look readLook() {
    auto m = Mod::get();
    Look l;
    l.size = static_cast<float>(m->getSettingValue<double>("box-size"));
    l.radius = static_cast<float>(m->getSettingValue<double>("corner-radius")) / 100.f;
    l.gap = static_cast<float>(m->getSettingValue<double>("box-gap"));
    l.tileOpacity = static_cast<GLubyte>(std::clamp<int64_t>(m->getSettingValue<int64_t>("tile-opacity"), 0, 255));
    l.tile = m->getSettingValue<ccColor3B>("tile-color");
    l.pressed = m->getSettingValue<ccColor3B>("pressed-color");
    l.accentJump = m->getSettingValue<ccColor3B>("accent-jump");
    l.accentLeft = m->getSettingValue<ccColor3B>("accent-left");
    l.accentRight = m->getSettingValue<ccColor3B>("accent-right");
    l.gradEnd = m->getSettingValue<ccColor3B>("gradient-color");
    auto gm = m->getSettingValue<std::string>("gradient-mode");
    l.gradNotes = (gm == "Notes" || gm == "Both");
    l.gradTile = (gm == "Tile" || gm == "Both");
    l.labelJump = shortLabel(m->getSettingValue<std::string>("label-jump"));
    l.labelLeft = shortLabel(m->getSettingValue<std::string>("label-left"));
    l.labelRight = shortLabel(m->getSettingValue<std::string>("label-right"));
    l.noteSpeed = static_cast<float>(m->getSettingValue<double>("note-speed"));
    l.noteHeight = static_cast<float>(m->getSettingValue<double>("note-height"));
    l.noteWidth = static_cast<float>(m->getSettingValue<double>("note-width")) / 100.f;
    l.noteMin = static_cast<float>(m->getSettingValue<double>("note-min-length"));
    l.jumpWidth = static_cast<float>(m->getSettingValue<double>("jump-width"));
    l.noteOpacity = static_cast<float>(m->getSettingValue<double>("note-opacity")) / 100.f;
    l.noteFade = static_cast<float>(m->getSettingValue<double>("note-fade"));
    l.noteRadius = static_cast<float>(m->getSettingValue<double>("note-radius")) / 100.f;
    l.noteBorder = static_cast<float>(m->getSettingValue<double>("note-border"));
    l.noteBorderColor = m->getSettingValue<ccColor3B>("note-border-color");
    l.glowStrength = static_cast<float>(m->getSettingValue<double>("note-glow-strength")) / 100.f;
    l.glowSize = static_cast<float>(m->getSettingValue<double>("note-glow-size")) / 100.f;
    l.glowCustom = m->getSettingValue<bool>("note-glow-custom");
    l.glowColor = m->getSettingValue<ccColor3B>("note-glow-color");
    l.viewerVisible = m->getSettingValue<bool>("show-viewer");
    l.timeline = m->getSettingValue<bool>("show-timeline");
    l.counter = m->getSettingValue<bool>("show-counter");
    l.kps = m->getSettingValue<bool>("show-kps");
    l.total = m->getSettingValue<bool>("show-total");
    l.glow = m->getSettingValue<bool>("show-glow");
    l.noteGlow = m->getSettingValue<bool>("note-glow");
    l.resetOnRestart = m->getSettingValue<bool>("reset-on-restart");
    l.posX = static_cast<float>(m->getSettingValue<double>("pos-x"));
    l.posY = static_cast<float>(m->getSettingValue<double>("pos-y"));
    return l;
}

// ---------------------------------------------------------------
// 키 목록 (에디터에서 추가, 삭제, 수정하고 저장된다)
// 입력 종류(map): 0 점프 P1, 1 왼쪽 P1, 2 오른쪽 P1, 3 점프 P2, 4 왼쪽 P2, 5 오른쪽 P2
// ---------------------------------------------------------------
using StyleValues = std::map<std::string, double>;

static std::string encodeStyle(StyleValues const& values) {
    std::string result;
    for (auto const& [key, value] : values) {
        result += key + "=" + numStr(value) + "|";
    }
    return result;
}

static StyleValues decodeStyle(std::string const& encoded) {
    StyleValues values;
    for (auto const& item : splitStr(encoded, '|')) {
        auto eq = item.find('=');
        if (eq == std::string::npos) continue;
        auto number = item.substr(eq + 1);
        char* end = nullptr;
        double v = std::strtod(number.c_str(), &end);
        if (end != number.c_str() && *end == 0 && std::isfinite(v))
            values[item.substr(0, eq)] = v;
    }
    return values;
}

struct KeyDef {
    StyleValues style;
    std::string text;
    int id = 0;
    int map = 0;
    float x = 0.f, y = 0.f;      // 키뷰어 기준 왼쪽 아래 위치
    float w = 0.f, h = 0.f;      // 0 = 전체 설정의 기본 크기
    int bg = -1, pressed = -1, border = -1, accent = -1; // 0xRRGGBB, -1 = 전체 설정 사용
    float borderW = 0.f;
    float radius = -1.f;         // %, -1 = 전체 설정 사용
    int label = 0;               // 0 = 자동, 그 외 kLabels 번호
};

static char const* const kLabels[] = {
    "(auto)", "JUMP", "Z", "X", "C", "V", "SPACE", "UP", "<", ">", "A", "S", "D", "W",
    "K", "L", "1", "2", "3", "4", "P1", "P2"
};
static constexpr int kLabelCount = static_cast<int>(sizeof(kLabels) / sizeof(kLabels[0]));
static char const* const kMapNames[6] = {"Jump P1", "Left P1", "Right P1", "Jump P2", "Left P2", "Right P2"};
static char const* const kGradNames[4] = {"Off", "Notes", "Tile", "Both"};

static std::vector<KeyDef> g_keys;
static int g_nextId = 0;
static bool g_keysLoaded = false;

static KeyDef* keyById(int id) {
    for (auto& k : g_keys) if (k.id == id) return &k;
    return nullptr;
}

static std::string kf(int id, char const* field) {
    return "k" + std::to_string(id) + "_" + field;
}

static void saveKeyList() {
    std::string ids;
    for (auto const& k : g_keys) {
        if (!ids.empty()) ids += ",";
        ids += std::to_string(k.id);
    }
    Mod::get()->setSavedValue<std::string>("key_ids", ids);
    Mod::get()->setSavedValue<int64_t>("key_next", g_nextId);
}

static void saveKey(KeyDef const& k) {
    auto m = Mod::get();
    m->setSavedValue<std::string>(kf(k.id, "text"), k.text);
    m->setSavedValue<std::string>(kf(k.id, "style"), encodeStyle(k.style));
    m->setSavedValue<int64_t>(kf(k.id, "map"), k.map);
    m->setSavedValue<double>(kf(k.id, "x"), k.x);
    m->setSavedValue<double>(kf(k.id, "y"), k.y);
    m->setSavedValue<double>(kf(k.id, "w"), k.w);
    m->setSavedValue<double>(kf(k.id, "h"), k.h);
    m->setSavedValue<double>(kf(k.id, "bw"), k.borderW);
    m->setSavedValue<double>(kf(k.id, "r"), k.radius);
    m->setSavedValue<int64_t>(kf(k.id, "bg"), k.bg);
    m->setSavedValue<int64_t>(kf(k.id, "pr"), k.pressed);
    m->setSavedValue<int64_t>(kf(k.id, "bc"), k.border);
    m->setSavedValue<int64_t>(kf(k.id, "ac"), k.accent);
    m->setSavedValue<int64_t>(kf(k.id, "lb"), k.label);
}

static KeyDef loadKey(int id) {
    auto m = Mod::get();
    KeyDef k;
    k.id = id;
    k.text = m->getSavedValue<std::string>(kf(id, "text"), "");
    k.style = decodeStyle(m->getSavedValue<std::string>(kf(id, "style"), ""));
    k.map = static_cast<int>(m->getSavedValue<int64_t>(kf(id, "map"), 0));
    k.x = static_cast<float>(m->getSavedValue<double>(kf(id, "x"), 0.0));
    k.y = static_cast<float>(m->getSavedValue<double>(kf(id, "y"), 0.0));
    k.w = static_cast<float>(m->getSavedValue<double>(kf(id, "w"), 0.0));
    k.h = static_cast<float>(m->getSavedValue<double>(kf(id, "h"), 0.0));
    k.borderW = static_cast<float>(m->getSavedValue<double>(kf(id, "bw"), 0.0));
    k.radius = static_cast<float>(m->getSavedValue<double>(kf(id, "r"), -1.0));
    k.bg = static_cast<int>(m->getSavedValue<int64_t>(kf(id, "bg"), -1));
    k.pressed = static_cast<int>(m->getSavedValue<int64_t>(kf(id, "pr"), -1));
    k.border = static_cast<int>(m->getSavedValue<int64_t>(kf(id, "bc"), -1));
    k.accent = static_cast<int>(m->getSavedValue<int64_t>(kf(id, "ac"), -1));
    k.label = static_cast<int>(m->getSavedValue<int64_t>(kf(id, "lb"), 0));
    k.map = std::clamp(k.map, 0, 5);
    k.label = std::clamp(k.label, 0, kLabelCount - 1);
    return k;
}

// 처음 실행할 때: 왼쪽, 점프, 오른쪽 세 키를 한 줄로 놓는다 (왼쪽/오른쪽은 플랫포머에서만 보인다)
static void createDefaultKeys() {
    Look l = readLook();
    float s = l.size, gap = l.gap;
    float jw = s * std::max(0.5f, l.jumpWidth);
    float total = s + gap + jw + gap + s;
    float x = -total / 2.f;
    int maps[3] = {1, 0, 2};
    float ws[3] = {s, jw, s};
    g_keys.clear();
    for (int i = 0; i < 3; i++) {
        KeyDef k;
        k.id = g_nextId++;
        k.map = maps[i];
        k.x = x;
        k.y = 0.f;
        g_keys.push_back(k);
        x += ws[i] + gap;
    }
    saveKeyList();
    for (auto const& k : g_keys) saveKey(k);
}

static void loadKeys() {
    if (g_keysLoaded) return;
    g_keysLoaded = true;
    auto m = Mod::get();
    std::string ids = m->getSavedValue<std::string>("key_ids", "");
    g_nextId = static_cast<int>(m->getSavedValue<int64_t>("key_next", 0));
    g_keys.clear();
    for (auto const& tok : splitStr(ids, ',')) {
        if (tok.empty()) continue;
        int id = std::atoi(tok.c_str());
        g_keys.push_back(loadKey(id));
        g_nextId = std::max(g_nextId, id + 1);
    }
    if (g_keys.empty()) createDefaultKeys();
}

// 전체 설정에 키별 설정을 덮어씌운다
static Look lookForKey(Look const& base, KeyDef const& k) {
    Look kl = base;
    int button = (k.map % 3) + 1;
    ccColor3B acc = k.accent >= 0 ? unpackC(k.accent) : accentFor(base, button);
    kl.accentJump = acc;
    kl.accentLeft = acc;
    kl.accentRight = acc;
    if (k.bg >= 0) kl.tile = unpackC(k.bg);
    if (k.pressed >= 0) kl.pressed = unpackC(k.pressed);
    if (k.border >= 0) kl.border = unpackC(k.border);
    kl.borderW = k.borderW;
    if (k.radius >= 0.f) kl.radius = k.radius / 100.f;
    for (auto const& [key, v] : k.style) {
        if (key == "key-code") kl.keyCode = static_cast<int>(std::clamp(v, 0., 65535.));
        if (key == "label-font") kl.labelFont = static_cast<int>(std::clamp(v, 0., 3.));
        if (key == "label-size") kl.labelSize = static_cast<float>(std::clamp(v, 4., 40.));
        if (key == "label-color") kl.labelColor = static_cast<int>(std::clamp(v, 0., 16777215.));
        if (key == "note-auto-y") kl.noteAutoY = v != 0;
        if (key == "count-area") kl.countArea = static_cast<int>(std::clamp(v, 0., 1.));
        if (key == "count-direction") kl.countDirection = static_cast<int>(std::clamp(v, 0., 3.));
        if (key == "count-align") kl.countAlign = static_cast<int>(std::clamp(v, 0., 2.));
        if (key == "count-gap") kl.countGap = static_cast<float>(std::clamp(v, 0., 40.));
        if (key == "count-fill") kl.countFill = v != 0;
        if (key == "count-color") kl.countColor = static_cast<int>(std::clamp(v, 0., 16777215.));
        if (key == "count-stroke") kl.countOutline = static_cast<float>(std::clamp(v, 0., 4.));
        if (key == "count-stroke-color") kl.countOutlineColor = unpackC(static_cast<int>(std::clamp(v, 0., 16777215.)));
        if (key == "count-font") kl.countFont = static_cast<int>(std::clamp(v, 0., 3.));
        if (key == "count-size") kl.countSize = static_cast<float>(std::clamp(v, 4., 40.));
        if (key == "count-motion") kl.countMotion = v != 0;
        if (key == "count-motion-type") kl.countMotionType = v != 0;
        if (key == "count-motion-amount") kl.countMotionAmount = static_cast<float>(std::clamp(v, 0., 100.));
        if (key == "count-motion-time") kl.countMotionTime = static_cast<float>(std::clamp(v, 0.05, 1.0));
        if (key == "note-width") kl.noteWidth = static_cast<float>(std::clamp(v, 20.0, 100.0)) / 100.f;
        if (key == "note-min-length") kl.noteMin = static_cast<float>(std::clamp(v, 2.0, 60.0));
        if (key == "note-speed") kl.noteSpeed = static_cast<float>(std::clamp(v, 100.0, 600.0));
        if (key == "note-height") kl.noteHeight = static_cast<float>(std::clamp(v, 40.0, 220.0));
        if (key == "note-opacity") kl.noteOpacity = static_cast<float>(std::clamp(v, 20.0, 100.0)) / 100.f;
        if (key == "note-fade") kl.noteFade = static_cast<float>(std::clamp(v, 0.0, 3.0));
        if (key == "note-radius") kl.noteRadius = static_cast<float>(std::clamp(v, 0.0, 50.0)) / 100.f;
        if (key == "note-border") kl.noteBorder = static_cast<float>(std::clamp(v, 0.0, 4.0));
        if (key == "note-glow-strength") kl.glowStrength = static_cast<float>(std::clamp(v, 0.0, 200.0)) / 100.f;
        if (key == "note-glow-size") kl.glowSize = static_cast<float>(std::clamp(v, 50.0, 250.0)) / 100.f;
        if (key == "tile-opacity") kl.tileOpacity = static_cast<GLubyte>(std::clamp(v, 0.0, 255.0));
        if (key == "note-glow-custom") kl.glowCustom = v != 0;
        if (key == "show-glow") kl.glow = v != 0;
        if (key == "note-glow") kl.noteGlow = v != 0;
        if (key == "show-counter") kl.counter = v != 0;
        if (key == "show-timeline") kl.timeline = v != 0;
        if (key == "gradient-color") kl.gradEnd = unpackC(static_cast<int>(std::clamp(v, 0.0, 16777215.0)));
        if (key == "note-glow-color") kl.glowColor = unpackC(static_cast<int>(std::clamp(v, 0.0, 16777215.0)));
        if (key == "note-border-color") kl.noteBorderColor = unpackC(static_cast<int>(std::clamp(v, 0.0, 16777215.0)));
        if (key == "gradient-mode") {
            kl.gradNotes = v == 1 || v == 3;
            kl.gradTile = v == 2 || v == 3;
        }
    }
    return kl;
}

static std::string keyName(int code) {
    auto name = CCDirector::get()->getKeyboardDispatcher()->keyToString(static_cast<enumKeyCodes>(code));
    return name && *name ? name : std::to_string(code);
}

static std::string labelFor(KeyDef const& k, Look const& l) {
    if (!k.text.empty()) return k.text;
    if (k.label > 0 && k.label < kLabelCount) return kLabels[k.label];
    if (auto it = k.style.find("key-code"); it != k.style.end() && it->second > 0)
        return keyName(static_cast<int>(std::clamp(it->second, 1., 65535.)));
    switch (k.map) {
        case 0: return l.labelJump;
        case 1: return l.labelLeft;
        case 2: return l.labelRight;
        case 3: return "JUMP2";
        case 4: return l.labelLeft;
        default: return l.labelRight;
    }
}

static float autoWidth(KeyDef const& k, Look const& l) {
    return (k.map % 3 == 0) ? l.size * std::max(0.5f, l.jumpWidth) : l.size;
}

// ---------------------------------------------------------------
// 사용자 프리셋 (현재 설정과 키 목록 전체를 이름 붙여 저장한다)
// ---------------------------------------------------------------
enum class SetType { Bool, Num, Int, Color, Str };
struct SetDef {
    char const* key;
    SetType type;
};

static SetDef const kSetDefs[] = {
    {"show-viewer", SetType::Bool}, {"show-timeline", SetType::Bool}, {"show-counter", SetType::Bool}, {"show-kps", SetType::Bool},
    {"show-total", SetType::Bool}, {"show-glow", SetType::Bool}, {"note-glow", SetType::Bool},
    {"reset-on-restart", SetType::Bool},
    {"box-size", SetType::Num}, {"jump-width", SetType::Num}, {"corner-radius", SetType::Num},
    {"box-gap", SetType::Num}, {"note-width", SetType::Num}, {"note-min-length", SetType::Num},
    {"note-speed", SetType::Num}, {"note-height", SetType::Num}, {"pos-x", SetType::Num},
    {"pos-y", SetType::Num},
    {"tile-opacity", SetType::Int},
    {"note-opacity", SetType::Num}, {"note-fade", SetType::Num}, {"note-radius", SetType::Num},
    {"note-border", SetType::Num}, {"note-glow-strength", SetType::Num}, {"note-glow-size", SetType::Num},
    {"note-glow-custom", SetType::Bool}, {"note-border-color", SetType::Color}, {"note-glow-color", SetType::Color},
    {"tile-color", SetType::Color}, {"pressed-color", SetType::Color}, {"accent-jump", SetType::Color},
    {"accent-left", SetType::Color}, {"accent-right", SetType::Color}, {"gradient-color", SetType::Color},
    {"gradient-mode", SetType::Str}, {"label-jump", SetType::Str}, {"label-left", SetType::Str},
    {"label-right", SetType::Str}
};

static std::string serializePreset() {
    auto m = Mod::get();
    std::string out;
    for (auto const& d : kSetDefs) {
        std::string k = d.key;
        std::string v;
        switch (d.type) {
            case SetType::Bool: v = m->getSettingValue<bool>(k) ? "1" : "0"; break;
            case SetType::Num: v = numStr(m->getSettingValue<double>(k)); break;
            case SetType::Int: v = std::to_string(m->getSettingValue<int64_t>(k)); break;
            case SetType::Color: v = std::to_string(packC(m->getSettingValue<ccColor3B>(k))); break;
            case SetType::Str: v = cleanStr(m->getSettingValue<std::string>(k)); break;
        }
        out += "S," + k + "," + v + ";";
    }
    for (auto const& k : g_keys) {
        out += "K," + std::to_string(k.id) + "," + std::to_string(k.map) + "," + numStr(k.x) + "," +
               numStr(k.y) + "," + numStr(k.w) + "," + numStr(k.h) + "," + std::to_string(k.bg) + "," +
               std::to_string(k.pressed) + "," + std::to_string(k.border) + "," + std::to_string(k.accent) + "," +
               numStr(k.borderW) + "," + numStr(k.radius) + "," + std::to_string(k.label) + "," + encodeStyle(k.style) + "," + cleanStr(k.text) + ";";
    }
    return out;
}

static bool applyPresetData(std::string const& data) {
    auto m = Mod::get();
    std::vector<KeyDef> keys;
    for (auto const& rec : splitStr(data, ';')) {
        if (rec.empty()) continue;
        auto f = splitStr(rec, ',');
        if (f[0] == "K" && f.size() >= 14) {
            KeyDef k;
            k.id = std::atoi(f[1].c_str());
            k.map = std::clamp(std::atoi(f[2].c_str()), 0, 5);
            k.x = static_cast<float>(std::atof(f[3].c_str()));
            k.y = static_cast<float>(std::atof(f[4].c_str()));
            k.w = static_cast<float>(std::atof(f[5].c_str()));
            k.h = static_cast<float>(std::atof(f[6].c_str()));
            k.bg = std::atoi(f[7].c_str());
            k.pressed = std::atoi(f[8].c_str());
            k.border = std::atoi(f[9].c_str());
            k.accent = std::atoi(f[10].c_str());
            k.borderW = static_cast<float>(std::atof(f[11].c_str()));
            k.radius = static_cast<float>(std::atof(f[12].c_str()));
            k.label = std::clamp(std::atoi(f[13].c_str()), 0, kLabelCount - 1);
            if (f.size() >= 15) k.style = decodeStyle(f[14]);
            if (f.size() >= 16) k.text = f[15].substr(0, 24);
            if (k.id < 0 || k.id == INT32_MAX || !std::isfinite(k.x) || !std::isfinite(k.y) ||
                !std::isfinite(k.w) || !std::isfinite(k.h) ||
                std::any_of(keys.begin(), keys.end(), [&](KeyDef const& prior) { return prior.id == k.id; }))
                return false;
            keys.push_back(k);
        }
    }
    if (keys.empty()) return false;
    for (auto const& record : splitStr(data, ';')) {
        auto f = splitStr(record, ',');
        if (f[0] == "S" && f.size() >= 3) {
            for (auto const& d : kSetDefs) {
                if (f[1] != d.key) continue;
                std::string k = d.key;
                switch (d.type) {
                    case SetType::Bool: m->setSettingValue<bool>(k, f[2] == "1"); break;
                    case SetType::Num: m->setSettingValue<double>(k, std::atof(f[2].c_str())); break;
                    case SetType::Int: m->setSettingValue<int64_t>(k, std::atoll(f[2].c_str())); break;
                    case SetType::Color: m->setSettingValue<ccColor3B>(k, unpackC(std::atoi(f[2].c_str()))); break;
                    case SetType::Str: m->setSettingValue<std::string>(k, f[2]); break;
                }
                break;
            }
        }
    }
    {

        g_keys = keys;
        for (auto const& k : g_keys) g_nextId = std::max(g_nextId, k.id + 1);
        saveKeyList();
        for (auto const& k : g_keys) saveKey(k);
    }
    return true;
}

static int presetCount() {
    return static_cast<int>(Mod::get()->getSavedValue<int64_t>("pr_count", 0));
}

static std::string presetName(int i) {
    return Mod::get()->getSavedValue<std::string>("pr_" + std::to_string(i) + "_name", "Preset");
}

static std::string presetData(int i) {
    return Mod::get()->getSavedValue<std::string>("pr_" + std::to_string(i) + "_data", "");
}

static void presetSetData(int i, std::string const& data) {
    Mod::get()->setSavedValue<std::string>("pr_" + std::to_string(i) + "_data", data);
}

static void presetAdd(std::string const& data) {
    auto m = Mod::get();
    int n = presetCount();
    int next = static_cast<int>(m->getSavedValue<int64_t>("pr_next", 1));
    m->setSavedValue<std::string>("pr_" + std::to_string(n) + "_name", "Preset " + std::to_string(next));
    m->setSavedValue<std::string>("pr_" + std::to_string(n) + "_data", data);
    m->setSavedValue<int64_t>("pr_next", next + 1);
    m->setSavedValue<int64_t>("pr_count", n + 1);
}

static void presetDelete(int i) {
    auto m = Mod::get();
    int n = presetCount();
    for (int j = i; j < n - 1; j++) {
        m->setSavedValue<std::string>("pr_" + std::to_string(j) + "_name", presetName(j + 1));
        m->setSavedValue<std::string>("pr_" + std::to_string(j) + "_data", presetData(j + 1));
    }
    m->setSavedValue<int64_t>("pr_count", std::max(0, n - 1));
}

// ---------------------------------------------------------------
// 노트 하나가 쓰는 이미지 3장 (글로우, 본체, 색 그라데이션)
// ---------------------------------------------------------------
static std::array<char const*, 4> const kFonts = {
    "InterBold.fnt"_spr, "ViewerUI.fnt"_spr, "bigFont.fnt", "goldFont.fnt"
};

static void setFont(CCLabelBMFont* label, int font) {
    auto file = kFonts[font];
    if (std::string_view(label->getFntFile()) != file) label->setFntFile(file);
}

static float labelScale(CCLabelBMFont* label, float height, float maxWidth) {
    auto size = label->getContentSize();
    return std::min(height / std::max(1.f, size.height), maxWidth / std::max(1.f, size.width));
}

struct NoteSprites {
    CCScale9Sprite* glow = nullptr;
    CCScale9Sprite* base = nullptr;
    CCScale9Sprite* grad = nullptr;
};

struct Note {
    int source = 0;
    double start = 0.0;
    double end = -1.0; // 아직 누르는 중이면 -1
    NoteSprites sp;
    float lw = -1.f, lh = -1.f, ls = -1.f, lg = -1.f; // 마지막으로 적용한 크기 (불필요한 재계산 방지)
};

// ---------------------------------------------------------------
// 키 하나(타일 + 글로우 + 카운터 + 노트)
// ---------------------------------------------------------------
class KeyLane : public CCNode {
protected:
    unsigned m_held = 0;
    bool m_dirty = true;
    int m_presses = 0;
    float m_w = 0.f, m_h = 0.f;
    std::array<CCLabelBMFont*, 8> m_countOutline{};
    double m_countStart = -1.0;
    float m_noteSpace = 0.f;
    float m_noteStart = 0.f;
    float m_press = 0.f; // 0 = 평소, 1 = 눌림 (부드럽게 변함)
    float m_lw = -1.f, m_lh = -1.f, m_ls = -1.f;
    Look m_look;
    std::string m_nameStr;
    CCScale9Sprite* m_glow = nullptr;
    CCScale9Sprite* m_tile = nullptr;
    CCScale9Sprite* m_tileGrad = nullptr;
    CCDrawNode* m_border = nullptr;
    CCNode* m_noteLayer = nullptr;
    CCDrawNode* m_noteBorder = nullptr;
    CCLabelBMFont* m_name = nullptr;
    CCLabelBMFont* m_count = nullptr;
    std::deque<Note> m_notes;
    std::vector<NoteSprites> m_pool;

    bool init(std::string const& name) {
        if (!CCNode::init()) return false;
        m_nameStr = name;

        m_glow = CCScale9Sprite::create("kv_glow.png"_spr);
        m_tile = CCScale9Sprite::create("kv_solid.png"_spr);
        m_tileGrad = CCScale9Sprite::create("kv_grad2.png"_spr);
        if (!m_glow || !m_tile || !m_tileGrad) return false;

        m_noteLayer = CCNode::create();
        this->addChild(m_noteLayer, -1);
        m_noteBorder = CCDrawNode::create();
        m_noteLayer->addChild(m_noteBorder, 5);
        this->addChild(m_glow, -2);
        this->addChild(m_tile, 0);
        this->addChild(m_tileGrad, 1);

        m_border = CCDrawNode::create();
        this->addChild(m_border, 2);

        m_name = CCLabelBMFont::create(name.c_str(), "InterBold.fnt"_spr);
        this->addChild(m_name, 3);

        m_count = CCLabelBMFont::create("0", "InterBold.fnt"_spr);
        for (auto& border : m_countOutline) {
            border = CCLabelBMFont::create("0", kFonts[0]);
            this->addChild(border, 3);
        }
        this->addChild(m_count, 4);
        return true;
    }

    ccColor3B accent() const {
        return m_look.accentJump; // 키마다 자신의 색으로 이미 정리되어 들어온다
    }

    NoteSprites acquire() {
        if (!m_pool.empty()) {
            auto s = m_pool.back();
            m_pool.pop_back();
            return s;
        }
        NoteSprites s;
        s.glow = CCScale9Sprite::create("kv_glow.png"_spr);
        s.base = CCScale9Sprite::create("kv_fade2.png"_spr);
        s.grad = CCScale9Sprite::create("kv_grad2.png"_spr);
        if (!s.glow || !s.base || !s.grad) return NoteSprites{};
        m_noteLayer->addChild(s.glow, 0);
        m_noteLayer->addChild(s.base, 1);
        m_noteLayer->addChild(s.grad, 2);
        return s;
    }

    void release(NoteSprites const& s) {
        if (!s.base) return;
        s.glow->setVisible(false);
        s.base->setVisible(false);
        s.grad->setVisible(false);
        m_pool.push_back(s);
    }

    void fitCount(double now = nowSec()) {
        const bool outside = m_look.countArea == 1;
        float scale = labelScale(m_count, m_look.countSize, outside ? 400.f : m_w * 0.84f);
        float width = m_count->getContentSize().width * scale;
        float height = m_count->getContentSize().height * scale;
        float t = static_cast<float>((now - m_countStart) / m_look.countMotionTime);
        float motion = m_look.countMotion && m_countStart >= 0.0 && t >= 0.f && t < 1.f
            ? m_look.countMotionAmount / 100.f * (1.f - t) : 0.f;
        if (m_look.countMotionType == 0) scale *= 1.f + motion;
        float gap = m_look.countGap;
        float align = m_look.countAlign * 0.5f;
        float x = width / 2 + (m_w - width) * align;
        float y = m_h * 0.27f;
        if (m_look.countDirection <= 1) {
            if (outside) y = m_look.countDirection == 0 ? -gap - height / 2 : m_h + gap + height / 2;
            else if (m_look.countDirection == 1) y = m_h * 0.73f;
        } else {
            const bool left = m_look.countDirection == 2;
            x = left ? width / 2 + gap : m_w - width / 2 - gap;
            if (outside) x = left ? -width / 2 - gap : m_w + width / 2 + gap;
            y = height / 2 + (m_h - height) * align;
        }
        if (m_look.countMotionType == 1) y += 16.f * motion;
        m_count->setPosition({x, y});
        m_count->setScale(scale);
        m_count->setVisible(m_look.counter && m_look.countFill);
        for (size_t i = 0; i < m_countOutline.size(); ++i) {
            auto border = m_countOutline[i];
            float angle = i * kPi / 4.f;
            float offset = m_look.countOutline;
            border->setPosition({x + std::cos(angle) * offset, y + std::sin(angle) * offset});
            border->setScale(scale);
            border->setVisible(m_look.counter && offset > 0.f);
        }
        m_noteStart = m_h + 5.f;
        if (m_look.noteAutoY && m_look.counter && outside && m_look.countDirection == 1 &&
            (m_look.countFill || m_look.countOutline > 0))
            m_noteStart = std::max(m_noteStart, y + height / 2 + m_look.countOutline + 5.f);
    }

    void setCountText() {
        auto text = std::to_string(m_presses);
        m_count->setString(text.c_str());
        for (auto border : m_countOutline) border->setString(text.c_str());
    }

    void drawBorder() {
        m_border->clear();
        if (m_look.borderW <= 0.f) return;
        float r = std::min(std::max(m_look.radius * m_h, 2.f), std::min(m_w, m_h) / 2.f);
        dnStroke(m_border, 0.5f, 0.5f, m_w - 0.5f, m_h - 0.5f, r, m_look.border, m_look.borderW, 5);
    }

    // 눌림 정도에 따라 색과 투명도를 갱신 (이미지 색 입히기라 선이 생기지 않는다)
    void refreshTile() {
        float p = m_press;
        float idleA = m_look.tileOpacity / 255.f;

        ccColor3B bottom = lerpC(m_look.tile, m_look.pressed, p);
        m_tile->setColor(bottom);
        setOp(m_tile, idleA + (1.f - idleA) * p);

        if (m_look.gradTile && p > 0.01f) {
            m_tileGrad->setVisible(true);
            m_tileGrad->setColor(m_look.gradEnd);
            setOp(m_tileGrad, p);
        } else {
            m_tileGrad->setVisible(false);
        }

        bool showGlow = m_look.glow && p > 0.02f;
        m_glow->setVisible(showGlow);
        if (showGlow) {
            m_glow->setColor(this->accent());
            setOp(m_glow, 0.6f * p);
        }

        // 글자 색: 배경 밝기에 맞춰 자동 대비
        ccColor3B pressBg = m_look.gradTile ? lerpC(m_look.pressed, m_look.gradEnd, 0.5f) : m_look.pressed;
        ccColor3B idleText = m_look.tileOpacity < 128 ? ccColor3B{255, 255, 255} : textFor(m_look.tile);
        ccColor3B text = lerpC(idleText, textFor(pressBg), p);
        ccColor3B bg = lerpC(m_look.tile, pressBg, p);
        m_name->setColor(m_look.labelColor < 0 ? text : unpackC(m_look.labelColor));
        m_count->setColor(m_look.countColor < 0
            ? (m_look.countArea == 0 ? lerpC(text, bg, 0.45f) : ccColor3B{255, 255, 255})
            : unpackC(m_look.countColor));
        for (auto border : m_countOutline) border->setColor(m_look.countOutlineColor);
    }

public:
    static KeyLane* create(std::string const& name) {
        auto ret = new KeyLane();
        if (ret->init(name)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void setName(std::string const& name) {
        if (name == m_nameStr) return;
        m_nameStr = name;
        m_name->setString(name.c_str());
    }

    void layout(Look const& l, float w, float h) {
        m_look = l;
        m_w = w;
        m_h = h;
        this->setContentSize({m_w, m_h});

        float r = std::min(std::max(l.radius * m_h, 2.f), m_h / 2.f);
        float s = std::clamp(r / kBaseR, 0.2f, std::max(0.2f, std::min(m_w, m_h) / kCap));
        if (m_lw != m_w || m_lh != m_h || m_ls != s) {
            m_lw = m_w; m_lh = m_h; m_ls = s;
            fitSize(m_tile, m_w, m_h, s);
            fitSize(m_tileGrad, m_w, m_h, s);
            fitSize(m_glow, m_w + 2 * kGlowPad, m_h + 2 * kGlowPad, 1.f);
        }
        placeCenter(m_tile, 0.f, 0.f, m_w, m_h);
        placeCenter(m_tileGrad, 0.f, 0.f, m_w, m_h);
        placeCenter(m_glow, -kGlowPad, -kGlowPad, m_w + 2 * kGlowPad, m_h + 2 * kGlowPad);

        setFont(m_name, l.labelFont);
        setFont(m_count, l.countFont);
        for (auto border : m_countOutline) setFont(border, l.countFont);
        m_name->setScale(labelScale(m_name, l.labelSize, m_w * 0.84f));
        float labelY = m_h / 2.f;
        if (l.counter && l.countArea == 0 && (l.countFill || l.countOutline > 0)) {
            if (l.countDirection == 0) labelY = m_h * 0.65f;
            if (l.countDirection == 1) labelY = m_h * 0.35f;
        }
        m_name->setPosition({m_w / 2, labelY});
        this->fitCount();

        this->drawBorder();
        m_dirty = true;
    }

    // 상태가 실제로 바뀌었을 때만 true (같은 입력이 두 번 들어와도 한 번만 센다)
    void input(int source, bool down, double t, bool drawNotes) {
        const unsigned bit = 1u << source;
        if (down) {
            m_held |= bit;
            ++m_presses;
            m_countStart = t;
            setCountText();
            fitCount(t);
            if (drawNotes && m_look.timeline) {
                Note n;
                n.source = source;
                n.start = t;
                n.sp = acquire();
                m_notes.push_back(n);
            }
        } else if (m_held & bit) {
            m_held &= ~bit;
            for (auto it = m_notes.rbegin(); it != m_notes.rend(); ++it) {
                if (it->source == source && it->end < 0) {
                    it->end = t;
                    break;
                }
            }
        }
    }

    void resetInput() {
        m_held = 0;
        clearNotes();
    }

    void resetCount() {
        m_presses = 0;
        m_countStart = -1.0;
        setCountText();
        fitCount();
    }

    void clearNotes() {
        for (auto& n : m_notes) this->release(n.sp);
        m_notes.clear();
        m_noteBorder->clear();
    }

    // 노트: 둥근 막대 + 부드러운 빛 번짐 + 선택형 테두리. 모두 이미지 한 장씩이라 선이 생기지 않는다
    void tickNotes(double now) {
        m_noteBorder->clear();
        if (!m_look.timeline) {
            if (!m_notes.empty()) this->clearNotes();
            return;
        }

        float H = m_look.noteHeight;
        if (m_look.noteAutoY) H = std::clamp(m_noteSpace - m_noteStart, 0.f, H);
        if (H < kCap) {
            clearNotes();
            return;
        }
        float sp = m_look.noteSpeed;
        float minLen = std::max(kCap, m_look.noteMin);

        for (auto it = m_notes.begin(); it != m_notes.end();) {
            if (it->end >= 0 && (now - it->end) * sp >= H) {
                this->release(it->sp);
                it = m_notes.erase(it);
            } else {
                ++it;
            }
        }

        float baseY = m_noteStart;
        float nw = std::max(kCap, std::clamp(m_look.noteWidth, 0.2f, 1.f) * m_w);
        float x0 = (m_w - nw) / 2.f;
        float noteR = std::clamp(m_look.noteRadius * nw, 1.5f, 16.f);
        float sg = std::clamp(m_look.glowSize, 0.3f, 3.f);
        float baseOp = std::clamp(m_look.noteOpacity, 0.05f, 1.f);
        auto acc = this->accent();
        bool grad = m_look.gradNotes;
        ccColor3B glowCol = m_look.glowCustom ? m_look.glowColor
                                              : (grad ? lerpC(acc, m_look.gradEnd, 0.5f) : acc);

        for (auto& n : m_notes) {
            auto& s = n.sp;
            if (!s.base) continue;

            double e = n.end < 0 ? now : n.end;
            float yb = static_cast<float>((now - e) * sp);
            if (yb >= H) {
                s.base->setVisible(false);
                s.grad->setVisible(false);
                s.glow->setVisible(false);
                continue;
            }
            float yt = static_cast<float>((now - n.start) * sp);
            float len = std::max(minLen, yt - yb);
            len = std::min(len, std::max(kCap, H - yb));

            float fade = std::pow(1.f - yb / H, std::max(0.f, m_look.noteFade));
            // 모서리 이미지가 노트 길이의 1/8을 넘지 않게 해서 그라데이션이 매끄럽게 이어진다
            float sN = std::clamp(noteR / kBaseR, 0.2f, std::max(0.2f, std::min(nw / kCap, len / 64.f)));
            float py = baseY + yb;
            float pad = kGlowPad * sg;

            if (n.lw != nw || n.lh != len || n.ls != sN || n.lg != sg) {
                n.lw = nw; n.lh = len; n.ls = sN; n.lg = sg;
                fitSize(s.base, nw, len, sN);
                fitSize(s.grad, nw, len, sN);
                fitSize(s.glow, nw + 2 * pad, len + 2 * pad, sg);
            }
            placeCenter(s.base, x0, py, nw, len);
            placeCenter(s.grad, x0, py, nw, len);
            placeCenter(s.glow, x0 - pad, py - pad, nw + 2 * pad, len + 2 * pad);

            float op = baseOp * fade;
            s.base->setVisible(true);
            s.base->setColor(acc);
            setOp(s.base, op);

            s.grad->setVisible(grad);
            if (grad) {
                s.grad->setColor(m_look.gradEnd);
                setOp(s.grad, op);
            }

            bool gl = m_look.noteGlow && m_look.glowStrength > 0.01f;
            s.glow->setVisible(gl);
            if (gl) {
                s.glow->setColor(glowCol);
                setOp(s.glow, 0.45f * m_look.glowStrength * fade);
            }

            if (m_look.noteBorder > 0.f) {
                dnStrokeA(m_noteBorder, x0, py, x0 + nw, py + len, sN * kBaseR,
                          m_look.noteBorderColor, m_look.noteBorder, std::min(1.f, op + 0.15f), 4);
            }
        }
    }

    int keyCode() const { return m_look.keyCode; }

    void tick(double now, float dt, float noteSpace) {
        m_noteSpace = noteSpace;
        if (m_look.countMotion && m_countStart >= 0.0) {
            fitCount(now);
            if (now - m_countStart >= m_look.countMotionTime) m_countStart = -1.0;
        }
        float target = m_held ? 1.f : 0.f;
        float rate = m_held ? 40.f : 14.f;
        float old = m_press;
        m_press += (target - m_press) * std::min(1.f, dt * rate);
        if (std::abs(m_press - target) < 0.01f) m_press = target;

        if (m_press != old || m_dirty) {
            m_dirty = false;
            this->refreshTile();
        }
        this->tickNotes(now);
    }
};

// ---------------------------------------------------------------
// 키뷰어 전체 (일시정지 화면에서 드래그로 이동)
// ---------------------------------------------------------------
struct LaneGeo {
    int id = 0;
    int map = 0;
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    std::string name;
};

struct LaneRef {
    int id = 0;
    KeyLane* lane = nullptr;
};

class KeyViewer : public CCLayer {
protected:
    std::vector<LaneRef> m_lanes;
    std::vector<LaneGeo> m_geo;
    CCLabelBMFont* m_info = nullptr;
    CCLabelBMFont* m_hint = nullptr;
    CCDrawNode* m_frame = nullptr;
    std::string m_infoStr;
    std::deque<double> m_hits;
    unsigned m_heldSources = 0;
    std::set<int> m_heldKeys;
    bool m_koreanStats = false;
    int m_total = 0;
    int m_peak = 0;
    float m_refresh = 0.f;
    float m_poll = 0.f;
    float m_minX = -20.f, m_maxX = 20.f, m_minY = 0.f, m_maxY = 40.f;
    bool m_platformer = false;
    bool m_editing = false;
    bool m_dragging = false;
    CCPoint m_dragOffset = {0.f, 0.f};
    Look m_look;

    bool init(bool platformer) {
        if (!CCLayer::init()) return false;
        m_platformer = platformer;
        this->setTouchEnabled(true);

        m_frame = CCDrawNode::create();
        this->addChild(m_frame, 5);

        m_koreanStats = koreanUI();
        m_info = CCLabelBMFont::create("", koreanUI() ? "ViewerUI.fnt"_spr : "InterBold.fnt"_spr);
        m_info->setScale(0.38f);
        m_info->setOpacity(190);
        this->addChild(m_info);

        m_hint = CCLabelBMFont::create(uiText("DRAG TO MOVE").c_str(), koreanUI() ? "ViewerUI.fnt"_spr : "InterBold.fnt"_spr);
        m_hint->setScale(0.38f);
        m_hint->setVisible(false);
        this->addChild(m_hint, 6);

        this->applyLook();
        if (m_lanes.empty()) return false;
        this->scheduleUpdate();
        return true;
    }

    KeyLane* laneFor(int id) {
        for (auto& r : m_lanes) if (r.id == id) return r.lane;
        return nullptr;
    }

    // 키 목록과 화면의 키 노드를 맞춘다
    void syncLanes() {
        for (auto it = m_lanes.begin(); it != m_lanes.end();) {
            if (!keyById(it->id)) {
                it->lane->removeFromParentAndCleanup(true);
                it = m_lanes.erase(it);
            } else {
                ++it;
            }
        }
        for (auto const& k : g_keys) {
            if (laneFor(k.id)) continue;
            auto lane = KeyLane::create(kLabels[1]);
            if (!lane) continue;
            this->addChild(lane);
            m_lanes.push_back({k.id, lane});
        }
    }

    std::vector<KeyLane*> lanesForMap(int map) {
        std::vector<KeyLane*> out;
        for (auto const& k : g_keys) {
            if (k.map != map) continue;
            if (auto lane = laneFor(k.id); lane && lane->keyCode() == 0) out.push_back(lane);
        }
        return out;
    }

public:
    static KeyViewer* create(bool platformer) {
        auto ret = new KeyViewer();
        if (ret->init(platformer)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    std::vector<LaneGeo> const& geo() const { return m_geo; }
    float minX() const { return m_minX; }
    float maxX() const { return m_maxX; }

    // 터치를 가장 먼저 받되, 편집 중이고 키뷰어 위일 때만 가져간다
    void registerWithTouchDispatcher() override {
        CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -1000, true);
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        if (!m_editing || !this->getParent()) return false;
        auto p = this->getParent()->convertTouchToNodeSpace(touch);
        auto pos = this->getPosition();
        CCRect r(pos.x + m_minX - 12.f, pos.y + m_minY - 24.f,
                 (m_maxX - m_minX) + 24.f, (m_maxY - m_minY) + 48.f);
        if (!r.containsPoint(p)) return false;
        m_dragging = true;
        m_dragOffset = pos - p;
        return true;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        if (!m_dragging || !this->getParent()) return;
        auto p = this->getParent()->convertTouchToNodeSpace(touch) + m_dragOffset;
        auto win = CCDirector::get()->getWinSize();
        p.x = std::clamp(p.x, 0.f, win.width);
        p.y = std::clamp(p.y, 0.f, win.height);
        this->setPosition(p);
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override { this->finishDrag(); }
    void ccTouchCancelled(CCTouch*, CCEvent*) override { this->finishDrag(); }

    // 드래그가 끝나면 위치를 설정에 저장한다
    void finishDrag() {
        if (!m_dragging) return;
        m_dragging = false;
        auto win = CCDirector::get()->getWinSize();
        auto pos = this->getPosition();
        Mod::get()->setSettingValue<double>("pos-x", std::clamp<double>(pos.x / win.width * 100.0, 0.0, 100.0));
        Mod::get()->setSettingValue<double>("pos-y", std::clamp<double>(pos.y / win.height * 100.0, 0.0, 100.0));
    }

    void setEditing(bool e) {
        if (e == m_editing) return;
        m_editing = e;
        if (!e) this->finishDrag();
        m_hint->setVisible(e);
        this->updateFrame();
    }

    void updateFrame() {
        m_frame->clear();
        if (!m_editing) return;
        float pad = 6.f;
        float x0 = m_minX - pad, x1 = m_maxX + pad;
        float y0 = m_minY - pad, y1 = m_maxY + pad;
        CCPoint v[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        ccColor4F none = {0.f, 0.f, 0.f, 0.f};
        ccColor4F border = {1.f, 1.f, 1.f, 0.9f};
        m_frame->drawPolygon(v, 4, none, 1.f, border);
    }

    void applyLook() {
        loadKeys();
        const bool wasVisible = m_look.viewerVisible;
        m_look = readLook();
        if (wasVisible && !m_look.viewerVisible) {
            for (auto& r : m_lanes) r.lane->clearNotes();
        }
        this->setVisible(m_look.viewerVisible);
        this->syncLanes();

        m_geo.clear();
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        for (auto const& k : g_keys) {
            auto lane = this->laneFor(k.id);
            if (!lane) continue;

            // 왼쪽/오른쪽 키는 플랫포머 레벨에서만 보인다
            bool vis = k.style.contains("key-code") || m_platformer || (k.map % 3 == 0);
            lane->setVisible(vis);

            float w = k.w > 0.5f ? k.w : autoWidth(k, m_look);
            float h = k.h > 0.5f ? k.h : m_look.size;
            std::string name = labelFor(k, m_look);
            lane->setName(name);
            lane->layout(lookForKey(m_look, k), w, h);
            lane->setPosition({k.x, k.y});
            if (!vis) continue;

            LaneGeo g;
            g.id = k.id;
            g.map = k.map;
            g.x = k.x; g.y = k.y; g.w = w; g.h = h;
            g.name = name;
            m_geo.push_back(g);

            minX = std::min(minX, k.x);
            maxX = std::max(maxX, k.x + w);
            minY = std::min(minY, k.y);
            maxY = std::max(maxY, k.y + h);
        }
        if (m_geo.empty()) {
            minX = -20.f; maxX = 20.f; minY = 0.f; maxY = m_look.size;
        }
        m_minX = minX; m_maxX = maxX; m_minY = minY; m_maxY = maxY;

        float cxm = (minX + maxX) / 2.f;
        m_info->setPosition({cxm, minY - 10.f});
        m_hint->setPosition({cxm, maxY + 14.f});

        if (!m_dragging) {
            auto win = CCDirector::get()->getWinSize();
            this->setPosition({win.width * m_look.posX / 100.f, win.height * m_look.posY / 100.f});
        }
        this->updateFrame();
    }

    // 게임이 보내는 입력: 해당 입력에 연결된 키를 누른 상태로 바꾼다.
    // 2P 전용 키가 없으면 2P 입력도 1P 키가 받는다 (듀얼, 2인 모드에서 인식되지 않던 문제)
    void onButton(int button, bool down, bool isPlayer1) {
        if (button < 1 || button > 3) return;
        const int source = (isPlayer1 ? 0 : 3) + button - 1;
        const unsigned bit = 1u << source;
        if (down == bool(m_heldSources & bit)) return;
        const double t = nowSec();
        if (!down) {
            m_heldSources &= ~bit;
            // A key may have been remapped while this press was held.
            for (auto& r : m_lanes) r.lane->input(source, false, t, false);
            return;
        }
        m_heldSources |= bit;
        auto targets = lanesForMap(source);
        if (targets.empty()) targets = lanesForMap((source + 3) % 6);
        if (targets.empty()) return;
        for (auto lane : targets) {
            lane->input(source, true, t, m_look.viewerVisible && lane->isVisible());
        }
        m_hits.push_back(t);
        ++m_total;
    }

    void onKeyboard(int code, bool down) {
        double t = nowSec();
        if (down) {
            if (!m_heldKeys.insert(code).second) return;
            bool used = false;
            for (auto& r : m_lanes) {
                if (r.lane->keyCode() != code || !r.lane->isVisible()) continue;
                r.lane->input(6, true, t, m_look.viewerVisible);
                used = true;
            }
            if (used) {
                m_hits.push_back(t);
                ++m_total;
            }
        } else if (m_heldKeys.erase(code)) {
            for (auto& r : m_lanes)
                if (r.lane->keyCode() == code) r.lane->input(6, false, t, false);
        }
    }

    void resetInput() {
        m_heldSources = 0;
        m_heldKeys.clear();
        for (auto& r : m_lanes) r.lane->resetInput();
    }

    void onRestart() {
        resetInput();
        for (auto& r : m_lanes) {
            if (m_look.resetOnRestart) r.lane->resetCount();
        }
        if (m_look.resetOnRestart) {
            m_hits.clear();
            m_total = 0;
            m_peak = 0;
        }
    }

    void update(float dt) override {
        double now = nowSec();

        m_refresh += dt;
        if (m_refresh >= 0.5f) {
            m_refresh = 0.f;
            this->applyLook(); // 설정 변경을 실시간 반영
        }

        // 일시정지 화면이 떠 있으면 위치 편집 모드
        m_poll += dt;
        if (m_poll >= 0.1f) {
            m_poll = 0.f;
            bool paused = false;
            if (auto scene = CCDirector::get()->getRunningScene()) {
                paused = scene->getChildByType<PauseLayer>(0) != nullptr;
            }
            if (paused && !m_editing) resetInput();
            this->setEditing(paused);
        }

        while (!m_hits.empty() && now - m_hits.front() > 1.0) {
            m_hits.pop_front();
        }
        int kps = static_cast<int>(m_hits.size());
        m_peak = std::max(m_peak, kps);

        if (!m_look.viewerVisible) return;
        float top = CCDirector::get()->getWinSize().height - getPositionY();
        for (auto& r : m_lanes)
            if (r.lane->isVisible()) r.lane->tick(now, dt, top - r.lane->getPositionY());

        std::string s;
        if (m_look.kps) s += "KPS " + std::to_string(kps) + (m_koreanStats ? "  최고 " : "  MAX ") + std::to_string(m_peak);
        if (m_look.total) {
            if (!s.empty()) s += "   ";
            s += (m_koreanStats ? "합계 " : "TOTAL ") + std::to_string(m_total);
        }
        if (s != m_infoStr) {
            m_infoStr = s;
            m_info->setString(s.c_str());
        }
        m_info->setVisible(!s.empty());
    }
};

// ---------------------------------------------------------------
// 에디터 창 (일시정지 메뉴의 Key Viewer 버튼으로 연다)
// 왼쪽 캔버스: 키를 눌러 선택(파란 테두리), 끌어서 이동, 손잡이로 크기 조절
// 오른쪽: 탭별 설정. 아래: 키 추가/삭제, 지도 배치 모드, 미리보기 등
// ---------------------------------------------------------------
static ccColor3B const kPalette[12] = {
    {255, 255, 255}, {226, 226, 232}, {140, 140, 150}, {18, 18, 22},
    {255, 80, 90},   {255, 160, 60},  {255, 230, 90},  {110, 230, 120},
    {80, 220, 255},  {90, 130, 255},  {170, 110, 255}, {255, 120, 200}
};

static ccColor3B const kBlue = {90, 140, 255};
static ccColor3B const kWhite = {255, 255, 255};

struct Ctl {
    CCRect r = CCRect(0.f, 0.f, 0.f, 0.f);
    bool scroll = false;
    int mode = 0; // 0 = 창 모드에서만, 1 = 지도 모드에서만 (스크롤 컨트롤은 창 모드)
    int row = -1;
    std::function<void(CCPoint)> down, move, up;
};

struct RowInfo {
    float top = 0.f, bottom = 0.f;
    std::vector<CCNode*> nodes;
    bool visible = true;
};

struct Btn {
    std::string label;
    int id = 0; // 11 Show, 12 Press, 13 Reset, 20 Add, 21 Del, 22 Map, 30 Back
    float x0 = 0.f, x1 = 0.f, y0 = 0.f, y1 = 0.f;
};

struct View {
    float ox = 0.f, oy = 0.f, sc = 1.f;
};

class EditorInputPopup : public geode::Popup {
    using Action = std::function<bool(std::string const&)>;
    Action m_apply;
    std::function<void()> m_dismiss;
    geode::TextInput* m_field = nullptr;
    CCLabelBMFont* m_hint = nullptr;
    std::function<void(int)> m_capture;

    void onClose(CCObject* sender) override {
        if (m_dismiss) {
            auto dismiss = std::move(m_dismiss);
            dismiss();
        }
        Popup::onClose(sender);
    }

    void onApply(CCObject* sender) {
        if (m_apply(m_field->getString().c_str())) onClose(sender);
        else m_hint->setString(uiText("Enter a valid value.").c_str());
    }

    void keyDown(enumKeyCodes key, double time) override {
        if (m_capture && key != KEY_Escape && static_cast<int>(key) > 0) {
            m_capture(static_cast<int>(key));
            onClose(nullptr);
            return;
        }
        Popup::keyDown(key, time);
    }

public:
    static EditorInputPopup* create(std::string title, std::string value, bool numeric, Action apply,
                                     std::function<void(int)> capture, std::function<void()> dismiss) {
        auto popup = new EditorInputPopup;
        if (!popup->Popup::init(240.f, 118.f)) {
            delete popup;
            return nullptr;
        }
        popup->m_apply = std::move(apply);
        popup->m_capture = std::move(capture);
        popup->m_dismiss = std::move(dismiss);
        mkLabel(popup->m_mainLayer, title, 0.52f, kWhite, {120.f, 94.f}, 0.5f);
        if (popup->m_capture) {
            popup->setKeyboardEnabled(true);
            mkLabel(popup->m_mainLayer, "Press a keyboard key", 0.42f, kWhite, {120.f, 55.f}, 0.5f);
        } else {
            popup->m_field = geode::TextInput::create(172.f, "");
            popup->m_field->setPosition({120.f, 64.f});
            popup->m_field->setMaxCharCount(numeric ? 9 : 24);
            popup->m_field->setFilter(numeric ? "0123456789-." : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 !#$%&()+-./:<=>?@[]_");
            popup->m_field->setString(value.c_str());
            popup->m_mainLayer->addChild(popup->m_field);
            popup->m_hint = mkLabel(popup->m_mainLayer, "", 0.30f, kWhite, {120.f, 41.f}, 0.5f);
            auto sprite = ButtonSprite::create("OK");
            sprite->setScale(0.65f);
            auto confirm = CCMenuItemSpriteExtra::create(sprite, popup, menu_selector(EditorInputPopup::onApply));
            confirm->setPosition({120.f, 20.f});
            popup->m_buttonMenu->addChild(confirm);
        }
        popup->autorelease();
        return popup;
    }
};

class KVEditor : public CCLayer {
protected:
    KeyViewer* m_viewer = nullptr;
    float W = 0.f, H = 0.f;
    float winX0 = 0.f, winX1 = 0.f, winY0 = 0.f, winY1 = 0.f;
    float tbY0 = 0.f, tbY1 = 0.f;
    float cvX0 = 0.f, cvX1 = 0.f, cvY0 = 0.f, cvY1 = 0.f;
    float inX0 = 0.f, inX1 = 0.f, inY0 = 0.f, inY1 = 0.f;
    float tabY0 = 0.f, vpY0 = 0.f, vpY1 = 0.f;
    float m_inW = 0.f;
    float m_scroll = 0.f, m_contentH = 0.f, m_cursor = 0.f;
    int m_tab = 0;
    int m_sel = -1; // 선택한 키 id
    bool m_previewPressed = false;
    bool m_closing = false;
    bool m_reloadLanguage = false;
    bool m_mapMode = false;
    bool m_pendingRebuild = false;
    bool m_modal = false;

    CCDrawNode* m_back = nullptr;
    CCDrawNode* m_chrome = nullptr;
    CCDrawNode* m_canvas = nullptr;
    CCDrawNode* m_bar = nullptr;
    CCDrawNode* m_mapChrome = nullptr;
    CCNode* m_canvasLabels = nullptr;
    CCNode* m_content = nullptr;
    CCNode* m_winLabels = nullptr;
    CCNode* m_mapLabels = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_tabLabels[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    std::vector<CCLabelBMFont*> m_tileLabels;
    std::vector<Btn> m_btns, m_mapBtns;

    std::vector<Ctl> m_fixed, m_scrollCtls;
    std::vector<RowInfo> m_rows;
    int m_activeFixed = -1, m_activeScroll = -1;
    bool m_scrolling = false;
    float m_lastY = 0.f;

    // 캔버스 드래그: 0 없음, 1 이동, 2 크기 조절
    int m_dragMode = 0;
    int m_handle = -1;
    CCPoint m_dragStart = {0.f, 0.f};
    float m_dragRect[4] = {0.f, 0.f, 0.f, 0.f};

    static constexpr float kSc = 1.4f;
    View m_fit;

    bool init(KeyViewer* viewer) {
        if (!CCLayer::init()) return false;
        m_viewer = viewer;
        this->setTouchEnabled(true);
        this->setMouseEnabled(true);

        auto win = CCDirector::get()->getWinSize();
        W = win.width;
        H = win.height;
        winX0 = 10.f; winX1 = W - 10.f; winY0 = 6.f; winY1 = H - 6.f;
        tbY0 = winY0 + 4.f; tbY1 = tbY0 + 28.f;
        float bodyY0 = tbY1 + 6.f, bodyY1 = winY1 - 32.f;
        float inW = std::min(215.f, W * 0.34f);
        inX1 = winX1 - 8.f; inX0 = inX1 - inW;
        cvX0 = winX0 + 8.f; cvX1 = inX0 - 8.f;
        cvY0 = bodyY0; cvY1 = bodyY1;
        inY0 = bodyY0; inY1 = bodyY1;
        tabY0 = inY1 - 22.f;
        vpY1 = tabY0 - 4.f;
        vpY0 = inY0 + 4.f;
        m_inW = inW - 18.f;

        m_back = CCDrawNode::create();
        this->addChild(m_back, 0);
        m_chrome = CCDrawNode::create();
        this->addChild(m_chrome, 1);
        m_canvas = CCDrawNode::create();
        this->addChild(m_canvas, 2);
        m_canvasLabels = CCNode::create();
        this->addChild(m_canvasLabels, 3);
        auto clip = CCClippingNode::create();
        auto stencil = CCDrawNode::create();
        dnRect(stencil, inX0, vpY0, inX1, vpY1, {255, 255, 255}, 1.f);
        clip->setStencil(stencil);
        this->addChild(clip, 4);
        m_content = CCNode::create();
        clip->addChild(m_content);
        m_content->setPosition({inX0 + 6.f, vpY1});
        m_bar = CCDrawNode::create();
        this->addChild(m_bar, 5);
        m_winLabels = CCNode::create();
        this->addChild(m_winLabels, 6);
        m_mapChrome = CCDrawNode::create();
        this->addChild(m_mapChrome, 7);
        m_mapLabels = CCNode::create();
        this->addChild(m_mapLabels, 8);

        // 제목과 닫기
        mkLabel(m_winLabels, "KEY VIEWER", 0.42f, kWhite, CCPoint(W / 2.f, winY1 - 16.f), 0.5f);
        mkLabel(m_winLabels, "X", 0.45f, ccColor3B{230, 230, 240}, CCPoint(winX1 - 22.f, winY1 - 16.f), 0.5f);
        {
            Ctl c;
            c.r = CCRect(winX1 - 40.f, winY1 - 32.f, 34.f, 30.f);
            c.down = [this](CCPoint) { m_closing = true; };
            m_fixed.push_back(c);
        }
        m_status = mkLabel(m_winLabels, "Tap a key to select it. Drag it to move, drag the handles to resize.",
                           0.26f, ccColor3B{130, 130, 150}, CCPoint(cvX0 + 8.f, cvY0 + 8.f), 0.f);

        // 탭
        {
            char const* names[5] = {"KEY", "NOTE", "COUNT", "VIEW", "SETS"};
            float tw = (inX1 - inX0 - 12.f) / 5.f;
            for (int i = 0; i < 5; i++) {
                float x0 = inX0 + 6.f + i * tw;
                m_tabLabels[i] = mkLabel(m_winLabels, names[i], 0.28f, kWhite,
                                         CCPoint(x0 + tw / 2.f, (tabY0 + inY1 - 2.f) / 2.f), 0.5f);
                if (m_tabLabels[i]) {
                    float lw = m_tabLabels[i]->getContentSize().width * 0.28f;
                    if (lw > tw - 4.f && lw > 0.f) m_tabLabels[i]->setScale(0.28f * (tw - 4.f) / lw);
                }
                Ctl c;
                c.r = CCRect(x0, tabY0, tw, inY1 - tabY0);
                c.down = [this, i](CCPoint) {
                    m_tab = i;
                    m_scroll = 0.f;
                    this->redrawChrome();
                    this->rebuildInspector();
                };
                m_fixed.push_back(c);
            }
        }

        // 아래 도구 막대
        {
            struct Def { char const* label; int id; };
            Def defs[6] = {
                {"Add Key", 20}, {"Del Key", 21}, {"Map", 22}, {"Press", 12}, {"Reset", 13}, {"Show", 11}
            };
            float total = winX1 - winX0 - 16.f;
            float bw = (total - 5.f * 4.f) / 6.f;
            for (int i = 0; i < 6; i++) {
                Btn b;
                b.label = defs[i].label;
                b.id = defs[i].id;
                b.x0 = winX0 + 8.f + i * (bw + 4.f);
                b.x1 = b.x0 + bw;
                b.y0 = tbY0;
                b.y1 = tbY1;
                m_btns.push_back(b);

                auto lab = mkLabel(m_winLabels, b.label, 0.30f, kWhite,
                                   CCPoint((b.x0 + b.x1) / 2.f, (b.y0 + b.y1) / 2.f), 0.5f);
                if (lab) {
                    float lw = lab->getContentSize().width * 0.30f;
                    if (lw > bw - 6.f && lw > 0.f) lab->setScale(0.30f * (bw - 6.f) / lw);
                }
                Ctl c;
                c.r = CCRect(b.x0, b.y0, bw, b.y1 - b.y0);
                int id = b.id;
                c.down = [this, id](CCPoint) { this->handleBtn(id); };
                m_fixed.push_back(c);
            }
        }

        // 캔버스 (창 모드: 왼쪽 영역, 지도 모드: 화면 전체)
        {
            Ctl c;
            c.mode = 0;
            c.r = CCRect(cvX0, cvY0, cvX1 - cvX0, cvY1 - cvY0);
            c.down = [this](CCPoint p) { this->canvasDown(p); };
            c.move = [this](CCPoint p) { this->canvasMove(p); };
            c.up = [this](CCPoint p) { this->canvasUp(p); };
            m_fixed.push_back(c);

            Ctl m;
            m.mode = 1;
            m.r = CCRect(0.f, 0.f, W, H);
            m.down = c.down;
            m.move = c.move;
            m.up = c.up;
            m_fixed.push_back(m);
        }

        // 지도 모드 위쪽 막대 (BACK, ADD, DEL, PRESS)
        {
            struct Def { char const* label; int id; };
            Def defs[4] = {{"Back", 30}, {"Add Key", 20}, {"Del Key", 21}, {"Press", 12}};
            float bw = 62.f, gap = 6.f;
            float total = 4.f * bw + 3.f * gap;
            float x = W / 2.f - total / 2.f;
            for (int i = 0; i < 4; i++) {
                Btn b;
                b.label = defs[i].label;
                b.id = defs[i].id;
                b.x0 = x + i * (bw + gap);
                b.x1 = b.x0 + bw;
                b.y0 = H - 36.f;
                b.y1 = H - 10.f;
                m_mapBtns.push_back(b);

                mkLabel(m_mapLabels, b.label, 0.30f, kWhite,
                        CCPoint((b.x0 + b.x1) / 2.f, (b.y0 + b.y1) / 2.f), 0.5f);
                Ctl c;
                c.mode = 1;
                c.r = CCRect(b.x0, b.y0, bw, b.y1 - b.y0);
                int id = b.id;
                c.down = [this, id](CCPoint) { this->handleBtn(id); };
                m_fixed.push_back(c);
            }
            mkLabel(m_mapLabels, "Drag keys to move them. Resize in the window mode.", 0.26f,
                    ccColor3B{210, 210, 225}, CCPoint(14.f, H - 46.f), 0.f);
        }

        this->loadAndSelect();
        this->setMapMode(false);
        this->scheduleUpdate();
        return true;
    }

    void loadAndSelect() {
        loadKeys();
        auto const& geo = m_viewer->geo();
        if (!geo.empty()) m_sel = geo.front().id;
        else if (!g_keys.empty()) m_sel = g_keys.front().id;
    }

    // ---------- 도우미 ----------
    static std::string describeKey(int id) {
        auto k = keyById(id);
        if (!k) return "-";
        return "#" + std::to_string(id) + "  " + uiText(kMapNames[std::clamp(k->map, 0, 5)]);
    }

    View view() const {
        View v;
        if (m_mapMode) {
            auto p = m_viewer->getPosition();
            v.ox = p.x;
            v.oy = p.y;
            v.sc = 1.f;
        } else {
            v = m_fit;
        }
        return v;
    }

    // 모든 키가 캔버스 안에 보이도록 배율과 위치를 맞춘다 (끌고 있는 동안에는 고정)
    void updateFit() {
        auto const& geo = m_viewer->geo();
        float cw = cvX1 - cvX0, ch = cvY1 - cvY0;
        if (geo.empty()) {
            m_fit.ox = (cvX0 + cvX1) / 2.f;
            m_fit.oy = cvY0 + ch * 0.34f;
            m_fit.sc = kSc;
            return;
        }
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        for (auto const& g : geo) {
            minX = std::min(minX, g.x);
            maxX = std::max(maxX, g.x + g.w);
            minY = std::min(minY, g.y);
            maxY = std::max(maxY, g.y + g.h);
        }
        float bw = std::max(20.f, maxX - minX), bh = std::max(20.f, maxY - minY);
        float sc = std::min(kSc, std::min((cw - 40.f) / bw, (ch - 120.f) / bh));
        sc = std::max(0.25f, sc);
        m_fit.sc = sc;
        m_fit.ox = (cvX0 + cvX1) / 2.f - (minX + maxX) / 2.f * sc;
        m_fit.oy = cvY0 + 28.f - minY * sc;
    }

    LaneGeo const* geoById(int id) const {
        for (auto const& g : m_viewer->geo()) if (g.id == id) return &g;
        return nullptr;
    }

    CCLabelBMFont* tileLabel(int i) {
        while (static_cast<int>(m_tileLabels.size()) <= i) {
            auto l = mkLabel(m_canvasLabels, "", 0.5f, kWhite, CCPoint(0.f, 0.f), 0.5f);
            m_tileLabels.push_back(l);
        }
        return m_tileLabels[i];
    }

    void setStatus(std::string const& s) {
        if (!m_status) return;
        m_status->setString(uiText(s).c_str());
        m_status->setScale(std::min(0.26f, (cvX1 - cvX0 - 12.f) / std::max(1.f, m_status->getContentSize().width)));
    }

    void ensureSelection() {
        if (!keyById(m_sel)) loadAndSelect();
    }

    // 키별 설정을 바꾼 뒤: 저장하고 게임 화면과 캔버스에 반영
    void commitKey(int id) {
        if (auto k = keyById(id)) saveKey(*k);
        m_viewer->applyLook();
        this->redrawCanvas();
    }

    // 전체 설정을 바꾼 뒤
    void refreshAll(bool chrome = false) {
        m_viewer->applyLook();
        this->ensureSelection();
        this->redrawCanvas();
        if (chrome) this->redrawChrome();
    }

    bool btnActive(Btn const& b) const {
        if (b.id == 11) return Mod::get()->getSettingValue<bool>("show-viewer");
        if (b.id == 12) return m_previewPressed;
        return false;
    }

    void setPauseVisible(bool v) {
        if (auto scene = CCDirector::get()->getRunningScene()) {
            if (auto pl = scene->getChildByType<PauseLayer>(0)) pl->setVisible(v);
        }
    }

    void setMapMode(bool on) {
        m_mapMode = on;
        m_chrome->setVisible(!on);
        m_content->setVisible(!on);
        m_bar->setVisible(!on);
        m_winLabels->setVisible(!on);
        m_mapChrome->setVisible(on);
        m_mapLabels->setVisible(on);
        this->setPauseVisible(!on);
        m_dragMode = 0;
        this->redrawBack();
        this->redrawChrome();
        this->redrawMapBar();
        this->redrawCanvas();
        this->rebuildInspector();
    }

    void addKey() {
        loadKeys();
        Look look = readLook();
        auto const& geo = m_viewer->geo();
        float maxX = -look.size / 2.f - look.gap;
        float y = 0.f;
        for (auto const& g : geo) {
            if (g.x + g.w > maxX) {
                maxX = g.x + g.w;
                y = g.y;
            }
        }
        // 새 키는 항상 기본값으로 만든다 (입력은 점프 P1)
        KeyDef k;
        k.id = g_nextId++;
        k.map = 0;
        k.x = std::round(maxX + look.gap);
        k.y = std::round(y);
        g_keys.push_back(k);
        saveKeyList();
        saveKey(k);
        m_sel = k.id;
        m_scroll = 0.f;
        m_viewer->applyLook();
        this->redrawCanvas();
        m_pendingRebuild = true;
        this->setStatus(uiText("Added key ") + describeKey(k.id));
    }

    void deleteKey() {
        if (g_keys.size() <= 1) {
            this->setStatus("Keep at least one key.");
            return;
        }
        for (size_t i = 0; i < g_keys.size(); i++) {
            if (g_keys[i].id != m_sel) continue;
            g_keys.erase(g_keys.begin() + static_cast<long>(i));
            break;
        }
        saveKeyList();
        m_sel = g_keys.front().id;
        m_scroll = 0.f;
        m_viewer->applyLook();
        this->ensureSelection();
        this->redrawCanvas();
        m_pendingRebuild = true;
        this->setStatus("Deleted key.");
    }

    void resetKey() {
        auto k = keyById(m_sel);
        if (!k) return;
        KeyDef d;
        d.id = k->id;
        d.map = k->map;
        if (auto it = k->style.find("key-code"); it != k->style.end()) d.style.insert(*it);
        d.x = k->x;
        d.y = k->y;
        *k = d;
        saveKey(*k);
        m_viewer->applyLook();
        this->redrawCanvas();
        m_pendingRebuild = true;
        this->setStatus("Reset key style.");
    }

    void handleBtn(int id) {
        if (id == 11) {
            auto m = Mod::get();
            m->setSettingValue<bool>("show-viewer", !m->getSettingValue<bool>("show-viewer"));
        } else if (id == 12) {
            m_previewPressed = !m_previewPressed;
        } else if (id == 13) {
            this->resetKey();
        } else if (id == 20) {
            this->addKey();
        } else if (id == 21) {
            this->deleteKey();
        } else if (id == 22) {
            this->setMapMode(true);
            return;
        } else if (id == 30) {
            this->setMapMode(false);
            return;
        }
        m_viewer->applyLook();
        this->ensureSelection();
        this->redrawChrome();
        this->redrawMapBar();
        this->redrawCanvas();
    }

    // ---------- 그리기 ----------
    void redrawBack() {
        m_back->clear();
        // 지도 모드에서는 게임 화면이 은은하게 비쳐 보이도록 어둡게만 덮는다
        dnRect(m_back, 0.f, 0.f, W, H, ccColor3B{0, 0, 0}, m_mapMode ? 0.30f : 0.72f);
    }

    void redrawMapBar() {
        m_mapChrome->clear();
        for (auto const& b : m_mapBtns) {
            bool on = this->btnActive(b);
            dnRound(m_mapChrome, b.x0, b.y0, b.x1, b.y1, 6.f,
                    on ? ccColor3B{70, 110, 240} : ccColor3B{40, 40, 52}, 0.92f, 4);
        }
    }

    void redrawChrome() {
        m_chrome->clear();
        dnRound(m_chrome, winX0, winY0, winX1, winY1, 10.f, ccColor3B{30, 30, 38}, 1.f, 5);
        dnRect(m_chrome, winX0 + 10.f, winY1 - 31.f, winX1 - 10.f, winY1 - 30.4f, ccColor3B{62, 62, 76}, 1.f);

        dnRound(m_chrome, cvX0, cvY0, cvX1, cvY1, 6.f, ccColor3B{20, 20, 26}, 1.f, 4);
        for (float x = cvX0 + 20.f; x < cvX1 - 2.f; x += 20.f) {
            dnRect(m_chrome, x, cvY0 + 2.f, x + 0.5f, cvY1 - 2.f, kWhite, 0.05f);
        }
        for (float y = cvY0 + 20.f; y < cvY1 - 2.f; y += 20.f) {
            dnRect(m_chrome, cvX0 + 2.f, y, cvX1 - 2.f, y + 0.5f, kWhite, 0.05f);
        }

        dnRound(m_chrome, inX0, inY0, inX1, inY1, 6.f, ccColor3B{38, 38, 48}, 1.f, 4);
        float tw = (inX1 - inX0 - 12.f) / 5.f;
        for (int i = 0; i < 5; i++) {
            float x0 = inX0 + 6.f + i * tw;
            bool on = (i == m_tab);
            dnRound(m_chrome, x0 + 1.f, tabY0, x0 + tw - 1.f, inY1 - 3.f, 5.f,
                    on ? ccColor3B{70, 110, 240} : ccColor3B{52, 52, 66}, 1.f, 3);
            if (m_tabLabels[i]) m_tabLabels[i]->setColor(on ? kWhite : ccColor3B{170, 170, 190});
        }

        for (auto const& b : m_btns) {
            bool on = this->btnActive(b);
            dnRound(m_chrome, b.x0, b.y0, b.x1, b.y1, 6.f,
                    on ? ccColor3B{70, 110, 240} : ccColor3B{52, 52, 66}, 1.f, 4);
        }
    }

    void redrawCanvas() {
        m_canvas->clear();
        for (auto l : m_tileLabels) if (l) l->setVisible(false);
        if (!m_mapMode && m_dragMode == 0) this->updateFit();

        Look base = readLook();
        View v = this->view();
        auto const& geo = m_viewer->geo();
        int li = 0;

        for (auto const& g : geo) {
            auto kd = keyById(g.id);
            if (!kd) continue;
            Look kl = lookForKey(base, *kd);
            float x0 = v.ox + g.x * v.sc, y0 = v.oy + g.y * v.sc;
            float w = g.w * v.sc, h = g.h * v.sc;
            float x1 = x0 + w, y1 = y0 + h;
            float r = std::min(std::max(kl.radius * h, 2.f), std::min(w, h) / 2.f);

            // 노트 모양 미리보기 (창 모드에서만)
            if (!m_mapMode && kl.timeline) {
                ccColor3B acc = kl.accentJump;
                float nw = std::clamp(kl.noteWidth, 0.2f, 1.f) * w;
                float nx0 = x0 + (w - nw) / 2.f;
                float nr = std::clamp(kl.radius * h * 0.7f, 2.f, 9.f);
                dnRound(m_canvas, nx0, y1 + 6.f, nx0 + nw, y1 + 34.f, nr, acc, 0.85f, 3);
                dnRound(m_canvas, nx0, y1 + 42.f, nx0 + nw, y1 + 56.f, nr, acc, 0.55f, 3);
                dnRound(m_canvas, nx0, y1 + 64.f, nx0 + nw, y1 + 70.f, 3.f, acc, 0.3f, 2);
            }

            bool pressed = m_previewPressed;
            ccColor3B fill = pressed ? kl.pressed : kl.tile;
            float a = pressed ? 1.f : std::max(0.55f, kl.tileOpacity / 255.f);
            dnRound(m_canvas, x0, y0, x1, y1, r, fill, a, 5);
            if (kl.borderW > 0.f) dnStroke(m_canvas, x0, y0, x1, y1, r, kl.border, kl.borderW * v.sc, 5);

            // 선택한 키: 파란 테두리와 손잡이 (모서리 동그라미, 변 알약)
            if (g.id == m_sel) {
                dnStroke(m_canvas, x0, y0, x1, y1, r, kBlue, 1.6f, 5);
                if (!m_mapMode) {
                    float cx = (x0 + x1) / 2.f, cy = (y0 + y1) / 2.f;
                    CCPoint corners[4] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}};
                    for (auto const& c : corners) {
                        dnRound(m_canvas, c.x - 4.f, c.y - 4.f, c.x + 4.f, c.y + 4.f, 4.f, kWhite, 1.f, 3);
                    }
                    dnRound(m_canvas, cx - 7.f, y0 - 2.5f, cx + 7.f, y0 + 2.5f, 2.5f, kWhite, 1.f, 3);
                    dnRound(m_canvas, cx - 7.f, y1 - 2.5f, cx + 7.f, y1 + 2.5f, 2.5f, kWhite, 1.f, 3);
                    dnRound(m_canvas, x0 - 2.5f, cy - 7.f, x0 + 2.5f, cy + 7.f, 2.5f, kWhite, 1.f, 3);
                    dnRound(m_canvas, x1 - 2.5f, cy - 7.f, x1 + 2.5f, cy + 7.f, 2.5f, kWhite, 1.f, 3);
                }
            }

            auto lab = this->tileLabel(li++);
            if (lab) {
                setFont(lab, kl.labelFont);
                lab->setString(g.name.c_str());
                float sc = labelScale(lab, kl.labelSize * v.sc, w * 0.84f);
                lab->setScale(sc);
                float lw = lab->getContentSize().width * sc;
                if (lw > w * 0.84f && lw > 0.f) lab->setScale(sc * w * 0.84f / lw);
                lab->setColor(kl.labelColor < 0 ? textFor(fill) : unpackC(kl.labelColor));
                lab->setPosition({(x0 + x1) / 2.f, (y0 + y1) / 2.f});
                lab->setVisible(true);
            }
        }
    }

    // ---------- 캔버스 터치 ----------
    int hitHandle(CCPoint p, float x0, float y0, float x1, float y1) const {
        float cx = (x0 + x1) / 2.f, cy = (y0 + y1) / 2.f;
        CCPoint pts[8] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}, {cx, y0}, {cx, y1}, {x0, cy}, {x1, cy}};
        float best = 11.f * 11.f;
        int bi = -1;
        for (int i = 0; i < 8; i++) {
            float dx = p.x - pts[i].x, dy = p.y - pts[i].y;
            float d2 = dx * dx + dy * dy;
            if (d2 < best) {
                best = d2;
                bi = i;
            }
        }
        return bi;
    }

    void canvasDown(CCPoint p) {
        m_dragMode = 0;
        if (!m_mapMode) this->updateFit();
        View v = this->view();

        // 선택한 키의 손잡이를 먼저 확인 (지도 모드에서는 이동만 가능)
        if (auto g = this->geoById(m_sel); g && !m_mapMode) {
            float x0 = v.ox + g->x * v.sc, y0 = v.oy + g->y * v.sc;
            float x1 = x0 + g->w * v.sc, y1 = y0 + g->h * v.sc;
            int hnd = this->hitHandle(p, x0, y0, x1, y1);
            if (hnd >= 0) {
                m_dragMode = 2;
                m_handle = hnd;
                m_dragStart = p;
                m_dragRect[0] = g->x; m_dragRect[1] = g->y; m_dragRect[2] = g->w; m_dragRect[3] = g->h;
                return;
            }
        }

        // 키 본체: 선택하고 바로 끌어서 옮길 수 있다
        auto const& geo = m_viewer->geo();
        for (int i = static_cast<int>(geo.size()) - 1; i >= 0; i--) {
            auto const& g = geo[i];
            CCRect r(v.ox + g.x * v.sc, v.oy + g.y * v.sc, g.w * v.sc, g.h * v.sc);
            if (!r.containsPoint(p)) continue;
            bool changed = (m_sel != g.id);
            m_sel = g.id;
            m_dragMode = 1;
            m_dragStart = p;
            m_dragRect[0] = g.x; m_dragRect[1] = g.y; m_dragRect[2] = g.w; m_dragRect[3] = g.h;
            this->redrawCanvas();
            if (changed) {
                m_scroll = 0.f;
                m_pendingRebuild = true;
            }
            return;
        }
    }

    void canvasMove(CCPoint p) {
        if (m_dragMode == 0) return;
        auto k = keyById(m_sel);
        if (!k) return;
        View v = this->view();
        float dx = (p.x - m_dragStart.x) / v.sc;
        float dy = (p.y - m_dragStart.y) / v.sc;
        float x0 = m_dragRect[0], y0 = m_dragRect[1], w0 = m_dragRect[2], h0 = m_dragRect[3];

        if (m_dragMode == 1) {
            k->x = std::clamp(std::round(x0 + dx), -500.f, 500.f);
            k->y = std::clamp(std::round(y0 + dy), -300.f, 300.f);
        } else {
            int h = m_handle;
            bool left = (h == 0 || h == 2 || h == 6);
            bool right = (h == 1 || h == 3 || h == 7);
            bool bottom = (h == 0 || h == 1 || h == 4);
            bool top = (h == 2 || h == 3 || h == 5);
            float nx = x0, ny = y0, nw = w0, nh = h0;
            if (left) { nx = x0 + dx; nw = w0 - dx; }
            if (right) { nw = w0 + dx; }
            if (bottom) { ny = y0 + dy; nh = h0 - dy; }
            if (top) { nh = h0 + dy; }
            float mn = 12.f;
            if (nw < mn) {
                if (left) nx = x0 + w0 - mn;
                nw = mn;
            }
            if (nh < mn) {
                if (bottom) ny = y0 + h0 - mn;
                nh = mn;
            }
            k->x = std::round(nx);
            k->y = std::round(ny);
            k->w = std::round(nw);
            k->h = std::round(nh);
        }
        m_viewer->applyLook();
        this->redrawCanvas();
    }

    void canvasUp(CCPoint) {
        if (m_dragMode == 0) return;
        m_dragMode = 0;
        if (auto k = keyById(m_sel)) saveKey(*k);
        m_pendingRebuild = true; // 슬라이더 값을 새 값으로 갱신
        this->redrawCanvas();
    }

    // ---------- 인스펙터 (오른쪽 패널) ----------
    int beginRow(float h) {
        RowInfo r;
        r.top = m_cursor;
        r.bottom = m_cursor - h;
        m_rows.push_back(r);
        m_cursor -= h;
        return static_cast<int>(m_rows.size()) - 1;
    }

    CCLabelBMFont* rowLabel(int ri, std::string const& s, float x, float y, float scale, ccColor3B c, float ax) {
        auto l = mkLabel(m_content, s, scale, c, CCPoint(x, y), ax);
        if (l) m_rows[ri].nodes.push_back(l);
        return l;
    }

    CCDrawNode* rowDraw(int ri) {
        auto d = CCDrawNode::create();
        m_content->addChild(d);
        m_rows[ri].nodes.push_back(d);
        return d;
    }

    void rowInfo(std::string const& text) {
        int ri = beginRow(18.f);
        if (auto label = rowLabel(ri, text, 0.f, m_rows[ri].top - 9.f, 0.28f, {170, 170, 190}, 0.f))
            label->setScale(std::min(0.28f, m_inW / std::max(1.f, label->getContentSize().width)));
    }

    void rowButton(std::string const& label, ccColor3B color, std::function<void()> action) {
        int ri = beginRow(28.f);
        float top = m_rows[ri].top;
        float w = m_inW;
        auto d = rowDraw(ri);
        dnRound(d, 0.f, top - 24.f, w, top - 4.f, 6.f, color, 1.f, 4);
        rowLabel(ri, label, w / 2.f, top - 14.f, 0.30f, kWhite, 0.5f);
        Ctl c;
        c.scroll = true;
        c.row = ri;
        c.r = CCRect(0.f, top - 24.f, w, 20.f);
        c.down = [action](CCPoint) { action(); };
        m_scrollCtls.push_back(c);
    }

    void rowSlider(std::string const& label, float vmin, float vmax, float step,
                   std::function<float()> get, std::function<void(float)> set,
                   std::function<std::string(float)> fmt = nullptr) {
        int ri = beginRow(26.f);
        float top = m_rows[ri].top;
        float w = m_inW;
        rowLabel(ri, label, 0.f, top - 8.f, 0.30f, ccColor3B{235, 235, 240}, 0.f);
        auto val = rowLabel(ri, "", w, top - 8.f, 0.30f, ccColor3B{170, 170, 190}, 1.f);
        auto d = rowDraw(ri);
        float tx0 = 3.f, tx1 = w - 3.f, ty = top - 19.f;

        auto refresh = [=]() {
            float v = std::clamp(get(), vmin, vmax);
            float t = (vmax > vmin) ? (v - vmin) / (vmax - vmin) : 0.f;
            float kx = tx0 + t * (tx1 - tx0);
            d->clear();
            dnRect(d, tx0, ty - 1.2f, tx1, ty + 1.2f, ccColor3B{70, 70, 86}, 1.f);
            dnRect(d, tx0, ty - 1.2f, kx, ty + 1.2f, kBlue, 1.f);
            dnRound(d, kx - 4.f, ty - 4.f, kx + 4.f, ty + 4.f, 4.f, kWhite, 1.f, 3);
            if (val) val->setString(uiText(fmt ? fmt(v) : fmtNum(v)).c_str());
        };
        refresh();

        auto apply = [=](float x) {
            float t = std::clamp((x - tx0) / (tx1 - tx0), 0.f, 1.f);
            float v = vmin + t * (vmax - vmin);
            v = std::round(v / step) * step;
            set(std::clamp(v, vmin, vmax));
            refresh();
        };

        Ctl c;
        c.scroll = true;
        c.row = ri;
        c.r = CCRect(0.f, top - 26.f, w, 26.f);
        c.down = [apply](CCPoint p) { apply(p.x); };
        c.move = [apply](CCPoint p) { apply(p.x); };
        m_scrollCtls.push_back(c);
    }

    // 왼쪽 오른쪽 화살표로 선택지를 돌려 고르는 줄
    void rowCycle(std::string const& label, std::vector<std::string> opts,
                  std::function<int()> get, std::function<void(int)> set) {
        int ri = beginRow(24.f);
        float top = m_rows[ri].top;
        float w = m_inW;
        rowLabel(ri, label, 0.f, top - 12.f, 0.30f, ccColor3B{235, 235, 240}, 0.f);
        auto d = rowDraw(ri);
        float zx0 = w - 118.f;
        float mid = zx0 + 59.f;
        auto val = rowLabel(ri, "", mid, top - 12.f, 0.30f, kWhite, 0.5f);
        int n = static_cast<int>(opts.size());

        auto refresh = [=]() {
            d->clear();
            dnRound(d, zx0, top - 20.f, zx0 + 18.f, top - 4.f, 4.f, ccColor3B{58, 58, 72}, 1.f, 3);
            dnRound(d, w - 18.f, top - 20.f, w, top - 4.f, 4.f, ccColor3B{58, 58, 72}, 1.f, 3);
            int cur = std::clamp(get(), 0, n - 1);
            if (val) {
                val->setString(uiText(opts[cur]).c_str());
                float lw = val->getContentSize().width * 0.30f;
                float maxW = 76.f;
                val->setScale(lw > maxW && lw > 0.f ? 0.30f * maxW / lw : 0.30f);
            }
        };
        rowLabel(ri, "<", zx0 + 9.f, top - 12.f, 0.34f, kWhite, 0.5f);
        rowLabel(ri, ">", w - 9.f, top - 12.f, 0.34f, kWhite, 0.5f);
        refresh();

        Ctl c;
        c.scroll = true;
        c.row = ri;
        c.r = CCRect(zx0, top - 24.f, 118.f, 24.f);
        c.down = [=](CCPoint p) {
            int cur = std::clamp(get(), 0, n - 1);
            int next = (p.x < mid) ? (cur + n - 1) % n : (cur + 1) % n;
            set(next);
            refresh();
        };
        m_scrollCtls.push_back(c);
    }

    void rowColor(std::string const& label, std::function<int()> get, std::function<void(int)> set, bool allowAuto) {
        int ri = beginRow(36.f);
        float top = m_rows[ri].top;
        float w = m_inW;
        rowLabel(ri, label, 0.f, top - 8.f, 0.25f, ccColor3B{235, 235, 240}, 0.f);
        auto d = rowDraw(ri);
        int n = allowAuto ? 13 : 12;
        float cell = w / n;
        float sw = cell - 2.f;
        float y0 = top - 31.f;
        if (allowAuto) rowLabel(ri, "A", cell / 2.f, y0 + sw / 2.f, 0.26f, ccColor3B{200, 200, 210}, 0.5f);

        auto hex = rowLabel(ri, "", w, top - 8.f, 0.28f, kWhite, 1.f);
        auto refresh = [=]() {
            d->clear();
            int cur = get();
            char value[12];
            if (cur < 0) std::snprintf(value, sizeof value, "AUTO");
            else std::snprintf(value, sizeof value, "#%06X", unsigned(cur) & 0xffffffu);
            if (hex) hex->setString(uiText(value).c_str());
            for (int i = 0; i < n; i++) {
                float x0 = i * cell + 1.f;
                bool isAuto = allowAuto && i == 0;
                ccColor3B col = isAuto ? ccColor3B{50, 50, 60} : kPalette[i - (allowAuto ? 1 : 0)];
                dnRound(d, x0, y0, x0 + sw, y0 + sw, 3.f, col, 1.f, 3);
                bool sel = isAuto ? (cur < 0) : (cur == packC(col));
                if (sel) dnStroke(d, x0 - 1.f, y0 - 1.f, x0 + sw + 1.f, y0 + sw + 1.f, 4.f, kBlue, 1.5f, 3);
            }
        };
        refresh();

        Ctl c;
        c.scroll = true;
        c.row = ri;
        c.r = CCRect(0.f, top - 36.f, w, 30.f);
        c.down = [=, this](CCPoint p) {
            int i = std::clamp(static_cast<int>(p.x / cell), 0, n - 1);
            if (allowAuto && i == 0) set(-1);
            else set(packC(kPalette[i - (allowAuto ? 1 : 0)]));
            refresh();
            m_pendingRebuild = true;
        };
        m_scrollCtls.push_back(c);
        int digits = beginRow(24.f);
        auto digitBar = rowDraw(digits);
        float topY = m_rows[digits].top;
        float buttonW = w / 7.f;
        for (int digit = 0; digit < 6; ++digit) {
            float x = (digit + 1) * buttonW;
            dnRound(digitBar, x, topY - 21.f, x + buttonW - 2.f, topY - 2.f, 3.f,
                    {58, 58, 72}, 1.f, 3);
            int shift = (5 - digit) * 4;
            auto number = rowLabel(digits, "", x + buttonW / 2.f, topY - 12.f, 0.28f, kWhite, 0.5f);
            int color = std::max(get(), 0);
            if (number) number->setString(std::string(1, "0123456789ABCDEF"[(color >> shift) & 15]).c_str());
            Ctl field;
            field.row = digits;
            field.scroll = true;
            field.r = CCRect(x, topY - 23.f, buttonW, 23.f);
            field.down = [=, this](CCPoint) {
                int current = std::max(get(), 0);
                int next = (((current >> shift) + 1) & 15) << shift;
                set((current & ~(15 << shift)) | next);
                m_pendingRebuild = true;
            };
            m_scrollCtls.push_back(field);
        }
        dnRound(digitBar, 1.f, topY - 20.f, buttonW - 3.f, topY - 3.f, 3.f,
                unpackC(std::max(get(), 0)), 1.f, 3);

    }

    void rowToggle(std::string const& label, std::function<bool()> get, std::function<void(bool)> set) {
        int ri = beginRow(22.f);
        float top = m_rows[ri].top;
        float w = m_inW;
        rowLabel(ri, label, 0.f, top - 11.f, 0.30f, ccColor3B{235, 235, 240}, 0.f);
        auto d = rowDraw(ri);

        auto refresh = [=]() {
            bool on = get();
            d->clear();
            dnRound(d, w - 30.f, top - 17.f, w - 2.f, top - 5.f, 6.f,
                    on ? ccColor3B{70, 110, 240} : ccColor3B{70, 70, 86}, 1.f, 4);
            float kx = on ? w - 8.f : w - 24.f;
            dnRound(d, kx - 5.f, top - 16.f, kx + 5.f, top - 6.f, 5.f, kWhite, 1.f, 4);
        };
        refresh();

        Ctl c;
        c.scroll = true;
        c.row = ri;
        c.r = CCRect(0.f, top - 22.f, w, 22.f);
        c.down = [=](CCPoint) {
            set(!get());
            refresh();
        };
        m_scrollCtls.push_back(c);
    }

    void rowChoice(std::string const& label, std::vector<std::string> opts,
                   std::function<int()> get, std::function<void(int)> set) {
        int ri = beginRow(36.f);
        float top = m_rows[ri].top;
        float w = m_inW;
        rowLabel(ri, label, 0.f, top - 8.f, 0.30f, ccColor3B{235, 235, 240}, 0.f);
        auto d = rowDraw(ri);
        int n = static_cast<int>(opts.size());
        float bw = (w - (n - 1) * 3.f) / n;
        float y0 = top - 32.f, y1 = top - 16.f;

        for (int i = 0; i < n; i++) {
            float cxm = i * (bw + 3.f) + bw / 2.f;
            rowLabel(ri, opts[i], cxm, (y0 + y1) / 2.f, 0.28f, kWhite, 0.5f);
        }

        auto refresh = [=]() {
            d->clear();
            int cur = get();
            for (int i = 0; i < n; i++) {
                float x0 = i * (bw + 3.f);
                dnRound(d, x0, y0, x0 + bw, y1, 4.f,
                        i == cur ? ccColor3B{70, 110, 240} : ccColor3B{58, 58, 72}, 1.f, 3);
            }
        };
        refresh();

        Ctl c;
        c.scroll = true;
        c.row = ri;
        c.r = CCRect(0.f, top - 36.f, w, 24.f);
        c.down = [=](CCPoint p) {
            int i = std::clamp(static_cast<int>(p.x / (bw + 3.f)), 0, n - 1);
            set(i);
            refresh();
        };
        m_scrollCtls.push_back(c);
    }

    // 사용자 프리셋 한 줄: 이름 + LOAD / SAVE / DEL
    void rowPresetEntry(int i) {
        int ri = beginRow(28.f);
        float top = m_rows[ri].top;
        float w = m_inW;
        auto d = rowDraw(ri);
        float bw = std::min(32.f, w * 0.22f), bh = 18.f;
        float y0 = top - 23.f, y1 = y0 + bh;
        float xs[3] = {w - 3.f * bw - 4.f, w - 2.f * bw - 2.f, w - bw};
        char const* names[3] = {"LOAD", "SAVE", "DEL"};
        ccColor3B cols[3] = {{70, 110, 240}, {60, 150, 90}, {170, 70, 80}};

        std::function<void()> actions[3];
        actions[0] = [this, i]() {
            if (!applyPresetData(presetData(i))) {
                setStatus("Invalid preset.");
                return;
            }
            m_viewer->resetInput();
            bool persisted = Mod::get()->saveData().isOk();
            m_viewer->applyLook();
            this->ensureSelection();
            this->redrawCanvas();
            m_pendingRebuild = true;
            this->setStatus(persisted ? uiText("Loaded ") + presetName(i) : uiText("Preset storage failed."));
            redrawChrome();
        };
        actions[1] = [this, i]() {
            presetSetData(i, serializePreset());
            this->setStatus(Mod::get()->saveData().isOk() ? uiText("Saved over ") + presetName(i) : uiText("Preset storage failed."));
        };
        actions[2] = [this, i]() {
            std::string n = presetName(i);
            presetDelete(i);
            m_pendingRebuild = true;
            this->setStatus(Mod::get()->saveData().isOk() ? uiText("Deleted ") + n : uiText("Preset storage failed."));
        };

        for (int k = 0; k < 3; k++) {
            dnRound(d, xs[k], y0, xs[k] + bw, y1, 4.f, cols[k], 1.f, 3);
            rowLabel(ri, names[k], xs[k] + bw / 2.f, (y0 + y1) / 2.f, 0.26f, kWhite, 0.5f);
            Ctl c;
            c.scroll = true;
            c.row = ri;
            c.r = CCRect(xs[k], y0, bw, bh);
            auto act = actions[k];
            c.down = [act](CCPoint) { act(); };
            m_scrollCtls.push_back(c);
        }

        auto nameLab = rowLabel(ri, presetName(i), 0.f, (y0 + y1) / 2.f, 0.30f, kWhite, 0.f);
        if (nameLab) {
            float lw = nameLab->getContentSize().width * 0.30f;
            float maxW = xs[0] - 6.f;
            if (lw > maxW && lw > 0.f) nameLab->setScale(0.30f * maxW / lw);
        }
    }

    // 전체 설정(모드 설정)과 연결된 슬라이더, 스위치, 색
    int styleKey() const { return m_tab <= 2 ? m_sel : -1; }

    static double styleValue(int id, std::string const& key, double fallback) {
        if (auto k = keyById(id)) {
            auto it = k->style.find(key);
            if (it != k->style.end()) return it->second;
        }
        return fallback;
    }

    bool writeStyle(int id, std::string const& key, double value) {
        auto k = keyById(id);
        if (!k) return false;
        if (value < 0) k->style.erase(key);
        else k->style[key] = value;
        commitKey(id);
        return true;
    }

    void settingSlider(std::string const& label, char const* key, float vmin, float vmax, float step, bool isInt = false) {
        std::string k = key;
        int id = styleKey();
        rowSlider(label, vmin, vmax, step,
            [k, isInt, id]() {
                double base = isInt ? double(Mod::get()->getSettingValue<int64_t>(k)) : Mod::get()->getSettingValue<double>(k);
                return static_cast<float>(styleValue(id, k, base));
            },
            [this, k, isInt, id](float v) {
                if (writeStyle(id, k, v)) return;
                if (isInt) Mod::get()->setSettingValue<int64_t>(k, static_cast<int64_t>(v));
                else Mod::get()->setSettingValue<double>(k, static_cast<double>(v));
                this->refreshAll();
            });
    }

    void settingToggle(std::string const& label, char const* key) {
        std::string k = key;
        int id = styleKey();
        rowToggle(label,
            [id, k]() { return styleValue(id, k, Mod::get()->getSettingValue<bool>(k)) != 0; },
            [this, id, k](bool v) {
                if (writeStyle(id, k, v)) return;
                Mod::get()->setSettingValue<bool>(k, v);
                this->refreshAll();
            });
    }

    void settingColor(std::string const& label, char const* key) {
        std::string k = key;
        int id = styleKey();
        rowColor(label,
            [id, k]() { return static_cast<int>(styleValue(id, k, packC(Mod::get()->getSettingValue<ccColor3B>(k)))); },
            [this, id, k](int v) {
                if (writeStyle(id, k, v)) return;
                if (v >= 0) Mod::get()->setSettingValue<ccColor3B>(k, unpackC(v));
                this->refreshAll();
            },
            id >= 0);
    }

    void showInput(std::string title, std::string value, bool numeric,
                   std::function<bool(std::string const&)> apply, std::function<void(int)> capture = {}) {
        auto popup = EditorInputPopup::create(std::move(title), std::move(value), numeric, std::move(apply),
            std::move(capture), [this] { m_modal = false; m_pendingRebuild = true; });
        if (popup) {
            m_modal = true;
            popup->show();
        }
    }

    void geometryRow(std::string const& title, std::array<char const*, 2> const& names, int id,
                     std::array<float KeyDef::*, 2> const& fields,
                     std::array<float, 2> const& low, std::array<float, 2> const& high) {
        int row = beginRow(44.f);
        float top = m_rows[row].top;
        rowLabel(row, title, 0.f, top - 8.f, 0.30f, kWhite, 0.f);
        auto bg = rowDraw(row);
        float width = (m_inW - 4.f) / 2.f;
        for (size_t i = 0; i < fields.size(); ++i) {
            auto key = keyById(id);
            if (!key) return;
            float x = i * (width + 4.f);
            dnRound(bg, x, top - 39.f, x + width, top - 17.f, 4.f, {58, 58, 72}, 1.f, 3);
            std::string label = std::string(names[i]) + "  " + fmtNum(key->*fields[i]);
            rowLabel(row, label, x + width / 2.f, top - 28.f, 0.32f, kWhite, 0.5f);
            Ctl ctl;
            ctl.row = row;
            ctl.r = CCRect(x, top - 39.f, width, 22.f);
            ctl.down = [this, id, field = fields[i], min = low[i], max = high[i], name = std::string(names[i])](CCPoint) {
                auto key = keyById(id);
                if (!key) return;
                showInput(name, fmtNum(key->*field), true, [this, id, field, min, max](std::string const& input) {
                    char* end = nullptr;
                    float value = std::strtof(input.c_str(), &end);
                    if (end == input.c_str() || *end || !std::isfinite(value) || value < min || value > max)
                        return false;
                    if (auto key = keyById(id)) {
                        key->*field = value;
                        commitKey(id);
                    }
                    return true;
                });
            };
            m_scrollCtls.push_back(ctl);
        }
    }

    void localSlider(std::string const& label, std::string const& style, float low, float high, float step, float base) {
        int id = m_sel;
        rowSlider(label, low, high, step,
            [id, style, base] { return static_cast<float>(styleValue(id, style, base)); },
            [this, id, style](float value) { writeStyle(id, style, value); });
    }

    void localToggle(std::string const& label, std::string const& style, bool base) {
        int id = m_sel;
        rowToggle(label, [id, style, base] { return styleValue(id, style, base) != 0; },
            [this, id, style](bool value) { writeStyle(id, style, value); });
    }

    void localChoice(std::string const& label, std::string const& style, std::vector<std::string> choices, int base) {
        int id = m_sel;
        rowCycle(label, std::move(choices), [id, style, base] { return static_cast<int>(styleValue(id, style, base)); },
            [this, id, style](int value) { writeStyle(id, style, value); });
    }

    void localColor(std::string const& label, std::string const& style, int base, bool autoColor) {
        int id = m_sel;
        rowColor(label, [id, style, base] { return static_cast<int>(styleValue(id, style, base)); },
            [this, id, style](int value) { writeStyle(id, style, value); }, autoColor);
    }

    void fontChoice(std::string const& style) {
        localChoice("Font", style, {"Inter", "Noto Sans", "GD Big", "GD Gold"}, 0);
    }

    void buildKeyTab() {
        int id = m_sel;
        {
            int ri = beginRow(18.f);
            rowLabel(ri, uiText("KEY  ") + describeKey(id), 0.f, m_rows[ri].top - 9.f, 0.32f, kWhite, 0.f);
        }
        std::vector<int> ids;
        std::vector<std::string> names;
        for (auto const& key : g_keys) {
            ids.push_back(key.id);
            names.push_back(describeKey(key.id));
        }
        rowCycle("Select key", names,
            [id, ids]() { return int(std::find(ids.begin(), ids.end(), id) - ids.begin()); },
            [this, ids](int index) {
                m_sel = ids[index];
                redrawCanvas();
                m_pendingRebuild = true;
            });
        std::vector<std::string> maps(kMapNames, kMapNames + 6);
        rowCycle("Input", maps,
            [id]() { auto k = keyById(id); return k ? k->map : 0; },
            [this, id](int v) {
                if (auto k = keyById(id)) k->map = std::clamp(v, 0, 5);
                this->commitKey(id);
            });
        std::vector<std::string> labels(kLabels, kLabels + kLabelCount);
        rowCycle("Label", labels,
            [id]() { auto k = keyById(id); return k ? k->label : 0; },
            [this, id](int v) {
                if (auto k = keyById(id)) k->label = std::clamp(v, 0, kLabelCount - 1);
                this->commitKey(id);
            });
        auto key = keyById(id);
        if (!key) return;
        int code = lookForKey(readLook(), *key).keyCode;
        rowInfo(code ? uiText("Bound key ") + keyName(code) : uiText("Game action input"));
        rowButton("Bind keyboard key", {70, 110, 240}, [this, id] {
            showInput("Bind keyboard key", "", false, {}, [this, id](int code) {
                m_viewer->resetInput();
                writeStyle(id, "key-code", code);
            });
        });
        if (code) rowButton("Use game action", {58, 58, 72}, [this, id] {
            m_viewer->resetInput();
            writeStyle(id, "key-code", -1);
            m_pendingRebuild = true;
        });
        geometryRow("Position", {"X", "Y"}, id, {&KeyDef::x, &KeyDef::y}, {-500.f, -300.f}, {500.f, 300.f});
        geometryRow("Size", {"W", "H"}, id, {&KeyDef::w, &KeyDef::h}, {0.f, 0.f}, {500.f, 500.f});
        rowInfo("Size 0 uses the default.");
        rowButton(key->text.empty() ? "Set custom text" : uiText("Text ") + key->text, {58, 58, 72}, [this, id] {
            if (auto key = keyById(id)) showInput("Set custom text", key->text, false, [this, id](std::string const& text) {
                if (auto k = keyById(id)) {
                    k->text = cleanStr(text.substr(0, 24));
                    commitKey(id);
                }
                return true;
            });
        });
        fontChoice("label-font");
        localSlider("Font size", "label-size", 4.f, 40.f, 1.f, 10.f);
        localColor("Font color", "label-color", -1, true);
        float gr = readLook().radius * 100.f;
        rowSlider("Rounding %", 0.f, 50.f, 1.f,
            [id, gr]() { auto k = keyById(id); return (k && k->radius >= 0.f) ? k->radius : gr; },
            [this, id](float v) { if (auto k = keyById(id)) k->radius = v; this->commitKey(id); });
        rowSlider("Border width", 0.f, 6.f, 0.5f,
            [id]() { auto k = keyById(id); return k ? k->borderW : 0.f; },
            [this, id](float v) { if (auto k = keyById(id)) k->borderW = v; this->commitKey(id); });
        settingSlider("Idle opacity", "tile-opacity", 0.f, 255.f, 1.f, true);
        settingToggle("Press glow", "show-glow");
        settingToggle("Key counters", "show-counter");
        rowInfo("Hex: tap a digit to change it.");
        rowColor("Background color",
            [id]() { auto k = keyById(id); return k ? k->bg : -1; },
            [this, id](int v) { if (auto k = keyById(id)) k->bg = v; this->commitKey(id); }, true);
        rowColor("Pressed color",
            [id]() { auto k = keyById(id); return k ? k->pressed : -1; },
            [this, id](int v) { if (auto k = keyById(id)) k->pressed = v; this->commitKey(id); }, true);
        rowColor("Border color",
            [id]() { auto k = keyById(id); return k ? k->border : -1; },
            [this, id](int v) { if (auto k = keyById(id)) k->border = v; this->commitKey(id); }, true);
    }

    void buildNoteTab() {
        int id = m_sel;
        {
            int ri = beginRow(18.f);
            rowLabel(ri, uiText("NOTES  ") + describeKey(id), 0.f, m_rows[ri].top - 9.f, 0.32f, kWhite, 0.f);
        }
        rowInfo("Hex: tap a digit to change it.");
        rowColor("Note color (this key)",
            [id]() { auto k = keyById(id); return k ? k->accent : -1; },
            [this, id](int v) { if (auto k = keyById(id)) k->accent = v; this->commitKey(id); }, true);
        this->settingToggle("Note timeline", "show-timeline");
        localToggle("Auto Y correction", "note-auto-y", false);
        this->settingToggle("Note glow", "note-glow");
        this->settingSlider("Note width %", "note-width", 20.f, 100.f, 1.f);
        this->settingSlider("Min length", "note-min-length", 2.f, 60.f, 1.f);
        this->settingSlider("Speed", "note-speed", 100.f, 600.f, 5.f);
        this->settingSlider("Timeline length", "note-height", 40.f, 220.f, 5.f);
        rowChoice("Gradient colors", {"Off", "Notes", "Tile", "Both"},
            [id]() {
                auto g = Mod::get()->getSettingValue<std::string>("gradient-mode");
                for (int i = 0; i < 4; i++) if (g == kGradNames[i]) return static_cast<int>(std::clamp(styleValue(id, "gradient-mode", i), 0.0, 3.0));
                return 0;
            },
            [this, id](int i) {
                writeStyle(id, "gradient-mode", i);
            });
        this->settingColor("Gradient end color", "gradient-color");
        this->settingSlider("Note opacity %", "note-opacity", 20.f, 100.f, 1.f);
        this->settingSlider("Note fade", "note-fade", 0.f, 3.f, 0.1f);
        this->settingSlider("Note round %", "note-radius", 0.f, 50.f, 1.f);
        this->settingSlider("Note border", "note-border", 0.f, 4.f, 0.5f);
        this->settingColor("Note border color", "note-border-color");
        this->settingSlider("Glow strength %", "note-glow-strength", 0.f, 200.f, 5.f);
        this->settingSlider("Glow size %", "note-glow-size", 50.f, 250.f, 5.f);
        this->settingToggle("Custom glow color", "note-glow-custom");
        this->settingColor("Glow color", "note-glow-color");
    }

    void buildCounterTab() {
        rowInfo(uiText("Selected counter ") + describeKey(m_sel));
        settingToggle("Key counters", "show-counter");
        localChoice("Placement", "count-area", {"Inside", "Outside"}, 0);
        localChoice("Direction", "count-direction", {"Below", "Above", "Left", "Right"}, 0);
        localChoice("Alignment", "count-align", {"Start", "Center", "End"}, 1);
        localSlider("Spacing", "count-gap", 0.f, 40.f, 1.f, 3.f);
        localToggle("Fill", "count-fill", true);
        localColor("Fill color", "count-color", -1, true);
        localSlider("Outline thickness", "count-stroke", 0.f, 4.f, 0.25f, 0.f);
        localColor("Outline color", "count-stroke-color", 0, false);
        fontChoice("count-font");
        localSlider("Font size", "count-size", 4.f, 40.f, 1.f, 7.f);
        localToggle("Counter animation", "count-motion", false);
        localChoice("Motion", "count-motion-type", {"Pulse", "Bounce"}, 0);
        localSlider("Motion amount %", "count-motion-amount", 0.f, 100.f, 5.f, 25.f);
        localSlider("Motion time (s)", "count-motion-time", 0.05f, 1.f, 0.05f, 0.22f);
    }

    void buildViewTab() {
        rowChoice("Language", {"English", "Korean"},
            []() { return koreanUI() ? 1 : 0; },
            [this](int choice) {
                Mod::get()->setSettingValue<std::string>("language", choice ? "Korean" : "English");
                m_reloadLanguage = true;
            });
        this->settingSlider("Box size", "box-size", 24.f, 80.f, 1.f);
        this->settingSlider("Jump width x", "jump-width", 0.5f, 3.f, 0.1f);
        this->settingSlider("Corner round %", "corner-radius", 0.f, 50.f, 1.f);
        this->settingSlider("New key gap", "box-gap", 0.f, 24.f, 1.f);
        this->settingSlider("Idle opacity", "tile-opacity", 0.f, 255.f, 5.f, true);
        rowInfo("Hex: tap a digit to change it.");
        this->settingColor("Idle box color", "tile-color");
        this->settingColor("Pressed box color", "pressed-color");
        this->settingToggle("Press glow", "show-glow");
        settingToggle("KPS", "show-kps");
        settingToggle("Total presses", "show-total");
        settingToggle("Reset on restart", "reset-on-restart");
        this->settingSlider("Position X %", "pos-x", 0.f, 100.f, 1.f);
        this->settingSlider("Position Y %", "pos-y", 0.f, 100.f, 1.f);
    }

    void buildPresetTab() {
        this->rowButton("Save current as new preset", ccColor3B{70, 110, 240}, [this]() {
            presetAdd(serializePreset());
            m_pendingRebuild = true;
            this->setStatus(Mod::get()->saveData().isOk() ? "Saved a new preset." : "Preset storage failed.");
        });
        int n = presetCount();
        if (n == 0) this->rowInfo("No presets yet.");
        for (int i = 0; i < n; i++) this->rowPresetEntry(i);
        this->rowInfo("A preset stores all settings and keys.");
    }

    void rebuildInspector() {
        m_content->removeAllChildrenWithCleanup(true);
        m_scrollCtls.clear();
        m_rows.clear();
        m_cursor = 0.f;
        if (m_tab == 0) this->buildKeyTab();
        else if (m_tab == 1) this->buildNoteTab();
        else if (m_tab == 2) this->buildCounterTab();
        else if (m_tab == 3) this->buildViewTab();
        else this->buildPresetTab();
        m_contentH = -m_cursor;
        float maxS = std::max(0.f, m_contentH - (vpY1 - vpY0));
        m_scroll = std::clamp(m_scroll, 0.f, maxS);
        this->applyScroll();
    }

    void scrollInspector(float delta) {
        float maxScroll = std::max(0.f, m_contentH - (vpY1 - vpY0));
        m_scroll = std::clamp(m_scroll + delta, 0.f, maxScroll);
        applyScroll();
    }

    void applyScroll() {
        m_content->setPosition({inX0 + 6.f, vpY1 + m_scroll});
        float vpH = vpY1 - vpY0;
        for (auto& r : m_rows) {
            bool vis = r.bottom + m_scroll < 0.f && r.top + m_scroll > -vpH;
            r.visible = vis;
            for (auto n : r.nodes) n->setVisible(vis);
        }
        m_bar->clear();
        float maxS = std::max(0.f, m_contentH - vpH);
        if (maxS > 0.5f && m_contentH > 0.f) {
            float barH = std::max(16.f, vpH * vpH / m_contentH);
            float t = m_scroll / maxS;
            float y1 = vpY1 - t * (vpH - barH);
            dnRect(m_bar, inX1 - 4.f, y1 - barH, inX1 - 2.5f, y1, ccColor3B{150, 150, 170}, 0.7f);
        }
    }

    // ---------- 터치 ----------
    bool inViewport(CCPoint p) const {
        return p.x >= inX0 && p.x <= inX1 && p.y >= vpY0 && p.y <= vpY1;
    }

    CCPoint toLocal(CCPoint p) const {
        return CCPoint(p.x - (inX0 + 6.f), p.y - (vpY1 + m_scroll));
    }

    int hitScroll(CCPoint p) const {
        if (m_mapMode || !this->inViewport(p)) return -1;
        CCPoint lp = this->toLocal(p);
        for (int i = static_cast<int>(m_scrollCtls.size()) - 1; i >= 0; i--) {
            auto const& c = m_scrollCtls[i];
            if (c.row >= 0 && c.row < static_cast<int>(m_rows.size()) && !m_rows[c.row].visible) continue;
            if (c.r.containsPoint(lp)) return i;
        }
        return -1;
    }

public:
    static KVEditor* create(KeyViewer* viewer) {
        auto ret = new KVEditor();
        if (ret->init(viewer)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    // 가장 먼저 터치를 받아서 뒤쪽(일시정지 메뉴 등)으로 새지 않게 한다
    void registerWithTouchDispatcher() override {
        CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -2000, true);
    }

    void scrollWheel(float y, float) override {
        if (m_modal || m_dragMode || !std::isfinite(y) || y == 0.f) return;
        float delta = std::copysign(std::clamp(std::abs(y), 24.f, 72.f), y);
        if (m_mapMode) {
            auto pos = m_viewer->getPosition();
            Mod::get()->setSettingValue<double>("pos-y", std::clamp((pos.y - delta) / H * 100., 0., 100.));
            refreshAll();
        } else {
            scrollInspector(delta);
        }
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        if (m_modal) return false;
        auto p = this->convertTouchToNodeSpace(touch);
        m_activeFixed = -1;
        m_activeScroll = -1;
        m_scrolling = false;
        int wantMode = m_mapMode ? 1 : 0;

        for (int i = static_cast<int>(m_fixed.size()) - 1; i >= 0; i--) {
            auto& c = m_fixed[i];
            if (c.mode != wantMode) continue;
            if (!c.r.containsPoint(p)) continue;
            m_activeFixed = i;
            auto fn = c.down;
            if (fn) fn(p);
            return true;
        }
        if (!m_mapMode && this->inViewport(p)) {
            int i = p.x >= inX1 - 10.f ? -1 : this->hitScroll(p);
            if (i >= 0) {
                m_activeScroll = i;
                auto fn = m_scrollCtls[i].down;
                if (fn) fn(this->toLocal(p));
            } else {
                m_scrolling = true;
                m_lastY = p.y;
            }
        }
        return true;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        auto p = this->convertTouchToNodeSpace(touch);
        if (m_activeFixed >= 0 && m_activeFixed < static_cast<int>(m_fixed.size())) {
            auto fn = m_fixed[m_activeFixed].move;
            if (fn) fn(p);
        } else if (m_activeScroll >= 0 && m_activeScroll < static_cast<int>(m_scrollCtls.size())) {
            auto fn = m_scrollCtls[m_activeScroll].move;
            if (fn) fn(this->toLocal(p));
        } else if (m_scrolling) {
            float dy = p.y - m_lastY;
            m_lastY = p.y;
            scrollInspector(dy);
        }
    }

    void finishTouch(CCTouch* touch) {
        auto p = this->convertTouchToNodeSpace(touch);
        if (m_activeFixed >= 0 && m_activeFixed < static_cast<int>(m_fixed.size())) {
            auto fn = m_fixed[m_activeFixed].up;
            if (fn) fn(p);
        } else if (m_activeScroll >= 0 && m_activeScroll < static_cast<int>(m_scrollCtls.size())) {
            auto fn = m_scrollCtls[m_activeScroll].up;
            if (fn) fn(this->toLocal(p));
        }
        m_activeFixed = -1;
        m_activeScroll = -1;
        m_scrolling = false;
    }

    void ccTouchEnded(CCTouch* touch, CCEvent*) override { this->finishTouch(touch); }
    void ccTouchCancelled(CCTouch* touch, CCEvent*) override { this->finishTouch(touch); }

    void update(float) override {
        if (m_reloadLanguage) {
            m_reloadLanguage = false;
            if (auto next = KVEditor::create(m_viewer)) {
                next->m_sel = m_sel;
                next->m_tab = m_tab;
                next->setMapMode(false);
                getParent()->addChild(next, 100000);
                removeFromParentAndCleanup(true);
                return;
            }
        }
        if (m_pendingRebuild) {
            m_pendingRebuild = false;
            this->rebuildInspector();
        }
        if (m_closing) {
            m_closing = false;
            this->setPauseVisible(true);
            m_viewer->applyLook();
            this->removeFromParentAndCleanup(true);
        }
    }
};

// ---------------------------------------------------------------
// 훅
// ---------------------------------------------------------------
class $modify(KVPlayLayer, PlayLayer) {
    struct Fields {
        KeyViewer* viewer = nullptr;
    };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        auto viewer = KeyViewer::create(m_levelSettings->m_platformerMode);
        if (!viewer) return;
        this->addChild(viewer, 1000);
        m_fields->viewer = viewer;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (m_fields->viewer) m_fields->viewer->onRestart();
    }
};

class $modify(KVBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        auto pl = PlayLayer::get();
        if (!pl) return;
        auto viewer = static_cast<KVPlayLayer*>(pl)->m_fields->viewer;
        if (viewer) viewer->onButton(button, down, isPlayer1);
    }
};

class $modify(KVKeyboard, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        auto result = CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
        if (down && repeat) return result;
        auto scene = CCDirector::get()->getRunningScene();
        auto play = PlayLayer::get();
        if (!play || !scene) return result;
        auto viewer = static_cast<KVPlayLayer*>(play)->m_fields->viewer;
        if (viewer && (!down || !scene->getChildByType<PauseLayer>(0)))
            viewer->onKeyboard(static_cast<int>(key), down);
        return result;
    }
};

// 일시정지 화면에 "Key Viewer" 버튼을 추가하고, 누르면 에디터를 연다
class $modify(KVPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto win = CCDirector::get()->getWinSize();
        auto spr = ButtonSprite::create("Key Viewer");
        if (koreanUI()) {
            if (auto label = spr->getChildByType<CCLabelBMFont>(0)) label->setVisible(false);
            auto size = spr->getContentSize();
            mkLabel(spr, "KEY VIEWER", 0.52f, kWhite, {size.width / 2, size.height / 2}, 0.5f);
        }
        spr->setScale(0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(KVPauseLayer::onKeyViewer));
        auto menu = CCMenu::create();
        menu->addChild(btn);
        btn->setPosition({0.f, 0.f});
        menu->setPosition({win.width - 64.f, 24.f});
        this->addChild(menu, 20);
    }

    void onKeyViewer(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl) return;
        auto viewer = static_cast<KVPlayLayer*>(pl)->m_fields->viewer;
        if (!viewer) return;
        auto scene = CCDirector::get()->getRunningScene();
        if (!scene) return;
        auto editor = KVEditor::create(viewer);
        if (!editor) return;
        scene->addChild(editor, 100000);
    }
};
