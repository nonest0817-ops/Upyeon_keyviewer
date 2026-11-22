#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

using namespace geode::prelude;

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

static CCLabelBMFont* mkLabel(CCNode* parent, std::string const& s, float scale,
                              ccColor3B c, CCPoint pos, float ax) {
    auto l = CCLabelBMFont::create(s.c_str(), "InterBold.fnt"_spr);
    if (!l) return nullptr;
    l->setScale(scale);
    l->setColor(c);
    l->setPosition(pos);
    l->setAnchorPoint({ax, 0.5f});
    parent->addChild(l);
    return l;
}

// ---------------------------------------------------------------
// 설정값 묶음
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
    bool viewerVisible = true;
    bool timeline = true;
    bool counter = true;
    bool kps = true;
    bool total = true;
    bool glow = true;
    bool noteGlow = true;
    bool showP2 = false;
    bool resetOnRestart = false;
    float posX = 85.f;
    float posY = 10.f;
};

static ccColor3B accentFor(Look const& l, int button) {
    if (button == 2) return l.accentLeft;
    if (button == 3) return l.accentRight;
    return l.accentJump;
}

static char const* const kPresetNames[6] = {"Custom", "Dark", "Neon", "Rhythm", "Light", "Minimal"};

// 내장 프리셋: Custom이 아니면 색과 효과를 덮어쓴다 (위치와 크기는 유지)
static void applyPreset(std::string const& p, Look& l) {
    auto accent = [&](ccColor3B c) { l.accentJump = c; l.accentLeft = c; l.accentRight = c; };
    if (p == "Dark") {
        l.tile = {18, 18, 22}; l.pressed = {226, 226, 232}; l.tileOpacity = 200;
        accent({255, 255, 255}); l.glow = true; l.noteGlow = true; l.timeline = true;
    } else if (p == "Neon") {
        l.tile = {14, 20, 40}; l.pressed = {190, 250, 255}; l.tileOpacity = 210;
        accent({0, 255, 220}); l.glow = true; l.noteGlow = true; l.timeline = true;
    } else if (p == "Rhythm") {
        l.tile = {26, 16, 26}; l.pressed = {255, 232, 240}; l.tileOpacity = 210;
        accent({255, 90, 150}); l.glow = true; l.noteGlow = true; l.timeline = true;
        l.noteSpeed = 340.f; l.noteHeight = 130.f;
    } else if (p == "Light") {
        l.tile = {240, 240, 246}; l.pressed = {40, 40, 50}; l.tileOpacity = 170;
        accent({80, 160, 255}); l.glow = true; l.noteGlow = true; l.timeline = true;
    } else if (p == "Minimal") {
        l.tile = {255, 255, 255}; l.pressed = {255, 255, 255}; l.tileOpacity = 40;
        accent({255, 255, 255}); l.glow = false; l.noteGlow = false; l.timeline = false;
        l.counter = false; l.kps = false; l.total = false;
    }
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
    l.viewerVisible = m->getSettingValue<bool>("show-viewer");
    l.timeline = m->getSettingValue<bool>("show-timeline");
    l.counter = m->getSettingValue<bool>("show-counter");
    l.kps = m->getSettingValue<bool>("show-kps");
    l.total = m->getSettingValue<bool>("show-total");
    l.glow = m->getSettingValue<bool>("show-glow");
    l.noteGlow = m->getSettingValue<bool>("note-glow");
    l.showP2 = m->getSettingValue<bool>("show-p2");
    l.resetOnRestart = m->getSettingValue<bool>("reset-on-restart");
    l.posX = static_cast<float>(m->getSettingValue<double>("pos-x"));
    l.posY = static_cast<float>(m->getSettingValue<double>("pos-y"));
    applyPreset(m->getSettingValue<std::string>("preset"), l);
    return l;
}

// ---------------------------------------------------------------
// 키별 스타일 (에디터에서 바꾸고 저장된다). 슬롯: 0~2 = 1P(점프, 왼쪽, 오른쪽), 3~5 = 2P
// ---------------------------------------------------------------
struct KeyStyle {
    float dx = 0.f, dy = 0.f;   // 자동 배치 위치에서의 이동량
    float w = 0.f, h = 0.f;     // 0 = 자동
    int bg = -1, pressed = -1, border = -1, accent = -1; // 0xRRGGBB, -1 = 전체 설정 사용
    float borderW = 0.f;
    float radius = -1.f;        // %, -1 = 전체 설정 사용
};

static KeyStyle g_style[6];
static bool g_styleLoaded = false;

static int slotOf(int button, int player) {
    return std::clamp((player - 1) * 3 + (button - 1), 0, 5);
}

static std::string sk(int slot, char const* field) {
    return "k" + std::to_string(slot) + "_" + field;
}

static void loadStyles() {
    if (g_styleLoaded) return;
    g_styleLoaded = true;
    auto m = Mod::get();
    for (int i = 0; i < 6; i++) {
        auto& s = g_style[i];
        s.dx = static_cast<float>(m->getSavedValue<double>(sk(i, "dx"), 0.0));
        s.dy = static_cast<float>(m->getSavedValue<double>(sk(i, "dy"), 0.0));
        s.w = static_cast<float>(m->getSavedValue<double>(sk(i, "w"), 0.0));
        s.h = static_cast<float>(m->getSavedValue<double>(sk(i, "h"), 0.0));
        s.borderW = static_cast<float>(m->getSavedValue<double>(sk(i, "bw"), 0.0));
        s.radius = static_cast<float>(m->getSavedValue<double>(sk(i, "r"), -1.0));
        s.bg = static_cast<int>(m->getSavedValue<int64_t>(sk(i, "bg"), -1));
        s.pressed = static_cast<int>(m->getSavedValue<int64_t>(sk(i, "pr"), -1));
        s.border = static_cast<int>(m->getSavedValue<int64_t>(sk(i, "bc"), -1));
        s.accent = static_cast<int>(m->getSavedValue<int64_t>(sk(i, "ac"), -1));
    }
}

