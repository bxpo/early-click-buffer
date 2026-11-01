// OP Click Buffer
//
// Every physics tick the mod knows two things: what button state you are
// physically holding ("desired") and what the game currently has ("applied").
// It keeps them equal whenever that would not kill you, and when it would,
// it runs the game's own physics on an invisible copy of your icon to find a
// different input timing that survives:
//
//   * early click   -> held back until the tick it stops being lethal
//   * wrong click   -> dropped if no delay makes it safe
//   * early release -> the button stays held until letting go is safe
//   * missed click  -> if doing nothing would kill you within the rescue
//                      window, the latest press / release / tap that
//                      survives is injected for you
//
// If the search finds nothing that survives, you die like normal.

#include <Geode/Geode.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/RingObject.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace ocb {

// ---------------------------------------------------------------- settings

struct Config {
    bool enabled = true;
    int maxInputDelay = 24;
    bool rescue = true;
    int rescueWindow = 32;
    int lookahead = 96;
    bool assistShipSwing = true;
    double budgetMs = 8.0;
    bool showIndicator = true;
    bool logActions = false;
};

static Config readConfig() {
    auto mod = Mod::get();
    Config c;
    c.enabled = mod->getSettingValue<bool>("enabled");
    c.maxInputDelay = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("max-input-delay"), 0, 120));
    c.rescue = mod->getSettingValue<bool>("rescue");
    c.rescueWindow = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("rescue-window"), 4, 120));
    c.lookahead = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("lookahead"), 32, 240));
    c.lookahead = std::max(c.lookahead, c.rescueWindow + 16);
    c.assistShipSwing = mod->getSettingValue<bool>("assist-ship-swing");
    c.budgetMs = std::clamp(mod->getSettingValue<double>("time-budget"), 1.0, 30.0);
    c.showIndicator = mod->getSettingValue<bool>("show-indicator");
    c.logActions = mod->getSettingValue<bool>("log-actions");
    return c;
}

// ---------------------------------------------------------------- player rhythm

// Running average of how long the player holds a click, in physics ticks
// (240 per second). Used to model the release that follows a press.
struct Rhythm {
    double hold = 8.0;
    long long lastPress = -1;

    void note(bool down, long long tick) {
        if (down) {
            lastPress = tick;
        }
        else if (lastPress >= 0 && tick - lastPress <= 120) {
            hold += (static_cast<double>(std::max<long long>(tick - lastPress, 1)) - hold) * 0.3;
            lastPress = -1;
        }
    }

    void reset() { lastPress = -1; }
    int holdTicks() const { return std::clamp(static_cast<int>(std::lround(hold)), 2, 40); }
};

// ---------------------------------------------------------------- simulation

struct SimInput {
    int tick;
    bool down;
};

struct SimResult {
    bool died = false;
    int deathTick = -1;   // tick (0 = now) on which the ghost died
    bool boundary = false; // reached a portal / trigger it can't model, alive
};

// Objects the ghost collides with exactly like the real player does.
static bool isSimulated(GameObjectType t) {
    switch (t) {
        case GameObjectType::Solid:
        case GameObjectType::Hazard:
        case GameObjectType::AnimatedHazard:
        case GameObjectType::Slope:
        case GameObjectType::YellowJumpPad:
        case GameObjectType::PinkJumpPad:
        case GameObjectType::RedJumpPad:
        case GameObjectType::GravityPad:
        case GameObjectType::SpiderPad:
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::DropRing:
        case GameObjectType::CustomRing:
        case GameObjectType::DashRing:
        case GameObjectType::GravityDashRing:
        case GameObjectType::SpiderOrb:
        case GameObjectType::InverseGravityPortal:
        case GameObjectType::NormalGravityPortal:
        case GameObjectType::GravityTogglePortal:
        case GameObjectType::RegularSizePortal:
        case GameObjectType::MiniSizePortal:
            return true;
        default:
            return false;
    }
}

