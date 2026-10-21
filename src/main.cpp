#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/file.hpp>
#include <gdr/gdr.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <vector>

using namespace geode::prelude;

namespace {

    constexpr int TAB_COUNT = 9;
    constexpr int SLOT_COUNT = 9;
    constexpr double GAME_TPS = 240.0;

    // ------------------------------------------------------------------
    // Replay veri yapilari
    // ------------------------------------------------------------------
    struct MacroReplay : gdr::Replay<MacroReplay, gdr::Input> {
        MacroReplay() : Replay("MacroSwitcher", 1) {}
    };

    struct InputEvent {
        uint64_t frame = 0;
        int button = 1;
        bool down = false;
        bool player2 = false;
    };

    struct LoadedMacro {
        std::vector<InputEvent> inputs;
        double fps = GAME_TPS;
        bool valid = false;
    };

    struct State {
        int activeTab = 0;
        int activeSlot = -1;   // -1 = Legit Mode
        bool starHeld = false;
        bool injecting = false;
        size_t nextIndex = 0;
        int64_t lastStep = -1;
        std::map<int, LoadedMacro> cache;
    };

    State& state() {
        static State s;
        return s;
    }

    // ------------------------------------------------------------------
    // Dosya yolu ve kayit yardimcilari
    // ------------------------------------------------------------------
    std::filesystem::path macroDir() {
        return Mod::get()->getSaveDir() / "macros";
    }

    std::filesystem::path macroPath(int tab, int slot) {
        return macroDir() / fmt::format("t{}_s{}.macro", tab + 1, slot + 1);
    }

    std::string slotKey(int tab, int slot) {
        return fmt::format("name-t{}-s{}", tab + 1, slot + 1);
    }

    std::string tabKey(int tab) {
        return fmt::format("tabname-{}", tab + 1);
    }

    std::string savedTabName(int tab) {
        return Mod::get()->getSavedValue<std::string>(tabKey(tab), "");
    }

    std::string savedSlotLabel(int tab, int slot) {
        if (!std::filesystem::exists(macroPath(tab, slot))) return "";
        return Mod::get()->getSavedValue<std::string>(slotKey(tab, slot), "");
    }

