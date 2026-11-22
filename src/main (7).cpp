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
    if (std::fabs(v - std::round(v)) < 0.0005f) std::snprintf(buf, sizeof buf, "%.0f", v);
    else std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string result = buf;
    if (result.find('.') != std::string::npos) {
        while (result.back() == '0') result.pop_back();
        if (result.back() == '.') result.pop_back();
    }
    return result;
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

static bool hasHangul(std::string_view text) {
    return std::any_of(text.begin(), text.end(), [](unsigned char byte) { return byte >= 0x80; });
}

static std::string const& nameCharacters() {
    static const auto supported = [] {
        std::string result;
        for (int code = 32; code < 127; ++code) result += static_cast<char>(code);
        auto addHangul = [&](int first, int last) {
            for (int code = first; code <= last; ++code) {
                result += static_cast<char>(0xe0 | (code >> 12));
                result += static_cast<char>(0x80 | ((code >> 6) & 63));
                result += static_cast<char>(0x80 | (code & 63));
            }
        };
        addHangul(0x3131, 0x318e);
        addHangul(0xac00, 0xd7a3);
        return result;
    }();
    return supported;
}

static bool validName(std::string_view name) {
    size_t count = 0;
    for (size_t i = 0; i < name.size();) {
        unsigned char first = name[i];
        if (++count > 24) return false;
        if (first >= 32 && first < 127) {
            ++i;
        } else if (first >= 0xe0 && first < 0xf0 && i + 2 < name.size()) {
            unsigned char second = name[i + 1], third = name[i + 2];
            if ((second & 0xc0) != 0x80 || (third & 0xc0) != 0x80) return false;
            int code = (first & 15) * 4096 + (second & 63) * 64 + (third & 63);
            if (!(code >= 0x3131 && code <= 0x318e) && !(code >= 0xac00 && code <= 0xd7a3)) return false;
            i += 3;
        } else {
            return false;
        }
    }
    return true;
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
struct GradientStop {
    int color = 0xffffff;
    int alpha = 255;
    bool operator==(GradientStop const&) const = default;
};

using GradientStops = std::array<GradientStop, 2>;

static GradientStop gradientAt(GradientStops const& stops, float t) {
    auto a = unpackC(stops[0].color), b = unpackC(stops[1].color);
    return {packC(lerpC(a, b, t)),
            static_cast<int>(std::lround(stops[0].alpha + (stops[1].alpha - stops[0].alpha) * t))};
}

static CCSpriteFrame* makeGradientFrame(GradientStops const& stops) {
    constexpr int side = 64;
    constexpr float radius = 7.f;
    std::array<unsigned char, side * side * 4> pixels{};
    for (int y = 0; y < side; ++y) {
        auto stop = gradientAt(stops, 1.f - y / float(side - 1));
        auto color = unpackC(stop.color);
        for (int x = 0; x < side; ++x) {
            float dx = std::max(0.f, std::abs(x + 0.5f - side / 2.f) - (side / 2.f - radius));
            float dy = std::max(0.f, std::abs(y + 0.5f - side / 2.f) - (side / 2.f - radius));
            float mask = std::clamp(radius + 0.5f - std::hypot(dx, dy), 0.f, 1.f);
            auto offset = (y * side + x) * 4;
            pixels[offset] = color.r;
            pixels[offset + 1] = color.g;
            pixels[offset + 2] = color.b;
            pixels[offset + 3] = static_cast<unsigned char>(std::lround(mask * stop.alpha));
        }
    }
    auto texture = new CCTexture2D;
    if (!texture->initWithData(pixels.data(), kCCTexture2DPixelFormat_RGBA8888, side, side, {side, side})) {
        delete texture;
        return nullptr;
    }
    texture->autorelease();
    return CCSpriteFrame::createWithTexture(texture, {0.f, 0.f, side, side});
}

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
    bool idleGradient = false;
    GradientStops noteGradient = {{{0xffffff, 255}, {0xffffff, 0}}};
    std::string labelJump = "JUMP";
    std::string labelLeft = "<";
    std::string labelRight = ">";
    float noteSpeed = 260.f;
    float noteHeight = 90.f;
    float noteWidth = 1.0f;
    float holdLengthScale = 1.f;
    float noteRimAlpha = 0.96f;
    float noteBaseGap = 5.f;
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
    bool statTile = false;
    bool statSideBySide = true;
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

enum StatMetric { TotalMetric, KpsMetric, PeakMetric, StatCount };

static const std::array<char const*, StatCount> kStatNames = {"Total", "KPS", "Peak KPS"};
static std::array<StyleValues, StatCount> g_statSettings;
static std::array<std::string, StatCount> g_statNames;

static std::string statKey(int index, char const* suffix) {
    return "stat_" + std::to_string(index) + "_" + suffix;
}

static void loadStats() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    auto mod = Mod::get();
    for (int i = 0; i < StatCount; ++i) {
        g_statSettings[i] = decodeStyle(mod->getSavedValue<std::string>(statKey(i, "style"), ""));
        g_statNames[i] = mod->getSavedValue<std::string>(statKey(i, "name"), "");
    }
}

static double statValue(int index, std::string const& key, double fallback) {
    auto const& settings = g_statSettings[index];
    if (auto it = settings.find(key); it != settings.end()) return it->second;
    return fallback;
}

static void saveStat(int index) {
    auto mod = Mod::get();
    mod->setSavedValue<std::string>(statKey(index, "style"), encodeStyle(g_statSettings[index]));
    mod->setSavedValue<std::string>(statKey(index, "name"), g_statNames[index]);
}

static std::string statName(int index) {
    if (!g_statNames[index].empty()) return g_statNames[index];
    int language = static_cast<int>(std::clamp(statValue(index, "language", 0), 0., 2.));
    if (language == 0 ? koreanUI() : language == 1) {
        static const std::array<char const*, StatCount> korean = {"합계", "초당 입력", "최고 KPS"};
        return korean[index];
    }
    return kStatNames[index];
}