// Objects whose effects reach past the player (camera, dual, mirror, teleport,
// speed). The ghost stops when it touches one: it got there alive.
static bool isBoundary(GameObjectType t) {
    switch (t) {
        case GameObjectType::ShipPortal:
        case GameObjectType::CubePortal:
        case GameObjectType::BallPortal:
        case GameObjectType::UfoPortal:
        case GameObjectType::WavePortal:
        case GameObjectType::RobotPortal:
        case GameObjectType::SpiderPortal:
        case GameObjectType::SwingPortal:
        case GameObjectType::DualPortal:
        case GameObjectType::SoloPortal:
        case GameObjectType::InverseMirrorPortal:
        case GameObjectType::NormalMirrorPortal:
        case GameObjectType::TeleportPortal:
        case GameObjectType::TeleportOrb:
        case GameObjectType::Modifier:
        case GameObjectType::Special:
            return true;
        default:
            return false;
    }
}

class Simulator {
public:
    static Simulator& get() {
        static Simulator s;
        return s;
    }

    bool running() const { return m_running; }
    bool isGhost(PlayerObject* p) const { return m_running && p && (p == m_ghosts[0] || p == m_ghosts[1]); }

    void markDead() { m_dead = true; }
    void markBoundary() { m_boundary = true; }

    void setTickDelta(float dt) {
        if (dt > 0.0001f && dt < 2.f) m_dt = dt;
    }

    // Builds the collision list the ghost is allowed to touch and remembers the
    // state of every object it might change so it can be put back afterwards.
    void filterObjects(PlayerObject* player, gd::vector<GameObject*>* list, int count, gd::vector<GameObject*>& out) {
        out.clear();
        if (!list) return;
        int limit = std::min<int>(count, static_cast<int>(list->size()));
        cocos2d::CCRect rect = player->getObjectRect();
        for (int i = 0; i < limit; ++i) {
            GameObject* o = (*list)[i];
            if (!o) continue;
            auto t = o->m_objectType;
            if (isSimulated(t)) {
                remember(o);
                out.push_back(o);
            }
            else if (isBoundary(t) && rect.intersectsRect(o->getObjectRect())) {
                m_boundary = true;
            }
        }
    }

    // Runs the ghost(s) for `horizon` ticks. sources[i] is the real player that
    // ghost i stands in for; `inputs` are jump-button events sorted by tick,
    // applied to every ghost (tick 0 = the tick being processed right now).
    SimResult run(PlayLayer* pl, PlayerObject* const* sources, PlayerObject* const* ghosts, int n,
        std::vector<SimInput> const& inputs, int horizon)
    {
        SimResult res;
        if (!pl || n <= 0) return res;

        m_running = true;
        m_dead = false;
        m_boundary = false;
        m_ghosts[0] = ghosts[0];
        m_ghosts[1] = n > 1 ? ghosts[1] : nullptr;
        saveScene(pl);

        PlayerObject* realP1 = pl->m_player1;
        PlayerObject* realP2 = pl->m_player2;
        for (int i = 0; i < n; ++i) {
            clone(ghosts[i], sources[i]);
            if (sources[i] == realP1) pl->m_player1 = ghosts[i];
            else if (sources[i] == realP2) pl->m_player2 = ghosts[i];
        }

        size_t next = 0;
        for (int tick = 0; tick < horizon; ++tick) {
            while (next < inputs.size() && inputs[next].tick <= tick) {
                for (int i = 0; i < n; ++i) {
                    if (inputs[next].down) ghosts[i]->pushButton(PlayerButton::Jump);
                    else ghosts[i]->releaseButton(PlayerButton::Jump);
                }
                ++next;
            }
            for (int i = 0; i < n && !m_dead; ++i) step(pl, ghosts[i]);
            if (m_dead) {
                res.died = true;
                res.deathTick = tick;
                break;
            }
            if (m_boundary) {
                res.boundary = true;
                break;
            }
        }

        pl->m_player1 = realP1;
        pl->m_player2 = realP2;
        loadScene(pl);
        for (int i = 0; i < n; ++i) hideEffects(ghosts[i]);
        m_ghosts[0] = m_ghosts[1] = nullptr;
        m_running = false;
        return res;
    }

