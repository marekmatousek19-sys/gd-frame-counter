#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/ui/Notification.hpp>

#include <algorithm>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace geode::prelude;

static constexpr const char* OVERLAY_ID = "frame-counter/overlay";
static constexpr const char* MENU_ID    = "frame-counter/menu";
static constexpr const char* BTN_REC    = "frame-counter/btn-rec";
static constexpr const char* BTN_PLAY   = "frame-counter/btn-play";
static constexpr const char* BTN_DEL    = "frame-counter/btn-del";
static constexpr const char* BTN_PREV   = "frame-counter/btn-prev";
static constexpr const char* BTN_NEXT   = "frame-counter/btn-next";
static constexpr const char* BTN_SLOT   = "frame-counter/btn-slot";

// 30 game units = 1 block, used to convert horizontal speed to BPS
static constexpr double UNITS_PER_BLOCK = 30.0;

// GD's physics rate: 1 macro frame = 1/240s (community "1f")
static constexpr double MACRO_TPS = 240.0;

namespace macro {
    constexpr int NUM_SLOTS = 4;

    struct Action {
        int frame;
        int button;
        bool press;
        int sub = 0;  // step offset inside the tick (Click Between Steps)
    };

    struct State {
        bool recording = false;
        bool playing = false;

        int activeSlot = 0;
        int levelId = 0;
        size_t playIndex = 0;
        std::vector<Action> actions;

        long long lastTick = -1;          // tick the engine is currently in
        long long lastTickStartStep = 0;  // step counter value when that tick began
    };

    static State state;

    // Frame number straight from the level's internal clock - drift-free
    long long tick() {
        auto pl = PlayLayer::get();
        if (!pl) return 0;
        return static_cast<long long>(pl->m_gameState.m_levelTime * MACRO_TPS);
    }

    std::filesystem::path filePath(int levelId, int slot) {
        return Mod::get()->getSaveDir() / fmt::format("macro_{}_{}.txt", levelId, slot);
    }

    void save(int levelId) {
        std::string out;
        for (auto const& a : state.actions)
            out += fmt::format("{} {} {} {}\n", a.frame, a.button, a.press ? 1 : 0, a.sub);
        (void)geode::utils::file::writeString(filePath(levelId, state.activeSlot), out);
    }

    void load(int levelId) {
        state.actions.clear();
        state.playIndex = 0;
        auto res = geode::utils::file::readString(filePath(levelId, state.activeSlot));
        // v1.2 macros had no slot suffix; treat them as slot 1
        if (!res && state.activeSlot == 0)
            res = geode::utils::file::readString(Mod::get()->getSaveDir() / fmt::format("macro_{}.txt", levelId));
        if (!res) return;
        for (auto const& line : geode::utils::string::split(*res, "\n")) {
            int frame = 0, button = 0, press = 0, sub = 0;
            int n = sscanf(line.c_str(), "%d %d %d %d", &frame, &button, &press, &sub);
            if (n >= 3)
                state.actions.push_back({ frame, button, press != 0, n >= 4 ? sub : 0 });
        }
    }

    void saveActiveSlot() {
        (void)geode::utils::file::writeString(
            Mod::get()->getSaveDir() / "active-slot.txt", fmt::format("{}", state.activeSlot)
        );
    }

    void loadActiveSlot() {
        if (auto res = geode::utils::file::readString(Mod::get()->getSaveDir() / "active-slot.txt")) {
            int s = 0;
            if (sscanf(res->c_str(), "%d", &s) == 1 && s >= 0 && s < NUM_SLOTS)
                state.activeSlot = s;
        }
    }

    void notify(std::string const& msg) {
        Notification::create(msg, NotificationIcon::Success, 1.f)->show();
    }

    bool enabled() {
        return PlayLayer::get() && Mod::get()->getSettingValue<bool>("macro-enabled");
    }

    void stopAll() {
        state.recording = false;
        state.playing = false;
        state.playIndex = 0;
    }

    void toggleRecord() {
        if (!enabled()) return;
        state.recording = !state.recording;
        if (state.recording) {
            state.playing = false;
            state.actions.clear();
            state.playIndex = 0;
            notify(fmt::format("Recording slot {}/{}", state.activeSlot + 1, NUM_SLOTS));
        } else {
            if (state.actions.empty()) {
                notify("Nothing recorded");
            } else {
                save(state.levelId);
                state.playing = true;
                state.playIndex = 0;
                notify(fmt::format("Saved {} inputs - replaying", state.actions.size()));
            }
        }
    }

