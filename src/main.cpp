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
    bool resetOnRestart = false;
    float posX = 85.f;
    float posY = 10.f;
};

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
    l.resetOnRestart = m->getSettingValue<bool>("reset-on-restart");
    l.posX = static_cast<float>(m->getSettingValue<double>("pos-x"));
    l.posY = static_cast<float>(m->getSettingValue<double>("pos-y"));
    return l;
}

// ---------------------------------------------------------------
// 키 하나(박스 + 카운터 + 노트 타임라인)
// ---------------------------------------------------------------
struct Note {
    double start;
    double end; // 아직 누르는 중이면 -1
};

class KeyLane : public CCNode {
protected:
    int m_button = 0;
    bool m_down = false;
    int m_presses = 0;
    float m_w = 0.f, m_h = 0.f;
    Look m_look;
    CCScale9Sprite* m_bg = nullptr;
    CCLabelBMFont* m_name = nullptr;
    CCLabelBMFont* m_count = nullptr;
    CCDrawNode* m_draw = nullptr;
    std::deque<Note> m_notes;

    bool init(int button, std::string const& name) {
        if (!CCNode::init()) return false;
        m_button = button;

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
    static KeyLane* create(int button, std::string const& name) {
        auto ret = new KeyLane();
        if (ret->init(button, name)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    int button() const { return m_button; }

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

    // 매 프레임 노트 그리기
    void tick(double now) {
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
            // CCDrawNode는 미리 곱한 알파를 쓴다
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
};

// ---------------------------------------------------------------
// 키뷰어 전체
// ---------------------------------------------------------------
class KeyViewer : public CCNode {
protected:
    std::vector<KeyLane*> m_lanes;
    CCLabelBMFont* m_info = nullptr;
    std::string m_infoStr;
    std::deque<double> m_hits;
    int m_total = 0;
    int m_peak = 0;
    float m_refresh = 0.f;
    bool m_platformer = false;
    Look m_look;

    bool init(bool platformer) {
        if (!CCNode::init()) return false;
        m_platformer = platformer;

        auto add = [&](int button, char const* name) {
            auto lane = KeyLane::create(button, name);
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

        m_info = CCLabelBMFont::create("", "bigFont.fnt");
        m_info->setScale(0.3f);
        m_info->setOpacity(190);
        this->addChild(m_info);

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

    void applyLook() {
        m_look = readLook();
        float s = m_look.size;
        float gap = 6.f;

        std::vector<float> widths;
        float total = 0.f;
        for (auto lane : m_lanes) {
            float w = lane->button() == 1 ? (m_platformer ? s * 1.6f : s * 2.2f) : s;
            widths.push_back(w);
            total += w;
        }
        total += gap * (m_lanes.size() - 1);

        float x = -total / 2;
        for (size_t i = 0; i < m_lanes.size(); i++) {
            m_lanes[i]->layout(m_look, widths[i]);
            m_lanes[i]->setPosition({x, 0.f});
            x += widths[i] + gap;
        }

        m_info->setPosition({0.f, -10.f});

        auto win = CCDirector::get()->getWinSize();
        this->setPosition({win.width * m_look.posX / 100.f, win.height * m_look.posY / 100.f});
    }

    void onButton(int button, bool down) {
        double t = nowSec();
        for (auto lane : m_lanes) {
            if (lane->button() != button) continue;
            bool wasDown = down; // 눌림 판정은 lane 내부에서 중복 방지
            lane->setDown(down, t);
            if (wasDown) {
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

        while (!m_hits.empty() && now - m_hits.front() > 1.0) {
            m_hits.pop_front();
        }
        int kps = static_cast<int>(m_hits.size());
        m_peak = std::max(m_peak, kps);

        for (auto lane : m_lanes) lane->tick(now);

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

        // 2P 중복 집계 방지: 1P 입력만 표시
        if (!isPlayer1) return;

        auto pl = PlayLayer::get();
        if (!pl) return;
        auto viewer = static_cast<KVPlayLayer*>(pl)->m_fields->viewer;
        if (viewer) viewer->onButton(button, down);
    }
};
