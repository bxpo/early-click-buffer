// Early Click Buffer
//
// When you press (or, in wave, release) jump, the mod runs the next few physics
// ticks on an invisible copy of your player. If acting right now would kill you
// but acting a tick or a few later would not, your input is held for exactly
// that many ticks and then handed to the game. If no small delay saves you, the
// input goes through untouched.

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
#include <deque>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace ecb {

// ---------------------------------------------------------------- settings

struct Config {
    bool enabled = true;
    int maxDelay = 6;
    bool bufferReleases = true;
    int lookahead = 90;
    double budgetMs = 6.0;
    bool showIndicator = true;
    bool logSaves = false;
};

static Config readConfig() {
    auto mod = Mod::get();
    Config c;
    c.enabled = mod->getSettingValue<bool>("enabled");
    c.maxDelay = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("max-delay"), 1, 24));
    c.bufferReleases = mod->getSettingValue<bool>("buffer-wave-releases");
    c.lookahead = static_cast<int>(std::clamp<int64_t>(mod->getSettingValue<int64_t>("lookahead"), 30, 240));
    c.budgetMs = std::clamp(mod->getSettingValue<double>("time-budget"), 1.0, 16.0);
    c.showIndicator = mod->getSettingValue<bool>("show-indicator");
    c.logSaves = mod->getSettingValue<bool>("log-saves");
    return c;
}

// ---------------------------------------------------------------- gamemodes

enum class Mode { Cube, Ship, Ball, Ufo, Wave, Robot, Spider, Swing };

static Mode modeOf(PlayerObject const* p) {
    if (p->m_isShip) return Mode::Ship;
    if (p->m_isBird) return Mode::Ufo;
    if (p->m_isBall) return Mode::Ball;
    if (p->m_isDart) return Mode::Wave;
    if (p->m_isRobot) return Mode::Robot;
    if (p->m_isSpider) return Mode::Spider;
    if (p->m_isSwing) return Mode::Swing;
    return Mode::Cube;
}

// Modes judged one jump at a time: a prediction ends once the jump lands.
static bool isJumpMode(Mode m) {
    return m == Mode::Cube || m == Mode::Robot || m == Mode::Ball || m == Mode::Spider;
}

// Ship and swing are continuous steering; delaying them only feels laggy.
static bool isBufferable(Mode m) {
    return m != Mode::Ship && m != Mode::Swing;
}

// ---------------------------------------------------------------- player rhythm

// Running averages of how the player clicks, in physics ticks (240 per second).
struct Rhythm {
    double hold = 8.0;
    double gap = 10.0;
    double pressInterval = 20.0;
    long long lastPress = -1;
    long long lastRelease = -1;

    static void blend(double& avg, long long sample) {
        avg += (static_cast<double>(std::max<long long>(sample, 1)) - avg) * 0.3;
    }

    void note(bool down, long long tick) {
        constexpr long long kMaxGap = 120;
        if (down) {
            if (lastRelease >= 0 && tick - lastRelease <= kMaxGap) blend(gap, tick - lastRelease);
            if (lastPress >= 0 && tick - lastPress <= kMaxGap) blend(pressInterval, tick - lastPress);
            lastPress = tick;
        }
        else {
            if (lastPress > lastRelease && tick - lastPress <= kMaxGap) blend(hold, tick - lastPress);
            lastRelease = tick;
        }
    }

    void reset() {
        lastPress = -1;
        lastRelease = -1;
    }

    int holdTicks() const { return std::clamp(static_cast<int>(std::lround(hold)), 1, 40); }
    int gapTicks() const { return std::clamp(static_cast<int>(std::lround(gap)), 1, 40); }
    int intervalTicks() const { return std::clamp(static_cast<int>(std::lround(pressInterval)), 2, 60); }
};

// ---------------------------------------------------------------- simulation

enum class Outcome { Survived, Died, Unknown };

struct SimInput {
    int tick;
    bool down;
};

struct SimRequest {
    std::vector<SimInput> inputs; // sorted by tick; tick 0 = the tick being processed now
    int evaluatedTick = 0;        // tick of the input under test
    int horizon = 60;             // total ticks to run
    int actWindow = 0;            // ticks after evaluatedTick in which the input should visibly act
    bool jumpMode = false;        // end once an acted jump lands / reaches a new orb
};

