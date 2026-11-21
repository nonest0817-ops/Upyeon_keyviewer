#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <algorithm>
#include <chrono>
#include <deque>
#include <string>
#include <vector>

using namespace geode::prelude;

static double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// 모서리를 깎은 사각형(둥근 모서리 흉내)
static std::vector<CCPoint> chamfer(float x0, float y0, float x1, float y1, float r) {
    r = std::min(r, std::min(x1 - x0, y1 - y0) / 2.f);
    return {
        {x0 + r, y0}, {x1 - r, y0}, {x1, y0 + r}, {x1, y1 - r},
        {x1 - r, y1}, {x0 + r, y1}, {x0, y1 - r}, {x0, y0 + r}
    };
}

// ---------------------------------------------------------------
// 설정값 묶음
// ---------------------------------------------------------------
struct Look {
    float size = 40.f;
    GLubyte opacity = 90;
    ccColor3B idle = {255, 255, 255};
    ccColor3B press = {120, 220, 255};
    ccColor3B note = {120, 220, 255};
    float noteSpeed = 260.f;
    float noteHeight = 90.f;
    bool timeline = true;
    bool counter = true;
    bool kps = true;
    bool total = true;
    bool glow = true;
    bool showP2 = false;
    bool resetOnRestart = false;
    float posX = 85.f;
    float posY = 10.f;
};

// 내장 프리셋: Custom이 아니면 색/효과를 덮어쓴다 (위치와 크기는 유지)
static void applyPreset(std::string const& p, Look& l) {
    if (p == "Minimal") {
        l.idle = {255, 255, 255}; l.press = {255, 255, 255}; l.note = {255, 255, 255};
        l.opacity = 60; l.timeline = false; l.glow = false;
        l.counter = false; l.kps = false; l.total = false;
    } else if (p == "Neon") {
        l.idle = {40, 40, 70}; l.press = {0, 255, 200}; l.note = {0, 255, 200};
        l.opacity = 120; l.timeline = true; l.glow = true; l.counter = true;
    } else if (p == "Rhythm") {
        l.idle = {255, 255, 255}; l.press = {255, 90, 150}; l.note = {255, 90, 150};
        l.opacity = 90; l.timeline = true; l.glow = false;
        l.noteSpeed = 340.f; l.noteHeight = 130.f;
        l.counter = true; l.kps = true; l.total = true;
    } else if (p == "Mono") {
        l.idle = {200, 200, 200}; l.press = {255, 255, 255}; l.note = {255, 255, 255};
        l.opacity = 80; l.timeline = true; l.glow = false;
    }
}

static Look readLook() {
    auto m = Mod::get();
    Look l;
    l.size = static_cast<float>(m->getSettingValue<double>("box-size"));
    l.opacity = static_cast<GLubyte>(std::clamp<int64_t>(m->getSettingValue<int64_t>("opacity"), 0, 255));
    l.idle = m->getSettingValue<ccColor3B>("idle-color");
    l.press = m->getSettingValue<ccColor3B>("press-color");
    l.note = m->getSettingValue<ccColor3B>("note-color");
    l.noteSpeed = static_cast<float>(m->getSettingValue<double>("note-speed"));
    l.noteHeight = static_cast<float>(m->getSettingValue<double>("note-height"));
    l.timeline = m->getSettingValue<bool>("show-timeline");
    l.counter = m->getSettingValue<bool>("show-counter");
    l.kps = m->getSettingValue<bool>("show-kps");
    l.total = m->getSettingValue<bool>("show-total");
    l.glow = m->getSettingValue<bool>("show-glow");
    l.showP2 = m->getSettingValue<bool>("show-p2");
    l.resetOnRestart = m->getSettingValue<bool>("reset-on-restart");
    l.posX = static_cast<float>(m->getSettingValue<double>("pos-x"));
    l.posY = static_cast<float>(m->getSettingValue<double>("pos-y"));
    applyPreset(m->getSettingValue<std::string>("preset"), l);
    return l;
}