    // Particles, trails and fire sprites belong to the parent layer, so hiding the
    // ghost itself is not enough.
    static void hideEffects(PlayerObject* g) {
        if (!g) return;
        g->setVisible(false);
        g->m_playEffects = false;
        cocos2d::CCNode* nodes[] = {
            g->m_playerGroundParticles, g->m_trailingParticles, g->m_shipClickParticles,
            g->m_vehicleGroundParticles, g->m_ufoClickParticles, g->m_robotBurstParticles,
            g->m_dashParticles, g->m_swingBurstParticles1, g->m_swingBurstParticles2,
            g->m_landParticles0, g->m_landParticles1, g->m_regularTrail, g->m_shipStreak,
            g->m_waveTrail, g->m_ghostTrail, g->m_dashFireSprite, g->m_swingFireMiddle,
            g->m_swingFireBottom, g->m_swingFireTop, g->m_robotFire,
        };
        for (auto node : nodes) {
            if (node) node->setVisible(false);
        }
        cocos2d::CCParticleSystemQuad* particles[] = {
            g->m_playerGroundParticles, g->m_trailingParticles, g->m_shipClickParticles,
            g->m_vehicleGroundParticles, g->m_ufoClickParticles, g->m_robotBurstParticles,
            g->m_dashParticles, g->m_swingBurstParticles1, g->m_swingBurstParticles2,
            g->m_landParticles0, g->m_landParticles1,
        };
        for (auto ps : particles) {
            if (ps) ps->stopSystem();
        }
    }

private:
    struct SavedObject {
        GameObject* obj;
        bool activated;
        bool ringPowered;
        bool enhanced;
        bool eActivated;
        bool eByP1;
        bool eByP2;
        bool eAnim;
    };

    void remember(GameObject* o) {
        if (!m_seen.insert(o).second) return;
        SavedObject s{ o, o->m_isActivated, o->m_isRingPoweredOn, false, false, false, false, false };
        if (auto e = typeinfo_cast<EnhancedGameObject*>(o)) {
            s.enhanced = true;
            s.eActivated = e->m_activated;
            s.eByP1 = e->m_activatedByPlayer1;
            s.eByP2 = e->m_activatedByPlayer2;
            s.eAnim = e->m_animationTriggered;
        }
        m_saved.push_back(s);
    }

    void saveScene(PlayLayer* pl) {
        m_saved.clear();
        m_seen.clear();
        auto& gs = pl->m_gameState;
        m_portalY = gs.m_portalY;
        m_lastModeHeight = gs.m_lastModeHeight;
        m_gravityRelated = gs.m_gravityRelated;
        m_portal1 = gs.m_lastActivatedPortal1;
        m_portal2 = gs.m_lastActivatedPortal2;
        m_activatedIDs = gs.m_activatedObjectIDs;
    }

    void loadScene(PlayLayer* pl) {
        for (auto const& s : m_saved) {
            s.obj->m_isActivated = s.activated;
            s.obj->m_isRingPoweredOn = s.ringPowered;
            if (s.enhanced) {
                auto e = static_cast<EnhancedGameObject*>(s.obj);
                e->m_activated = s.eActivated;
                e->m_activatedByPlayer1 = s.eByP1;
                e->m_activatedByPlayer2 = s.eByP2;
                e->m_animationTriggered = s.eAnim;
            }
        }
        m_saved.clear();
        m_seen.clear();
        auto& gs = pl->m_gameState;
        gs.m_portalY = m_portalY;
        gs.m_lastModeHeight = m_lastModeHeight;
        gs.m_gravityRelated = m_gravityRelated;
        gs.m_lastActivatedPortal1 = m_portal1;
        gs.m_lastActivatedPortal2 = m_portal2;
        gs.m_activatedObjectIDs = m_activatedIDs;
    }

    static void clone(PlayerObject* dst, PlayerObject* src) {
        // Gamemode, size, gravity and speed, the way GD copies them for dual spawns.
        dst->copyAttributes(src);
        // Every other plain-data physics field (generated from the bindings).
#include "PlayerCopy.inc"
        dst->m_isDead = false;
        dst->setPosition(src->getPosition());
        dst->m_positionX = src->m_positionX;
        dst->m_positionY = src->m_positionY;
        dst->setRotation(src->getRotation());
        if (dst->m_touchingRings) {
            dst->m_touchingRings->removeAllObjects();
            if (src->m_touchingRings) dst->m_touchingRings->addObjectsFromArray(src->m_touchingRings);
        }
        hideEffects(dst);
    }