struct SimResult {
    Outcome outcome = Outcome::Unknown;
    bool acted = false;
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
    void noteJump() { m_jumps++; }

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

    // Runs the ghost(s). sources[i] is the real player ghost i stands in for.
    SimResult run(PlayLayer* pl, PlayerObject* const* sources, PlayerObject* const* ghosts, int n,
        SimRequest const& req)
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

        PlayerObject* lead = ghosts[0];
        std::vector<CCObject*> startOrbs;
        if (auto rings = sources[0]->m_touchingRings) {
            for (unsigned i = 0; i < rings->count(); ++i) startOrbs.push_back(rings->objectAtIndex(i));
        }

        res.outcome = Outcome::Survived;
        bool airborneAfterAct = false;
        int groundedTicks = 0;
        size_t next = 0;

        for (int tick = 0; tick < req.horizon; ++tick) {
            while (next < req.inputs.size() && req.inputs[next].tick <= tick) {
                for (int i = 0; i < n; ++i) {
                    if (req.inputs[next].down) ghosts[i]->pushButton(PlayerButton::Jump);
                    else ghosts[i]->releaseButton(PlayerButton::Jump);
                }
                ++next;
            }

            double prevY = lead->getPositionY();
            double prevVy = lead->m_yVelocity * (lead->m_isUpsideDown ? -1.0 : 1.0);
            bool prevFlip = lead->m_isUpsideDown;
            bool prevDash = lead->m_isDashing;
            int prevJumps = m_jumps;

            for (int i = 0; i < n && !m_dead; ++i) step(pl, ghosts[i]);

            if (m_dead) {
                res.outcome = Outcome::Died;
                break;
            }
            if (m_boundary) break; // reached a portal/trigger alive: counts as survived
            if (tick < req.evaluatedTick) continue;

            // Did the input under test do something? (jump, flip, orb, dash, spider teleport)
            if (tick <= req.evaluatedTick + req.actWindow) {
                double vy = lead->m_yVelocity * (lead->m_isUpsideDown ? -1.0 : 1.0);
                bool teleported = std::abs(lead->getPositionY() - prevY) > 10.0;
                bool launched = (vy - prevVy > 2.0 && vy > 2.0) || (vy - prevVy < -4.0);
                if (m_jumps != prevJumps || launched || teleported || lead->m_isUpsideDown != prevFlip ||
                    (lead->m_isDashing && !prevDash))
                {
                    res.acted = true;
                }
            }

            if (!req.jumpMode || !res.acted || tick <= req.evaluatedTick) continue;

            // After the input has acted: reaching a new orb means the next click
            // decides what happens, so stop there.
            bool newOrb = false;
            if (auto rings = lead->m_touchingRings) {
                for (unsigned i = 0; i < rings->count() && !newOrb; ++i) {
                    auto r = rings->objectAtIndex(i);
                    newOrb = std::find(startOrbs.begin(), startOrbs.end(), r) == startOrbs.end();
                }
            }
            if (newOrb) break;

            // ...and a few safe ticks on the ground after the jump means it worked.
            if (!lead->m_isOnGround) {
                airborneAfterAct = true;
                groundedTicks = 0;
            }
            else if (airborneAfterAct && ++groundedTicks >= 3) {
                break;
            }
        }

        if (res.outcome == Outcome::Survived && !m_boundary && req.jumpMode && !lead->m_isOnGround &&
            !lead->m_isDashing && airborneAfterAct)
        {
            // Ran out of lookahead mid-air: nothing proves this timing is safe.
            res.outcome = Outcome::Unknown;
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
    int m_jumps = 0;
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
// True while the game is inside a physics tick. An input that arrives outside one
// (between frames) is applied on the next tick, so it needs one extra tick of wait.
static bool g_inTick = false;

} // namespace ecb

using namespace ecb;

// ---------------------------------------------------------------- PlayLayer

class $modify(BufferPlayLayer, PlayLayer) {
    struct Fields {
        Ref<PlayerObject> ghost1;
        Ref<PlayerObject> ghost2;
        Ref<CCLabelBMFont> label;