    void togglePlay() {
        if (!enabled()) return;
        if (state.actions.empty()) {
            notify("No macro in this slot");
            return;
        }
        state.playing = !state.playing;
        state.recording = false;
        state.playIndex = 0;
        notify(state.playing ? "Replay on" : "Replay off");
    }

    void clearMacro() {
        if (!enabled()) return;
        state.actions.clear();
        state.playIndex = 0;
        stopAll();
        save(state.levelId);
        notify(fmt::format("Slot {}/{} cleared", state.activeSlot + 1, NUM_SLOTS));
    }

    void switchSlot(int dir) {
        if (!enabled()) return;
        state.activeSlot = (state.activeSlot + dir + NUM_SLOTS) % NUM_SLOTS;
        saveActiveSlot();
        stopAll();
        load(state.levelId);
        notify(fmt::format("Slot {}/{}", state.activeSlot + 1, NUM_SLOTS));
    }
}

// Draggable overlay holding the counter label + the macro menu
class CounterOverlay : public cocos2d::CCLayer {
public:
    CCLabelBMFont* m_label = nullptr;
    CCMenu* m_menu = nullptr;
    float m_menuWidth = 0.f;

    static CounterOverlay* create() {
        auto ret = new CounterOverlay();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

protected:
    bool init() override {
        if (!CCLayer::init()) return false;
        this->setID(OVERLAY_ID);

        m_label = CCLabelBMFont::create("", "bigFont.fnt");
        m_label->setAnchorPoint({ 0.f, 1.f });
        this->addChild(m_label);
        this->loadPosition();

        m_menu = CCMenu::create();
        m_menu->setID(MENU_ID);
        m_menu->setVisible(false);
        this->addChild(m_menu);
        this->buildMenu();

        auto win = CCDirector::get()->getWinSize();
        m_menu->setPosition({ win.width - 8.f - m_menuWidth, 26.f });

        this->setTouchMode(cocos2d::kCCTouchesOneByOne);
        this->setTouchEnabled(true);
        return true;
    }

    // Claim touches before gameplay so dragging the label works in levels
    void registerWithTouchDispatcher() override {
        CCTouchDispatcher::get()->addTargetedDelegate(this, -129, false);
    }

    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent*) override {
        if (!m_label) return false;
        auto pos = this->convertToNodeSpace(touch->getLocation());
        if (m_label->boundingBox().containsPoint(pos)) {
            return true;
        }
        return false;
    }

    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent*) override {
        if (!m_label || !m_label->boundingBox().containsPoint(this->convertToNodeSpace(touch->getLocation())))
            return;
        m_label->setPosition(m_label->getPosition() + touch->getDelta());
    }

    void ccTouchEnded(cocos2d::CCTouch*, cocos2d::CCEvent*) override {
        this->savePosition();
    }

    void savePosition() {
        if (!m_label) return;
        auto win = CCDirector::get()->getWinSize();
        auto p = m_label->getPosition();
        (void)geode::utils::file::writeString(
            Mod::get()->getSaveDir() / "counter-position.txt",
            fmt::format("{} {}", p.x / win.width, p.y / win.height)
        );
    }

    void loadPosition() {
        auto win = CCDirector::get()->getWinSize();
        float fx = 0.005f, fy = 0.995f;
        if (auto res = geode::utils::file::readString(Mod::get()->getSaveDir() / "counter-position.txt")) {
            sscanf(res->c_str(), "%f %f", &fx, &fy);
        }
        m_label->setPosition({ fx * win.width, fy * win.height });
    }

    CCMenuItemSprite* button(const char* text, cocos2d::SEL_MenuHandler handler, const char* id) {
        auto spr = CCSprite::createWithSpriteFrameName("GJ_button_04.png");
        auto lbl = CCLabelBMFont::create(text, "bigFont.fnt");
        lbl->setScale(0.35f);
        lbl->setPosition({ spr->getContentSize().width / 2.f, spr->getContentSize().height / 2.f });
        spr->addChild(lbl);
        auto item = CCMenuItemSprite::create(spr, spr, this, handler);
        item->setID(id);
        return item;
    }

    void buildMenu() {
        float w = 44.f, gap = 6.f, x = 0.f;
        auto prev = this->button("<",    menu_selector(CounterOverlay::onPrev),   BTN_PREV);
        auto slot = CCLabelBMFont::create("1/4", "goldFont.fnt");
        slot->setScale(0.45f);
        slot->setID(BTN_SLOT);
        auto next = this->button(">",    menu_selector(CounterOverlay::onNext),   BTN_NEXT);
        auto rec  = this->button("REC",  menu_selector(CounterOverlay::onRec),    BTN_REC);
        auto play = this->button("PLAY", menu_selector(CounterOverlay::onPlay),   BTN_PLAY);
        auto del  = this->button("DEL",  menu_selector(CounterOverlay::onDelete), BTN_DEL);

        std::vector<std::pair<cocos2d::CCNode*, float>> placed = {
            { prev, x }, { slot, (x += w + gap, x) }, { next, (x += w, x) },
            { rec, (x += w + gap, x) }, { play, (x += w + gap, x) }, { del, (x += w + gap, x) }
        };
        for (auto& [node, px] : placed) {
            node->setPosition({ px + w / 2.f, 0.f });
            m_menu->addChild(node);
        }
        m_menuWidth = x + w;
    }

    void onRec(CCObject*) { macro::toggleRecord(); }
    void onPlay(CCObject*) { macro::togglePlay(); }
    void onDelete(CCObject*) { macro::clearMacro(); }
    void onPrev(CCObject*) { macro::switchSlot(-1); }
    void onNext(CCObject*) { macro::switchSlot(1); }
};