    // One physics tick for one player, in the order the game runs it.
    void step(PlayLayer* pl, PlayerObject* p) {
        float dt = m_dt;
        p->m_totalTime += dt / 60.0;
        p->resetTouchedRings(false);
        p->m_collisionLogTop->removeAllObjects();
        p->m_collisionLogBottom->removeAllObjects();
        p->m_collisionLogLeft->removeAllObjects();
        p->m_collisionLogRight->removeAllObjects();
        p->m_unk50C = p->m_lastCollisionBottom;
        p->m_unk510 = p->m_lastCollisionTop;
        p->m_lastCollisionBottom = -1;
        p->m_lastCollisionTop = -1;
        p->m_lastCollisionLeft = -1;
        p->m_lastCollisionRight = -1;

        p->updateInternalActions(dt);
        p->update(dt);
        if (m_dead || m_boundary) return;
        pl->checkCollisions(p, dt, false);

        // Previous-tick rotation and position, read by next tick's collisions.
        p->m_unkUnused3 = p->getRotation();
        p->updateRotation(dt);
        p->m_shipRotation = p->getPosition();
    }

    bool m_running = false;
    bool m_dead = false;
    bool m_boundary = false;
    float m_dt = 0.25f;
    PlayerObject* m_ghosts[2] = { nullptr, nullptr };

    std::vector<SavedObject> m_saved;
    std::unordered_set<GameObject*> m_seen;
    float m_portalY = 0.f;
    float m_lastModeHeight = 0.f;
    bool m_gravityRelated = false;
    GameObject* m_portal1 = nullptr;
    GameObject* m_portal2 = nullptr;
    gd::map<std::pair<int, int>, int> m_activatedIDs;
};

static bool g_injecting = false;

} // namespace ocb

using namespace ocb;

// ---------------------------------------------------------------- PlayLayer

class $modify(AssistPlayLayer, PlayLayer) {
    struct Fields {
        Ref<PlayerObject> ghost1;
        Ref<PlayerObject> ghost2;
        Ref<CCLabelBMFont> label;
        Config cfg;

        long long tick = 0;        // physics ticks since the attempt started
        bool desired = false;      // what the player is physically holding
        bool applied = false;      // what the game currently has
        long long desiredSince = 0;
        long long nextCheck = 0;   // next tick to run the "do nothing" death check
        Rhythm rhythm;

        // A rescue the mod decided on: toggle at t1, optionally toggle back at t2.
        struct Plan {
            bool active = false;
            long long t1 = 0;
            bool s1 = false;
            bool fired1 = false;
            bool has2 = false;
            long long t2 = 0;
            bool s2 = false;
        } plan;