        struct Event {
            bool down;
            bool isP1;
        };
        std::deque<Event> queue; // inputs being held back, oldest first
        int ticksLeft = 0;       // ticks until queue[0] is released
        long long tick = 0;      // physics ticks since the attempt started
        Rhythm rhythm;
        int lastSaved = 0;
        float labelFade = 0.f;
        Config cfg;
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
        label->setID("buffer-indicator"_spr);
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
        std::string text = f->cfg.enabled ? fmt::format("Buffer: {}t", f->cfg.maxDelay) : std::string("Buffer: off");
        if (f->labelFade > 0.f) text += fmt::format("  saved +{}t", f->lastSaved);
        f->label->setString(text.c_str());
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto f = m_fields.self();
        f->queue.clear();
        f->ticksLeft = 0;
        f->rhythm.reset();
        f->cfg = readConfig();
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
            f->queue.clear();
            f->ticksLeft = 0;
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

    // ------------------------------------------------------------ input logic

    bool dualMerged() {
        return m_gameState.m_isDualMode && m_player2 && !isTwoPlayer();
    }

    bool isTwoPlayer() {
        return m_levelSettings && m_levelSettings->m_twoPlayerMode;
    }

    // Returns how many ticks to hold this input back (0 = let it through now).
    int chooseDelay(bool down) {
        auto f = m_fields.self();
        auto const& cfg = f->cfg;
        if (!f->ghost1 || !m_player1) return 0;

        PlayerObject* sources[2] = { m_player1, nullptr };
        PlayerObject* ghosts[2] = { f->ghost1, f->ghost2 };
        int n = 1;
        if (dualMerged() && f->ghost2) {
            sources[1] = m_player2;
            n = 2;
        }

        Mode mode = modeOf(m_player1);
        for (int i = 0; i < n; ++i) {
            if (!isBufferable(modeOf(sources[i]))) return 0;
        }
        bool wave = mode == Mode::Wave;
        if (!down && !(wave && cfg.bufferReleases)) return 0;

        auto& rhythm = f->rhythm;
        int latest = cfg.maxDelay;
        auto makeRequest = [&](int at) {
            SimRequest r;
            r.evaluatedTick = at;
            r.inputs.push_back({ at, down });
            if (wave) {
                int nextIn = down ? rhythm.holdTicks() : rhythm.gapTicks();
                r.inputs.push_back({ at + nextIn, !down });
                r.horizon = at + std::clamp(nextIn + 4, 6, 30);
            }
            else if (mode == Mode::Ufo) {
                int hold = rhythm.holdTicks();
                int interval = std::max(hold + 1, rhythm.intervalTicks());
                r.inputs.push_back({ at + hold, false });
                r.horizon = at + std::clamp(interval + 4, 8, 40);
            }
            else {
                int hold = rhythm.holdTicks();
                r.inputs.push_back({ at + hold, false });
                r.horizon = at + cfg.lookahead;
                r.jumpMode = true;
                r.actWindow = hold + 2;
            }
            return r;
        };

        // Never hold an input past the moment the player's next input is due,
        // or the delay would swallow it instead of shifting it.
        if (wave) latest = std::min(latest, (down ? rhythm.gapTicks() : rhythm.holdTicks()) - 1);
        if (mode == Mode::Ufo) latest = std::min(latest, rhythm.intervalTicks() - 1);
        if (latest < 1) return 0;

        auto& sim = Simulator::get();
        auto started = std::chrono::steady_clock::now();
        auto overBudget = [&] {
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count();
            return us > static_cast<long long>(cfg.budgetMs * 1000.0);
        };

        SimResult base = sim.run(this, sources, ghosts, n, makeRequest(0));
        if (base.outcome != Outcome::Died) return 0;

        for (int d = 1; d <= latest && !overBudget(); ++d) {
            SimResult r = sim.run(this, sources, ghosts, n, makeRequest(d));
            if (r.outcome != Outcome::Survived) continue;
            // A press that jumped on time must still jump when delayed
            // (e.g. not get eaten because you walked off a ledge meanwhile).
            if (down && isJumpMode(mode) && base.acted && !r.acted) continue;
            return d;
        }
        return 0;
    }

    void fire(Fields::Event const& e) {
        g_injecting = true;
        this->handleButton(e.down, static_cast<int>(PlayerButton::Jump), e.isP1);
        g_injecting = false;
    }