    bool readFileBytes(std::filesystem::path const& path, std::vector<uint8_t>& out) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return false;
        out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return !out.empty();
    }

    // Kutuphane surumune gore importData dogrudan nesne ya da Result donebilir.
    template <class R>
    bool extractReplay(R&& result, MacroReplay& out) {
        if constexpr (requires { result.isOk(); }) {
            if (!result.isOk()) return false;
            out = std::move(result.unwrap());
            return true;
        }
        else if constexpr (requires { result.has_value(); }) {
            if (!result.has_value()) return false;
            out = std::move(result.value());
            return true;
        }
        else {
            out = std::move(result);
            return true;
        }
    }

    LoadedMacro* getMacro(int tab, int slot) {
        if (tab < 0 || tab >= TAB_COUNT || slot < 0 || slot >= SLOT_COUNT) return nullptr;
        auto& s = state();
        int key = tab * 10 + slot;
        auto it = s.cache.find(key);
        if (it != s.cache.end()) return &it->second;

        LoadedMacro macro;
        std::vector<uint8_t> bytes;
        if (readFileBytes(macroPath(tab, slot), bytes)) {
            try {
                MacroReplay replay;
                auto imported = MacroReplay::importData(bytes);
                if (extractReplay(std::move(imported), replay)) {
                    for (auto const& in : replay.inputs) {
                        InputEvent ev;
                        ev.frame = static_cast<uint64_t>(in.frame);
                        ev.button = static_cast<int>(in.button);
                        ev.down = in.down;
                        ev.player2 = in.player2;
                        macro.inputs.push_back(ev);
                    }
                    if (replay.framerate > 0) macro.fps = static_cast<double>(replay.framerate);
                    std::stable_sort(
                        macro.inputs.begin(), macro.inputs.end(),
                        [](InputEvent const& a, InputEvent const& b) { return a.frame < b.frame; }
                    );
                    macro.valid = !macro.inputs.empty();
                }
            }
            catch (...) {
                macro.valid = false;
            }
        }
        s.cache[key] = std::move(macro);
        return &s.cache[key];
    }

    LoadedMacro* activeMacro() {
        auto& s = state();
        if (s.activeSlot < 0) return nullptr;
        auto* macro = getMacro(s.activeTab, s.activeSlot);
        if (!macro || !macro->valid) return nullptr;
        return macro;
    }

    bool isPlayLayerInstance(GJBaseGameLayer* layer) {
        auto* pl = PlayLayer::get();
        if (!pl || !layer) return false;
        return static_cast<GJBaseGameLayer*>(pl) == layer;
    }

    void releaseButtons() {
        auto* pl = PlayLayer::get();
        if (!pl) return;
        auto& s = state();
        s.injecting = true;
        static_cast<GJBaseGameLayer*>(pl)->handleButton(false, 1, true);
        static_cast<GJBaseGameLayer*>(pl)->handleButton(false, 1, false);
        s.injecting = false;
    }

    void resetPlayback() {
        auto& s = state();
        s.nextIndex = 0;
        s.lastStep = -1;
    }

    void setLegitMode() {
        auto& s = state();
        if (s.activeSlot < 0) return;
        releaseButtons();
        s.activeSlot = -1;
        resetPlayback();
    }

    void activateSlot(int slot) {
        auto& s = state();
        releaseButtons();
        s.activeSlot = slot;
        resetPlayback();
        if (auto* pl = PlayLayer::get()) {
            pl->resetLevel();
        }
    }

    void setActiveTab(int tab) {
        auto& s = state();
        if (tab < 0 || tab >= TAB_COUNT) return;
        releaseButtons();
        s.activeTab = tab;
        s.activeSlot = -1;
        resetPlayback();
    }

    int digitOf(enumKeyCodes key) {
        switch (key) {
            case KEY_Zero: case KEY_NumPad0: return 0;
            case KEY_One: case KEY_NumPad1: return 1;
            case KEY_Two: case KEY_NumPad2: return 2;
            case KEY_Three: case KEY_NumPad3: return 3;
            case KEY_Four: case KEY_NumPad4: return 4;
            case KEY_Five: case KEY_NumPad5: return 5;
            case KEY_Six: case KEY_NumPad6: return 6;
            case KEY_Seven: case KEY_NumPad7: return 7;
            case KEY_Eight: case KEY_NumPad8: return 8;
            case KEY_Nine: case KEY_NumPad9: return 9;
            default: return -1;
        }
    }

    void fitLabel(CCLabelBMFont* label, float maxWidth, float baseScale) {
        float width = label->getContentSize().width;
        float scale = baseScale;
        if (width > 0.f && width * scale > maxWidth) scale = maxWidth / width;
        label->setScale(scale);
    }

} // namespace

// ----------------------------------------------------------------------
// Menu (Popup)
// ----------------------------------------------------------------------
class MacroMenu;
static MacroMenu* s_menu = nullptr;

class MacroMenu : public Popup<> {
protected:
    struct Pending {
        std::array<std::string, TAB_COUNT> tabNames;
        std::array<std::array<std::filesystem::path, SLOT_COUNT>, TAB_COUNT> src;
        std::array<std::array<std::string, SLOT_COUNT>, TAB_COUNT> label;
    };

    Pending m_pending;
    std::vector<CCNode*> m_dynamic;
    TextInput* m_nameInput = nullptr;
    EventListener<Task<Result<std::filesystem::path>>> m_pickListener;
    int m_pickTab = 0;
    int m_pickSlot = 0;