static void saveStyle(int i) {
    auto m = Mod::get();
    auto const& s = g_style[i];
    m->setSavedValue<double>(sk(i, "dx"), s.dx);
    m->setSavedValue<double>(sk(i, "dy"), s.dy);
    m->setSavedValue<double>(sk(i, "w"), s.w);
    m->setSavedValue<double>(sk(i, "h"), s.h);
    m->setSavedValue<double>(sk(i, "bw"), s.borderW);
    m->setSavedValue<double>(sk(i, "r"), s.radius);
    m->setSavedValue<int64_t>(sk(i, "bg"), s.bg);
    m->setSavedValue<int64_t>(sk(i, "pr"), s.pressed);
    m->setSavedValue<int64_t>(sk(i, "bc"), s.border);
    m->setSavedValue<int64_t>(sk(i, "ac"), s.accent);
}

// 전체 설정에 키별 설정을 덮어씌운다
static Look lookForSlot(Look const& base, int slot) {
    Look kl = base;
    auto const& st = g_style[std::clamp(slot, 0, 5)];
    if (st.bg >= 0) kl.tile = unpackC(st.bg);
    if (st.pressed >= 0) kl.pressed = unpackC(st.pressed);
    if (st.border >= 0) kl.border = unpackC(st.border);
    if (st.accent >= 0) {
        auto c = unpackC(st.accent);
        kl.accentJump = c; kl.accentLeft = c; kl.accentRight = c;
    }
    kl.borderW = st.borderW;
    if (st.radius >= 0.f) kl.radius = st.radius / 100.f;
    return kl;
}

// ---------------------------------------------------------------
// 노트 하나가 쓰는 이미지 3장 (글로우, 본체, 색 그라데이션)
// ---------------------------------------------------------------
struct NoteSprites {
    CCScale9Sprite* glow = nullptr;
    CCScale9Sprite* base = nullptr;
    CCScale9Sprite* grad = nullptr;
};

struct Note {
    double start = 0.0;
    double end = -1.0; // 아직 누르는 중이면 -1
    NoteSprites sp;
    float lw = -1.f, lh = -1.f, ls = -1.f; // 마지막으로 적용한 크기 (불필요한 재계산 방지)
};

// ---------------------------------------------------------------
// 키 하나(타일 + 글로우 + 카운터 + 노트)
// ---------------------------------------------------------------
class KeyLane : public CCNode {
protected:
    int m_button = 0;
    int m_player = 1;
    bool m_down = false;
    bool m_dirty = true;
    int m_presses = 0;
    float m_w = 0.f, m_h = 0.f;
    float m_press = 0.f; // 0 = 평소, 1 = 눌림 (부드럽게 변함)
    float m_lw = -1.f, m_lh = -1.f, m_ls = -1.f;
    Look m_look;
    std::string m_nameStr;
    CCScale9Sprite* m_glow = nullptr;
    CCScale9Sprite* m_tile = nullptr;
    CCScale9Sprite* m_tileGrad = nullptr;
    CCDrawNode* m_border = nullptr;
    CCNode* m_noteLayer = nullptr;
    CCLabelBMFont* m_name = nullptr;
    CCLabelBMFont* m_count = nullptr;
    std::deque<Note> m_notes;
    std::vector<NoteSprites> m_pool;

    bool init(int button, int player, std::string const& name) {
        if (!CCNode::init()) return false;
        m_button = button;
        m_player = player;
        m_nameStr = name;

        m_glow = CCScale9Sprite::create("kv_glow.png"_spr);
        m_tile = CCScale9Sprite::create("kv_solid.png"_spr);
        m_tileGrad = CCScale9Sprite::create("kv_grad.png"_spr);
        if (!m_glow || !m_tile || !m_tileGrad) return false;

        m_noteLayer = CCNode::create();
        this->addChild(m_noteLayer, -1);
        this->addChild(m_glow, -2);
        this->addChild(m_tile, 0);
        this->addChild(m_tileGrad, 1);

        m_border = CCDrawNode::create();
        this->addChild(m_border, 2);

        m_name = CCLabelBMFont::create(name.c_str(), "InterBold.fnt"_spr);
        this->addChild(m_name, 3);

        m_count = CCLabelBMFont::create("0", "InterBold.fnt"_spr);
        this->addChild(m_count, 3);
        return true;
    }

    ccColor3B accent() const {
        return accentFor(m_look, m_button);
    }