    // Called at the start of every physics tick, before queued inputs are applied.
    void onTick() {
        auto f = m_fields.self();
        f->tick++;
        if (f->labelFade > 0.f) {
            f->labelFade -= 1.f;
            if (f->labelFade <= 0.f) refreshLabel();
        }
        if (f->queue.empty()) return;
        if (--f->ticksLeft > 0) return;

        // Release queued inputs in order. A press and its release must land on
        // different ticks or the game would never see the press.
        bool pressedP1 = false, releasedP1 = false, pressedP2 = false, releasedP2 = false;
        while (!f->queue.empty()) {
            auto e = f->queue.front();
            bool& sameDir = e.isP1 ? (e.down ? pressedP1 : releasedP1) : (e.down ? pressedP2 : releasedP2);
            bool& otherDir = e.isP1 ? (e.down ? releasedP1 : pressedP1) : (e.down ? releasedP2 : pressedP2);
            if (otherDir) break;
            f->queue.pop_front();
            sameDir = true;
            fire(e);
        }
        f->ticksLeft = f->queue.empty() ? 0 : 1;
    }

    // Returns true if the input was taken over by the buffer.
    bool interceptInput(bool down, int button, bool isP1) {
        auto f = m_fields.self();
        if (button != static_cast<int>(PlayerButton::Jump)) return false;
        if (m_isPlatformer || isTwoPlayer()) return false;
        if (!m_player1 || m_player1->m_isDead || m_hasCompletedLevel) return false;

        // Something is already being held back: keep everything in order behind it.
        if (!f->queue.empty()) {
            auto it = std::find_if(f->queue.rbegin(), f->queue.rend(),
                [&](Fields::Event const& e) { return e.isP1 == isP1; });
            if (it == f->queue.rend() || it->down != down) f->queue.push_back({ down, isP1 });
            if (isP1) f->rhythm.note(down, f->tick);
            return true;
        }
        if (!isP1) return false;

        // Settings can change from the pause menu mid-level.
        f->cfg = readConfig();
        if (!f->cfg.enabled) {
            refreshLabel();
            return false;
        }

        int delay = chooseDelay(down);
        f->rhythm.note(down, f->tick + delay);
        if (delay <= 0) return false;

        f->queue.push_back({ down, true });
        f->ticksLeft = delay + (g_inTick ? 0 : 1);
        f->lastSaved = delay;
        f->labelFade = 240.f;
        refreshLabel();
        if (f->cfg.logSaves) {
            log::info("Held {} for {} tick(s) ({:.1f} ms) - it would have killed you",
                down ? "press" : "release", delay, delay * 1000.0 / 240.0);
        }
        return true;
    }
};

// ---------------------------------------------------------------- GJBaseGameLayer

class $modify(BufferBaseLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::First);
    }

    BufferPlayLayer* asBufferedPlayLayer() {
        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) return nullptr;
        return static_cast<BufferPlayLayer*>(pl);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!g_injecting && !Simulator::get().running()) {
            if (auto pl = asBufferedPlayLayer()) {
                if (pl->interceptInput(down, button, isPlayer1)) return;
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!Simulator::get().running()) {
            if (auto pl = asBufferedPlayLayer()) pl->onTick();
        }
        bool wasInTick = g_inTick;
        g_inTick = true;
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        g_inTick = wasInTick;
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

class $modify(BufferPlayerObject, PlayerObject) {
    void update(float dt) {
        auto& sim = Simulator::get();
        if (!sim.running()) {
            if (auto pl = PlayLayer::get(); pl && this == pl->m_player1) sim.setTickDelta(dt);
        }
        PlayerObject::update(dt);
    }

    void incrementJumps() {
        if (Simulator::get().running()) {
            Simulator::get().noteJump();
            return;
        }
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

class $modify(BufferEffectObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (Simulator::get().running()) return;
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(BufferGameObject, GameObject) {
    void playShineEffect() {
        if (Simulator::get().running()) return;
        GameObject::playShineEffect();
    }
};

class $modify(BufferRingObject, RingObject) {
    void spawnCircle() {
        if (Simulator::get().running()) return;
        RingObject::spawnCircle();
    }
};

class $modify(BufferHardStreak, HardStreak) {
    void addPoint(cocos2d::CCPoint point) {
        if (Simulator::get().running()) return;
        HardStreak::addPoint(point);
    }
};