    bool setup() override {
        this->setTitle("Macro Switcher");

        for (int t = 0; t < TAB_COUNT; t++) {
            m_pending.tabNames[t] = savedTabName(t);
            for (int s = 0; s < SLOT_COUNT; s++) {
                m_pending.label[t][s] = savedSlotLabel(t, s);
            }
        }

        auto* nameLabel = CCLabelBMFont::create("Sekme adi:", "goldFont.fnt");
        nameLabel->setScale(0.5f);
        m_mainLayer->addChildAtPosition(nameLabel, Anchor::Center, ccp(-120.f, 45.f));

        m_nameInput = TextInput::create(160.f, "Sekme adi", "bigFont.fnt");
        m_nameInput->setMaxCharCount(20);
        m_buttonMenu->addChildAtPosition(m_nameInput, Anchor::Center, ccp(0.f, 45.f));
        m_nameInput->setPosition(m_nameInput->getPosition());

        auto* applySpr = ButtonSprite::create("Apply", 90, true, "bigFont.fnt", "GJ_button_02.png", 30.f, 0.7f);
        auto* applyBtn = CCMenuItemSpriteExtra::create(applySpr, this, menu_selector(MacroMenu::onApply));
        m_buttonMenu->addChildAtPosition(applyBtn, Anchor::Center, ccp(0.f, -100.f));

        this->rebuild();
        return true;
    }

    void clearDynamic() {
        for (auto* node : m_dynamic) {
            node->removeFromParent();
        }
        m_dynamic.clear();
    }

    void rebuild() {
        this->clearDynamic();
        int tab = state().activeTab;

        m_nameInput->setString(m_pending.tabNames[tab]);

        for (int i = 0; i < TAB_COUNT; i++) {
            bool selected = (i == tab);
            auto* spr = ButtonSprite::create(
                std::to_string(i + 1).c_str(), 28, true, "bigFont.fnt",
                selected ? "GJ_button_02.png" : "GJ_button_01.png", 28.f, 0.7f
            );
            auto* btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(MacroMenu::onTab));
            btn->setTag(i);
            m_buttonMenu->addChildAtPosition(btn, Anchor::Center, ccp((i - 4) * 46.f, 78.f));
            m_dynamic.push_back(btn);
        }

        for (int i = 0; i < SLOT_COUNT; i++) {
            float x = (i - 4) * 46.f;

            auto* num = CCLabelBMFont::create(std::to_string(i + 1).c_str(), "bigFont.fnt");
            num->setScale(0.6f);
            m_mainLayer->addChildAtPosition(num, Anchor::Center, ccp(x, 12.f));
            m_dynamic.push_back(num);

            auto* plusSpr = ButtonSprite::create("+", 36, true, "bigFont.fnt", "GJ_button_01.png", 30.f, 0.8f);
            auto* plusBtn = CCMenuItemSpriteExtra::create(plusSpr, this, menu_selector(MacroMenu::onPlus));
            plusBtn->setTag(i);
            m_buttonMenu->addChildAtPosition(plusBtn, Anchor::Center, ccp(x, -18.f));
            m_dynamic.push_back(plusBtn);

            std::string text = m_pending.label[tab][i];
            if (text.empty()) text = "-";
            auto* fileLabel = CCLabelBMFont::create(text.c_str(), "chatFont.fnt");
            fitLabel(fileLabel, 42.f, 0.5f);
            m_mainLayer->addChildAtPosition(fileLabel, Anchor::Center, ccp(x, -48.f));
            m_dynamic.push_back(fileLabel);
        }
    }

    void storeNameInput() {
        if (!m_nameInput) return;
        m_pending.tabNames[state().activeTab] = m_nameInput->getString();
    }

    void onTab(CCObject* sender) {
        this->storeNameInput();
        int tab = static_cast<CCNode*>(sender)->getTag();
        setActiveTab(tab);
        this->rebuild();
    }

    void onPlus(CCObject* sender) {
        this->storeNameInput();
        m_pickSlot = static_cast<CCNode*>(sender)->getTag();
        m_pickTab = state().activeTab;

        file::FilePickOptions options;
        file::FilePickOptions::Filter filter;
        filter.description = "GD Replay (.gdr, .gdr2)";
        filter.files = {"*.gdr", "*.gdr2"};
        options.filters.push_back(filter);

        m_pickListener.bind(this, &MacroMenu::onPicked);
        m_pickListener.setFilter(file::pick(file::PickMode::OpenFile, options));
    }

    void onPicked(Task<Result<std::filesystem::path>>::Event* event) {
        auto* result = event->getValue();
        if (!result || !result->isOk()) return;

        std::filesystem::path path = result->unwrap();
        if (path.empty()) return;

        m_pending.src[m_pickTab][m_pickSlot] = path;
        m_pending.label[m_pickTab][m_pickSlot] = geode::utils::string::pathToString(path.filename());
        if (m_pickTab == state().activeTab) this->rebuild();
    }

    void onApply(CCObject*) {
        this->storeNameInput();

        std::error_code ec;
        std::filesystem::create_directories(macroDir(), ec);

        for (int t = 0; t < TAB_COUNT; t++) {
            Mod::get()->setSavedValue<std::string>(tabKey(t), m_pending.tabNames[t]);
            for (int s = 0; s < SLOT_COUNT; s++) {
                if (m_pending.src[t][s].empty()) continue;
                std::error_code copyEc;
                std::filesystem::copy_file(
                    m_pending.src[t][s], macroPath(t, s),
                    std::filesystem::copy_options::overwrite_existing, copyEc
                );
                if (!copyEc) {
                    Mod::get()->setSavedValue<std::string>(slotKey(t, s), m_pending.label[t][s]);
                }
                m_pending.src[t][s].clear();
            }
        }

        state().cache.clear();
        resetPlayback();
        this->closeMenu();
    }

    void onClose(CCObject* sender) override {
        if (s_menu == this) s_menu = nullptr;
        Popup<>::onClose(sender);
    }