    NoteSprites acquire() {
        if (!m_pool.empty()) {
            auto s = m_pool.back();
            m_pool.pop_back();
            return s;
        }
        NoteSprites s;
        s.glow = CCScale9Sprite::create("kv_glow.png"_spr);
        s.base = CCScale9Sprite::create("kv_fade.png"_spr);
        s.grad = CCScale9Sprite::create("kv_grad.png"_spr);
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

    void fitCount() {
        float f = m_h / 46.f;
        float countScale = 0.34f * f;
        m_count->setScale(countScale);
        float cw = m_count->getContentSize().width * countScale;
        float maxW = m_w * 0.84f;
        if (cw > maxW && cw > 0.f) m_count->setScale(countScale * maxW / cw);
    }

    void drawBorder() {
        m_border->clear();
        if (m_look.borderW <= 0.f) return;
        float r = std::clamp(m_look.radius * m_h, 2.f, std::min(m_w, m_h) / 2.f);
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
        m_name->setColor(text);
        m_count->setColor(lerpC(text, bg, 0.45f));
    }

public:
    static KeyLane* create(int button, int player, std::string const& name) {
        auto ret = new KeyLane();
        if (ret->init(button, player, name)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    int button() const { return m_button; }
    int player() const { return m_player; }

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

        float r = std::clamp(l.radius * m_h, 2.f, m_h / 2.f);
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

        float f = m_h / 46.f;
        float maxW = m_w * 0.84f;

        float nameScale = 0.50f * f;
        m_name->setScale(nameScale);
        float nw = m_name->getContentSize().width * nameScale;
        if (nw > maxW && nw > 0.f) m_name->setScale(nameScale * maxW / nw);
        m_name->setPosition({m_w / 2, l.counter ? m_h * 0.62f : m_h / 2});

        m_count->setVisible(l.counter);
        this->fitCount();
        m_count->setPosition({m_w / 2, m_h * 0.27f});

        this->drawBorder();
        m_dirty = true;
    }

    void setDown(bool down, double t) {
        if (down == m_down) return;
        m_down = down;
        if (down) {
            m_presses++;
            m_count->setString(std::to_string(m_presses).c_str());
            this->fitCount();
            Note n;
            n.start = t;
            n.end = -1.0;
            n.sp = this->acquire();
            m_notes.push_back(n);
        } else if (!m_notes.empty() && m_notes.back().end < 0) {
            m_notes.back().end = t;
        }
    }

    void resetCount() {
        m_presses = 0;
        m_count->setString("0");
        this->fitCount();
    }

    void clearNotes() {
        for (auto& n : m_notes) this->release(n.sp);
        m_notes.clear();
    }

    // 노트: 타일과 같은 폭의 둥근 막대. 이미지 한 장이라 선이 생기지 않는다
    void tickNotes(double now) {
        if (!m_look.timeline) {
            this->clearNotes();
            return;
        }

        float H = std::max(20.f, m_look.noteHeight);
        float sp = m_look.noteSpeed;
        float minLen = std::max(kCap, m_look.noteMin);

        while (!m_notes.empty() && m_notes.front().end >= 0 &&
               (now - m_notes.front().end) * sp >= H) {
            this->release(m_notes.front().sp);
            m_notes.pop_front();
        }

        float baseY = m_h + 5.f;
        float nw = std::max(kCap, std::clamp(m_look.noteWidth, 0.2f, 1.f) * m_w);
        float x0 = (m_w - nw) / 2.f;
        float noteR = std::max(2.f, std::min(m_look.radius * m_h * 0.7f, 9.f));
        auto acc = this->accent();

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

            float fade = std::pow(1.f - yb / H, 1.3f);
            float sN = std::clamp(noteR / kBaseR, 0.2f, std::max(0.2f, std::min(nw, len) / kCap));
            float py = baseY + yb;

            if (n.lw != nw || n.lh != len || n.ls != sN) {
                n.lw = nw; n.lh = len; n.ls = sN;
                fitSize(s.base, nw, len, sN);
                fitSize(s.grad, nw, len, sN);
                fitSize(s.glow, nw + 2 * kGlowPad, len + 2 * kGlowPad, 1.f);
            }
            placeCenter(s.base, x0, py, nw, len);
            placeCenter(s.grad, x0, py, nw, len);
            placeCenter(s.glow, x0 - kGlowPad, py - kGlowPad, nw + 2 * kGlowPad, len + 2 * kGlowPad);

            s.base->setVisible(true);
            s.base->setColor(acc);
            setOp(s.base, 0.92f * fade);

            bool g = m_look.gradNotes;
            s.grad->setVisible(g);
            if (g) {
                s.grad->setColor(m_look.gradEnd);
                setOp(s.grad, 0.92f * fade);
            }

            bool gl = m_look.noteGlow;
            s.glow->setVisible(gl);
            if (gl) {
                s.glow->setColor(g ? lerpC(acc, m_look.gradEnd, 0.5f) : acc);
                setOp(s.glow, 0.45f * fade);
            }
        }
    }

    void tick(double now, float dt) {
        float target = m_down ? 1.f : 0.f;
        float rate = m_down ? 40.f : 14.f;
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
    int slot = 0;
    int button = 1;
    int player = 1;
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    std::string name;
};

class KeyViewer : public CCLayer {
protected:
    std::vector<KeyLane*> m_lanes;
    std::vector<LaneGeo> m_geo;
    CCLabelBMFont* m_info = nullptr;
    CCLabelBMFont* m_hint = nullptr;
    CCDrawNode* m_frame = nullptr;
    std::string m_infoStr;
    std::deque<double> m_hits;
    int m_total = 0;
    int m_peak = 0;
    float m_refresh = 0.f;
    float m_poll = 0.f;
    float m_rowW = 0.f;
    bool m_platformer = false;
    bool m_editing = false;
    bool m_dragging = false;
    CCPoint m_dragOffset = {0.f, 0.f};
    Look m_look;

    bool init(bool platformer) {
        if (!CCLayer::init()) return false;
        m_platformer = platformer;
        this->setTouchEnabled(true);
        loadStyles();

        // 1P 줄, 2P 줄을 모두 만들어 두고 설정에 따라 보이거나 숨긴다
        bool ok = true;
        for (int player = 1; player <= 2; player++) {
            auto add = [&](int button, char const* name) {
                auto lane = KeyLane::create(button, player, name);
                if (!lane) {
                    ok = false;
                    return;
                }
                this->addChild(lane);
                m_lanes.push_back(lane);
            };
            if (platformer) {
                add(2, "<");
                add(1, "JUMP");
                add(3, ">");
            } else {
                add(1, "JUMP");
            }
        }
        if (!ok) return false;

        m_frame = CCDrawNode::create();
        this->addChild(m_frame, 5);

        m_info = CCLabelBMFont::create("", "InterBold.fnt"_spr);
        m_info->setScale(0.38f);
        m_info->setOpacity(190);
        this->addChild(m_info);

        m_hint = CCLabelBMFont::create("DRAG TO MOVE", "InterBold.fnt"_spr);
        m_hint->setScale(0.38f);
        m_hint->setVisible(false);
        this->addChild(m_hint, 6);

        this->applyLook();
        this->scheduleUpdate();
        return true;
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

    // 터치를 가장 먼저 받되, 편집 중이고 키뷰어 위일 때만 가져간다
    void registerWithTouchDispatcher() override {
        CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -1000, true);
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        if (!m_editing || !this->getParent()) return false;
        auto p = this->getParent()->convertTouchToNodeSpace(touch);
        auto pos = this->getPosition();
        CCRect r(pos.x - m_rowW / 2 - 12.f, pos.y - 24.f, m_rowW + 24.f, m_look.size + 48.f);
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
        float x0 = -m_rowW / 2 - pad, x1 = m_rowW / 2 + pad;
        float y0 = -pad, y1 = m_look.size + pad;
        CCPoint v[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        ccColor4F none = {0.f, 0.f, 0.f, 0.f};
        ccColor4F border = {1.f, 1.f, 1.f, 0.9f};
        m_frame->drawPolygon(v, 4, none, 1.f, border);
    }

    void applyLook() {
        loadStyles();
        m_look = readLook();
        this->setVisible(m_look.viewerVisible);
        float s = m_look.size;
        float gap = m_look.gap, groupGap = gap + 10.f;

        // 배치 순서: (2P 줄) 그 다음 1P 줄
        std::vector<KeyLane*> order;
        if (m_look.showP2) {
            for (auto lane : m_lanes) if (lane->player() == 2) order.push_back(lane);
        }
        for (auto lane : m_lanes) if (lane->player() == 1) order.push_back(lane);
        for (auto lane : m_lanes) lane->setVisible(lane->player() == 1 || m_look.showP2);

        float jumpMul = std::max(0.5f, m_look.jumpWidth);
        std::vector<float> xs, ws;
        float x = 0.f;
        for (size_t i = 0; i < order.size(); i++) {
            if (i > 0) x += (order[i]->player() != order[i - 1]->player()) ? groupGap : gap;
            float w = order[i]->button() == 1 ? s * jumpMul : s;
            xs.push_back(x);
            ws.push_back(w);
            x += w;
        }
        m_rowW = x;

        m_geo.clear();
        for (size_t i = 0; i < order.size(); i++) {
            auto lane = order[i];
            int slot = slotOf(lane->button(), lane->player());
            auto const& st = g_style[slot];

            std::string name;
            if (lane->button() == 1) {
                if (m_look.showP2) name = (lane->player() == 1) ? "P1" : "P2";
                else name = m_look.labelJump;
            } else if (lane->button() == 2) {
                name = m_look.labelLeft;
            } else {
                name = m_look.labelRight;
            }
            lane->setName(name);

            float w = st.w > 0.5f ? st.w : ws[i];
            float h = st.h > 0.5f ? st.h : s;
            lane->layout(lookForSlot(m_look, slot), w, h);

            float px = xs[i] - m_rowW / 2 + st.dx;
            float py = st.dy;
            lane->setPosition({px, py});

            LaneGeo g;
            g.slot = slot;
            g.button = lane->button();
            g.player = lane->player();
            g.x = px; g.y = py; g.w = w; g.h = h;
            g.name = name;
            m_geo.push_back(g);
        }

        m_info->setPosition({0.f, -10.f});
        m_hint->setPosition({0.f, s + 14.f});

        if (!m_dragging) {
            auto win = CCDirector::get()->getWinSize();
            this->setPosition({win.width * m_look.posX / 100.f, win.height * m_look.posY / 100.f});
        }
        this->updateFrame();
    }

    void onButton(int button, bool down, bool isPlayer1) {
        int player = isPlayer1 ? 1 : 2;
        // 2P 줄을 끈 상태에서는 2P 입력을 무시해서 중복 집계를 막는다
        if (player == 2 && !m_look.showP2) return;

        double t = nowSec();
        for (auto lane : m_lanes) {
            if (lane->button() != button || lane->player() != player) continue;
            lane->setDown(down, t);
            if (down) {
                m_hits.push_back(t);
                m_total++;
            }
            return;
        }
    }

    void onRestart() {
        for (auto lane : m_lanes) {
            lane->setDown(false, nowSec());
            lane->clearNotes();
            if (m_look.resetOnRestart) lane->resetCount();
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
            this->setEditing(paused);
        }

        while (!m_hits.empty() && now - m_hits.front() > 1.0) {
            m_hits.pop_front();
        }
        int kps = static_cast<int>(m_hits.size());
        m_peak = std::max(m_peak, kps);

        for (auto lane : m_lanes) lane->tick(now, dt);

        std::string s;
        if (m_look.kps) s += "KPS " + std::to_string(kps) + "  MAX " + std::to_string(m_peak);
        if (m_look.total) {
            if (!s.empty()) s += "   ";
            s += "TOTAL " + std::to_string(m_total);
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
// 왼쪽 캔버스: 키를 눌러 선택하고 끌어서 옮긴다. 오른쪽: 탭별 설정. 아래: 프리셋과 스위치
// ---------------------------------------------------------------
static ccColor3B const kPalette[12] = {
    {255, 255, 255}, {226, 226, 232}, {140, 140, 150}, {18, 18, 22},
    {255, 80, 90},   {255, 160, 60},  {255, 230, 90},  {110, 230, 120},
    {80, 220, 255},  {90, 130, 255},  {170, 110, 255}, {255, 120, 200}
};

static char const* const kGradNames[4] = {"Off", "Notes", "Tile", "Both"};

struct Ctl {
    CCRect r = CCRect(0.f, 0.f, 0.f, 0.f);
    bool scroll = false;
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
    int id = 0; // 0~5 프리셋, 10 P2, 11 Show, 12 Press, 13 Reset
    float x0 = 0.f, x1 = 0.f;
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
    int m_sel = 0;
    bool m_previewPressed = false;
    bool m_closing = false;

    CCDrawNode* m_chrome = nullptr;
    CCDrawNode* m_canvas = nullptr;
    CCDrawNode* m_bar = nullptr;
    CCNode* m_canvasLabels = nullptr;
    CCNode* m_content = nullptr;
    CCLabelBMFont* m_tileLabels[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    CCLabelBMFont* m_tabLabels[4] = {nullptr, nullptr, nullptr, nullptr};
    std::vector<Btn> m_btns;

    std::vector<Ctl> m_fixed, m_scrollCtls;
    std::vector<RowInfo> m_rows;
    int m_activeFixed = -1, m_activeScroll = -1;
    bool m_scrolling = false;
    float m_lastY = 0.f;

    bool m_dragTile = false;
    CCPoint m_dragStart = {0.f, 0.f};
    float m_dragDx = 0.f, m_dragDy = 0.f;

    static constexpr float kSc = 1.4f;

    bool init(KeyViewer* viewer) {
        if (!CCLayer::init()) return false;
        m_viewer = viewer;
        this->setTouchEnabled(true);

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
        m_inW = inW - 12.f;

        auto back = CCDrawNode::create();
        this->addChild(back, 0);
        dnRect(back, 0.f, 0.f, W, H, ccColor3B{0, 0, 0}, 0.72f);

        m_chrome = CCDrawNode::create();
        this->addChild(m_chrome, 1);
        m_canvas = CCDrawNode::create();
        this->addChild(m_canvas, 2);
        m_canvasLabels = CCNode::create();
        this->addChild(m_canvasLabels, 3);
        m_content = CCNode::create();
        this->addChild(m_content, 4);
        m_content->setPosition({inX0 + 6.f, vpY1});
        m_bar = CCDrawNode::create();
        this->addChild(m_bar, 5);
        auto fixedLabels = CCNode::create();
        this->addChild(fixedLabels, 6);

        // 제목과 닫기
        mkLabel(fixedLabels, "KEY VIEWER", 0.42f, ccColor3B{255, 255, 255}, CCPoint(W / 2.f, winY1 - 16.f), 0.5f);
        mkLabel(fixedLabels, "X", 0.45f, ccColor3B{230, 230, 240}, CCPoint(winX1 - 22.f, winY1 - 16.f), 0.5f);
        {
            Ctl c;
            c.r = CCRect(winX1 - 40.f, winY1 - 32.f, 34.f, 30.f);
            c.down = [this](CCPoint) { m_closing = true; };
            m_fixed.push_back(c);
        }
        mkLabel(fixedLabels, "Tap a key to select it, drag to move", 0.26f, ccColor3B{130, 130, 150},
                CCPoint(cvX0 + 8.f, cvY0 + 8.f), 0.f);

        // 탭
        {
            char const* names[4] = {"KEY", "NOTE", "COUNT", "VIEW"};
            float tw = (inX1 - inX0 - 12.f) / 4.f;
            for (int i = 0; i < 4; i++) {
                float x0 = inX0 + 6.f + i * tw;
                m_tabLabels[i] = mkLabel(fixedLabels, names[i], 0.30f, ccColor3B{255, 255, 255},
                                         CCPoint(x0 + tw / 2.f, (tabY0 + inY1 - 2.f) / 2.f), 0.5f);
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
            Def defs[10] = {
                {"Custom", 0}, {"Dark", 1}, {"Neon", 2}, {"Rhythm", 3}, {"Light", 4}, {"Minimal", 5},
                {"P2", 10}, {"Show", 11}, {"Press", 12}, {"Reset", 13}
            };
            float total = winX1 - winX0 - 16.f;
            float bw = (total - 9.f * 4.f) / 10.f;
            for (int i = 0; i < 10; i++) {
                Btn b;
                b.label = defs[i].label;
                b.id = defs[i].id;
                b.x0 = winX0 + 8.f + i * (bw + 4.f);
                b.x1 = b.x0 + bw;
                m_btns.push_back(b);

                auto lab = mkLabel(fixedLabels, b.label, 0.30f, ccColor3B{255, 255, 255},
                                   CCPoint((b.x0 + b.x1) / 2.f, (tbY0 + tbY1) / 2.f), 0.5f);
                if (lab) {
                    float lw = lab->getContentSize().width * 0.30f;
                    if (lw > bw - 6.f && lw > 0.f) lab->setScale(0.30f * (bw - 6.f) / lw);
                }
                Ctl c;
                c.r = CCRect(b.x0, tbY0, bw, tbY1 - tbY0);
                int id = b.id;
                c.down = [this, id](CCPoint) { this->handleBtn(id); };
                m_fixed.push_back(c);
            }
        }

        // 캔버스 (맨 앞에 두면 다른 버튼과 겹치지 않는다)
        {
            Ctl c;
            c.r = CCRect(cvX0, cvY0, cvX1 - cvX0, cvY1 - cvY0);
            c.down = [this](CCPoint p) { this->canvasDown(p); };
            c.move = [this](CCPoint p) { this->canvasMove(p); };
            c.up = [this](CCPoint p) { this->canvasUp(p); };
            m_fixed.push_back(c);
        }

        // 타일 글자 6개 (필요할 때만 보인다)
        for (int i = 0; i < 6; i++) {
            m_tileLabels[i] = mkLabel(m_canvasLabels, "", 0.5f, ccColor3B{255, 255, 255}, CCPoint(0.f, 0.f), 0.5f);
            if (m_tileLabels[i]) m_tileLabels[i]->setVisible(false);
        }

        auto const& geo = m_viewer->geo();
        m_sel = geo.empty() ? 0 : geo.front().slot;

        this->redrawChrome();
        this->redrawCanvas();
        this->rebuildInspector();
        this->scheduleUpdate();
        return true;
    }

    // ---------- 도우미 ----------
    static std::string describeSlot(int slot) {
        static char const* const names[3] = {"JUMP", "LEFT", "RIGHT"};
        return std::string(names[slot % 3]) + (slot < 3 ? " (P1)" : " (P2)");
    }

    void ensureSelection() {
        auto const& geo = m_viewer->geo();
        if (geo.empty()) return;
        for (auto const& g : geo) if (g.slot == m_sel) return;
        m_sel = geo.front().slot;
    }

    // 키별 설정을 바꾼 뒤: 저장하고 게임 화면과 캔버스에 반영
    void commit() {
        saveStyle(m_sel);
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
        auto m = Mod::get();
        if (b.id >= 0 && b.id < 6) return m->getSettingValue<std::string>("preset") == kPresetNames[b.id];
        if (b.id == 10) return m->getSettingValue<bool>("show-p2");
        if (b.id == 11) return m->getSettingValue<bool>("show-viewer");
        if (b.id == 12) return m_previewPressed;
        return false;
    }

    void handleBtn(int id) {
        auto m = Mod::get();
        if (id >= 0 && id < 6) {
            m->setSettingValue<std::string>("preset", kPresetNames[id]);
        } else if (id == 10) {
            m->setSettingValue<bool>("show-p2", !m->getSettingValue<bool>("show-p2"));
        } else if (id == 11) {
            m->setSettingValue<bool>("show-viewer", !m->getSettingValue<bool>("show-viewer"));
        } else if (id == 12) {
            m_previewPressed = !m_previewPressed;
        } else if (id == 13) {
            g_style[m_sel] = KeyStyle{};
            saveStyle(m_sel);
        }
        m_viewer->applyLook();
        this->ensureSelection();
        this->redrawChrome();
        this->redrawCanvas();
        this->rebuildInspector();
    }

    // ---------- 그리기 ----------
    void redrawChrome() {
        m_chrome->clear();
        dnRound(m_chrome, winX0, winY0, winX1, winY1, 10.f, ccColor3B{30, 30, 38}, 1.f, 5);
        dnRect(m_chrome, winX0 + 10.f, winY1 - 31.f, winX1 - 10.f, winY1 - 30.4f, ccColor3B{62, 62, 76}, 1.f);

        dnRound(m_chrome, cvX0, cvY0, cvX1, cvY1, 6.f, ccColor3B{20, 20, 26}, 1.f, 4);
        for (float x = cvX0 + 20.f; x < cvX1 - 2.f; x += 20.f) {
            dnRect(m_chrome, x, cvY0 + 2.f, x + 0.5f, cvY1 - 2.f, ccColor3B{255, 255, 255}, 0.05f);
        }
        for (float y = cvY0 + 20.f; y < cvY1 - 2.f; y += 20.f) {
            dnRect(m_chrome, cvX0 + 2.f, y, cvX1 - 2.f, y + 0.5f, ccColor3B{255, 255, 255}, 0.05f);
        }

        dnRound(m_chrome, inX0, inY0, inX1, inY1, 6.f, ccColor3B{38, 38, 48}, 1.f, 4);
        float tw = (inX1 - inX0 - 12.f) / 4.f;
        for (int i = 0; i < 4; i++) {
            float x0 = inX0 + 6.f + i * tw;
            bool on = (i == m_tab);
            dnRound(m_chrome, x0 + 1.f, tabY0, x0 + tw - 1.f, inY1 - 3.f, 5.f,
                    on ? ccColor3B{70, 110, 240} : ccColor3B{52, 52, 66}, 1.f, 3);
            if (m_tabLabels[i]) m_tabLabels[i]->setColor(on ? ccColor3B{255, 255, 255} : ccColor3B{170, 170, 190});
        }

        for (auto const& b : m_btns) {
            bool on = this->btnActive(b);
            dnRound(m_chrome, b.x0, tbY0, b.x1, tbY1, 6.f,
                    on ? ccColor3B{70, 110, 240} : ccColor3B{52, 52, 66}, 1.f, 4);
        }
    }

    void redrawCanvas() {
        m_canvas->clear();
        for (auto l : m_tileLabels) if (l) l->setVisible(false);

        Look base = readLook();
        float cx = (cvX0 + cvX1) / 2.f;
        float cy = cvY0 + (cvY1 - cvY0) * 0.34f;
        auto const& geo = m_viewer->geo();

        for (auto const& g : geo) {
            Look kl = lookForSlot(base, g.slot);
            float x0 = cx + g.x * kSc, y0 = cy + g.y * kSc;
            float w = g.w * kSc, h = g.h * kSc;
            float x1 = x0 + w, y1 = y0 + h;
            float r = std::clamp(kl.radius * h, 2.f, std::min(w, h) / 2.f);

            // 노트 모양 미리보기
            if (kl.timeline) {
                ccColor3B acc = accentFor(kl, g.button);
                float nw = std::clamp(kl.noteWidth, 0.2f, 1.f) * w;
                float nx0 = x0 + (w - nw) / 2.f;
                float nr = std::clamp(kl.radius * h * 0.7f, 2.f, 9.f);
                dnRound(m_canvas, nx0, y1 + 6.f, nx0 + nw, y1 + 34.f, nr, acc, 0.85f, 3);
                dnRound(m_canvas, nx0, y1 + 42.f, nx0 + nw, y1 + 56.f, nr, acc, 0.55f, 3);
                dnRound(m_canvas, nx0, y1 + 64.f, nx0 + nw, y1 + 70.f, 3.f, acc, 0.3f, 2);
            }

            bool pressed = m_previewPressed;
            ccColor3B fill = pressed ? kl.pressed : kl.tile;
            float a = pressed ? 1.f : std::max(0.45f, kl.tileOpacity / 255.f);
            dnRound(m_canvas, x0, y0, x1, y1, r, fill, a, 5);
            if (kl.borderW > 0.f) dnStroke(m_canvas, x0, y0, x1, y1, r, kl.border, kl.borderW * kSc, 5);
            if (g.slot == m_sel) dnStroke(m_canvas, x0 - 3.f, y0 - 3.f, x1 + 3.f, y1 + 3.f, r + 3.f, ccColor3B{90, 140, 255}, 2.f, 5);

            auto lab = m_tileLabels[g.slot];
            if (lab) {
                lab->setString(g.name.c_str());
                float sc = 0.5f * (h / 46.f);
                lab->setScale(sc);
                float lw = lab->getContentSize().width * sc;
                if (lw > w * 0.84f && lw > 0.f) lab->setScale(sc * w * 0.84f / lw);
                lab->setColor(textFor(fill));
                lab->setPosition({(x0 + x1) / 2.f, (y0 + y1) / 2.f});
                lab->setVisible(true);
            }
        }
    }

    // ---------- 캔버스 터치 ----------
    void canvasDown(CCPoint p) {
        m_dragTile = false;
        float cx = (cvX0 + cvX1) / 2.f;
        float cy = cvY0 + (cvY1 - cvY0) * 0.34f;
        auto const& geo = m_viewer->geo();
        for (int i = static_cast<int>(geo.size()) - 1; i >= 0; i--) {
            auto const& g = geo[i];
            CCRect r(cx + g.x * kSc, cy + g.y * kSc, g.w * kSc, g.h * kSc);
            if (!r.containsPoint(p)) continue;
            bool changed = (m_sel != g.slot);
            m_sel = g.slot;
            m_dragTile = true;
            m_dragStart = p;
            m_dragDx = g_style[m_sel].dx;
            m_dragDy = g_style[m_sel].dy;
            this->redrawCanvas();
            if (changed) {
                m_scroll = 0.f;
                this->rebuildInspector();
            }
            return;
        }
    }

    void canvasMove(CCPoint p) {
        if (!m_dragTile) return;
        auto& st = g_style[m_sel];
        st.dx = std::clamp(std::round(m_dragDx + (p.x - m_dragStart.x) / kSc), -300.f, 300.f);
        st.dy = std::clamp(std::round(m_dragDy + (p.y - m_dragStart.y) / kSc), -200.f, 200.f);
        m_viewer->applyLook();
        this->redrawCanvas();
    }

    void canvasUp(CCPoint) {
        if (!m_dragTile) return;
        m_dragTile = false;
        saveStyle(m_sel);
        this->rebuildInspector();
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
        rowLabel(ri, text, 0.f, m_rows[ri].top - 9.f, 0.28f, ccColor3B{170, 170, 190}, 0.f);
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
            dnRect(d, tx0, ty - 1.2f, kx, ty + 1.2f, ccColor3B{90, 130, 255}, 1.f);
            dnRound(d, kx - 4.f, ty - 4.f, kx + 4.f, ty + 4.f, 4.f, ccColor3B{255, 255, 255}, 1.f, 3);
            if (val) val->setString((fmt ? fmt(v) : fmtNum(v)).c_str());
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

    void rowColor(std::string const& label, std::function<int()> get, std::function<void(int)> set, bool allowAuto) {
        int ri = beginRow(36.f);
        float top = m_rows[ri].top;
        float w = m_inW;
        rowLabel(ri, label, 0.f, top - 8.f, 0.30f, ccColor3B{235, 235, 240}, 0.f);
        auto d = rowDraw(ri);
        int n = allowAuto ? 13 : 12;
        float cell = w / n;
        float sw = cell - 2.f;
        float y0 = top - 31.f;
        if (allowAuto) rowLabel(ri, "A", cell / 2.f, y0 + sw / 2.f, 0.26f, ccColor3B{200, 200, 210}, 0.5f);

        auto refresh = [=]() {
            d->clear();
            int cur = get();
            for (int i = 0; i < n; i++) {
                float x0 = i * cell + 1.f;
                bool isAuto = allowAuto && i == 0;
                ccColor3B col = isAuto ? ccColor3B{50, 50, 60} : kPalette[i - (allowAuto ? 1 : 0)];
                dnRound(d, x0, y0, x0 + sw, y0 + sw, 3.f, col, 1.f, 3);
                bool sel = isAuto ? (cur < 0) : (cur == packC(col));
                if (sel) dnStroke(d, x0 - 1.f, y0 - 1.f, x0 + sw + 1.f, y0 + sw + 1.f, 4.f, ccColor3B{90, 140, 255}, 1.5f, 3);
            }
        };
        refresh();

        Ctl c;
        c.scroll = true;
        c.row = ri;
        c.r = CCRect(0.f, top - 36.f, w, 30.f);
        c.down = [=](CCPoint p) {
            int i = std::clamp(static_cast<int>(p.x / cell), 0, n - 1);
            if (allowAuto && i == 0) set(-1);
            else set(packC(kPalette[i - (allowAuto ? 1 : 0)]));
            refresh();
        };
        m_scrollCtls.push_back(c);
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
            dnRound(d, kx - 5.f, top - 16.f, kx + 5.f, top - 6.f, 5.f, ccColor3B{255, 255, 255}, 1.f, 4);
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

        std::vector<CCLabelBMFont*> labs;
        for (int i = 0; i < n; i++) {
            float cxm = i * (bw + 3.f) + bw / 2.f;
            labs.push_back(rowLabel(ri, opts[i], cxm, (y0 + y1) / 2.f, 0.28f, ccColor3B{255, 255, 255}, 0.5f));
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

    // 전체 설정(모드 설정)과 연결된 슬라이더, 스위치, 색
    void settingSlider(std::string const& label, char const* key, float vmin, float vmax, float step, bool isInt = false) {
        std::string k = key;
        rowSlider(label, vmin, vmax, step,
            [k, isInt]() {
                if (isInt) return static_cast<float>(Mod::get()->getSettingValue<int64_t>(k));
                return static_cast<float>(Mod::get()->getSettingValue<double>(k));
            },
            [this, k, isInt](float v) {
                if (isInt) Mod::get()->setSettingValue<int64_t>(k, static_cast<int64_t>(v));
                else Mod::get()->setSettingValue<double>(k, static_cast<double>(v));
                this->refreshAll();
            });
    }

    void settingToggle(std::string const& label, char const* key) {
        std::string k = key;
        rowToggle(label,
            [k]() { return Mod::get()->getSettingValue<bool>(k); },
            [this, k](bool v) {
                Mod::get()->setSettingValue<bool>(k, v);
                this->refreshAll();
            });
    }

    void settingColor(std::string const& label, char const* key) {
        std::string k = key;
        rowColor(label,
            [k]() { return packC(Mod::get()->getSettingValue<ccColor3B>(k)); },
            [this, k](int v) {
                if (v >= 0) Mod::get()->setSettingValue<ccColor3B>(k, unpackC(v));
                this->refreshAll();
            },
            false);
    }

    void presetNote() {
        auto p = Mod::get()->getSettingValue<std::string>("preset");
        if (p != "Custom") this->rowInfo("Preset \"" + p + "\" overrides colors. Pick Custom.");
    }

    void buildKeyTab() {
        int s = m_sel;
        {
            int ri = beginRow(18.f);
            rowLabel(ri, "KEY  " + describeSlot(s), 0.f, m_rows[ri].top - 9.f, 0.32f, ccColor3B{255, 255, 255}, 0.f);
        }
        rowSlider("Offset X", -300.f, 300.f, 1.f,
            [s]() { return g_style[s].dx; },
            [this, s](float v) { g_style[s].dx = v; this->commit(); });
        rowSlider("Offset Y", -200.f, 200.f, 1.f,
            [s]() { return g_style[s].dy; },
            [this, s](float v) { g_style[s].dy = v; this->commit(); });
        auto autoFmt = [](float v) { return v < 0.5f ? std::string("AUTO") : fmtNum(v); };
        rowSlider("Width", 0.f, 140.f, 1.f,
            [s]() { return g_style[s].w; },
            [this, s](float v) { g_style[s].w = v; this->commit(); }, autoFmt);
        rowSlider("Height", 0.f, 140.f, 1.f,
            [s]() { return g_style[s].h; },
            [this, s](float v) { g_style[s].h = v; this->commit(); }, autoFmt);
        float gr = readLook().radius * 100.f;
        rowSlider("Rounding %", 0.f, 50.f, 1.f,
            [s, gr]() { return g_style[s].radius >= 0.f ? g_style[s].radius : gr; },
            [this, s](float v) { g_style[s].radius = v; this->commit(); });
        rowSlider("Border width", 0.f, 6.f, 0.5f,
            [s]() { return g_style[s].borderW; },
            [this, s](float v) { g_style[s].borderW = v; this->commit(); });
        this->presetNote();
        rowColor("Background color",
            [s]() { return g_style[s].bg; },
            [this, s](int v) { g_style[s].bg = v; this->commit(); }, true);
        rowColor("Pressed color",
            [s]() { return g_style[s].pressed; },
            [this, s](int v) { g_style[s].pressed = v; this->commit(); }, true);
        rowColor("Border color",
            [s]() { return g_style[s].border; },
            [this, s](int v) { g_style[s].border = v; this->commit(); }, true);
        this->settingToggle("Press glow", "show-glow");
    }

    void buildNoteTab() {
        int s = m_sel;
        {
            int ri = beginRow(18.f);
            rowLabel(ri, "NOTES  " + describeSlot(s), 0.f, m_rows[ri].top - 9.f, 0.32f, ccColor3B{255, 255, 255}, 0.f);
        }
        this->presetNote();
        rowColor("Note color (this key)",
            [s]() { return g_style[s].accent; },
            [this, s](int v) { g_style[s].accent = v; this->commit(); }, true);
        this->settingToggle("Note timeline", "show-timeline");
        this->settingToggle("Note glow", "note-glow");
        this->settingSlider("Note width %", "note-width", 20.f, 100.f, 1.f);
        this->settingSlider("Min length", "note-min-length", 2.f, 60.f, 1.f);
        this->settingSlider("Speed", "note-speed", 100.f, 600.f, 5.f);
        this->settingSlider("Timeline length", "note-height", 40.f, 220.f, 5.f);
        rowChoice("Gradient colors", {"Off", "Notes", "Tile", "Both"},
            []() {
                auto g = Mod::get()->getSettingValue<std::string>("gradient-mode");
                for (int i = 0; i < 4; i++) if (g == kGradNames[i]) return i;
                return 0;
            },
            [this](int i) {
                Mod::get()->setSettingValue<std::string>("gradient-mode", kGradNames[std::clamp(i, 0, 3)]);
                this->refreshAll();
            });
        this->settingColor("Gradient end color", "gradient-color");
    }

    void buildCounterTab() {
        this->settingToggle("Key counters", "show-counter");
        this->settingToggle("KPS", "show-kps");
        this->settingToggle("Total presses", "show-total");
        this->settingToggle("Reset on restart", "reset-on-restart");
    }

    void buildViewTab() {
        this->presetNote();
        this->settingSlider("Box size", "box-size", 24.f, 80.f, 1.f);
        this->settingSlider("Jump width x", "jump-width", 0.5f, 3.f, 0.1f);
        this->settingSlider("Corner round %", "corner-radius", 0.f, 50.f, 1.f);
        this->settingSlider("Box gap", "box-gap", 0.f, 24.f, 1.f);
        this->settingSlider("Idle opacity", "tile-opacity", 0.f, 255.f, 5.f, true);
        this->settingColor("Idle box color", "tile-color");
        this->settingColor("Pressed box color", "pressed-color");
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
        else this->buildViewTab();
        m_contentH = -m_cursor;
        float maxS = std::max(0.f, m_contentH - (vpY1 - vpY0));
        m_scroll = std::clamp(m_scroll, 0.f, maxS);
        this->applyScroll();
    }

    void applyScroll() {
        m_content->setPosition({inX0 + 6.f, vpY1 + m_scroll});
        float vpH = vpY1 - vpY0;
        for (auto& r : m_rows) {
            bool vis = (r.top + m_scroll <= 0.5f) && (r.bottom + m_scroll >= -vpH - 0.5f);
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
        if (!inViewport(p)) return -1;
        CCPoint lp = toLocal(p);
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

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        auto p = this->convertTouchToNodeSpace(touch);
        m_activeFixed = -1;
        m_activeScroll = -1;
        m_scrolling = false;

        for (int i = static_cast<int>(m_fixed.size()) - 1; i >= 0; i--) {
            auto& c = m_fixed[i];
            if (!c.r.containsPoint(p)) continue;
            m_activeFixed = i;
            if (c.down) c.down(p);
            return true;
        }
        if (inViewport(p)) {
            int i = this->hitScroll(p);
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
            float maxS = std::max(0.f, m_contentH - (vpY1 - vpY0));
            m_scroll = std::clamp(m_scroll + dy, 0.f, maxS);
            this->applyScroll();
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
        if (!m_closing) return;
        m_closing = false;
        m_viewer->applyLook();
        this->removeFromParentAndCleanup(true);
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

// 일시정지 화면에 "Key Viewer" 버튼을 추가하고, 누르면 에디터를 연다
class $modify(KVPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto win = CCDirector::get()->getWinSize();
        auto spr = ButtonSprite::create("Key Viewer");
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