        int delayed = 0;  // user inputs that were held back and later applied
        int dropped = 0;  // user inputs that never became safe
        int rescues = 0;  // inputs the mod injected on its own
        float flash = 0.f;
        std::string flashText;
    };

    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::First);
    }

    PlayerObject* makeGhost() {
        auto g = PlayerObject::create(1, 1, this, m_objectLayer, true);
        if (!g) return nullptr;
        g->setVisible(false);
        m_objectLayer->addChild(g);
        Simulator::hideEffects(g);
        return g;
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto f = m_fields.self();
        f->cfg = readConfig();
        f->ghost1 = makeGhost();
        f->ghost2 = makeGhost();

        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setAnchorPoint({ 0.f, 1.f });
        label->setScale(0.3f);
        label->setOpacity(140);
        auto win = CCDirector::get()->getWinSize();
        label->setPosition({ 6.f, win.height - 4.f });
        label->setZOrder(1000);
        label->setID("assist-indicator"_spr);
        this->addChild(label);
        f->label = label;
        refreshLabel();
        return true;
    }

    void refreshLabel() {
        auto f = m_fields.self();
        if (!f->label) return;
        f->label->setVisible(f->cfg.showIndicator);
        if (!f->cfg.showIndicator) return;
        std::string text;
        if (!f->cfg.enabled) text = "Assist: off";
        else text = fmt::format("Assist: on  delayed {}  dropped {}  rescued {}", f->delayed, f->dropped, f->rescues);
        if (f->flash > 0.f) text += "  |  " + f->flashText;
        f->label->setString(text.c_str());
    }

    void flash(std::string text) {
        auto f = m_fields.self();
        f->flashText = std::move(text);
        f->flash = 240.f;
        refreshLabel();
        if (f->cfg.logActions) log::info("[tick {}] {}", f->tick, f->flashText);
    }

    void resetState() {
        auto f = m_fields.self();
        f->tick = 0;
        f->desired = false;
        f->applied = false;
        f->desiredSince = 0;
        f->nextCheck = 0;
        f->plan = {};
        f->rhythm.reset();
        f->delayed = f->dropped = f->rescues = 0;
        f->flash = 0.f;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        resetState();
        m_fields->cfg = readConfig();
        refreshLabel();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& sim = Simulator::get();
        if (sim.running()) {
            if (sim.isGhost(player)) sim.markDead();
            return;
        }
        PlayLayer::destroyPlayer(player, object);
        if (player && player->m_isDead) {
            auto f = m_fields.self();
            f->plan = {};
            f->applied = f->desired;
        }
    }

    void playEndAnimationToPos(cocos2d::CCPoint pos) {
        if (Simulator::get().running()) {
            Simulator::get().markBoundary();
            return;
        }
        PlayLayer::playEndAnimationToPos(pos);
    }

    void playGravityEffect(bool flip) {
        if (Simulator::get().running()) return;
        PlayLayer::playGravityEffect(flip);
    }

    // ------------------------------------------------------------ helpers

    bool isTwoPlayer() {
        return m_levelSettings && m_levelSettings->m_twoPlayerMode;
    }

    bool dualMerged() {
        return m_gameState.m_isDualMode && m_player2 && !isTwoPlayer();
    }

    bool assistActive() {
        auto f = m_fields.self();
        if (!f->cfg.enabled || !f->ghost1 || m_isPlatformer || isTwoPlayer()) return false;
        if (!m_player1 || m_player1->m_isDead || m_hasCompletedLevel) return false;
        if (!f->cfg.assistShipSwing) {
            if (m_player1->m_isShip || m_player1->m_isSwing) return false;
            if (dualMerged() && (m_player2->m_isShip || m_player2->m_isSwing)) return false;
        }
        return true;
    }

    // Simulates the given button events on top of the game's current state.
    SimResult simulate(std::vector<SimInput> const& inputs, int horizon) {
        auto f = m_fields.self();
        PlayerObject* sources[2] = { m_player1, nullptr };
        PlayerObject* ghosts[2] = { f->ghost1, f->ghost2 };
        int n = 1;
        if (dualMerged() && f->ghost2) {
            sources[1] = m_player2;
            n = 2;
        }
        return Simulator::get().run(this, sources, ghosts, n, inputs, horizon);
    }

    // "Switch to `state` now": the future if the player's own input is applied.
    // A press is modelled as a tap of the player's usual length unless they have
    // clearly been holding for longer than that already.
    SimResult simulateSwitch(bool state, long long heldFor) {
        auto f = m_fields.self();
        std::vector<SimInput> in{ { 0, state } };
        int hold = f->rhythm.holdTicks();
        if (state && heldFor < hold) in.push_back({ hold - static_cast<int>(heldFor), false });
        return simulate(in, f->cfg.lookahead);
    }

    void setApplied(bool state) {
        auto f = m_fields.self();
        if (f->applied == state) return;
        f->applied = state;
        g_injecting = true;
        this->handleButton(state, static_cast<int>(PlayerButton::Jump), true);
        g_injecting = false;
    }

    // ------------------------------------------------------------ rescue search

    // Doing nothing dies at `deathTick`. Finds the latest input change that
    // keeps the ghost alive, or failing that the one that dies latest.
    bool planRescue(int deathTick) {
        auto f = m_fields.self();
        auto const& cfg = f->cfg;
        auto started = std::chrono::steady_clock::now();
        auto overBudget = [&] {
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count();
            return us > static_cast<long long>(cfg.budgetMs * 1000.0);
        };

        constexpr int kLead = 2; // act at least this many ticks before the death
        int latest = std::min(deathTick - 1 - kLead, cfg.rescueWindow);
        if (latest < 0) latest = 0;

        bool toggled = !f->applied;
        int hold = f->rhythm.holdTicks();
        int backs[] = { hold, 3, 6, 12, 24, 48 };

        Fields::Plan best;
        int bestDeath = deathTick;
        bool found = false;

        auto consider = [&](int d, int back) -> bool {
            std::vector<SimInput> in{ { d, toggled } };
            if (back > 0) in.push_back({ d + back, !toggled });
            SimResult r = simulate(in, cfg.lookahead);
            if (!r.died) {
                best = {};
                best.active = true;
                best.t1 = f->tick + d;
                best.s1 = toggled;
                if (back > 0) {
                    best.has2 = true;
                    best.t2 = f->tick + d + back;
                    best.s2 = !toggled;
                }
                found = true;
                return true;
            }
            if (r.deathTick > bestDeath) {
                bestDeath = r.deathTick;
                best = {};
                best.active = true;
                best.t1 = f->tick + d;
                best.s1 = toggled;
                if (back > 0) {
                    best.has2 = true;
                    best.t2 = f->tick + d + back;
                    best.s2 = !toggled;
                }
            }
            return false;
        };

        // Latest first, so the player keeps control for as long as possible.
        for (int d = latest; d >= 0 && !overBudget(); --d) {
            if (consider(d, 0)) break;
            bool done = false;
            for (int back : backs) {
                if (back <= 0 || overBudget()) continue;
                if (consider(d, back)) {
                    done = true;
                    break;
                }
            }
            if (done) break;
        }

        if (!best.active) return false;
        f->plan = best;
        f->rescues++;
        int d = static_cast<int>(best.t1 - f->tick);
        flash(fmt::format("rescue: {} in {}t{}{}", best.s1 ? "press" : "release", d,
            best.has2 ? fmt::format(", back after {}t", best.t2 - best.t1) : "",
            found ? "" : " (partial)"));
        return true;
    }

    // ------------------------------------------------------------ per tick

    // Runs at the start of every physics tick, before the game processes inputs.
    void onTick() {
        auto f = m_fields.self();
        f->tick++;
        if (f->flash > 0.f) {
            f->flash -= 1.f;
            if (f->flash <= 0.f) refreshLabel();
        }
        if (!assistActive()) {
            if (m_player1 && m_player1->m_isDead) f->applied = f->desired;
            else if (f->applied != f->desired) setApplied(f->desired);
            f->plan = {};
            return;
        }
        auto const& cfg = f->cfg;

        // 1. A planned rescue fires on its ticks.
        if (f->plan.active) {
            auto& p = f->plan;
            bool firedNow = false;
            if (!p.fired1 && f->tick >= p.t1) {
                setApplied(p.s1);
                p.fired1 = true;
                firedNow = true;
            }
            if (p.fired1 && (!p.has2 || f->tick >= p.t2)) {
                if (p.has2) setApplied(p.s2);
                p = {};
                f->nextCheck = f->tick + 1;
            }
            if (firedNow || f->plan.active) return;
        }

        // 2. Bring the game back to what the player is holding, as soon as that is safe.
        if (f->applied != f->desired) {
            long long age = f->tick - f->desiredSince;
            // Retry every tick inside the delay window, then back off to save time.
            bool test = age <= cfg.maxInputDelay || age % 4 == 0;
            if (test) {
                SimResult r = simulateSwitch(f->desired, age);
                if (!r.died) {
                    setApplied(f->desired);
                    if (age > 0) {
                        f->delayed++;
                        flash(fmt::format("{} delayed {}t", f->desired ? "press" : "release", age));
                    }
                    f->nextCheck = f->tick + 1;
                }
            }
            if (age == cfg.maxInputDelay + 1 && f->applied != f->desired) {
                flash(fmt::format("{} still lethal after {}t, waiting", f->desired ? "press" : "release", age - 1));
            }
            if (f->applied != f->desired) return;
        }

        // 3. Would doing nothing kill you? Then find a way out.
        if (!cfg.rescue || f->tick < f->nextCheck) return;
        SimResult r = simulate({}, cfg.lookahead);
        if (!r.died) {
            // Safe for the whole lookahead: no need to look again until a death
            // could sneak inside the rescue window.
            f->nextCheck = f->tick + std::clamp(cfg.lookahead - cfg.rescueWindow - 8, 2, 24);
            return;
        }
        if (r.deathTick > cfg.rescueWindow) {
            f->nextCheck = f->tick + std::max(1, r.deathTick - cfg.rescueWindow);
            return;
        }
        if (!planRescue(r.deathTick)) f->nextCheck = f->tick + 2;
    }

    // ------------------------------------------------------------ player input

    // Returns true if the input must not reach the game right now.
    bool interceptInput(bool down, int button, bool isP1) {
        auto f = m_fields.self();
        if (button != static_cast<int>(PlayerButton::Jump) || !isP1) return false;
        f->cfg = readConfig(); // settings can change from the pause menu
        if (!assistActive()) {
            f->desired = f->applied = down;
            return false;
        }

        if (f->applied != f->desired && f->desired != down) {
            // The previous input never became safe before the player moved on.
            f->dropped++;
            flash(fmt::format("{} dropped (lethal for {}t)", f->desired ? "press" : "release", f->tick - f->desiredSince));
        }
        f->rhythm.note(down, f->tick);
        f->desired = down;
        f->desiredSince = f->tick;
        f->plan = {};
        f->nextCheck = f->tick + 1;

        if (f->applied == down) return true; // the mod already did this for you

        SimResult r = simulateSwitch(down, 0);
        if (!r.died) {
            f->applied = down;
            return false;
        }
        return true; // held back; onTick keeps retrying until it is safe
    }
};