// ---------------------------------------------------------------
// 키 하나(박스 + 글로우 + 카운터 + 노트 타임라인)
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
    int m_presses = 0;
    float m_w = 0.f, m_h = 0.f;
    float m_glowAmt = 0.f;
    Look m_look;
    std::string m_nameStr;
    CCScale9Sprite* m_bg = nullptr;
    CCLabelBMFont* m_name = nullptr;
    CCLabelBMFont* m_count = nullptr;
    CCDrawNode* m_draw = nullptr;
    CCDrawNode* m_glow = nullptr;
    std::deque<Note> m_notes;

    bool init(int button, int player, std::string const& name) {
        if (!CCNode::init()) return false;
        m_button = button;
        m_player = player;
        m_nameStr = name;

        m_glow = CCDrawNode::create();
        this->addChild(m_glow, -1);

        m_bg = CCScale9Sprite::create("square02_001.png");
        this->addChild(m_bg);

        m_name = CCLabelBMFont::create(name.c_str(), "bigFont.fnt");
        this->addChild(m_name, 2);

        m_count = CCLabelBMFont::create("0", "goldFont.fnt");
        this->addChild(m_count, 2);

        m_draw = CCDrawNode::create();
        this->addChild(m_draw, 1);
        return true;
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

    GLubyte pressedOpacity() const {
        return static_cast<GLubyte>(std::min<int>(255, m_look.opacity + 150));
    }

    void layout(Look const& l, float w) {
        bool sizeChanged = (w != m_w || l.size != m_h);
        m_look = l;
        m_w = w;
        m_h = l.size;

        if (sizeChanged) {
            this->setContentSize({m_w, m_h});
            m_bg->setContentSize({m_w, m_h});
            m_bg->setPosition({m_w / 2, m_h / 2});
        }

        float f = m_h / 46.f;
        m_name->setScale(0.42f * f);
        m_name->setPosition({m_w / 2, l.counter ? m_h * 0.62f : m_h / 2});
        m_count->setVisible(l.counter);
        m_count->setScale(0.32f * f);
        m_count->setPosition({m_w / 2, m_h * 0.27f});
        m_count->setOpacity(190);

        // 애니메이션 도중이 아닐 때만 색을 직접 덮어쓴다
        if (m_bg->numberOfRunningActions() == 0) {
            m_bg->setColor(m_down ? l.press : l.idle);
            m_bg->setOpacity(m_down ? pressedOpacity() : l.opacity);
            m_bg->setScale(m_down ? 0.92f : 1.f);
        }
    }

    void setDown(bool down, double t) {
        if (down == m_down) return;
        m_down = down;
        m_bg->stopAllActions();

        if (down) {
            m_presses++;
            m_count->setString(std::to_string(m_presses).c_str());
            m_notes.push_back({t, -1.0});
            auto c = m_look.press;
            m_bg->runAction(CCSpawn::create(
                CCEaseOut::create(CCScaleTo::create(0.05f, 0.92f), 2.f),
                CCTintTo::create(0.05f, c.r, c.g, c.b),
                CCFadeTo::create(0.05f, pressedOpacity()),
                nullptr
            ));
        } else {
            if (!m_notes.empty() && m_notes.back().end < 0) {
                m_notes.back().end = t;
            }
            auto c = m_look.idle;
            m_bg->runAction(CCSpawn::create(
                CCEaseOut::create(CCScaleTo::create(0.14f, 1.f), 2.f),
                CCTintTo::create(0.14f, c.r, c.g, c.b),
                CCFadeTo::create(0.14f, m_look.opacity),
                nullptr
            ));
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

    // 누르는 동안 박스 주변이 은은하게 빛난다
    void tickGlow(float dt) {
        float target = m_down ? 1.f : 0.f;
        m_glowAmt += (target - m_glowAmt) * std::min(1.f, dt * 16.f);

        m_glow->clear();
        if (!m_look.glow || m_glowAmt < 0.02f) return;

        float a = 0.11f * m_glowAmt;
        // CCDrawNode는 미리 곱한 알파를 쓴다
        ccColor4F c = {
            m_look.press.r / 255.f * a,
            m_look.press.g / 255.f * a,
            m_look.press.b / 255.f * a,
            a
        };
        for (int i = 4; i >= 1; i--) {
            float e = i * 2.5f;
            auto pts = chamfer(-e, -e, m_w + e, m_h + e, 3.f + e);
            m_glow->drawPolygon(pts.data(), static_cast<unsigned int>(pts.size()), c, 0.f, c);
        }
    }

    // 매 프레임 노트 그리기
    void tickNotes(double now) {
        m_draw->clear();
        if (!m_look.timeline) {
            m_notes.clear();
            return;
        }

        float H = m_look.noteHeight;
        float sp = m_look.noteSpeed;

        // 위로 사라진 오래된 노트 제거
        while (!m_notes.empty() && m_notes.front().end >= 0 &&
               (now - m_notes.front().end) * sp >= H) {
            m_notes.pop_front();
        }

        float x0 = 4.f, x1 = m_w - 4.f;
        float baseY = m_h + 6.f;
        float baseAlpha = std::max(m_look.opacity / 255.f, 0.55f);

        for (auto const& n : m_notes) {
            double e = n.end < 0 ? now : n.end;
            float yb = static_cast<float>((now - e) * sp);
            if (yb >= H) continue;
            float yt = static_cast<float>((now - n.start) * sp);
            yt = std::min(H, std::max(yt, yb + 2.f));

            float a = baseAlpha * (1.f - yb / H);
            ccColor4F c = {
                m_look.note.r / 255.f * a,
                m_look.note.g / 255.f * a,
                m_look.note.b / 255.f * a,
                a
            };
            CCPoint v[4] = {
                {x0, baseY + yb}, {x1, baseY + yb},
                {x1, baseY + yt}, {x0, baseY + yt}
            };
            m_draw->drawPolygon(v, 4, c, 0.f, c);
        }
    }

    void tick(double now, float dt) {
        this->tickGlow(dt);
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
        float gap = 6.f, groupGap = 16.f;

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
                lane->setName(m_look.showP2 ? (lane->player() == 1 ? "P1" : "P2") : "JUMP");
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