public:
    ~MacroMenu() override {
        if (s_menu == this) s_menu = nullptr;
    }

    void closeMenu() {
        this->onClose(nullptr);
    }

    static MacroMenu* create() {
        auto* ret = new MacroMenu();
        if (ret->initAnchored(460.f, 260.f)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

static void toggleMenu() {
    if (s_menu) {
        s_menu->closeMenu();
        return;
    }
    if (auto* menu = MacroMenu::create()) {
        s_menu = menu;
        menu->show();
    }
}

// ----------------------------------------------------------------------
// Klavye hook'u
// ----------------------------------------------------------------------
class $modify(MSKeyboardDispatcher, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
        auto& s = state();

        if (key == KEY_Multiply) {
            s.starHeld = down;
        }

        if (down && !repeat) {
            if (key == KEY_RightShift) {
                toggleMenu();
            }
            else if (!s_menu) {
                int digit = digitOf(key);
                if (digit >= 0) {
                    if (s.starHeld) {
                        if (digit >= 1) setActiveTab(digit - 1);
                    }
                    else if (PlayLayer::get()) {
                        if (digit == 0) setLegitMode();
                        else activateSlot(digit - 1);
                    }
                }
            }
        }

        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
    }
};

// ----------------------------------------------------------------------
// Oyun hook'lari (macro oynatma)
// ----------------------------------------------------------------------
class $modify(MSBaseGameLayer, GJBaseGameLayer) {
    void processCommands(float dt) {
        auto& s = state();
        if (isPlayLayerInstance(this)) {
            if (auto* macro = activeMacro()) {
                int64_t step = static_cast<int64_t>(m_gameState.m_currentProgress);

                if (step != s.lastStep) {
                    uint64_t target = static_cast<uint64_t>(
                        static_cast<double>(step) * macro->fps / GAME_TPS
                    );

                    if (s.lastStep < 0 || step < s.lastStep) {
                        auto it = std::lower_bound(
                            macro->inputs.begin(), macro->inputs.end(), target,
                            [](InputEvent const& ev, uint64_t value) { return ev.frame < value; }
                        );
                        s.nextIndex = static_cast<size_t>(std::distance(macro->inputs.begin(), it));
                    }

                    s.injecting = true;
                    while (s.nextIndex < macro->inputs.size() && macro->inputs[s.nextIndex].frame <= target) {
                        auto const& ev = macro->inputs[s.nextIndex];
                        GJBaseGameLayer::handleButton(ev.down, ev.button, !ev.player2);
                        s.nextIndex++;
                    }
                    s.injecting = false;
                    s.lastStep = step;
                }
            }
        }
        GJBaseGameLayer::processCommands(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& s = state();
        if (!s.injecting && isPlayLayerInstance(this) && activeMacro()) {
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(MSPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        resetPlayback();
    }
};