// ---------------------------------------------------------------- GJBaseGameLayer

class $modify(AssistBaseLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::First);
    }

    AssistPlayLayer* asAssisted() {
        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) return nullptr;
        return static_cast<AssistPlayLayer*>(pl);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!g_injecting && !Simulator::get().running()) {
            if (auto pl = asAssisted()) {
                if (pl->interceptInput(down, button, isPlayer1)) return;
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!Simulator::get().running()) {
            if (auto pl = asAssisted()) pl->onTick();
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int count, float dt) {
        auto& sim = Simulator::get();
        if (!sim.running() || !objects) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
            return;
        }
        static gd::vector<GameObject*> kept;
        sim.filterObjects(player, objects, count, kept);
        GJBaseGameLayer::collisionCheckObjects(player, &kept, static_cast<int>(kept.size()), dt);
    }

    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        GJBaseGameLayer::flipGravity(player, flip, noEffects || Simulator::get().running());
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (Simulator::get().running()) return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        if (Simulator::get().running()) {
            Simulator::get().markBoundary();
            return;
        }
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }

    void toggleDualMode(GameObject* object, bool dual, PlayerObject* player, bool noEffects) {
        if (Simulator::get().running()) {
            Simulator::get().markBoundary();
            return;
        }
        GJBaseGameLayer::toggleDualMode(object, dual, player, noEffects);
    }

    void toggleFlipped(bool flip, bool noEffects) {
        if (Simulator::get().running()) {
            Simulator::get().markBoundary();
            return;
        }
        GJBaseGameLayer::toggleFlipped(flip, noEffects);
    }

    void teleportPlayer(TeleportPortalObject* object, PlayerObject* player) {
        if (Simulator::get().running()) {
            Simulator::get().markBoundary();
            return;
        }
        GJBaseGameLayer::teleportPlayer(object, player);
    }
};