class $modify(FrameCounter, cocos2d::CCScheduler) {
    struct Fields {
        double accum = 0.0;
        int frames = 0;
        long long totalFrames = 0;

        double bpsAccum = 0.0;
        double lastX = -1.0;

        double fps = 0.0;
        double bps = 0.0;
    };

    void update(float dt) {
        CCScheduler::update(dt);

        if (dt <= 0.f) return;

        auto f = m_fields;
        auto mod = Mod::get();

        // Keep the game locked to the target FPS (240 = frame-perfect 1f units)
        if (mod->getSettingValue<bool>("unlock-fps")) {
            double targetInterval = 1.0 / static_cast<double>(mod->getSettingValue<int64_t>("target-fps"));
            auto app = CCApplication::get();
            if (std::fabs(app->getAnimationInterval() - targetInterval) > 1e-6) {
                app->setAnimationInterval(targetInterval);
            }
        }

        f->frames += 1;
        f->totalFrames += 1;
        f->accum += dt;

        bool inLevel = PlayLayer::get() != nullptr;
        if (inLevel) {
            if (auto player = inLevel ? PlayLayer::get()->m_player1 : nullptr) {
                double x = player->getPositionX();
                if (f->lastX >= 0.0) f->bpsAccum += std::fabs(x - f->lastX);
                f->lastX = x;
            }
        } else {
            f->lastX = -1.0;
            f->bpsAccum = 0.0;
        }

        bool boundary = f->accum >= 0.5;
        if (boundary) {
            f->fps = f->frames / f->accum;
            f->bps = f->bpsAccum / f->accum / UNITS_PER_BLOCK;
            f->frames = 0;
            f->accum = 0.0;
            f->bpsAccum = 0.0;
        }

        // In levels refresh every frame so the macro counter is live
        if (!boundary && !inLevel) return;

        if (!mod->getSettingValue<bool>("enabled")) return;

        auto scene = CCDirector::get()->getRunningScene();
        if (!scene) return;

        // Recreate the overlay if the scene changed (menu, level, etc.)
        auto existing = scene->getChildByID(OVERLAY_ID);
        auto overlay = cast::type_cast<CounterOverlay*>(existing);
        if (existing && !overlay) {
            existing->removeFromParent();
            overlay = nullptr;
        }
        if (!overlay) {
            overlay = CounterOverlay::create();
            scene->addChild(overlay, 9999);
        }

        auto label = overlay->m_label;
        auto winSize = CCDirector::get()->getWinSize();
        float scale = mod->getSettingValue<float>("scale");
        label->setVisible(true);
        label->setScale(scale);

        std::string text = fmt::format("FPS: {:.0f}", f->fps);
        if (mod->getSettingValue<bool>("show-bps")) {
            text += fmt::format("\nBPS: {:.1f}", f->bps);
        }
        if (mod->getSettingValue<bool>("show-frames")) {
            long long targetBase = mod->getSettingValue<int64_t>("target-fps");
            if (mod->getSettingValue<bool>("unlock-fps") && targetBase != 120 && targetBase > 0) {
                text += fmt::format("\nFrames: {} ({} at 120fps)", f->totalFrames, f->totalFrames * 120 / targetBase);
            } else {
                text += fmt::format("\nFrames: {}", f->totalFrames);
            }
        }

        // Live macro line while inside a level
        if (mod->getSettingValue<bool>("macro-enabled") && inLevel) {
            if (macro::state.recording) {
                text += fmt::format("\nREC: frame {}", macro::tick());
            } else if (macro::state.playing) {
                text += fmt::format(
                    "\nMacro {}/{}: frame {} / {}",
                    macro::state.activeSlot + 1, macro::NUM_SLOTS, macro::tick(), macro::state.actions.size()
                );
            } else if (!macro::state.actions.empty()) {
                text += fmt::format(
                    "\nSlot {}/{}: {} inputs",
                    macro::state.activeSlot + 1, macro::NUM_SLOTS, macro::state.actions.size()
                );
            }
        }

        label->setString(text.c_str());

        double threshold = static_cast<double>(mod->getSettingValue<int64_t>("low-fps"));
        if (macro::state.recording || f->fps < threshold) label->setColor({ 255, 80, 80 });
        else label->setColor({ 255, 255, 255 });

        // Macro menu: visible in levels, with live button states
        if (auto menu = cast::type_cast<CCMenu*>(overlay->getChildByID(MENU_ID))) {
            menu->setVisible(inLevel && mod->getSettingValue<bool>("macro-enabled"));
            if (menu->isVisible()) {
                if (auto slotLabel = cast::type_cast<CCLabelBMFont*>(menu->getChildByID(BTN_SLOT)))
                    slotLabel->setString(fmt::format("{}/{}", macro::state.activeSlot + 1, macro::NUM_SLOTS).c_str());
                if (auto rec = cast::type_cast<CCMenuItemSprite*>(menu->getChildByID(BTN_REC)))
                    rec->setColor(macro::state.recording ? ccColor3B{ 255, 80, 80 } : ccColor3B{ 255, 255, 255 });
                if (auto play = cast::type_cast<CCMenuItemSprite*>(menu->getChildByID(BTN_PLAY)))
                    play->setColor(macro::state.playing ? ccColor3B{ 90, 255, 120 } : ccColor3B{ 255, 255, 255 });
            }
        }
    }
};

