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

static constexpr float kPi = 3.14159265358979f;

static double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------
// 그리기 도우미
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

// CCDrawNode는 미리 곱한 알파를 쓴다
static ccColor4F premul(ccColor3B c, float a) {
    a = std::clamp(a, 0.f, 1.f);
    return {c.r / 255.f * a, c.g / 255.f * a, c.b / 255.f * a, a};
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

static void fillPoly(CCDrawNode* n, std::vector<CCPoint>& pts, ccColor4F c) {
    n->drawPolygon(pts.data(), static_cast<unsigned int>(pts.size()), c, 0.f, c);
}

// ---------------------------------------------------------------
// 설정값 묶음
// ---------------------------------------------------------------
struct Look {
    float size = 44.f;
    float radius = 0.20f; // 박스 크기 대비 모서리 비율
    float gap = 6.f;
    GLubyte tileOpacity = 200;
    ccColor3B tile = {18, 18, 22};
    ccColor3B pressed = {226, 226, 232};
    ccColor3B accentJump = {255, 255, 255};
    ccColor3B accentLeft = {255, 255, 255};
    ccColor3B accentRight = {255, 255, 255};
    std::string labelJump = "JUMP";
    std::string labelLeft = "<";
    std::string labelRight = ">";
    float noteSpeed = 260.f;
    float noteHeight = 90.f;
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
    l.labelJump = shortLabel(m->getSettingValue<std::string>("label-jump"));
    l.labelLeft = shortLabel(m->getSettingValue<std::string>("label-left"));
    l.labelRight = shortLabel(m->getSettingValue<std::string>("label-right"));
    l.noteSpeed = static_cast<float>(m->getSettingValue<double>("note-speed"));
    l.noteHeight = static_cast<float>(m->getSettingValue<double>("note-height"));
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
// 키 하나(타일 + 글로우 + 카운터 + 노트)
// ---------------------------------------------------------------
struct Note {
    double start;
    double end; // 아직 누르는 중이면 -1
};

class KeyLane : public CCNode {
protected:
    int m_button = 0;
    int m_player = 1;
    bool m_down = false;
    bool m_dirty = true;
    int m_presses = 0;
    float m_w = 0.f, m_h = 0.f;
    float m_press = 0.f; // 0 = 평소, 1 = 눌림 (부드럽게 변함)
    Look m_look;
    std::string m_nameStr;
    CCLabelBMFont* m_name = nullptr;
    CCLabelBMFont* m_count = nullptr;
    CCDrawNode* m_tile = nullptr;
    CCDrawNode* m_glow = nullptr;
    CCDrawNode* m_draw = nullptr;
    std::deque<Note> m_notes;

    bool init(int button, int player, std::string const& name) {
        if (!CCNode::init()) return false;
        m_button = button;
        m_player = player;
        m_nameStr = name;

        m_glow = CCDrawNode::create();
        this->addChild(m_glow, -1);

        m_tile = CCDrawNode::create();
        this->addChild(m_tile, 0);

        m_draw = CCDrawNode::create();
        this->addChild(m_draw, 1);

        m_name = CCLabelBMFont::create(name.c_str(), "bigFont.fnt");
        this->addChild(m_name, 2);

        m_count = CCLabelBMFont::create("0", "bigFont.fnt");
        this->addChild(m_count, 2);
        return true;
    }

    ccColor3B accent() const {
        if (m_button == 2) return m_look.accentLeft;
        if (m_button == 3) return m_look.accentRight;
        return m_look.accentJump;
    }

    static ccColor3B textFor(ccColor3B bg) {
        return lum(bg) > 140.f ? ccColor3B{28, 28, 34} : ccColor3B{255, 255, 255};
    }

    void drawTile() {
        m_tile->clear();
        m_glow->clear();

        float p = m_press;
        float r = m_look.radius * m_h;

        // 눌렀을 때 박스 주변 빛 번짐
        if (m_look.glow && p > 0.02f) {
            auto acc = this->accent();
            for (int i = 3; i >= 1; i--) {
                float e = i * 3.f;
                auto pts = roundedPoly(-e, -e, m_w + e, m_h + e, r + e, 4);
                fillPoly(m_glow, pts, premul(acc, 0.10f * p));
            }
        }

        // 본체: 평소 색 -> 눌림 색
        ccColor3B col = lerpC(m_look.tile, m_look.pressed, p);
        float a = (m_look.tileOpacity / 255.f) + (1.f - m_look.tileOpacity / 255.f) * p;
        auto pts = roundedPoly(0.f, 0.f, m_w, m_h, r, 5);
        ccColor4F border = premul({255, 255, 255}, 0.16f * (1.f - p * 0.6f));
        m_tile->drawPolygon(pts.data(), static_cast<unsigned int>(pts.size()), premul(col, a), 1.f, border);
    }

    void updateColors() {
        ccColor3B idleText = m_look.tileOpacity < 128 ? ccColor3B{255, 255, 255} : textFor(m_look.tile);
        ccColor3B pressText = textFor(m_look.pressed);
        ccColor3B text = lerpC(idleText, pressText, m_press);
        ccColor3B bg = lerpC(m_look.tile, m_look.pressed, m_press);
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

        float f = m_h / 46.f;
        m_name->setScale(0.42f * f);
        m_name->setPosition({m_w / 2, l.counter ? m_h * 0.62f : m_h / 2});
        m_count->setVisible(l.counter);
        m_count->setScale(0.28f * f);
        m_count->setPosition({m_w / 2, m_h * 0.27f});
        m_dirty = true;
    }

    void setDown(bool down, double t) {
        if (down == m_down) return;
        m_down = down;
        if (down) {
            m_presses++;
            m_count->setString(std::to_string(m_presses).c_str());
            m_notes.push_back({t, -1.0});
        } else if (!m_notes.empty() && m_notes.back().end < 0) {
            m_notes.back().end = t;
        }
    }

    void resetCount() {
        m_presses = 0;
        m_count->setString("0");
    }

    void clearNotes() {
        m_notes.clear();
        m_draw->clear();
    }

    // 노트: 둥근 막대 여러 겹으로 위로 갈수록 흐려지는 그라데이션 + 빛 번짐
    void tickNotes(double now) {
        m_draw->clear();
        if (!m_look.timeline) {
            m_notes.clear();
            return;
        }

        float H = m_look.noteHeight;
        float sp = m_look.noteSpeed;

        while (!m_notes.empty() && m_notes.front().end >= 0 &&
               (now - m_notes.front().end) * sp >= H) {
            m_notes.pop_front();
        }

        float baseY = m_h + 5.f;
        float noteR = std::min(m_look.radius * m_h * 0.7f, 9.f);
        auto acc = this->accent();
        bool doGlow = m_look.noteGlow && m_notes.size() <= 14;
        static const float fracs[4] = {1.0f, 0.7f, 0.45f, 0.22f};

        for (auto const& n : m_notes) {
            double e = n.end < 0 ? now : n.end;
            float yb = static_cast<float>((now - e) * sp);
            if (yb >= H) continue;
            float yt = static_cast<float>((now - n.start) * sp);
            yt = std::min(H, std::max(yt, yb + 3.f));
            float len = std::max(2.f, yt - yb);

            float fade = std::pow(1.f - yb / H, 1.3f);
            float x0 = 1.f, x1 = m_w - 1.f;

            if (doGlow) {
                for (int i = 3; i >= 1; i--) {
                    float ex = i * 2.5f;
                    auto pts = roundedPoly(x0 - ex, baseY + yb - ex, x1 + ex, baseY + yb + len + ex, noteR + ex, 3);
                    fillPoly(m_draw, pts, premul(acc, 0.05f * fade));
                }
            }

            for (int k = 0; k < 4; k++) {
                float h = len * fracs[k];
                if (k > 0 && h < 3.f) break;
                h = std::max(h, 2.f);
                auto pts = roundedPoly(x0, baseY + yb, x1, baseY + yb + h, std::min(noteR, h / 2.f - 0.3f), 3);
                fillPoly(m_draw, pts, premul(acc, 0.38f * fade));
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
            this->drawTile();
            this->updateColors();
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
        for (int player = 1; player <= 2; player++) {
            auto add = [&](int button, char const* name) {
                auto lane = KeyLane::create(button, player, name);
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

        m_frame = CCDrawNode::create();
        this->addChild(m_frame, 5);

        m_info = CCLabelBMFont::create("", "bigFont.fnt");
        m_info->setScale(0.3f);
        m_info->setOpacity(190);
        this->addChild(m_info);

        m_hint = CCLabelBMFont::create("DRAG TO MOVE", "bigFont.fnt");
        m_hint->setScale(0.3f);
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

        float jumpMul = m_platformer ? 1.6f : (m_look.showP2 ? 1.4f : 2.2f);
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