// ---------------------------------------------------------------- PlayerObject

class $modify(AssistPlayerObject, PlayerObject) {
    void update(float dt) {
        auto& sim = Simulator::get();
        if (!sim.running()) {
            if (auto pl = PlayLayer::get(); pl && this == pl->m_player1) sim.setTickDelta(dt);
        }
        PlayerObject::update(dt);
    }

    void incrementJumps() {
        if (Simulator::get().running()) return;
        PlayerObject::incrementJumps();
    }

    void spawnCircle() {
        if (Simulator::get().running()) return;
        PlayerObject::spawnCircle();
    }

    void spawnDualCircle() {
        if (Simulator::get().running()) return;
        PlayerObject::spawnDualCircle();
    }

    void spawnPortalCircle(cocos2d::ccColor3B color, float startRadius) {
        if (Simulator::get().running()) return;
        PlayerObject::spawnPortalCircle(color, startRadius);
    }

    void spawnScaleCircle() {
        if (Simulator::get().running()) return;
        PlayerObject::spawnScaleCircle();
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        if (Simulator::get().running()) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void playBumpEffect(int objectType, GameObject* player) {
        if (Simulator::get().running()) return;
        PlayerObject::playBumpEffect(objectType, player);
    }
};

// ---------------------------------------------------------------- cosmetic side effects

class $modify(AssistEffectObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (Simulator::get().running()) return;
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(AssistGameObject, GameObject) {
    void playShineEffect() {
        if (Simulator::get().running()) return;
        GameObject::playShineEffect();
    }
};

class $modify(AssistRingObject, RingObject) {
    void spawnCircle() {
        if (Simulator::get().running()) return;
        RingObject::spawnCircle();
    }
};

class $modify(AssistHardStreak, HardStreak) {
    void addPoint(cocos2d::CCPoint point) {
        if (Simulator::get().running()) return;
        HardStreak::addPoint(point);
    }
};