// 전체 설정에 키별 설정을 덮어씌운다
static Look lookForKey(Look const& base, KeyDef const& k) {
    Look kl = base;
    int button = (k.map % 3) + 1;
    ccColor3B acc = k.accent >= 0 ? unpackC(k.accent) : accentFor(base, button);
    kl.accentJump = acc;
    kl.accentLeft = acc;
    kl.accentRight = acc;
    kl.noteGradient[0].color = packC(acc);
    kl.noteGradient[1].color = packC(acc);
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
        if (key == "note-gradient-bottom") kl.noteGradient[0].color = static_cast<int>(std::clamp(v, 0., 16777215.));
        if (key == "note-gradient-top") kl.noteGradient[1].color = static_cast<int>(std::clamp(v, 0., 16777215.));
        if (key == "gradient-bottom-alpha") kl.noteGradient[0].alpha = static_cast<int>(std::clamp(v, 0., 255.));
        if (key == "gradient-top-alpha") kl.noteGradient[1].alpha = static_cast<int>(std::clamp(v, 0., 255.));
        if (key == "tile-idle-gradient") kl.idleGradient = v != 0;
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
        if (key == "hold-length-scale") kl.holdLengthScale = static_cast<float>(std::clamp(v, 25., 200.)) / 100.f;
        if (key == "note-rim-opacity") kl.noteRimAlpha = static_cast<float>(std::clamp(v, 0., 100.)) / 100.f;
        if (key == "note-base-gap") kl.noteBaseGap = static_cast<float>(std::clamp(v, 0., 30.));
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
    // Explicit note settings take precedence over the legacy combined tile/notes selector.
    if (auto it = k.style.find("note-gradient-enabled"); it != k.style.end()) kl.gradNotes = it->second != 0;
    kl.gradTile = kl.gradTile || kl.idleGradient;
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
static std::array<char const*, 4> const kFonts = {
    "InterBold.fnt"_spr, "ViewerUI.fnt"_spr, "bigFont.fnt", "goldFont.fnt"
};

static void setFont(CCLabelBMFont* label, int font, bool hangul = false) {
    auto file = hangul ? "KeyNames.fnt"_spr : kFonts[font];
    auto current = label->getFntFile();
    if (!current || std::string_view(current) != file) label->setFntFile(file);
}

static float labelScale(CCLabelBMFont* label, float height, float maxWidth) {
    auto size = label->getContentSize();
    return std::min(height / std::max(1.f, size.height), maxWidth / std::max(1.f, size.width));
}

static CCPoint counterPosition(Look const& look, float w, float h, float width, float height) {
    const bool outside = look.countArea == 1;
    const float align = look.countAlign * 0.5f;
    const float gap = look.countGap;
    if (look.countDirection <= 1) {
        const bool below = look.countDirection == 0;
        const float x = width / 2 + (w - width) * align;
        if (outside) return {x, below ? -gap - height / 2 : h + gap + height / 2};
        const float inset = std::min(h / 2, height / 2 + gap);
        return {x, below ? inset : h - inset};
    }
    const bool left = look.countDirection == 2;
    const float y = height / 2 + (h - height) * align;
    if (outside) return {left ? -width / 2 - gap : w + width / 2 + gap, y};
    const float inset = std::min(w / 2, width / 2 + gap);
    return {left ? inset : w - inset, y};
}

static float textCenterY(Look const& look, float height) {
    if (!look.counter || look.countArea != 0 || (!look.countFill && look.countOutline <= 0.f))
        return height / 2.f;
    if (look.countDirection == 0) return height * 0.65f;
    if (look.countDirection == 1) return height * 0.35f;
    return height / 2.f;
}

struct NoteSprites {
    CCScale9Sprite* glow = nullptr;
    CCScale9Sprite* base = nullptr;
    CCScale9Sprite* gradient = nullptr;

    void hide() const {
        if (glow) glow->setVisible(false);
        if (base) base->setVisible(false);
        if (gradient) gradient->setVisible(false);
    }
};

static NoteSprites createNoteSprites(CCNode* parent) {
    NoteSprites s;
    s.glow = CCScale9Sprite::create("kv_glow.png"_spr);
    s.base = CCScale9Sprite::create("kv_solid.png"_spr);
    s.gradient = CCScale9Sprite::create("kv_solid.png"_spr);
    if (!s.glow || !s.base || !s.gradient) return {};
    parent->addChild(s.glow, 0);
    parent->addChild(s.base, 1);
    parent->addChild(s.gradient, 1);
    s.hide();
    return s;
}

static float holdNoteLength(double start, double end, float speed, float scale, float available) {
    return std::clamp(static_cast<float>(std::max(0.0, end - start) * speed * scale),
                      0.f, std::max(0.f, available));
}

static float noteSpriteScale(float radius, float width, float length) {
    return std::min({std::max(0.04f, radius / kBaseR), width / kCap, length / kCap});
}

static void sizeNote(NoteSprites const& s, float w, float h, float rounding, float glowScale) {
    float pad = kGlowPad * glowScale;
    fitSize(s.base, w, h, rounding);
    fitSize(s.gradient, w, h, rounding);
    fitSize(s.glow, w + 2 * pad, h + 2 * pad, glowScale);
}

static void useGradientFrame(NoteSprites const& sprites, CCSpriteFrame* frame) {
    if (!frame) return;
    sprites.gradient->setSpriteFrame(frame);
    sprites.gradient->setCapInsets({8.f, 8.f, 48.f, 48.f});
}

static void paintNote(NoteSprites const& s, CCDrawNode* border, Look const& look,
                      float x, float y, float w, float h, float rounding, float glowScale, float fade, bool gradient) {
    float pad = kGlowPad * glowScale;
    float opacity = std::clamp(look.noteOpacity, 0.05f, 1.f) * fade;
    auto accent = look.accentJump;
    auto fill = gradient ? s.gradient : s.base;
    s.base->setVisible(!gradient);
    s.gradient->setVisible(gradient);
    placeCenter(fill, x, y, w, h);
    fill->setColor(gradient ? ccColor3B{255, 255, 255} : accent);
    setOp(fill, opacity);

    bool glow = look.noteGlow && look.glowStrength > 0.01f;
    s.glow->setVisible(glow);
    if (glow) {
        auto color = look.glowCustom ? look.glowColor : gradient ? unpackC(look.noteGradient[0].color) : accent;
        placeCenter(s.glow, x - pad, y - pad, w + 2 * pad, h + 2 * pad);
        s.glow->setColor(color);
        float visible = gradient ? (look.noteGradient[0].alpha + look.noteGradient[1].alpha) / 510.f : 1.f;
        setOp(s.glow, 0.45f * look.glowStrength * fade * visible);
    }

    if (look.noteBorder > 0.f) {
        float thickness = std::min(look.noteBorder, h * 0.28f);
        dnStrokeA(border, x, y, x + w, y + h, rounding * kBaseR, look.noteBorderColor,
                  thickness, opacity * look.noteRimAlpha, 4);
    }
}

struct Note {
    int source = 0;
    double start = 0.0;
    double end = -1.0; // 아직 누르는 중이면 -1
    NoteSprites sp;
    unsigned gradientVersion = 0;
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
    CCSpriteFrame* m_gradientFrame = nullptr;
    GradientStops m_gradientStops{};
    unsigned m_gradientVersion = 0;

    ~KeyLane() override {
        if (m_gradientFrame) m_gradientFrame->release();
    }

    void refreshGradient(Look const& look) {
        if (!look.gradNotes || (m_gradientFrame && m_gradientStops == look.noteGradient)) return;
        auto frame = makeGradientFrame(look.noteGradient);
        if (!frame) return;
        frame->retain();
        if (m_gradientFrame) m_gradientFrame->release();
        m_gradientFrame = frame;
        m_gradientStops = look.noteGradient;
        ++m_gradientVersion;
    }

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

        m_name = CCLabelBMFont::create(name.c_str(), hasHangul(name) ? "KeyNames.fnt"_spr : "InterBold.fnt"_spr);
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
        return createNoteSprites(m_noteLayer);
    }

    void release(NoteSprites const& s) {
        if (!s.base) return;
        s.hide();
        m_pool.push_back(s);
    }

    void fitCount(double now = nowSec()) {
        const bool outside = m_look.countArea == 1;
        bool inlineStat = m_look.statTile && m_look.statSideBySide;
        float scale = labelScale(m_count, m_look.countSize, outside ? 400.f : m_w * (inlineStat ? 0.34f : 0.84f));
        float width = m_count->getContentSize().width * scale;
        float height = m_count->getContentSize().height * scale;
        float t = static_cast<float>((now - m_countStart) / m_look.countMotionTime);
        float motion = m_look.countMotion && m_countStart >= 0.0 && t >= 0.f && t < 1.f
            ? m_look.countMotionAmount / 100.f * (1.f - t) : 0.f;
        if (m_look.countMotionType == 0) scale *= 1.f + motion;
        auto pos = counterPosition(m_look, m_w, m_h, width, height);
        float x = inlineStat ? m_w - width / 2.f - 6.f : pos.x;
        float y = inlineStat ? m_h / 2.f : pos.y;
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
        m_noteStart = m_h + m_look.noteBaseGap;
        if (m_look.noteAutoY && m_look.counter && outside && m_look.countDirection == 1 &&
            (m_look.countFill || m_look.countOutline > 0))
            m_noteStart = std::max(m_noteStart, y + height / 2 + m_look.countOutline + m_look.noteBaseGap);
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

        if (m_look.gradTile && (m_look.idleGradient || p > 0.01f)) {
            m_tileGrad->setVisible(true);
            m_tileGrad->setColor(m_look.gradEnd);
            setOp(m_tileGrad, m_look.idleGradient ? idleA : p);
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
        auto idleBackground = m_look.idleGradient ? lerpC(m_look.tile, m_look.gradEnd, 0.5f) : m_look.tile;
        ccColor3B idleText = m_look.tileOpacity < 128 ? ccColor3B{255, 255, 255} : textFor(idleBackground);
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

    void setStatValue(int value) {
        if (m_presses == value) return;
        m_presses = value;
        setCountText();
        fitCount();
    }

    void setName(std::string const& name) {
        if (name == m_nameStr) return;
        m_nameStr = name;
        setFont(m_name, m_look.labelFont, hasHangul(name));
        m_name->setString(name.c_str());
    }

    void layout(Look const& l, float w, float h) {
        refreshGradient(l);
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

        setFont(m_name, l.labelFont, hasHangul(m_nameStr));
        setFont(m_count, l.countFont);
        for (auto border : m_countOutline) setFont(border, l.countFont);
        bool inlineStat = l.statTile && l.statSideBySide;
        m_name->setAnchorPoint({inlineStat ? 0.f : 0.5f, 0.5f});
        m_name->setScale(labelScale(m_name, l.labelSize, m_w * (inlineStat ? 0.52f : 0.84f)));
        m_name->setPosition({inlineStat ? 6.f : m_w / 2.f, inlineStat ? m_h / 2.f : textCenterY(l, m_h)});
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
        if (H <= 0.f) {
            clearNotes();
            return;
        }
        float sp = m_look.noteSpeed;

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
        for (auto& n : m_notes) {
            auto& s = n.sp;
            if (!s.base) continue;

            double e = n.end < 0 ? now : n.end;
            float yb = static_cast<float>((now - e) * sp);
            float len = holdNoteLength(n.start, e, sp, m_look.holdLengthScale, H - yb);
            if (yb >= H || len < 0.05f) {
                s.hide();
                continue;
            }
            float fade = std::pow(1.f - yb / H, std::max(0.f, m_look.noteFade));
            float sN = noteSpriteScale(noteR, nw, len);
            float py = baseY + yb;
            bool applyGradient = m_look.gradNotes && m_gradientFrame;
            if (applyGradient && n.gradientVersion != m_gradientVersion) {
                useGradientFrame(s, m_gradientFrame);
                n.gradientVersion = m_gradientVersion;
                n.lw = -1.f;
            }
            if (n.lw != nw || n.lh != len || n.ls != sN || n.lg != sg) {
                n.lw = nw; n.lh = len; n.ls = sN; n.lg = sg;
                sizeNote(s, nw, len, sN, sg);
            }
            paintNote(s, m_noteBorder, m_look, x0, py, nw, len, sN, sg, fade, applyGradient);
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

struct StatBox {
    int metric;
    CCPoint position;
    float width;
    float height;
};

static Look statLook(Look const& base, int metric) {
    KeyDef style;
    style.style = g_statSettings[metric];
    style.bg = static_cast<int>(std::clamp(statValue(metric, "background", -1), -1., 16777215.));
    auto border = static_cast<int>(std::clamp(statValue(metric, "rim", -1), -1., 16777215.));
    style.border = border;
    style.borderW = static_cast<float>(std::clamp(statValue(metric, "rim-width", 0), 0., 4.));
    style.radius = static_cast<float>(std::clamp(statValue(metric, "roundness", 7), 0., 50.));
    Look result = lookForKey(base, style);
    result.timeline = false;
    result.gradNotes = false;
    result.statTile = true;
    result.statSideBySide = statValue(metric, "stat-inline", 1) != 0;
    result.counter = true;
    result.countArea = result.countDirection = 0;
    result.countAlign = 1;
    result.countGap = 2.f;
    result.gradTile = result.idleGradient = statValue(metric, "idle-gradient", 0) != 0;
    return result;
}

class KeyViewer : public CCLayer {
protected:
    std::vector<LaneRef> m_lanes;
    std::vector<LaneGeo> m_geo;
    std::array<KeyLane*, StatCount> m_stats{};
    std::vector<StatBox> m_statGeo;
    CCLabelBMFont* m_hint = nullptr;
    CCDrawNode* m_frame = nullptr;
    std::deque<double> m_hits;
    unsigned m_heldSources = 0;
    std::set<int> m_heldKeys;
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

        loadStats();
        for (int i = 0; i < StatCount; ++i) {
            m_stats[i] = KeyLane::create(statName(i));
            if (m_stats[i]) {
                m_stats[i]->setVisible(false);
                this->addChild(m_stats[i], 4);
            }
        }

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
        layoutStats(m_minX, m_maxX, m_minY, m_maxY);
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

    std::vector<StatBox> const& statGeo() const { return m_statGeo; }

    StatBox const* statBox(int metric) const {
        for (auto const& box : m_statGeo) if (box.metric == metric) return &box;
        return nullptr;
    }

    void layoutStats(float minX, float maxX, float minY, float maxY) {
        m_statGeo.clear();
        auto win = CCDirector::get()->getWinSize();
        auto origin = getPosition();
        if (win.width <= 0 || win.height <= 0) return;
        std::array<float, 4> distance{};
        std::array<float, 4> extent{};
        std::array<int, StatCount> anchors{};
        std::array<float, StatCount> widths{}, heights{};
        std::array<bool, StatCount> shown{};
        for (int i = 0; i < StatCount; ++i) {
            bool baseVisible = i == TotalMetric ? m_look.total : i == KpsMetric && m_look.kps;
            shown[i] = m_stats[i] && statValue(i, "visible", baseVisible) != 0;
            widths[i] = static_cast<float>(std::clamp(statValue(i, "width", 72), 24., 200.));
            heights[i] = static_cast<float>(std::clamp(statValue(i, "height", 19), 12., 100.));
            anchors[i] = static_cast<int>(std::clamp(statValue(i, "anchor", 0), 0., 5.));
        }
        float availableBelow = origin.y + minY;
        int automatic = availableBelow >= 24.f ? 3 : 4;
        for (int i = 0; i < StatCount; ++i) {
            if (!shown[i]) continue;
            if (anchors[i] == 0) anchors[i] = automatic;
            if (anchors[i] == 5) continue;
            extent[anchors[i] - 1] += (anchors[i] <= 2 ? heights[i] : widths[i]) + 3.f;
        }
        for (int i : {KpsMetric, TotalMetric, PeakMetric}) {
            auto lane = m_stats[i];
            if (!lane) continue;
            lane->setVisible(shown[i]);
            if (!shown[i]) continue;

            float w = widths[i], h = heights[i];
            int anchor = anchors[i];
            float x, y;
            if (anchor == 5) {
                x = y = 0.f;
            } else if (anchor <= 2) {
                x = anchor == 1 ? maxX + 5.f : minX - w - 5.f;
                x = std::clamp(origin.x + x, 0.f, std::max(0.f, win.width - w)) - origin.x;
                float stack = std::clamp(origin.y + maxY, std::min(win.height, extent[anchor - 1]), win.height);
                y = stack - origin.y - h - distance[anchor - 1];
                distance[anchor - 1] += h + 3.f;
            } else {
                float start = std::clamp(origin.x + minX, 0.f, std::max(0.f, win.width - extent[anchor - 1]));
                x = start - origin.x + distance[anchor - 1];
                y = anchor == 3 ? minY - h - 5.f : maxY + 5.f;
                y = std::clamp(origin.y + y, 0.f, std::max(0.f, win.height - h)) - origin.y;
                distance[anchor - 1] += w + 3.f;
            }
            float shiftX = static_cast<float>(std::clamp(statValue(i, "x", 0), -500., 500.));
            float shiftY = static_cast<float>(std::clamp(statValue(i, "y", 0), -300., 300.));
            CCPoint pos{x + shiftX, y + shiftY};
            lane->setName(statName(i));
            int values[StatCount] = {m_total, static_cast<int>(m_hits.size()), m_peak};
            lane->setStatValue(values[i]);
            lane->layout(statLook(m_look, i), w, h);
            lane->setPosition(pos);
            lane->tick(nowSec(), 0.f, 0.f);
            m_statGeo.push_back({i, pos, w, h});
        }
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
        m_hint->setPosition({cxm, maxY + 14.f});

        if (!m_dragging) {
            auto win = CCDirector::get()->getWinSize();
            this->setPosition({win.width * m_look.posX / 100.f, win.height * m_look.posY / 100.f});
        }
        this->layoutStats(minX, maxX, minY, maxY);
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

        int values[StatCount] = {m_total, kps, m_peak};
        for (auto const& box : m_statGeo) {
            auto lane = m_stats[box.metric];
            lane->setStatValue(values[box.metric]);
            lane->tick(now, dt, 0.f);
        }
    }
};

// ---------------------------------------------------------------
// 에디터 창 (일시정지 메뉴의 Key Viewer 버튼으로 연다)
// 왼쪽 캔버스: 키를 눌러 선택(파란 테두리), 끌어서 이동, 손잡이로 크기 조절
// 오른쪽: 탭별 설정. 아래: 키 추가/삭제, 지도 배치 모드, 미리보기 등
// ---------------------------------------------------------------
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
    static inline EditorInputPopup* s_activeCapture = nullptr;
    static inline int s_skipRelease = 0;

    void stopCapture() {
        if (s_activeCapture == this) s_activeCapture = nullptr;
    }

    void onExit() override {
        stopCapture();
        Popup::onExit();
    }

    void onClose(CCObject* sender) override {
        stopCapture();
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

public:
    void focusInput() { if (m_field) m_field->focus(); }

    static bool captureKeyboardEvent(enumKeyCodes key, bool down, bool repeat) {
        const int code = static_cast<int>(key);
        if (s_skipRelease != 0 && code == s_skipRelease) {
            if (down && repeat) return true;
            s_skipRelease = 0;
            if (!down) return true;
        }
        auto popup = s_activeCapture;
        if (!popup || !down || code <= 0) return false;
        if (repeat) return true;
        s_skipRelease = code;
        if (key != KEY_Escape) {
            auto bind = std::move(popup->m_capture);
            bind(code);
        }
        popup->onClose(nullptr);
        return true;
    }

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
            s_activeCapture = popup;
            mkLabel(popup->m_mainLayer, "Press a keyboard key", 0.42f, kWhite, {120.f, 55.f}, 0.5f);
        } else {
            popup->m_field = geode::TextInput::create(172.f, "", numeric ? "bigFont.fnt" : "KeyNames.fnt"_spr);
            popup->m_field->setPosition({120.f, 64.f});
            popup->m_field->setMaxCharCount(numeric ? 9 : 72);
            popup->m_field->setFilter(numeric ? std::string("0123456789-.") : nameCharacters());
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

static ccColor3B hsvColor(float hue, float saturation, float value) {
    auto channel = [&](float offset) {
        float phase = std::fmod(hue * 6.f + offset, 6.f);
        float amount = value * (1.f - saturation * std::clamp(std::min(phase, 4.f - phase), 0.f, 1.f));
        return static_cast<GLubyte>(std::lround(amount * 255.f));
    };
    return {channel(5.f), channel(3.f), channel(1.f)};
}

static std::array<float, 3> hsvFromColor(int rgb) {
    auto color = unpackC(rgb);
    float r = color.r / 255.f, g = color.g / 255.f, b = color.b / 255.f;
    float max = std::max({r, g, b}), delta = max - std::min({r, g, b});
    float hue = 0.f;
    if (delta > 0.f) {
        if (r == max) hue = (g - b) / delta;
        else if (g == max) hue = 2.f + (b - r) / delta;
        else hue = 4.f + (r - g) / delta;
    }
    return {std::fmod(hue + 6.f, 6.f) / 6.f, max == 0.f ? 0.f : delta / max, max};
}

static void drawChecker(CCDrawNode* draw, float x, float y, float w, float h, float cell) {
    for (int iy = 0; iy * cell < h; ++iy) {
        for (int ix = 0; ix * cell < w; ++ix) {
            auto color = (ix + iy) % 2 ? ccColor3B{200, 205, 211} : ccColor3B{243, 246, 248};
            dnRect(draw, x + ix * cell, y + iy * cell, x + std::min(w, (ix + 1) * cell),
                   y + std::min(h, (iy + 1) * cell), color, 1.f);
        }
    }
}

static void drawRamp(CCDrawNode* draw, GradientStops const& stops, float x, float y, float w, float h) {
    if (w <= 0 || h <= 0) return;
    drawChecker(draw, x, y, w, h, 4.f);
    constexpr int slices = 36;
    for (int i = 0; i < slices; ++i) {
        auto color = gradientAt(stops, (i + 0.5f) / slices);
        dnRect(draw, x + w * i / slices, y, x + w * (i + 1) / slices, y + h,
               unpackC(color.color), color.alpha / 255.f);
    }
}

class VisualColorPopup : public geode::Popup {
    GradientStops m_colors;
    bool m_gradient = false;
    bool m_light = true;
    int m_stop = 0;
    int m_drag = 0;
    std::array<float, 3> m_hsv{};
    CCDrawNode* m_paint = nullptr;
    CCLabelBMFont* m_opacity = nullptr;
    std::function<void(GradientStops const&)> m_apply;
    std::function<void()> m_dismiss;

    void refresh() {
        m_paint->clear();
        auto field = m_light ? ccColor3B{246, 248, 251} : ccColor3B{38, 44, 53};
        auto mark = m_light ? ccColor3B{91, 83, 168} : ccColor3B{175, 166, 246};
        auto cursor = [&](float x, float y, float radius) {
            dnRound(m_paint, x - radius, y - radius, x + radius, y + radius, radius, kWhite, 1.f, 5);
            dnStrokeA(m_paint, x - radius, y - radius, x + radius, y + radius, radius, mark, 0.85f, 1.f, 5);
        };
        if (m_gradient) {
            drawRamp(m_paint, m_colors, 22.f, 181.f, 176.f, 12.f);
            cursor(m_stop == 0 ? 22.f : 198.f, 187.f, 4.5f);
        } else {
            dnRound(m_paint, 22.f, 180.f, 198.f, 193.f, 3.f, unpackC(m_colors[0].color), 1.f);
        }
        for (int y = 0; y < 18; ++y) {
            for (int x = 0; x < 24; ++x) {
                auto color = hsvColor(m_hsv[0], x / 23.f, y / 17.f);
                dnRect(m_paint, 22.f + 176.f * x / 24, 79.f + 92.f * y / 18,
                       22.f + 176.f * (x + 1) / 24, 79.f + 92.f * (y + 1) / 18, color, 1.f);
            }
        }
        cursor(22.f + m_hsv[1] * 176.f, 79.f + m_hsv[2] * 92.f, 3.6f);
        for (int i = 0; i < 36; ++i) {
            dnRect(m_paint, 22.f + 176.f * i / 36, 60.f, 22.f + 176.f * (i + 1) / 36,
                   69.f, hsvColor(i / 36.f, 1.f, 1.f), 1.f);
        }
        cursor(22.f + m_hsv[0] * 176.f, 64.5f, 3.7f);
        if (m_gradient) {
            auto color = m_colors[m_stop].color;
            drawRamp(m_paint, {{{color, 0}, {color, 255}}}, 22.f, 42.f, 176.f, 9.f);
            cursor(22.f + 176.f * m_colors[m_stop].alpha / 255.f, 46.5f, 3.7f);
            auto percent = std::to_string(std::lround(m_colors[m_stop].alpha * 100.f / 255.f)) + "%";
            if (m_opacity) m_opacity->setString(percent.c_str());
        }
        dnRound(m_paint, 131.f, 10.f, 199.f, 32.f, 4.f, field, 1.f);
    }

    void updateColor(CCPoint point) {
        float t = std::clamp((point.x - 22.f) / 176.f, 0.f, 1.f);
        if (m_drag == 1) {
            m_hsv[1] = t;
            m_hsv[2] = std::clamp((point.y - 79.f) / 92.f, 0.f, 1.f);
        } else if (m_drag == 2) {
            m_hsv[0] = t;
        } else if (m_drag == 3) {
            m_colors[m_stop].alpha = static_cast<int>(std::lround(t * 255.f));
        }
        if (m_drag == 1 || m_drag == 2) m_colors[m_stop].color = packC(hsvColor(m_hsv[0], m_hsv[1], m_hsv[2]));
        refresh();
    }

    void onClose(CCObject* sender) override {
        if (m_dismiss) {
            auto dismiss = std::move(m_dismiss);
            dismiss();
        }
        Popup::onClose(sender);
    }

public:
    static VisualColorPopup* create(GradientStops stops, bool gradient, bool light,
        std::function<void(GradientStops const&)> apply, std::function<void()> dismiss) {
        auto popup = new VisualColorPopup;
        if (!popup->Popup::init(220.f, 226.f)) {
            delete popup;
            return nullptr;
        }
        popup->m_colors = stops;
        popup->m_gradient = gradient;
        popup->m_light = light;
        popup->m_hsv = hsvFromColor(stops[0].color);
        popup->m_apply = std::move(apply);
        popup->m_dismiss = std::move(dismiss);
        popup->m_paint = CCDrawNode::create();
        auto background = CCDrawNode::create();
        dnRound(background, 8.f, 5.f, 212.f, 220.f, 9.f,
                light ? ccColor3B{230, 235, 241} : ccColor3B{27, 33, 40}, 1.f, 6);
        popup->m_mainLayer->addChild(background, 1);
        popup->m_mainLayer->addChild(popup->m_paint, 2);
        auto text = light ? ccColor3B{41, 48, 55} : kWhite;
        auto title = mkLabel(popup->m_mainLayer, gradient ? "Note gradient" : "Choose color",
                            0.40f, text, {110.f, 208.f}, 0.5f);
        if (title) title->setZOrder(3);
        if (gradient) {
            auto start = mkLabel(popup->m_mainLayer, "Bottom", 0.23f, text, {22.f, 175.f}, 0.f);
            auto tip = mkLabel(popup->m_mainLayer, "Fading tip", 0.23f, text, {198.f, 175.f}, 1.f);
            if (start) start->setZOrder(3);
            if (tip) tip->setZOrder(3);
            popup->m_opacity = mkLabel(popup->m_mainLayer, "", 0.29f, text, {24.f, 21.f}, 0.f);
            if (popup->m_opacity) popup->m_opacity->setZOrder(3);
        }
        auto save = mkLabel(popup->m_mainLayer, "Apply color", 0.29f, text, {165.f, 21.f}, 0.5f);
        if (save) save->setZOrder(3);
        if (popup->m_closeBtn) popup->m_closeBtn->setZOrder(4);
        if (popup->m_buttonMenu) popup->m_buttonMenu->setZOrder(4);
        popup->refresh();
        popup->autorelease();
        return popup;
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent* event) override {
        auto p = m_mainLayer->convertTouchToNodeSpace(touch);
        if (m_gradient && CCRect(16.f, 177.f, 188.f, 21.f).containsPoint(p)) {
            m_stop = p.x < 110.f ? 0 : 1;
            m_hsv = hsvFromColor(m_colors[m_stop].color);
            refresh();
            return true;
        }
        if (CCRect(131.f, 10.f, 68.f, 22.f).containsPoint(p)) {
            m_drag = 4;
            return true;
        }
        if (CCRect(16.f, 75.f, 188.f, 99.f).containsPoint(p)) m_drag = 1;
        else if (CCRect(16.f, 56.f, 188.f, 17.f).containsPoint(p)) m_drag = 2;
        else if (m_gradient && CCRect(16.f, 38.f, 188.f, 17.f).containsPoint(p)) m_drag = 3;
        else return Popup::ccTouchBegan(touch, event);
        updateColor(p);
        return true;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        if (m_drag > 0 && m_drag < 4) updateColor(m_mainLayer->convertTouchToNodeSpace(touch));
    }
    void ccTouchEnded(CCTouch* touch, CCEvent*) override {
        bool confirm = m_drag == 4 &&
            CCRect(131.f, 10.f, 68.f, 22.f).containsPoint(m_mainLayer->convertTouchToNodeSpace(touch));
        m_drag = 0;
        if (confirm) {
            m_apply(m_colors);
            onClose(nullptr);
        }
    }
    void ccTouchCancelled(CCTouch*, CCEvent*) override { m_drag = 0; }
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
    bool m_light = true;
    int m_dragStat = -1;
    int m_statTab = 0;

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
    CCLabelBMFont* m_tabLabels[4] = {};
    std::vector<CCLabelBMFont*> m_tileLabels;
    CCNode* m_counterPreview = nullptr;
    CCNode* m_notePreview = nullptr;
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

    ccColor3B uiInk() const { return m_light ? ccColor3B{40, 48, 56} : ccColor3B{234, 237, 240}; }
    ccColor3B uiMuted() const { return m_light ? ccColor3B{88, 97, 108} : ccColor3B{171, 179, 188}; }
    ccColor3B uiAccent() const { return m_light ? ccColor3B{83, 99, 181} : ccColor3B{151, 162, 239}; }
    ccColor3B uiField() const { return m_light ? ccColor3B{250, 251, 252} : ccColor3B{34, 40, 47}; }
    ccColor3B uiLine() const { return m_light ? ccColor3B{175, 186, 197} : ccColor3B{79, 89, 102}; }
    ccColor3B uiPanel() const { return m_light ? ccColor3B{246, 247, 249} : ccColor3B{29, 34, 39}; }

    bool init(KeyViewer* viewer) {
        if (!CCLayer::init()) return false;
        m_viewer = viewer;
        m_light = Mod::get()->getSettingValue<std::string>("editor-theme") != "Dark";
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
        m_notePreview = CCNode::create();
        m_canvasLabels->addChild(m_notePreview);
        m_counterPreview = CCNode::create();
        m_canvasLabels->addChild(m_counterPreview);
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
        mkLabel(m_winLabels, "KEY VIEWER", 0.40f, uiInk(), CCPoint(winX0 + 14.f, winY1 - 16.f), 0.f);
        mkLabel(m_winLabels, m_light ? "Dark mode" : "Light mode", 0.26f, uiInk(),
                CCPoint(winX1 - 72.f, winY1 - 16.f), 0.5f);
        {
            Ctl theme;
            theme.r = CCRect(winX1 - 111.f, winY1 - 27.f, 78.f, 22.f);
            theme.down = [this](CCPoint) { changeTheme(!m_light); };
            m_fixed.push_back(theme);
        }
        mkLabel(m_winLabels, "X", 0.40f, uiInk(), CCPoint(winX1 - 22.f, winY1 - 16.f), 0.5f);
        {
            Ctl c;
            c.r = CCRect(winX1 - 40.f, winY1 - 32.f, 34.f, 30.f);
            c.down = [this](CCPoint) { m_closing = true; };
            m_fixed.push_back(c);
        }
        m_status = mkLabel(m_winLabels, "Tap a key to select it. Drag it to move, drag the handles to resize.",
                           0.24f, uiMuted(), CCPoint(cvX0 + 8.f, cvY0 + 8.f), 0.f);

        // 탭
        {
            char const* names[4] = {"KEY", "NOTE", "COUNT", "VIEW"};
            float tw = (inX1 - inX0 - 12.f) / 4.f;
            for (int i = 0; i < 4; i++) {
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
                    this->redrawCanvas();
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

                auto lab = mkLabel(m_winLabels, b.label, 0.30f, uiInk(),
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
        {
            for (auto const& box : m_viewer->statGeo()) {
                minX = std::min(minX, box.position.x);
                maxX = std::max(maxX, box.position.x + box.width);
                minY = std::min(minY, box.position.y);
                maxY = std::max(maxY, box.position.y + box.height);
            }
        }
        float bw = std::max(20.f, maxX - minX), bh = std::max(20.f, maxY - minY);
        float sc = std::min(kSc, std::min(std::max(8.f, cw - 24.f) / bw, std::max(8.f, ch - 114.f) / bh));
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
        auto display = uiText(s);
        setFont(m_status, koreanUI() ? 1 : 0, hasHangul(display));
        m_status->setString(display.c_str());
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
        if (m_tab == 2 && m_statTab > 0) {
            updateStat(m_statTab - 1, "visible", 0);
            m_pendingRebuild = true;
            return;
        }
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
        if (m_tab == 2 && m_statTab > 0) {
            int metric = m_statTab - 1;
            auto& settings = g_statSettings[metric];
            for (auto it = settings.begin(); it != settings.end();) {
                if (it->first == "visible" || it->first == "x" || it->first == "y" || it->first == "anchor" ||
                    it->first == "width" || it->first == "height" || it->first == "language") ++it;
                else it = settings.erase(it);
            }
            saveStat(metric);
            refreshAll();
            m_pendingRebuild = true;
            return;
        }
        auto k = keyById(m_sel);
        if (!k) return;
        KeyDef d;
        d.id = k->id;
        d.map = k->map;
        d.label = k->label;
        d.text = k->text;
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
    void changeTheme(bool light) {
        Mod::get()->setSettingValue<std::string>("editor-theme", light ? "Light" : "Dark");
        (void)Mod::get()->saveData();
        m_reloadLanguage = true;
    }

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
        auto frame = m_light ? ccColor3B{224, 230, 236} : ccColor3B{40, 47, 54};
        auto canvas = m_light ? ccColor3B{222, 229, 235} : ccColor3B{20, 25, 30};
        auto grid = m_light ? ccColor3B{104, 119, 138} : ccColor3B{102, 116, 131};
        auto active = lerpC(uiField(), uiAccent(), m_light ? 0.16f : 0.33f);
        dnRound(m_chrome, winX0, winY0, winX1, winY1, 8.f, frame, 1.f);
        dnStrokeA(m_chrome, winX0 + 0.5f, winY0 + 0.5f, winX1 - 0.5f, winY1 - 0.5f,
                  7.5f, uiLine(), 0.75f, 0.79f);
        dnRect(m_chrome, winX0 + 6.f, winY1 - 31.5f, winX1 - 6.f, winY1 - 31.f, uiLine(), 0.78f);
        dnRound(m_chrome, winX1 - 110.f, winY1 - 25.f, winX1 - 35.f, winY1 - 7.f, 5.f, uiField());
        dnRound(m_chrome, winX1 - 31.f, winY1 - 25.f, winX1 - 13.f, winY1 - 7.f, 5.f, uiField());

        dnRound(m_chrome, cvX0, cvY0, cvX1, cvY1, 5.f, canvas, 1.f);
        for (int i = 1; cvX0 + i * 6.f < cvX1 - 2.f; ++i) {
            float x = cvX0 + i * 6.f;
            dnRect(m_chrome, x, cvY0 + 2.f, x + 0.4f, cvY1 - 2.f, grid, i % 5 == 0 ? 0.28f : 0.12f);
        }
        for (int i = 1; cvY0 + i * 6.f < cvY1 - 2.f; ++i) {
            float y = cvY0 + i * 6.f;
            dnRect(m_chrome, cvX0 + 2.f, y, cvX1 - 2.f, y + 0.4f, grid, i % 5 == 0 ? 0.28f : 0.12f);
        }
        dnRound(m_chrome, cvX0 + 2.f, cvY0 + 2.f, cvX1 - 2.f, cvY0 + 14.f, 3.f, uiPanel(), 0.95f);
        dnRound(m_chrome, inX0, inY0, inX1, inY1, 6.f, uiPanel(), 1.f);
        float tw = (inX1 - inX0 - 12.f) / 4.f;
        for (int i = 0; i < 4; ++i) {
            float x = inX0 + 6.f + i * tw;
            dnRound(m_chrome, x + 1.f, tabY0, x + tw - 1.f, inY1 - 3.f, 5.f,
                    i == m_tab ? active : uiField(), 1.f, 4);
            if (m_tabLabels[i]) m_tabLabels[i]->setColor(i == m_tab ? uiInk() : uiMuted());
        }

        for (auto const& b : m_btns) {
            bool on = btnActive(b);
            dnRound(m_chrome, b.x0, b.y0, b.x1, b.y1, 6.f, on ? active : uiField(), 1.f, 4);
            if (on) dnStrokeA(m_chrome, b.x0 + 0.5f, b.y0 + 0.5f, b.x1 - 0.5f, b.y1 - 0.5f,
                              5.5f, uiAccent(), 0.8f, 0.63f, 4);
        }
    }

    void drawCounterPreview(Look const& look, float x, float y, float w, float h, float zoom, ccColor3B fill) {
        if (!look.counter || (!look.countFill && look.countOutline <= 0.f)) return;
        auto sample = CCLabelBMFont::create("12", kFonts[look.countFont]);
        if (!sample) return;
        float scale = labelScale(sample, look.countSize, look.countArea == 1 ? 400.f : w * 0.84f);
        auto dimensions = sample->getContentSize();
        auto offset = counterPosition(look, w, h, dimensions.width * scale, dimensions.height * scale);
        CCPoint pos{x + offset.x * zoom, y + offset.y * zoom};
        if (look.countOutline > 0.f) {
            for (int i = 0; i < 8; ++i) {
                auto outline = CCLabelBMFont::create("12", kFonts[look.countFont]);
                if (!outline) continue;
                float angle = i * kPi / 4;
                float radius = look.countOutline * zoom;
                outline->setPosition({pos.x + std::cos(angle) * radius, pos.y + std::sin(angle) * radius});
                outline->setScale(scale * zoom);
                outline->setColor(look.countOutlineColor);
                m_counterPreview->addChild(outline);
            }
        }
        if (!look.countFill) return;
        sample->setPosition(pos);
        sample->setScale(scale * zoom);
        ccColor3B autoColor = look.countArea == 0 ? lerpC(textFor(fill), fill, 0.45f) : kWhite;
        sample->setColor(look.countColor < 0 ? autoColor : unpackC(look.countColor));
        m_counterPreview->addChild(sample, 1);
    }

    void drawNotePreview(Look const& look, float x, float y, float width, float zoom) {
        float w = std::max(kCap, width * look.noteWidth);
        float radius = std::clamp(look.noteRadius * w, 1.5f, 16.f);
        float glow = std::clamp(look.glowSize, 0.3f, 3.f);
        auto sample = CCNode::create();
        auto border = CCDrawNode::create();
        sample->addChild(border, 5);
        sample->setScale(zoom);
        sample->setPosition({x, y});
        m_notePreview->addChild(sample);
        auto frame = look.gradNotes ? makeGradientFrame(look.noteGradient) : nullptr;
        float bottom = 0.f;
        for (double hold : {0.075, 0.032, 0.017}) {
            float h = holdNoteLength(0.0, hold, look.noteSpeed, look.holdLengthScale, look.noteHeight - bottom);
            if (h < 0.05f) break;
            float rounding = noteSpriteScale(radius, w, h);
            auto sprites = createNoteSprites(sample);
            if (!sprites.base) break;
            useGradientFrame(sprites, frame);
            sizeNote(sprites, w, h, rounding, glow);
            float fade = std::pow(1.f - bottom / look.noteHeight, std::max(0.f, look.noteFade));
            paintNote(sprites, border, look, (width - w) / 2.f, look.noteBaseGap + bottom, w, h, rounding, glow, fade, frame != nullptr);
            bottom += h + 7.f;
        }
    }

    void redrawCanvas() {
        m_canvas->clear();
        m_counterPreview->removeAllChildrenWithCleanup(true);
        m_notePreview->removeAllChildrenWithCleanup(true);
        if (!m_mapMode && m_dragMode == 0) updateFit();
        if (!m_mapMode || !m_viewer->isVisible()) {
            auto v = this->view();
            auto base = readLook();
            int samples[StatCount] = {1324, 12, 18};
            for (auto const& stat : m_viewer->statGeo()) {
                auto box = KeyLane::create(statName(stat.metric));
                if (!box) continue;
                box->layout(statLook(base, stat.metric), stat.width, stat.height);
                box->setStatValue(samples[stat.metric]);
                box->tick(nowSec(), 0.f, 0.f);
                CCPoint pos{v.ox + stat.position.x * v.sc, v.oy + stat.position.y * v.sc};
                box->setPosition(pos);
                if (m_tab == 2 && m_statTab == stat.metric + 1) {
                    auto outline = CCDrawNode::create();
                    float x = pos.x, y = pos.y, width = stat.width * v.sc, height = stat.height * v.sc;
                    drawSelection(outline, x, y, x + width, y + height, 3.f);
                    m_counterPreview->addChild(outline, 2);
                }
                box->setScale(v.sc);
                m_counterPreview->addChild(box);
            }
        }
        if (m_mapMode && m_viewer->isVisible() && m_tab == 2 && m_statTab > 0) {
            if (auto box = m_viewer->statBox(m_statTab - 1)) {
                auto v = this->view();
                float x = v.ox + box->position.x, y = v.oy + box->position.y;
                drawSelection(m_canvas, x, y, x + box->width, y + box->height, 3.f);
            }
        }
        for (auto l : m_tileLabels) if (l) l->setVisible(false);

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

            if (!m_mapMode && kl.timeline && g.id == m_sel) drawNotePreview(kl, x0, y1, g.w, v.sc);

            bool pressed = m_previewPressed;
            ccColor3B fill = pressed ? kl.pressed : kl.tile;
            float a = pressed ? 1.f : std::max(0.55f, kl.tileOpacity / 255.f);
            dnRound(m_canvas, x0, y0, x1, y1, r, fill, a, 5);
            if (kl.borderW > 0.f) dnStroke(m_canvas, x0, y0, x1, y1, r, kl.border, kl.borderW * v.sc, 5);

            // 선택한 키: 파란 테두리와 손잡이 (모서리 동그라미, 변 알약)
            if (g.id == m_sel && !(m_tab == 2 && m_statTab > 0)) drawSelection(m_canvas, x0, y0, x1, y1, r);

            auto lab = this->tileLabel(li++);
            if (lab) {
                setFont(lab, kl.labelFont, hasHangul(g.name));
                lab->setString(g.name.c_str());
                float sc = labelScale(lab, kl.labelSize * v.sc, w * 0.84f);
                lab->setScale(sc);
                float lw = lab->getContentSize().width * sc;
                if (lw > w * 0.84f && lw > 0.f) lab->setScale(sc * w * 0.84f / lw);
                lab->setColor(kl.labelColor < 0 ? textFor(fill) : unpackC(kl.labelColor));
                lab->setPosition({(x0 + x1) / 2.f, y0 + textCenterY(kl, h)});
                lab->setVisible(true);
            }
            if (!m_mapMode && g.id == m_sel) drawCounterPreview(kl, x0, y0, g.w, g.h, v.sc, fill);
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

    void drawSelection(CCDrawNode* draw, float x0, float y0, float x1, float y1, float radius) {
        dnStroke(draw, x0 - 1.f, y0 - 1.f, x1 + 1.f, y1 + 1.f, radius, uiAccent(), 1.f, 5);
        if (m_mapMode) return;
        float cx = (x0 + x1) / 2.f, cy = (y0 + y1) / 2.f;
        for (auto point : {CCPoint{x0, y0}, CCPoint{x1, y0}, CCPoint{x0, y1}, CCPoint{x1, y1},
                           CCPoint{cx, y0}, CCPoint{cx, y1}, CCPoint{x0, cy}, CCPoint{x1, cy}}) {
            dnRound(draw, point.x - 3.4f, point.y - 3.4f, point.x + 3.4f, point.y + 3.4f,
                    2.5f, kWhite, 1.f, 4);
            dnStrokeA(draw, point.x - 3.4f, point.y - 3.4f, point.x + 3.4f, point.y + 3.4f,
                      2.5f, uiAccent(), 0.8f, 1.f, 4);
        }
    }

    void beginDrag(CCPoint p, float x, float y, float w, float h, int handle, int stat = -1) {
        m_dragMode = handle < 0 ? 1 : 2;
        m_dragStat = stat;
        m_handle = handle;
        m_dragStart = p;
        m_dragRect[0] = x; m_dragRect[1] = y; m_dragRect[2] = w; m_dragRect[3] = h;
    }

    void canvasDown(CCPoint p) {
        m_dragMode = 0;
        if (!m_mapMode) this->updateFit();
        View v = this->view();

        if (m_tab == 2 && m_statTab > 0 && !m_mapMode) {
            if (auto box = m_viewer->statBox(m_statTab - 1)) {
                float x = v.ox + box->position.x * v.sc, y = v.oy + box->position.y * v.sc;
                int handle = hitHandle(p, x, y, x + box->width * v.sc, y + box->height * v.sc);
                if (handle >= 0) {
                    beginDrag(p, box->position.x, box->position.y, box->width, box->height, handle, box->metric);
                    return;
                }
            }
        }
        auto const& stats = m_viewer->statGeo();
        for (auto it = stats.rbegin(); it != stats.rend(); ++it) {
            CCRect hit(v.ox + it->position.x * v.sc, v.oy + it->position.y * v.sc,
                       it->width * v.sc, it->height * v.sc);
            if (!hit.containsPoint(p)) continue;
            beginDrag(p, it->position.x, it->position.y, it->width, it->height, -1, it->metric);
            selectStatEditor(it->metric + 1);
            return;
        }

        // 선택한 키의 손잡이를 먼저 확인 (지도 모드에서는 이동만 가능)
        if (auto g = this->geoById(m_sel); g && !m_mapMode && !(m_tab == 2 && m_statTab > 0)) {
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
            bool changed = m_sel != g.id || (m_tab == 2 && m_statTab > 0);
            m_statTab = 0;
            m_dragStat = -1;
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
        auto key = m_dragStat < 0 ? keyById(m_sel) : nullptr;
        if (m_dragStat < 0 && !key) return;
        View v = view();
        if (v.sc <= 0.f) return;
        float dx = (p.x - m_dragStart.x) / v.sc, dy = (p.y - m_dragStart.y) / v.sc;
        float x = m_dragRect[0], y = m_dragRect[1], w = m_dragRect[2], h = m_dragRect[3];
        if (m_dragMode == 1) {
            x += dx;
            y += dy;
        } else {
            int edge = m_handle;
            bool left = edge == 0 || edge == 2 || edge == 6;
            bool right = edge == 1 || edge == 3 || edge == 7;
            bool bottom = edge == 0 || edge == 1 || edge == 4;
            bool top = edge == 2 || edge == 3 || edge == 5;
            float oldW = w, oldH = h;
            if (left || right) w = std::clamp(w + (left ? -dx : dx), m_dragStat < 0 ? 12.f : 24.f, m_dragStat < 0 ? 500.f : 200.f);
            if (bottom || top) h = std::clamp(h + (bottom ? -dy : dy), 12.f, m_dragStat < 0 ? 500.f : 100.f);
            if (left) x += oldW - w;
            if (bottom) y += oldH - h;
        }
        x = std::clamp(std::round(x), -500.f, 500.f);
        y = std::clamp(std::round(y), -300.f, 300.f);
        if (key) {
            key->x = x;
            key->y = y;
            if (m_dragMode == 2) {
                key->w = std::round(w);
                key->h = std::round(h);
            }
        } else {
            auto& box = g_statSettings[m_dragStat];
            box["anchor"] = 5;
            box["x"] = x;
            box["y"] = y;
            if (m_dragMode == 2) {
                box["width"] = std::round(w);
                box["height"] = std::round(h);
            }
        }
        m_viewer->applyLook();
        redrawCanvas();
    }

    void canvasUp(CCPoint) {
        if (m_dragMode == 0) return;
        m_dragMode = 0;
        if (m_dragStat >= 0) saveStat(m_dragStat);
        else if (auto key = keyById(m_sel)) saveKey(*key);
        m_dragStat = -1;
        m_pendingRebuild = true;
        redrawCanvas();
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
        if (auto label = rowLabel(ri, text, 0.f, m_rows[ri].top - 9.f, 0.28f, uiMuted(), 0.f))
            label->setScale(std::min(0.28f, m_inW / std::max(1.f, label->getContentSize().width)));
    }

    void rowButton(std::string const& label, ccColor3B color, std::function<void()> action) {
        int ri = beginRow(28.f);
        float top = m_rows[ri].top;
        float w = m_inW;
        auto d = rowDraw(ri);
        dnRound(d, 0.f, top - 24.f, w, top - 4.f, 6.f, color, 1.f, 4);
        rowLabel(ri, label, w / 2.f, top - 14.f, 0.30f, textFor(color), 0.5f);
        Ctl c;
        c.scroll = true;
        c.row = ri;
        c.r = CCRect(0.f, top - 24.f, w, 20.f);
        c.down = [action](CCPoint) { action(); };
        m_scrollCtls.push_back(c);
    }

    void rowNumber(std::string const& label, float min, float max, float step,
                   std::function<float()> get, std::function<void(float)> set) {
        int row = beginRow(27.f);
        float top = m_rows[row].top;
        float field = std::min(63.f, m_inW * 0.41f);
        float x = m_inW - field;
        auto title = rowLabel(row, label, 0.f, top - 13.5f, 0.28f, uiInk(), 0.f);
        if (title) title->setScale(std::min(0.28f, (x - 4.f) / std::max(1.f, title->getContentSize().width)));
        auto draw = rowDraw(row);
        dnRound(draw, x, top - 23.5f, m_inW, top - 3.5f, 5.f, uiField());
        dnStrokeA(draw, x + 0.5f, top - 23.f, m_inW - 0.5f, top - 4.f, 4.f, uiAccent(), 0.75f, 0.56f, 4);
        rowLabel(row, fmtNum(get()), x + field / 2.f, top - 13.5f, 0.29f, uiInk(), 0.5f);
        Ctl ctl;
        ctl.row = row;
        ctl.r = CCRect(x, top - 25.f, field, 23.f);
        ctl.down = [this, label, get, set, min, max, step](CCPoint) {
            showInput(label, fmtNum(get()), true, [set, min, max, step](std::string const& text) {
                char* end = nullptr;
                float value = std::strtof(text.c_str(), &end);
                if (end == text.c_str() || *end || !std::isfinite(value) || value < min || value > max) return false;
                if (step >= 1.f) value = std::round(value);
                set(value);
                return true;
            });
        };
        m_scrollCtls.push_back(ctl);
    }

    // 왼쪽 오른쪽 화살표로 선택지를 돌려 고르는 줄
    void rowCycle(std::string const& label, std::vector<std::string> opts,
                  std::function<int()> get, std::function<void(int)> set) {
        int ri = beginRow(24.f);
        float top = m_rows[ri].top;
        float w = m_inW;
        auto title = rowLabel(ri, label, 0.f, top - 12.f, 0.30f, uiInk(), 0.f);
        auto d = rowDraw(ri);
        float pickerWidth = std::min(118.f, w * 0.65f);
        float zx0 = w - pickerWidth;
        float mid = zx0 + pickerWidth / 2.f;
        if (title) title->setScale(std::min(0.30f, std::max(4.f, zx0 - 4.f) / std::max(1.f, title->getContentSize().width)));
        auto val = rowLabel(ri, "", mid, top - 12.f, 0.30f, uiInk(), 0.5f);
        int n = static_cast<int>(opts.size());

        auto refresh = [=, this]() {
            d->clear();
            dnRound(d, zx0, top - 20.f, zx0 + 18.f, top - 4.f, 4.f, uiField(), 1.f, 3);
            dnRound(d, w - 18.f, top - 20.f, w, top - 4.f, 4.f, uiField(), 1.f, 3);
            int cur = std::clamp(get(), 0, n - 1);
            if (val) {
                val->setString(uiText(opts[cur]).c_str());
                float lw = val->getContentSize().width * 0.30f;
                float maxW = std::max(5.f, pickerWidth - 40.f);
                val->setScale(lw > maxW && lw > 0.f ? 0.30f * maxW / lw : 0.30f);
            }
        };
        rowLabel(ri, "<", zx0 + 9.f, top - 12.f, 0.34f, uiInk(), 0.5f);
        rowLabel(ri, ">", w - 9.f, top - 12.f, 0.34f, uiInk(), 0.5f);
        refresh();

        Ctl c;
        c.scroll = true;
        c.row = ri;
        c.r = CCRect(zx0, top - 24.f, pickerWidth, 24.f);
        c.down = [=](CCPoint p) {
            int cur = std::clamp(get(), 0, n - 1);
            int next = (p.x < mid) ? (cur + n - 1) % n : (cur + 1) % n;
            set(next);
            refresh();
        };
        m_scrollCtls.push_back(c);
    }

    void openColors(GradientStops current, bool gradient,
                    std::function<void(GradientStops const&)> apply) {
        auto popup = VisualColorPopup::create(current, gradient, m_light, std::move(apply), [this] {
            m_modal = false;
            m_pendingRebuild = true;
        });
        if (!popup) return;
        m_modal = true;
        popup->show();
    }

    void rowGradient(int id) {
        auto key = keyById(id);
        if (!key) return;
        auto stops = lookForKey(readLook(), *key).noteGradient;
        int row = beginRow(33.f);
        float top = m_rows[row].top;
        auto draw = rowDraw(row);
        rowLabel(row, "Edit gradient", 0.f, top - 7.f, 0.28f, uiInk(), 0.f);
        drawRamp(draw, stops, 1.f, top - 28.f, m_inW - 2.f, 15.f);
        dnStrokeA(draw, 0.5f, top - 28.5f, m_inW - 0.5f, top - 12.5f, 2.f, uiLine(), 0.7f, 0.9f);
        Ctl control;
        control.row = row;
        control.r = CCRect(0.f, top - 30.f, m_inW, 30.f);
        control.down = [this, id](CCPoint) {
            auto key = keyById(id);
            if (!key) return;
            openColors(lookForKey(readLook(), *key).noteGradient, true, [this, id](GradientStops const& changed) {
                auto current = keyById(id);
                if (!current) return;
                current->style["note-gradient-enabled"] = 1;
                current->style["note-gradient-bottom"] = changed[0].color;
                current->style["note-gradient-top"] = changed[1].color;
                current->style["gradient-bottom-alpha"] = changed[0].alpha;
                current->style["gradient-top-alpha"] = changed[1].alpha;
                commitKey(id);
            });
        };
        m_scrollCtls.push_back(control);
    }

    void rowColor(std::string const& label, std::function<int()> get, std::function<void(int)> set, bool allowAuto) {
        int row = beginRow(40.f);
        float top = m_rows[row].top;
        float width = m_inW;
        rowLabel(row, label, 0.f, top - 8.f, 0.30f, uiInk(), 0.f);
        int color = get();
        float swatchRight = allowAuto ? width * 0.64f : width;
        auto draw = rowDraw(row);
        ccColor3B swatch = color < 0 ? uiField() : unpackC(color);
        dnRound(draw, 0.f, top - 35.f, swatchRight, top - 17.f, 4.f, swatch, 1.f, 3);
        rowLabel(row, "Choose color", swatchRight / 2.f, top - 26.f, 0.28f, textFor(swatch), 0.5f);

        Ctl choose;
        choose.row = row;
        choose.r = CCRect(0.f, top - 36.f, swatchRight, 20.f);
        choose.down = [this, get, set](CCPoint) {
            int color = get();
            GradientStops initial = {{{color < 0 ? 0xffffff : color, 255}, {0xffffff, 255}}};
            openColors(initial, false, [set](GradientStops const& edited) { set(edited[0].color); });
        };
        m_scrollCtls.push_back(choose);
        if (!allowAuto) return;

        dnRound(draw, swatchRight + 3.f, top - 35.f, width, top - 17.f, 4.f,
                color < 0 ? uiAccent() : uiField(), 1.f, 3);
        rowLabel(row, "AUTO", (swatchRight + 3.f + width) / 2.f, top - 26.f, 0.28f, uiInk(), 0.5f);
        Ctl reset;
        reset.row = row;
        reset.r = CCRect(swatchRight + 3.f, top - 36.f, width - swatchRight - 3.f, 20.f);
        reset.down = [this, set](CCPoint) {
            set(-1);
            m_pendingRebuild = true;
        };
        m_scrollCtls.push_back(reset);
    }

    void rowToggle(std::string const& label, std::function<bool()> get, std::function<void(bool)> set) {
        int ri = beginRow(22.f);
        float top = m_rows[ri].top;
        float w = m_inW;
        rowLabel(ri, label, 0.f, top - 11.f, 0.30f, uiInk(), 0.f);
        auto d = rowDraw(ri);

        auto refresh = [=, this]() {
            bool on = get();
            d->clear();
            dnRound(d, w - 30.f, top - 17.f, w - 2.f, top - 5.f, 6.f,
                    on ? lerpC(uiField(), uiAccent(), 0.28f) : uiLine(), 1.f, 4);
            float kx = on ? w - 8.f : w - 24.f;
            dnRound(d, kx - 5.f, top - 16.f, kx + 5.f, top - 6.f, 5.f, uiField(), 1.f, 4);
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
        rowLabel(ri, label, 0.f, top - 8.f, 0.30f, uiInk(), 0.f);
        auto d = rowDraw(ri);
        int n = static_cast<int>(opts.size());
        float bw = (w - (n - 1) * 3.f) / n;
        float y0 = top - 32.f, y1 = top - 16.f;

        for (int i = 0; i < n; i++) {
            float cxm = i * (bw + 3.f) + bw / 2.f;
            rowLabel(ri, opts[i], cxm, (y0 + y1) / 2.f, 0.28f, uiInk(), 0.5f);
        }

        auto refresh = [=, this]() {
            d->clear();
            int cur = get();
            for (int i = 0; i < n; i++) {
                float x0 = i * (bw + 3.f);
                dnRound(d, x0, y0, x0 + bw, y1, 4.f,
                        i == cur ? lerpC(uiField(), uiAccent(), 0.28f) : uiField(), 1.f, 3);
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
        rowNumber(label, vmin, vmax, step,
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
            if (numeric) popup->focusInput();
        }
    }

    void geometryRow(std::string const& title, std::array<char const*, 2> const& names, int id,
                     std::array<float KeyDef::*, 2> const& fields,
                     std::array<float, 2> const& low, std::array<float, 2> const& high) {
        int row = beginRow(44.f);
        float top = m_rows[row].top;
        rowLabel(row, title, 0.f, top - 8.f, 0.30f, uiInk(), 0.f);
        auto bg = rowDraw(row);
        float width = (m_inW - 4.f) / 2.f;
        for (size_t i = 0; i < fields.size(); ++i) {
            auto key = keyById(id);
            if (!key) return;
            float x = i * (width + 4.f);
            dnRound(bg, x, top - 39.f, x + width, top - 17.f, 4.f, {58, 58, 72}, 1.f, 3);
            std::string label = std::string(names[i]) + "  " + fmtNum(key->*fields[i]);
            rowLabel(row, label, x + width / 2.f, top - 28.f, 0.32f, uiInk(), 0.5f);
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
        rowNumber(label, low, high, step,
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
            rowLabel(ri, uiText("KEY  ") + describeKey(id), 0.f, m_rows[ri].top - 9.f, 0.32f, uiInk(), 0.f);
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
        auto key = keyById(id);
        if (!key) return;
        rowButton("Edit key name", {70, 110, 240}, [this, id] {
            if (auto key = keyById(id)) showInput("Edit key name", labelFor(*key, readLook()), false,
                [this, id](std::string const& name) {
                    if (!validName(name)) return false;
                    if (auto key = keyById(id)) {
                        key->text = name;
                        key->label = 0;
                        commitKey(id);
                    }
                    return true;
                });
        });
        int code = lookForKey(readLook(), *key).keyCode;
        rowInfo(code ? uiText("Bound key ") + keyName(code) : uiText("Game action input"));
        rowButton("Bind keyboard key", {70, 110, 240}, [this, id] {
            showInput("Bind keyboard key", "", false, {}, [this, id](int code) {
                m_viewer->resetInput();
                if (writeStyle(id, "key-code", code)) {
                    (void)Mod::get()->saveData();
                    setStatus(uiText("Bound key ") + keyName(code));
                }
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
        fontChoice("label-font");
        localSlider("Font size", "label-size", 4.f, 40.f, 1.f, 10.f);
        localColor("Font color", "label-color", -1, true);
        float gr = readLook().radius * 100.f;
        rowNumber("Rounding %", 0.f, 50.f, 1.f,
            [id, gr]() { auto k = keyById(id); return (k && k->radius >= 0.f) ? k->radius : gr; },
            [this, id](float v) { if (auto k = keyById(id)) k->radius = v; this->commitKey(id); });
        rowNumber("Border width", 0.f, 6.f, 0.5f,
            [id]() { auto k = keyById(id); return k ? k->borderW : 0.f; },
            [this, id](float v) { if (auto k = keyById(id)) k->borderW = v; this->commitKey(id); });
        settingSlider("Idle opacity", "tile-opacity", 0.f, 255.f, 1.f, true);
        settingToggle("Press glow", "show-glow");
        settingToggle("Key counters", "show-counter");
        localToggle("Box gradient", "tile-idle-gradient", false);
        settingColor("Gradient end color", "gradient-color");
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
            rowLabel(ri, uiText("NOTES  ") + describeKey(id), 0.f, m_rows[ri].top - 9.f, 0.32f, uiInk(), 0.f);
        }
        rowColor("Note color (this key)",
            [id]() { auto k = keyById(id); return k ? k->accent : -1; },
            [this, id](int v) { if (auto k = keyById(id)) k->accent = v; this->commitKey(id); }, true);
        this->settingToggle("Note timeline", "show-timeline");
        localToggle("Auto Y correction", "note-auto-y", false);
        this->settingToggle("Note glow", "note-glow");
        this->settingSlider("Note width %", "note-width", 20.f, 100.f, 1.f);
        localSlider("Hold length scale %", "hold-length-scale", 25.f, 200.f, 5.f, 100.f);
        localSlider("Gap above key", "note-base-gap", 0.f, 30.f, 1.f, 5.f);
        this->settingSlider("Speed", "note-speed", 100.f, 600.f, 5.f);
        this->settingSlider("Timeline length", "note-height", 40.f, 220.f, 5.f);
        rowToggle("Note gradient",
            [id] { auto key = keyById(id); return key && lookForKey(readLook(), *key).gradNotes; },
            [this, id](bool enabled) { writeStyle(id, "note-gradient-enabled", enabled); });
        rowGradient(id);
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

    void updateStat(int metric, std::string const& key, double value) {
        if (value < 0 && key != "x" && key != "y") g_statSettings[metric].erase(key);
        else g_statSettings[metric][key] = value;
        saveStat(metric);
        refreshAll();
    }

    void statSlider(int metric, std::string label, std::string key, float min, float max, float step, float base) {
        rowNumber(label, min, max, step, [metric, key, base] { return static_cast<float>(statValue(metric, key, base)); },
            [this, metric, key](float value) { updateStat(metric, key, value); });
    }

    void statChoice(int metric, std::string label, std::string key, std::vector<std::string> choices, int base) {
        rowCycle(label, std::move(choices), [metric, key, base] { return static_cast<int>(statValue(metric, key, base)); },
            [this, metric, key](int value) { updateStat(metric, key, value); });
    }

    void statToggle(int metric, std::string label, std::string key, bool base) {
        rowToggle(label, [metric, key, base] { return statValue(metric, key, base) != 0; },
            [this, metric, key](bool value) { updateStat(metric, key, value); });
    }

    void statColor(int metric, std::string label, std::string key, int base, bool allowAuto = true) {
        rowColor(label, [metric, key, base] { return static_cast<int>(statValue(metric, key, base)); },
            [this, metric, key](int value) { updateStat(metric, key, value); }, allowAuto);
    }

    void buildStatOptions(int metric) {
        auto base = readLook();
        statToggle(metric, "Show box", "visible", metric == TotalMetric ? base.total : metric == KpsMetric && base.kps);
        statChoice(metric, "Box language", "language", {"Same as editor", "Korean", "English"}, 0);
        rowButton("Edit box title", {58, 58, 72}, [this, metric] {
            showInput("Edit box title", statName(metric), false, [this, metric](std::string const& text) {
                if (!validName(text)) return false;
                g_statNames[metric] = text;
                saveStat(metric);
                refreshAll();
                return true;
            });
        });
        statChoice(metric, "Box anchor", "anchor", {"AUTO", "Right", "Left", "Below", "Above", "Free"}, 0);
        statSlider(metric, "Position X", "x", -500.f, 500.f, 1.f, 0.f);
        statSlider(metric, "Position Y", "y", -300.f, 300.f, 1.f, 0.f);
        statSlider(metric, "Width", "width", 24.f, 200.f, 1.f, 72.f);
        statSlider(metric, "Height", "height", 12.f, 100.f, 1.f, 19.f);
        statToggle(metric, "Side-by-side text", "stat-inline", true);
        statColor(metric, "Background color", "background", -1);
        statSlider(metric, "Idle opacity", "tile-opacity", 0.f, 255.f, 1.f, base.tileOpacity);
        statSlider(metric, "Rounding %", "roundness", 0.f, 50.f, 1.f, 7.f);
        statColor(metric, "Border color", "rim", -1);
        statSlider(metric, "Border width", "rim-width", 0.f, 4.f, 0.25f, 0.f);
        statToggle(metric, "Box gradient", "idle-gradient", false);
        statColor(metric, "Gradient end color", "gradient-color", packC(base.gradEnd));
        statChoice(metric, "Title font", "label-font", {"Inter", "Noto Sans", "GD Big", "GD Gold"}, 0);
        statSlider(metric, "Title size", "label-size", 4.f, 40.f, 1.f, 10.f);
        statColor(metric, "Title color", "label-color", -1);
        statChoice(metric, "Value font", "count-font", {"Inter", "Noto Sans", "GD Big", "GD Gold"}, 0);
        statSlider(metric, "Value size", "count-size", 4.f, 40.f, 1.f, 7.f);
        statColor(metric, "Value color", "count-color", -1);
    }

    void selectStatEditor(int index) {
        m_tab = 2;
        m_statTab = index;
        m_scroll = 0.f;
        rebuildInspector();
        redrawChrome();
        redrawCanvas();
    }

    void counterPicker() {
        static const std::array<char const*, 4> labels = {"Key counters", "Total", "KPS", "Peak KPS"};
        int row = beginRow(55.f);
        float top = m_rows[row].top;
        float width = (m_inW - 4.f) / 2.f;
        auto draw = rowDraw(row);
        for (int i = 0; i < 4; ++i) {
            float x = (i % 2) * (width + 4.f);
            float y = top - 24.f * (1 + i / 2);
            dnRound(draw, x, y, x + width, y + 20.f, 4.f,
                    m_statTab == i ? uiAccent() : uiField(), 1.f, 3);
            auto label = rowLabel(row, labels[i], x + width / 2.f, y + 10.f, 0.28f, uiInk(), 0.5f);
            if (label) label->setScale(std::min(0.28f, (width - 6.f) / std::max(1.f, label->getContentSize().width)));
            Ctl control;
            control.row = row;
            control.r = CCRect(x, y, width, 20.f);
            control.down = [this, i](CCPoint) { selectStatEditor(i); };
            m_scrollCtls.push_back(control);
        }
    }

    void buildCounterTab() {
        counterPicker();
        if (m_statTab > 0) {
            rowInfo(statName(m_statTab - 1));
            buildStatOptions(m_statTab - 1);
            return;
        }
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

    void alignKeys(bool horizontal, bool distribute) {
        auto const& geometry = m_viewer->geo();
        if (geometry.size() < 2) return;
        auto ordered = geometry;
        std::stable_sort(ordered.begin(), ordered.end(), [horizontal](auto const& a, auto const& b) {
            return horizontal ? a.x + a.w / 2.f < b.x + b.w / 2.f
                              : a.y + a.h / 2.f > b.y + b.h / 2.f;
        });
        auto const* selected = geoById(m_sel);
        auto const& reference = selected ? *selected : ordered.front();
        float center = horizontal ? reference.y + reference.h / 2.f : reference.x + reference.w / 2.f;
        float cursor = horizontal ? ordered.front().x : ordered.front().y + ordered.front().h;
        float gap = readLook().gap;
        for (auto const& item : ordered) {
            auto key = keyById(item.id);
            if (!key) continue;
            if (horizontal) {
                key->y = center - item.h / 2.f;
                if (distribute) {
                    key->x = cursor;
                    cursor += item.w + gap;
                }
            } else {
                key->x = center - item.w / 2.f;
                if (distribute) {
                    key->y = cursor - item.h;
                    cursor -= item.h + gap;
                }
            }
            saveKey(*key);
        }
        refreshAll();
        setStatus("Aligned visible keys.");
    }

    void buildViewTab() {
        rowChoice("Language", {"English", "Korean"},
            []() { return koreanUI() ? 1 : 0; },
            [this](int choice) {
                Mod::get()->setSettingValue<std::string>("language", choice ? "Korean" : "English");
                m_reloadLanguage = true;
            });
        rowInfo("Align to the selected key.");
        rowButton("Align horizontal", {58, 58, 72}, [this] { alignKeys(true, false); });
        rowButton("Align vertical", {58, 58, 72}, [this] { alignKeys(false, false); });
        rowButton("Space evenly in row", {58, 58, 72}, [this] { alignKeys(true, true); });
        rowButton("Space evenly in column", {58, 58, 72}, [this] { alignKeys(false, true); });
        this->settingSlider("Box size", "box-size", 24.f, 80.f, 1.f);
        this->settingSlider("Jump width x", "jump-width", 0.5f, 3.f, 0.1f);
        this->settingSlider("Corner round %", "corner-radius", 0.f, 50.f, 1.f);
        this->settingSlider("Key spacing", "box-gap", 0.f, 24.f, 1.f);
        this->settingSlider("Idle opacity", "tile-opacity", 0.f, 255.f, 5.f, true);
        this->settingColor("Idle box color", "tile-color");
        this->settingColor("Pressed box color", "pressed-color");
        this->settingToggle("Press glow", "show-glow");
        settingToggle("Reset on restart", "reset-on-restart");
        this->settingSlider("Position X %", "pos-x", 0.f, 100.f, 1.f);
        this->settingSlider("Position Y %", "pos-y", 0.f, 100.f, 1.f);
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
                next->m_statTab = m_statTab;
                next->m_scroll = m_scroll;
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
            (void)Mod::get()->saveData();
            (void)Mod::get()->saveData();
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
        if (EditorInputPopup::captureKeyboardEvent(key, down, repeat)) return true;
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
