#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <string>
#include <vector>

using namespace geode::prelude;

static constexpr float kBaseR = 7.f;    // 이미지 모서리 반지름 (게임 단위)
static constexpr float kGlowPad = 16.f; // 글로우 이미지가 바깥으로 퍼지는 거리
static constexpr float kCap = 16.f;     // 9-slice 최소 크기 (양쪽 모서리 합)

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

        m_name = CCLabelBMFont::create(name.c_str(), "InterBold.fnt"_spr);
        this->addChild(m_name, 3);

        m_count = CCLabelBMFont::create("0", "InterBold.fnt"_spr);
        this->addChild(m_count, 3);
        return true;
    }

    ccColor3B accent() const {
        if (m_button == 2) return m_look.accentLeft;
        if (m_button == 3) return m_look.accentRight;
        return m_look.accentJump;
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

    void layout(Look const& l, float w) {
        m_look = l;
        m_w = w;
        m_h = l.size;
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
class KeyViewer : public CCLayer {
protected:
    std::vector<KeyLane*> m_lanes;
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
        m_look = readLook();
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

        for (size_t i = 0; i < order.size(); i++) {
            auto lane = order[i];
            if (lane->button() == 1) {
                lane->setName(m_look.showP2 ? (lane->player() == 1 ? "P1" : "P2") : m_look.labelJump);
            } else if (lane->button() == 2) {
                lane->setName(m_look.labelLeft);
            } else {
                lane->setName(m_look.labelRight);
            }
            lane->layout(m_look, ws[i]);
            lane->setPosition({xs[i] - m_rowW / 2, 0.f});
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