// Records and injects inputs, one action per 240fps frame
class $modify(MacroBot, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool player1) {
        // Ignore real clicks while a replay is running so it stays clean
        if (macro::state.playing && player1 && PlayLayer::get()) return;

        GJBaseGameLayer::handleButton(down, button, player1);

        if (!player1 || !macro::state.recording || !PlayLayer::get()) return;
        int sub = (int)std::clamp<long long>(m_currentStep - macro::state.lastTickStartStep, 0LL, 3LL);
        macro::state.actions.push_back({ (int)macro::tick(), button, down, sub });
    }

    // Fire due inputs inside the game's own command processing loop.
    // Click Between Steps: within a tick, wait until the engine's step
    // counter reaches the recorded sub-step offset before firing.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (macro::state.playing && PlayLayer::get()) {
            long long tick = macro::tick();
            if (tick != macro::state.lastTick) {
                macro::state.lastTick = tick;
                macro::state.lastTickStartStep = m_currentStep;
            }
            int stepDelta = (int)std::max<long long>(0LL, m_currentStep - macro::state.lastTickStartStep);

            while (macro::state.playIndex < macro::state.actions.size()) {
                auto const& a = macro::state.actions[macro::state.playIndex];
                if (a.frame > tick) break;
                if (a.frame == tick && a.sub > stepDelta) break;
                GJBaseGameLayer::handleButton(a.press, a.button, true);
                macro::state.playIndex++;
            }
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// Per-attempt frame reset + macro loading when entering a level
class $modify(MacroLevel, PlayLayer) {
    void reset() {
        PlayLayer::reset();

        int lid = m_level ? m_level->m_levelID : 0;
        if (lid != macro::state.levelId) {
            macro::state.levelId = lid;
            macro::state.recording = macro::state.playing = false;
            macro::load(lid);
        }

        if (macro::state.recording) macro::state.actions.clear();
        macro::state.playIndex = 0;
        macro::state.lastTick = -1;
    }
};

// Hotkeys: F5 record, F6 replay, F7 clear
class $modify(MacroKeys, cocos2d::CCKeyboardDispatcher) {
    bool dispatchKeypadMSG(cocos2d::enumKeyCodes key, bool down) {
        if (down && PlayLayer::get()) {
            if (key == enumKeyCodes::KEY_F5) macro::toggleRecord();
            else if (key == enumKeyCodes::KEY_F6) macro::togglePlay();
            else if (key == enumKeyCodes::KEY_F7) macro::clearMacro();
        }
        return CCKeyboardDispatcher::dispatchKeypadMSG(key, down);
    }
};

$on_mod(Loaded) {
    // Enable GD's native Click Between Steps so inputs register at sub-step precision
    if (Mod::get()->getSettingValue<bool>("cbs"))
        GameManager::get()->setGameVariable("0177", true);
    macro::loadActiveSlot();
    log::info("Frame Counter loaded!");
}