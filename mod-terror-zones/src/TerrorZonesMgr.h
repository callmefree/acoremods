#ifndef MOD_TERROR_ZONES_MGR_H
#define MOD_TERROR_ZONES_MGR_H

#include "Define.h"
#include "ObjectGuid.h"
#include <atomic>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class Creature;
class LootStore;
class Player;
class Unit;
struct Loot;
struct LootStoreItem;

namespace mod_terror_zones
{

// Slice 4 — empowerment flavors. Persisted in terror_zones_history.flavor
// as the underlying tinyint value; keep these stable across releases.
// FLAVOR_NONE (0) is written for pre-Slice-4 rows and means "no flavor
// known" when displaying historical ticks.
enum Flavor : uint8
{
    FLAVOR_NONE        = 0,
    FLAVOR_BLOODBATH   = 1,   // +XP + combat gold   — the leveling zone
    FLAVOR_PROSPECTORS = 2,   // +gold + gathering yield — professions zone
    FLAVOR_WARLORDS    = 3,   // +tier-bump chance   — drop-hunting zone
    FLAVOR_ARCANE      = 4,   // +XP + explore stub  — mount-XP deferred
    FLAVOR_MERCHANTS   = 5,   // +gold               — the gold zone
    FLAVOR_MAX         = FLAVOR_MERCHANTS
};

// Slice 5 — empowerment tier. Rolled per slot per rotation, independent
// of flavor. Tier 5 is the ~2% "rotation of the week" moment. Persisted
// in terror_zones_history.tier; pre-Slice-5 rows default to TIER_NONE
// which resolves to Tier 1 behavior at read-time.
enum Tier : uint8
{
    TIER_NONE = 0,
    TIER_1    = 1,
    TIER_2    = 2,
    TIER_3    = 3,
    TIER_4    = 4,
    TIER_5    = 5,
    TIER_MAX  = TIER_5
};

// Five reward axes. Flavor's signature axis takes its tier-bracket ceiling
// bump; two thematic secondaries get a small floor bump; the remaining
// axes read the baseline tier bracket untouched.
enum RewardAxis : uint8
{
    AXIS_XP        = 0,
    AXIS_GOLD      = 1,
    AXIS_TIER_BUMP = 2,
    AXIS_GATHERING = 3,
    AXIS_UNIQUES   = 4,
    AXIS_COUNT     = 5
};

// Per-tier per-axis `(base, spread)` bracket. A roll lands uniformly in
// `[base × (1 - spread), base × (1 + spread)]` when the flavor's bias
// doesn't touch this axis.
struct TierAxisBracket
{
    float base;
    float spread;
};

// Per-flavor primary axis + two secondaries. Hardcoded in the .cpp as
// a constexpr table — flavor identity is design, not tuning.
struct FlavorBiasDef
{
    RewardAxis primary;
    RewardAxis secondaryA;
    RewardAxis secondaryB;
};

// Full runtime config for the Slice 5 roll math. All values are loaded
// from the .conf at boot. The per-flavor bias *identity* (primary axis,
// secondaries) is NOT in here — it's a constexpr in the .cpp per §2.8.
struct TierRollConfig
{
    TierAxisBracket tierTable[TIER_MAX][AXIS_COUNT];
    float signatureFloorBump;    // +floor on the flavor's primary axis
    float signatureCeilingBump;  // +ceiling on the flavor's primary axis
    float secondaryFloorBump;    // +floor on the flavor's secondaries
    float axisCaps[AXIS_COUNT];  // defensive ceilings per axis
};

// Slice 6 — dynamic events. Two event types ship (world boss + rare
// node surge); treasure caravan + champion grounds are deferred to
// Slice 6b but keep their enum values reserved so content drops in
// without schema churn. `EVENT_NONE` is the fall-through return from
// SelectEventType when every weight is zero.
enum EventType : uint8
{
    EVENT_NONE             = 0,
    EVENT_WORLD_BOSS       = 1,
    EVENT_TREASURE_CARAVAN = 2,   // RESERVED — Slice 6b
    EVENT_RARE_NODE_SURGE  = 3,
    EVENT_CHAMPION_GROUNDS = 4,   // RESERVED — future
    EVENT_TYPE_MAX         = EVENT_CHAMPION_GROUNDS
};

enum EventState : uint8
{
    EVENT_STATE_PENDING = 0,
    EVENT_STATE_ACTIVE  = 1,
    EVENT_STATE_EXPIRED = 2
};

// Pure config for the Slice 6 scheduling helpers. Populated from
// TerrorZones.Events.* at LoadConfig time. Kept separate from the
// full Mgr state so the pure helpers are unit-testable without
// standing up the Mgr singleton.
struct EventScheduleConfig
{
    float  fireChance;        // 0..1 — per-slot probability of any event
    float  secondChance;      // 0..1 — probability of a second event
    uint32 durationSec;       // event lifetime once active
    uint32 firstOffsetSec;    // when the 1st event fires (post-tick)
    uint32 secondOffsetSec;   // when the 2nd event fires (post-tick)
    uint32 typeWeights[EVENT_TYPE_MAX + 1];  // index by EventType enum
    bool   typeEnabled[EVENT_TYPE_MAX + 1];
};

// Curated world-boss definition loaded from terror_zones_event_bosses.
struct EventBossDef
{
    uint32 id;
    uint32 zoneId;
    uint8  levelMin;
    uint8  levelMax;
    uint32 creatureTemplateId;
    uint32 anchorMap;
    float  anchorX;
    float  anchorY;
    float  anchorZ;
    float  anchorO;
    std::string displayName;
    uint32 weight;
    bool   enabled;
    // Slice 6 — worldmap tracker quest. 0 = no tracker quest for
    // this row (boss has no map POI). Non-zero references a
    // dummy quest_template row whose quest_poi points at the
    // anchor — auto-granted/revoked at 1Hz while the event is
    // ACTIVE and the player is in the zone.
    uint32 trackerQuestId;
};

// Curated node-surge definition loaded from
// terror_zones_event_node_surges. `nodeIds` is the CSV parsed at load
// time; rows with no valid IDs are dropped with a warn log line.
struct EventNodeSurgeDef
{
    uint32 id;
    uint32 zoneId;
    uint8  levelMin;
    uint8  levelMax;
    uint32 anchorMap;
    float  anchorX;
    float  anchorY;
    float  anchorZ;
    float  radius;       // 0 → fall back to config default
    std::vector<uint32> nodeIds;
    uint32 nodeCount;    // 0 → fall back to config default
    std::string displayName;
    uint32 weight;
    bool   enabled;
};

// Live runtime event, held in Mgr::_activeEvents. Written alongside
// the parent rotation's history row via terror_zones_events.
struct ActiveEvent
{
    uint64 tickAt;
    uint32 slotIndex;
    uint32 eventId;
    EventType type;
    EventState state;
    uint64 startsAt;
    uint64 endsAt;
    uint32 zoneId;
    uint32 mapId;
    uint32 definitionId;
    float  anchorX;
    float  anchorY;
    float  anchorZ;
    std::string displayName;
    // Full ObjectGuids of live spawns — needed for Map::GetCreature /
    // Map::GetGameObject lookup at despawn time. Temp-summons die with
    // the worldserver process, so the restart-resume path does NOT
    // reattach; it re-summons fresh and keeps the same eventId.
    std::vector<ObjectGuid> spawnedCreatures;
    std::vector<ObjectGuid> spawnedGameObjects;
    // Slice 6 — worldmap tracker quest id copied from the def at
    // schedule time so the 1Hz grant/revoke tick doesn't need to
    // re-lookup the def. 0 = no tracker.
    uint32 trackerQuestId;
    // Slice 7 — countdown-fired bit. Persisted to
    // terror_zones_events.countdown_fired so a restart doesn't
    // re-fire the "ending in N minutes" zone-scoped warning.
    bool countdownFired = false;
};

struct PoolEntry
{
    uint32 zoneId;
    std::string displayName;
    uint16 levelMin;
    uint16 levelMax;
    bool   enabled;
};

struct SelectionConfig
{
    uint32 levelWindow;
    uint32 weightNear;
    uint32 weightOverlap;
    uint32 weightBelow;
    uint32 weightAbove;
    double recencyMultiplier;
    uint32 slotCount;
};

// Abstract RNG so unit tests can inject a deterministic stream.
// Returns a uniformly distributed integer in [0, maxExclusive).
class IRng
{
public:
    virtual ~IRng() = default;
    virtual uint32 NextUInt(uint32 maxExclusive) = 0;
};

class StdRng : public IRng
{
public:
    explicit StdRng(uint64 seed);
    uint32 NextUInt(uint32 maxExclusive) override;
private:
    std::mt19937_64 _engine;
};

// Pure target-level math for Slice 2 (plan §5). Returns 0 when the zone
// isn't currently empowered (caller must not scale); otherwise returns
// max(80, highestOnlineInZone) — empowered zones are endgame content
// by design, so the floor is the retail level cap. The old pool-based
// floor left mobs stuck at native zone cap (e.g. 64 in Zangarmarsh)
// whenever the rescale walk ran without any real player sessions online
// (startup-resume, solo-server bootstrap). Unit-tested at §8.1.
uint8 ComputeTargetLevelPure(bool zoneIsEmpowered,
                              uint8 highestOnlineInZone,
                              uint8 zoneLevelMin = 0,
                              uint8 zoneTier = 0,
                              uint8 maxPlayerLevel = 80);

// Given a baseline level that `SelectLevel` would have rolled and a
// target from `ComputeTargetLevelPure`, return the final level the hook
// should assign. Enforces the "never scale down" invariant.
uint8 ApplyScaling(uint8 baseline, uint8 target);

// Pure Slice 3 multiplier math. Returns `baseline * mult`, clamped to
// UINT32_MAX on overflow. Separated out so unit tests can exercise it
// without standing up a live session.
uint32 ComputeMultipliedValue(uint32 baseline, float mult);

// Pure weighted-draw for Slice 4 flavor selection. Weights are indexed by
// (Flavor - 1), so a 5-wide weights array covers BLOODBATH..MERCHANTS.
// Returns FLAVOR_NONE only if every weight is zero (the caller logs and
// falls back to FLAVOR_BLOODBATH). Unit-tested in §11.1.
Flavor SelectFlavor(uint32 const (&weights)[FLAVOR_MAX], IRng& rng);

// Slice 5 — pure weighted-draw for tier selection. Weights indexed
// (Tier - 1), so weights[0]=T1 weight … weights[4]=T5 weight. Returns
// TIER_NONE only when every weight is zero (caller logs + falls back to
// TIER_1). Unit-tested in §10.1.
Tier SelectTier(uint32 const (&weights)[TIER_MAX], IRng& rng);

// Human-facing flavor name for announcements + /zones output.
char const* FlavorDisplayName(Flavor flavor);

// Lowercase enum key for command parsing (".zones testflavor bloodbath").
char const* FlavorCommandKey(Flavor flavor);

// Human-facing tier name for announcements / `.zones` output. Returns
// "Tier N" for TIER_1..TIER_5, or "Tier 1" for TIER_NONE (§4.5 read-time
// compatibility — pre-Slice-5 rows display as if they were Tier 1).
char const* TierDisplayName(Tier tier);

// Short axis label used in the announcement line and `.zones` axis
// breakdown. "XP" / "gold" / "tier-bump" / "gathering" / "uniques".
char const* AxisShortName(RewardAxis axis);

// True when an axis is a probability (0..1), false when it's a
// multiplier (≥ 0). Controls how rolls render in chat output.
bool IsProbabilityAxis(RewardAxis axis);

// Access the hardcoded per-flavor bias definition (primary + two
// secondaries). FLAVOR_NONE returns a sentinel with all axes set to
// AXIS_COUNT (no-op) so callers can safely query without guarding.
FlavorBiasDef const& FlavorBiasOf(Flavor flavor);

// Pure, deterministic per-axis roll. Same `(tickAt, slotIndex, flavor,
// tier, axis)` tuple + same `cfg` ALWAYS produces the same output, so
// rolls survive worldserver restart without a persisted rolls column.
// TIER_NONE is treated as TIER_1 per §4.5. The returned value is
// clamped at `cfg.axisCaps[axis]`. Unit-tested in §10.2.
float ComputeAxisRoll(
    uint64 tickAt,
    uint32 slotIndex,
    Flavor flavor,
    Tier tier,
    RewardAxis axis,
    TierRollConfig const& cfg);

// Pure LootStore-name check — true for herb/mine (gameobject_loot_template)
// and skinning stores. Unit-tested; fishing intentionally excluded from
// this predicate (see plan §9.1).
bool IsGatheringStore(char const* storeName);

// Slice 6 — pure event helpers. Unit-tested in §9.1 of SLICE_6_PLAN.md.

// Weighted draw across the enabled EventType values. Ignores types
// whose typeEnabled[] is false, even if the weight is non-zero. Returns
// EVENT_NONE when every candidate is disabled or all weights zero.
EventType SelectEventType(EventScheduleConfig const& cfg, IRng& rng);

// 0/1 coin flip biased by `chance`. `chance <= 0` → false,
// `chance >= 1` → true.
bool ShouldFireSecondEvent(float chance, IRng& rng);

// Time-window membership. Inclusive on `starts`, exclusive on `ends`.
bool WithinEventWindow(uint64 now, uint64 starts, uint64 ends);

// Pick a uniformly-distributed point inside the disc of radius `r`
// around `(ax, ay)`. Z is unchanged (caller runs map height recovery).
// When `r <= 0`, returns the anchor point unmodified.
void PickSubregionAnchor(float ax, float ay, float r, IRng& rng,
                          float& outX, float& outY);

// Human-facing + command-parseable names for the event types.
char const* EventTypeDisplayName(EventType type);
char const* EventTypeCommandKey(EventType type);
EventType   ParseEventTypeKey(char const* key);
char const* EventStateDisplayName(EventState state);

// Slice 8 — combat-difficulty multipliers. Pure compositions of the
// baseline × per-tier × event-boss uplift formula documented in
// SLICE_8_PLAN.md §4. `tierBonus` is indexed 0..TIER_MAX (index 0 is
// the TIER_NONE sentinel, treated as a no-op 1.0). Floored at 1.0 so
// a misconfigured mult < 1 can't make empowered-zone mobs SOFTER than
// native (breaks the design identity). Unit-tested in §9.1.
//
// Slice 8b — `isPromotedElite` + `eliteUplift` extend the composition
// for elite-density promoted spawns. Composition order:
//   base × tierBonus × (promoted ? eliteUplift : 1.0) × (event ? eventUplift : 1.0)
// Defaults keep existing call sites (Slice 8 tests) working unchanged.
float ComputeCombatHpMult(float baseline,
                          Tier tier,
                          float const (&tierBonus)[TIER_MAX + 1],
                          bool isEventBoss,
                          float eventBossUplift,
                          bool isPromotedElite = false,
                          float eliteUplift = 1.0f);

float ComputeCombatDamageMult(float baseline,
                              Tier tier,
                              float const (&tierBonus)[TIER_MAX + 1],
                              bool isEventBoss,
                              float eventBossUplift,
                              bool isPromotedElite = false,
                              float eliteUplift = 1.0f);

// Slice 8b — pure deterministic per-spawn elite-promotion decision.
// Returns true when the spawn falls inside the per-tier density
// threshold. `thresholdPerMille` is [0, 1000] — 150 means 15% of
// eligible spawns promote. Same `(rawGuid, tickAt)` always returns
// the same answer, so a single creature stays promoted (or stays
// not-promoted) across rescale walks within a rotation, and
// re-rolls when a new rotation ticks. Unit-tested.
bool IsPromotedSpawn(uint64 rawGuid,
                     uint64 tickAt,
                     uint32 thresholdPerMille);

// Slice 7 — per-category announcement gating. Eight categories cover
// every player-visible chat line the module emits. Each category has
// (a) a server-side config gate stored in the Mgr's
// `_announceCategoryGlobal` bitmask, (b) a per-player bit in
// `character_terror_zones_prefs.announce_categories`, and (c) the
// existing per-player master toggle `announce_enabled`. A line fires
// only when ALL THREE are open. SLICE_7_PLAN.md §4 + §5 are the spec.
enum AnnounceCategory : uint8
{
    ANNOUNCE_ROTATION_TICK    = 0,
    ANNOUNCE_ROTATION_ENDING  = 1,
    ANNOUNCE_ROTATION_END     = 2,
    ANNOUNCE_ZONE_ENTRY       = 3,
    ANNOUNCE_ZONE_LEAVE       = 4,
    ANNOUNCE_EVENT_START      = 5,
    ANNOUNCE_EVENT_ENDING     = 6,
    ANNOUNCE_EVENT_END        = 7,
    ANNOUNCE_CATEGORY_COUNT
};

constexpr uint8 ANNOUNCE_CATEGORY_ALL = 0xFF;

constexpr uint8 AnnounceCategoryBit(AnnounceCategory c)
{
    return static_cast<uint8>(1u << static_cast<uint8>(c));
}

// Three-input gating composition. Master off → false; otherwise
// requires both global and player bits to be set for the category.
bool IsCategoryAnnouncementAllowed(AnnounceCategory cat,
                                    uint8 globalMask,
                                    bool playerMasterOn,
                                    uint8 playerMask);

// One-shot rotation-ending warning predicate. Fires when `now` lands
// in the `[nextTickAt - leadSec, nextTickAt - leadSec + windowSec]`
// slack window. Returns false if `leadSec == 0` (disabled), if the
// warning was already fired for this `nextTickAt` (dedupe via
// `lastWarnTickAt`), if the window has not yet opened, or if the
// window has already closed (missed-window suppress — better silent
// than late on a restart-resume). Unit-tested in §9.1.
bool ShouldFireRotationEndingWarning(uint64 now,
                                      uint64 nextTickAt,
                                      uint32 leadSec,
                                      uint32 windowSec,
                                      uint64 lastWarnTickAt);

// One-shot event-ending countdown predicate. Same shape as the
// rotation warning above, but per-event: dedupe via the persisted
// `countdown_fired` bit on `terror_zones_events`. Unit-tested in §9.1.
bool ShouldFireEventEndingCountdown(uint64 now,
                                     uint64 endsAt,
                                     uint32 leadSec,
                                     uint32 windowSec,
                                     bool   alreadyFired);

// Human-facing display name + lowercase command key per category.
char const* AnnounceCategoryDisplayName(AnnounceCategory cat);
char const* AnnounceCategoryCommandKey(AnnounceCategory cat);

// Parse the `<cat>` argument from `.zones announce <cat> on|off`.
// Returns ANNOUNCE_CATEGORY_COUNT when the key is unknown. The alias
// "event" is a special return — see ParseAnnounceCategoryAlias below.
AnnounceCategory ParseAnnounceCategoryKey(char const* key);

// Bitmask shortcut for command parsing. Returns the bit pattern to
// flip when the user types one of the recognized aliases:
//   "all"      → ANNOUNCE_CATEGORY_ALL
//   "event"    → bits for EventStart | EventEnding | EventEnd
//   "rotation" → bits for RotationTick | RotationEnding | RotationEnd
//   "zone"     → bits for ZoneEntry | ZoneLeave
// Returns 0 when no alias matches (caller falls back to single-cat
// parse via ParseAnnounceCategoryKey).
uint8 ParseAnnounceCategoryAlias(char const* key);

// Pure selection — no globals, no Player*, no DB access. This is the
// function exercised by Slice 1 unit tests (plan §10.1).
// `targets` is a list of effective levels: one entry per online solo
// player, one entry per group (lowest level in the group), dedup already
// applied by the caller. `recentZoneIds` is the set of zone IDs to
// dampen. Returns up to `cfg.slotCount` distinct zone IDs (fewer when
// the pool is too small).
std::vector<uint32> SelectZones(
    std::vector<PoolEntry> const& pool,
    std::vector<uint8> const& targets,
    std::vector<uint32> const& recentZoneIds,
    SelectionConfig const& cfg,
    IRng& rng);

struct ActiveSlot
{
    uint32 zoneId;
    std::string displayName;
    Flavor flavor = FLAVOR_NONE;   // Slice 4 — set at rotation time
    Tier   tier   = TIER_NONE;     // Slice 5 — rolled per slot
    uint32 slotIndex = 0;          // Slice 5 — seeds axis rolls
};

struct ActiveRotation
{
    uint64 tickAt = 0;       // aligned wall-clock boundary (unix seconds)
    uint64 expiresAt = 0;    // tickAt + intervalSec
    std::vector<ActiveSlot> slots;
};

struct HistoryTick
{
    uint64 tickAt;
    std::vector<ActiveSlot> slots;
};

class TerrorZonesMgr
{
public:
    static TerrorZonesMgr& Instance();

    void LoadConfig();
    void LoadPool();
    void InitializeOnStartup();
    void OnUpdate(uint32 diff);

    bool IsEnabled() const { return _enabled; }
    bool IsDebug() const { return _debug; }
    uint32 GetIntervalSec() const { return _intervalSec; }
    uint32 GetSlotCount() const { return _slotCount; }

    ActiveRotation GetActiveRotation() const;
    uint64 GetNextTickAt() const;
    std::vector<PoolEntry> GetPool() const;
    std::vector<HistoryTick> GetHistory(uint32 maxTicks) const;

    bool IsZoneEmpowered(uint32 zoneId, std::string* outName,
                         uint32* outRemainingSec) const;

    // GM .zones tick — advance to the next aligned boundary and run one
    // rotation immediately. Announces.
    void ForceTick();

    // Per-player pref cache.
    void LoadPlayerPref(Player* player);
    void FlushPlayerPref(Player* player);
    void UnloadPlayerPref(ObjectGuid guid);
    bool IsAnnounceEnabled(Player const* player) const;
    void SetAnnounceEnabled(Player* player, bool enabled);

    // Slice 7 — per-player category bitmask access. The bitmask
    // composes with the global config mask + the master switch via
    // IsCategoryEnabledFor; these helpers are for the
    // `.zones announce` subcommand wiring.
    uint8 GetAnnounceCategories(Player const* player) const;
    void  SetAnnounceCategories(Player* player, uint8 mask);
    uint8 GetGlobalAnnounceCategoryMask() const
    { return _announceCategoryGlobal; }

    // Hooks called from PlayerScripts.
    void OnPlayerLogin(Player* player);
    void OnPlayerUpdateZone(Player* player, uint32 newZone);

    // Remaining-seconds helper for UI/chat formatting.
    uint32 RemainingSeconds(uint64 now = 0) const;

    // --- Slice 2: combat scaling ---
    bool IsScalingEnabled() const { return _enabled && _scalingEnabled; }

    // Returns 0 when the zone isn't empowered (caller leaves baseline
    // alone). Otherwise returns max(pool.level_max, highest-online-in-zone)
    // — the target level mobs in that zone should scale to.
    uint8 ComputeTargetLevel(uint32 zoneId) const;

    // Eligibility predicate per SLICE_2_PLAN §6. Safe to call from the
    // OnBeforeCreatureSelectLevel hook (creature's position is already
    // set at that point, so faction and friendly checks are valid).
    bool IsScalingEligible(Creature const* creature) const;

    // OnBeforeCreatureSelectLevel entry point — mutates `level` in place
    // when scaling applies.
    void OnBeforeCreatureSelectLevel(Creature const* creature, uint8& level);

    // --- Slice 3: baseline rewards ---
    bool IsRewardsEnabled() const { return _enabled && _rewardsEnabled; }

    // Reward multiplier entry points. No-op when the player isn't in an
    // empowered zone or the rewards layer is disabled. Each mutates the
    // reward in place.
    void ApplyXpMultiplier(uint32& amount, Player* player) const;
    void ApplyGoldMultiplier(uint32& gold, Player* player) const;
    void ApplyQuestGoldMultiplier(int32& moneyRew, Player* player) const;

    // Drop-quality tier bump. Returns true when `item->itemid` was
    // substituted for a higher-rarity template matching its level band.
    // Internally gates on empowerment, rewards-enabled, epic cap, and
    // the configured roll chance.
    bool TryTierBump(Player const* player, ::LootStoreItem* item);

    // Build the (quality, level-band) → itemId index used by TryTierBump.
    // Called once at OnStartup after the item template store is loaded.
    void BuildRarityIndex();

    // --- Slice 4: empowerment flavors + atmosphere + gathering + uniques ---
    bool IsFlavorsEnabled() const { return _enabled && _flavorsEnabled; }

    // Look up the active flavor for the caller's current zone. Returns
    // FLAVOR_NONE when the zone isn't empowered or flavors are disabled.
    Flavor GetActiveFlavor(uint32 zoneId) const;

    // Push weather override to everyone in `zoneId` on every map.
    // `ApplyAtmosphereOverride` runs on tick-on; `RestoreAtmosphere` clears
    // on tick-off. Light-override was part of Slice 4 but stripped after
    // empirical testing showed no visible color changes on continent maps
    // (see ENGINEERING_NOTES 2026-04-22). Revisit if/when we have a
    // client-side DBC debugging path.
    void ApplyAtmosphereOverride(uint32 zoneId, Flavor flavor);
    void RestoreAtmosphere(uint32 zoneId);

    // Prospector's gathering-yield overlay. Runs inside OnBeforeDropAddItem
    // next to TryTierBump. Mutates `item->maxcount` when the active flavor
    // is Prospector's AND the store is a gathering store AND the yield
    // roll fires. Returns true on mutation for debug logging.
    bool TryGatheringYieldBump(Player const* player, LootStoreItem* item,
                               char const* storeName);

    // Per-bundle unique-drop roll. Fires at most once per Loot* bundle
    // regardless of how many times OnBeforeDropAddItem fires for that
    // bundle. Adds a bonus item to `loot` (does not substitute).
    void TryUniqueDrop(Player const* player, Loot* loot, uint32 zoneId);

    // Build the flavor → [itemId, weight, levelMin, levelMax] index from
    // terror_zones_unique_drops. Called at OnStartup.
    void BuildUniqueDropIndex();

    // GM-command entry points (bypass rotation state — see §5.6 of plan).
    void TestApplyWeather(Player* player, uint32 state, float grade);
    void TestApplyFlavor(Player* player, Flavor flavor);
    void TestClearAtmosphere(Player* player);

    // Force-set the active rotation's flavor on every slot. Persists to
    // terror_zones_history and re-applies the new flavor's atmosphere.
    // Returns true on success (an active rotation exists). Unlike
    // TestApplyFlavor (which is a local weather preview), SetActiveFlavor
    // changes server state — reward overlays, gathering bumps, unique
    // drops all flip immediately to the new flavor's behavior.
    bool SetActiveFlavor(Flavor flavor);

    // Configured weather for a flavor. Exposed so commands can read the
    // same source of truth as the rotation tick.
    uint32 GetFlavorWeatherState(Flavor flavor) const;
    float  GetFlavorWeatherGrade(Flavor flavor) const;
    bool   IsWeatherOverrideEnabled() const { return _flavorsEnabled && _flavorWeatherOverride; }

    // --- Slice 5: empowerment tiers ---
    bool IsTierEnabled() const { return _enabled && _tierEnabled; }

    // Copy-out a snapshot of the active slot for a given zone. Returns
    // false when the zone isn't empowered. The copy is safe to read
    // after the mutex releases (std::string + POD members).
    bool TryGetSlotForZone(uint32 zoneId, ActiveSlot& out) const;

    // Evaluate one axis roll for a given slot (uses the slot's persisted
    // flavor + tier + slotIndex together with the rotation's tickAt so
    // every call for this rotation returns the same number).
    float RollAxis(ActiveSlot const& slot, RewardAxis axis) const;

    // Read-only access to the loaded tier config (used by /zones output
    // + announcement helpers to render rolled values in the chat line).
    TierRollConfig const& GetTierConfig() const { return _tierCfg; }

    // Force-set the active rotation's tier on every slot. Persists to
    // terror_zones_history.tier and DOES NOT re-apply atmosphere (tier
    // doesn't drive weather — only flavor does). Rewards flip
    // immediately on the next loot/XP event because Apply*/Try* read
    // the live rotation. Returns false when there's no active rotation.
    bool SetActiveTier(Tier tier);

    // --- Slice 6: dynamic events ---
    bool IsEventsEnabled() const { return _enabled && _eventsEnabled; }

    // Loaded content counts — exposed for the boot-log echo.
    uint32 GetEventBossDefCount() const;
    uint32 GetEventNodeSurgeDefCount() const;

    // Resume + content hooks called from InitializeOnStartup.
    void LoadEventContent();
    void LoadActiveEvents();

    // Called from RunRotation (one call, after atmosphere apply).
    // Reads per-slot pending events, writes terror_zones_events rows,
    // populates _activeEvents in state=PENDING. Uses its own RNG seeded
    // off the rotation tickAt so resumes see the same events.
    void ScheduleEvents(uint64 tickAt,
                        std::vector<ActiveSlot> const& slots);

    // Called every OnUpdate tick. Fires pending events whose startsAt
    // is due, ends active events whose endsAt has passed, prunes
    // expired events from the in-memory vector after a short grace.
    void TickEvents(uint64 now);

    // GM command entry points. `FireEventNow` schedules a fresh event
    // in the GM's current zone, starting immediately, for the
    // configured duration. Returns the `eventId` on success.
    // `EndActiveEventsInZone` ends every active+pending event in the
    // given zone, returning how many were ended.
    uint32 FireEventNow(Player* gm, EventType type);
    uint32 EndActiveEventsInZone(uint32 zoneId);

    // Copy-out snapshot of the live event list. Safe to iterate after
    // the lock releases (all members are POD / string / vector).
    std::vector<ActiveEvent> GetEventsSnapshot() const;

    // Event-boss bonus loot chain. Fires from OnBeforeDropAddItem
    // alongside TryTierBump / TryGatheringYieldBump / TryUniqueDrop.
    // Slice 8 replaces the MVP stub with a real band-pool injection:
    // guaranteed blue + rolled purple + rolled gold per boss kill,
    // keyed by the scaled level of the zone. Additive, not
    // substitutive.
    bool TryEventBossDrop(Player const* player, Loot& loot);

    // Top up loot.gold with the event-boss bonus AFTER
    // generateMoneyLoot has run (which overwrites the gold field).
    // Called from OnPlayerBeforeLootMoney. No-op for non-event-boss
    // creatures.
    void ApplyEventBossGoldUplift(Loot& loot, Player const* player);

    // --- Slice 8: combat difficulty ---
    bool IsCombatEnabled() const { return _enabled && _combatEnabled; }

    // Post-SelectLevel HP mult entry point. Reads zone empowerment +
    // eligibility + event-boss status, multiplies the creature's
    // computed MaxHealth by `ComputeCombatHpMult(...)`. No-op when
    // the creature isn't eligible or the zone isn't empowered.
    void OnAfterCreatureSelectLevel(Creature* creature);

    // Outgoing-damage mult entry point. Fires from the UnitScript
    // OnDamage hook. Same predicate as the HP path — eligible
    // attacker, empowered zone, event-boss bonus if indexed.
    void OnUnitDealDamage(Unit* attacker, Unit* victim, uint32& damage);

    // Slice 8 Pass-2 loot-pool content load. Called from
    // InitializeOnStartup after LoadEventContent.
    void LoadEventBossLootPool();

    // Slice 9 Pass 1 — populate `_classDropEntries` from the
    // `terror_zones_event_boss_class_drops` table. Each row is a
    // (band, tier, archetype, slot) coordinate that has a populated
    // custom item template. The runtime drop hook does
    // `EncodeClassDropEntry(...)` then membership tests this set —
    // empty cells fizzle silently. Called from InitializeOnStartup
    // after LoadEventBossLootPool.
    void LoadClassDropIndex();

    // Slice 9 Pass 1 — class-targeted event-boss drop. Per §7 roll
    // path: archetype = ArchetypeForClassSpec(player class+spec);
    // tier = rotation slot tier for player's zone (T1 → no roll);
    // band = bucketed from zone level range; slot = uniform roll;
    // entry = EncodeClassDropEntry(...). Injected via Loot::AddItem
    // once per bundle (own dedup set). Called inline at the end of
    // TryEventBossDrop so it only fires on event-boss kills.
    bool TryClassDrop(Player const* player, Loot& loot);

    // Read-only getters the `.zones` command uses to render the
    // Difficulty sub-line without having to reach into private state.
    float GetCombatHpMult() const { return _combatHpMult; }
    float GetCombatDamageMult() const { return _combatDamageMult; }
    float GetTierHpBonus(Tier t) const
    {
        if (t == TIER_NONE || t > TIER_MAX) return 1.0f;
        return _tierHpBonus[t];
    }
    float GetTierDamageBonus(Tier t) const
    {
        if (t == TIER_NONE || t > TIER_MAX) return 1.0f;
        return _tierDamageBonus[t];
    }
    float GetEventBossHpUplift() const { return _eventBossHpMultUplift; }
    float GetEventBossDamageUplift() const { return _eventBossDamageMultUplift; }
    uint32 GetEliteDensityPerMille(Tier t) const
    {
        if (t == TIER_NONE || t > TIER_MAX) return 0;
        return _eliteDensityPerMille[t];
    }
    float GetEliteHpUplift() const { return _eliteHpMultUplift; }
    float GetEliteDamageUplift() const { return _eliteDamageMultUplift; }

private:
    TerrorZonesMgr() = default;

    // Slice 8b cleanup — lock-free atomic-shared_ptr snapshots.
    //
    // Every writer in this module runs on the world thread (AC's
    // single-threaded `MapUpdate.Threads = 1` model: rotation tick,
    // event tick, creature SelectLevel, damage dispatch, loot
    // mutation, player-script hooks, and command handlers all fire
    // on the same thread). Writers therefore mutate the underlying
    // mutable state (`_rotation`, `_activeEvents`,
    // `_eventBossSpawnIndex`) directly, then publish a fresh
    // immutable snapshot via the matching `Publish*Snap` helper.
    //
    // Readers atomic-load the snapshot and walk it without any
    // synchronization. Stored as plain shared_ptrs accessed via the
    // C++17 `std::atomic_load` / `std::atomic_store` free-function
    // overloads — `std::atomic<std::shared_ptr<...>>` only landed in
    // libstdc++ 12, but the free-function form is stable since C++11
    // and the AC worldserver build image ships libstdc++ 11.
    struct CombatHotState
    {
        struct SlotView
        {
            uint32 zoneId;
            Tier   tier;
        };
        // Small — at most 4 slots per spec. Linear scan beats map
        // lookup at this size.
        std::vector<SlotView> slots;
        // Event-boss GUID set for damage-path uplift membership
        // check. Raw ObjectGuid values, same keying as
        // `_eventBossSpawnIndex`.
        std::unordered_set<uint64> eventBossGuids;
        // Per-GUID tier captured at spawn time so damage scaling
        // uses the boss's source rotation slot tier, not whatever
        // tier the boss's current zone happens to have right now
        // (which may differ from spawn time, or be empty if the
        // boss is in a non-rotation zone).
        std::unordered_map<uint64, Tier> eventBossTiers;
        // Slice 8b — rotation tickAt copied at publish time so the
        // hot path can compute a deterministic per-spawn promotion
        // seed without dereferencing `_rotation` from a non-writer
        // thread. 0 when no rotation is active.
        uint64 tickAt = 0;
    };
    std::shared_ptr<CombatHotState const> _combatHot;

    // Slice 8b cleanup — broader read snapshots for reader paths
    // outside the combat kHz hot path. Each writer cycle ends with
    // the matching publisher; readers atomic-load and copy or walk
    // as needed. Cost analysis (rebuild on every write):
    //   _rotationSnap : 1-4 slots × small POD, ~hourly + GM ticks.
    //   _eventsSnap   : ~10 events × ActiveEvent, ≤4× per rotation.
    //   _eventBossSpawnSnap : ≤21 entries, ≤2× per rotation.
    // Read paths saved every time dominate the rebuild cost.
    std::shared_ptr<ActiveRotation const> _rotationSnap;
    std::shared_ptr<std::vector<ActiveEvent> const> _eventsSnap;
    std::shared_ptr<std::unordered_map<uint64,
                                        std::pair<uint64, uint32>> const>
        _eventBossSpawnSnap;

    // Snapshot publishers. Each rebuilds its target snapshot from the
    // writer-side mutable state and atomic-stores. Caller is on the
    // world thread (the writer-thread invariant — see CombatHotState
    // doc comment above).
    void PublishCombatHot();
    void PublishRotationSnap();
    void PublishEventsSnap();
    void PublishEventBossSpawnSnap();

    // Slice 8 — event-boss loot pool entry. Defined up here so the
    // private method declarations below can reference it as a
    // return-type. Populated by LoadEventBossLootPool() and consulted
    // by FindEventBossLootBand().
    struct LootPoolEntry
    {
        uint32 id;
        uint8  levelMin;
        uint8  levelMax;
        uint32 guaranteedBlueId;
        uint32 purpleItemId;
        float  purpleChance;
        uint32 goldMinCopper;
        uint32 goldMaxCopper;
    };

    void RunRotation(uint64 tickAt, bool announce);
    void AnnounceRotation(ActiveRotation const& rot);

    // Slice 7 — per-category gating helper. Compose `globalMask`,
    // the player's master toggle, and the player's per-category
    // bitmask. World-thread-only access to `_prefs`; no
    // synchronization needed.
    bool IsCategoryEnabledFor(Player const* player,
                               AnnounceCategory cat) const;

    // Slice 7 — new fire paths. Each respects the category gate.
    void SendRotationEndingWarning(uint64 nextTickAt);
    void SendRotationEndLineFor(uint32 zoneId,
                                 std::string const& zoneName);
    void SendZoneLeaveLineTo(Player* player,
                              std::string const& zoneName);
    void SendEventEndingCountdown(ActiveEvent const& evt,
                                   uint32 remainingSec);

    // Slice 6 — event lifecycle internals (called from TickEvents /
    // ScheduleEvents / the GM FireEventNow path). All assume the
    // ActiveEvent already exists in `_activeEvents` — they mutate it
    // in place and handle DB writes, spawn calls, and announcements.
    void FireEvent(ActiveEvent& evt);
    void EndEvent(ActiveEvent& evt);
    void SpawnWorldBoss(ActiveEvent& evt, EventBossDef const& def);
    void SpawnNodeSurge(ActiveEvent& evt, EventNodeSurgeDef const& def);
    void DespawnEventCreatures(ActiveEvent& evt);
    void DespawnEventGameObjects(ActiveEvent& evt);
    void BroadcastZoneLine(uint32 zoneId, std::string const& line);
    // Slice 7 — gated zone broadcast: same as BroadcastZoneLine but
    // only sends the line to in-zone players whose category mask
    // (server + per-player) allows the given category.
    void BroadcastZoneLineGated(uint32 zoneId, std::string const& line,
                                  AnnounceCategory cat);
    void PersistEventState(ActiveEvent const& evt);
    void PersistEventCountdownFired(ActiveEvent const& evt);
    void PruneExpiredEventDbRows();
    // Apply a stalker-aura from every player in a world-boss event's
    // zone onto the boss creature, if not already present. Puts a
    // minimap dot + tracker visual on the boss for that player. Called
    // from TickEvents at 1Hz for every ACTIVE world-boss event.
    void MarkWorldBossForPlayers();
    // Target level for an event-spawned world boss regardless of
    // rotation state. `max(poolLevelMax, highestOnlinePlayerLevel,
    // def.levelMax)` clamped to [def.levelMin, 83]. Used by
    // SpawnWorldBoss to seed _eventBossScaleOverride.
    uint8 ComputeEventBossApex(EventBossDef const& def) const;

    // Slice 8 — find the loot-pool band whose [levelMin, levelMax]
    // contains `scaledLevel`. First match wins. Returns nullptr when
    // no band matches or the pool is empty.
    LootPoolEntry const* FindEventBossLootBand(uint8 scaledLevel) const;

    // Tick-edge zone walks. `edgeOn=true` when a zone newly becomes
    // empowered; `edgeOn=false` when a zone leaves the empowered set.
    void WalkZoneRescale(uint32 zoneId, bool edgeOn,
                         bool force = false);
    void SendTickLineTo(Player* player, std::string const& zoneName,
                        uint32 remainingSec);
    void SendEntryLineTo(Player* player, std::string const& zoneName,
                         uint32 remainingSec);

    std::string FormatRemaining(uint32 secs) const;
    static uint64 AlignedBoundary(uint64 now, uint64 intervalSec);

    // Config.
    bool _enabled = true;
    bool _debug = false;
    uint32 _intervalSec = 3600;
    uint32 _slotCount = 1;
    uint32 _recencyWindow = 6;
    double _recencyMultiplier = 0.1;
    uint32 _levelWindow = 5;
    uint32 _weightNear = 100;
    uint32 _weightOverlap = 30;
    uint32 _weightBelow = 10;
    uint32 _weightAbove = 1;
    bool _announceServerWide = true;
    bool _announceStartupTick = true;
    bool _announceZoneEntry = true;
    bool _startupForceTick = false;

    // Slice 7 — per-category global bitmask, built from the eight
    // `Announce.*` knobs (RotationTick, RotationEnding, RotationEnd,
    // ZoneEntry, ZoneLeave, EventStart, EventEnding, EventEnd) at
    // LoadConfig time. ServerWide / ZoneEntry are OR'd into the
    // matching bits for backward compat.
    uint8 _announceCategoryGlobal = ANNOUNCE_CATEGORY_ALL;
    uint32 _rotationEndingLeadSec = 300;
    uint32 _eventEndingLeadSec    = 300;
    static constexpr uint32 kAnnounceWindowSec = 30;
    // Tracks the most recent `_nextTickAt` for which the rotation-
    // ending warning fired. Init 0 so a still-pending warning can
    // fire after a fresh boot (subject to the missed-window guard).
    uint64 _lastRotationEndingWarnTickAt = 0;

    // Slice 2 — scaling.
    bool _scalingEnabled = true;
    bool _scalingRescaleOnTick = true;
    // Server's max player level. Used as the mob-scaling ceiling
    // base in ComputeTargetLevelPure (final ceiling = max +
    // zoneTier). Configurable via TerrorZones.MaxPlayerLevel so
    // servers running a higher level cap (e.g. custom 85/90 caps)
    // can scale appropriately. 80 is the 3.3.5a retail cap.
    uint8 _maxPlayerLevel = 80;
    bool _scalingSkipWorldBosses = true;
    bool _scalingSkipFriendly = false;
    std::unordered_set<uint32> _scalingNeverEntries;

    // Slice 3 — rewards.
    bool _rewardsEnabled = true;
    float _xpMultiplier = 1.5f;
    float _goldMultiplier = 1.5f;
    float _tierBumpChance = 0.03f;
    uint32 _tierBumpLevelTolerance = 5;
    uint32 _maxBumpQuality = 4;   // ITEM_QUALITY_EPIC — never mint legendaries
    uint32 _levelBandWidth = 5;
    // Exponent applied to (scaledLevel / poolLevelMax) when computing the
    // extra loot-gold uplift inside an empowered zone. 0 = no uplift; 1 =
    // linear with level; 2 = quadratic (tracks WoW's gold-by-level curve).
    float _goldLevelRatioExp = 2.0f;

    // (quality << 8) | band → vector of itemIds. Built once at startup.
    std::unordered_map<uint32, std::vector<uint32>> _rarityIndex;
    bool _rarityIndexBuilt = false;

    // Slice 4 — flavors.
    bool _flavorsEnabled = true;
    uint32 _flavorWeights[FLAVOR_MAX] = {100, 100, 100, 100, 100};
    float  _flavorXpBoost[FLAVOR_MAX]      = {1.50f, 1.00f, 1.00f, 1.25f, 1.00f};
    float  _flavorGoldBoost[FLAVOR_MAX]    = {1.00f, 1.25f, 1.00f, 1.00f, 2.00f};
    float  _flavorTierBumpAdd[FLAVOR_MAX]  = {0.00f, 0.00f, 0.05f, 0.00f, 0.00f};
    bool   _flavorWeatherOverride = true;
    uint32 _flavorWeatherState[FLAVOR_MAX] = {90, 1, 86, 1, 3};
    float  _flavorWeatherGrade[FLAVOR_MAX] = {0.75f, 0.40f, 0.85f, 0.70f, 0.30f};
    float  _flavorGatheringYieldMult   = 2.0f;
    float  _flavorGatheringBonusChance = 0.50f;
    bool   _flavorUniquesEnabled  = true;
    float  _flavorUniquesBaseChance = 0.02f;
    uint32 _flavorUniquesMinMobLevel = 0;

    struct UniqueDropEntry
    {
        uint32 itemId;
        uint32 weight;
        uint16 levelMin;
        uint16 levelMax;
    };
    // Index keyed by Flavor (FLAVOR_NONE slot holds flavor=0 wildcards).
    std::vector<UniqueDropEntry> _uniqueDropsByFlavor[FLAVOR_MAX + 1];
    bool _uniqueDropsBuilt = false;

    // Short-lived dedup set so TryUniqueDrop fires at most once per loot
    // bundle regardless of how many OnBeforeDropAddItem calls the bundle
    // triggers. `Loot*` pointer cast to uint64 is stable per-bundle.
    std::unordered_set<uint64> _uniqueRolledBundles;
    uint64 _uniqueRolledBundlesClearedAt = 0;

    // Slice 5 — tiers.
    bool _tierEnabled = true;
    uint32 _tierWeights[TIER_MAX] = {40, 30, 20, 8, 2};
    TierRollConfig _tierCfg{};

    // Slice 6 — dynamic events.
    bool _eventsEnabled = true;
    EventScheduleConfig _eventCfg{};
    uint32 _eventRetentionHours = 24;
    float  _eventBossLootBaseChance = 1.0f;     // kill-switch
    float  _eventBossScaleMult          = 1.5f; // boss visual-size uplift
    uint32 _eventBossBeaconGoId         = 191123;  // DK convocation circle
    // Per-player stalker-aura spell applied to the boss so every
    // player in the zone gets a minimap dot + visible tracker visual.
    // Default 1130 Hunter's Mark: only mod_stalked aura that ships in
    // 3.3.5 with acceptable visuals (glowing arrow overhead). Zero
    // disables the mechanic. Refresh cadence is the 1Hz OnUpdate tick.
    uint32 _eventBossTrackerSpellId     = 1130;
    float  _eventNodeSurgeDefaultRadius = 40.0f;
    uint32 _eventNodeSurgeDefaultCount  = 8;
    float  _eventNodeSurgeZIgnore       = 10.0f;
    uint32 _eventNodeSurgeBeaconGoId    = 177015;  // standard bonfire

    std::vector<EventBossDef> _eventBossDefs;
    std::vector<EventNodeSurgeDef> _eventNodeSurgeDefs;
    // Active / pending / recently-expired events. Indexed by
    // (tickAt, slotIndex, eventId). Expired rows live ~60s for
    // visibility in `.zones event list` before being pruned.
    std::vector<ActiveEvent> _activeEvents;
    // Per-event-spawn index so the reward chain can O(1)-identify a
    // looted event-boss spawn. Keyed by the creature's raw ObjectGuid
    // value (full guid, not just the low counter) to avoid collisions
    // between different creature entries that share a low counter.
    std::unordered_map<uint64 /*raw object guid*/,
                        std::pair<uint64, uint32>> _eventBossSpawnIndex;
    // Parallel map: guid → tier of the rotation slot the event fired
    // from. Captured at spawn (rotation may roll over before the boss
    // is killed; we want the spawn-time tier, not the current one).
    // GM-forced events use TIER_5 since slotIndex is 0xFFFF.
    std::unordered_map<uint64 /*raw object guid*/,
                        Tier> _eventBossTierMap;
    // Monotonic eventId generator inside a rotation. Reset at each
    // ScheduleEvents call so (tickAt, slotIndex, eventId) stays
    // unique without a persisted counter.
    uint32 _nextEventIdWithinRotation = 0;

    // Force-scale override read by OnBeforeCreatureSelectLevel. Set
    // by SpawnWorldBoss right before `map->SummonCreature` and
    // cleared immediately after. When non-zero, the hook scales
    // to that level regardless of zone-rotation state or the
    // `SkipWorldBosses` eligibility check. Atomic because the
    // reader (creature hook) and writer (spawn path) run on the
    // same thread in practice, but belt-and-suspenders for future
    // map-thread work.
    std::atomic<uint8> _eventBossScaleOverride{0};

    // Bridge for the event-boss HP/damage uplift across the spawn
    // race: `_eventBossSpawnIndex` (and the published snapshot's
    // `eventBossGuids`) only get the new GUID *after* SummonCreature
    // returns, but `OnAfterCreatureSelectLevel` fires *during* the
    // SummonCreature call. Without this flag the event-boss creature
    // gets normal-mob HP scaling instead of the +EventBoss.HpMult
    // uplift. Set true before SummonCreature, cleared after — same
    // single-thread invariant as `_eventBossScaleOverride`.
    std::atomic<bool> _eventBossSpawnPending{false};

    // Tier of the event being summoned RIGHT NOW. Set just before
    // SummonCreature alongside `_eventBossSpawnPending`, cleared
    // after. Read by `OnAfterCreatureSelectLevel` so the in-flight
    // boss gets its source rotation slot's tier even though the
    // GUID isn't yet in `_eventBossTierMap` / `eventBossTiers`.
    // 0 means "no override" (treat as not set).
    std::atomic<uint8> _eventBossTierOverride{0};

    // Slice 8 — combat difficulty. Applied post-SelectLevel (HP) +
    // at outgoing damage dispatch. Per-tier HP bonus composes
    // multiplicatively on top of the base mult; damage tier bonus is
    // flat 1.0 by default per plan §2.2 but tunable. Event-boss
    // uplift stacks on top when the attacker's GUID is in
    // `_eventBossSpawnIndex`.
    bool  _combatEnabled   = true;
    float _combatHpMult    = 2.0f;
    float _combatDamageMult = 1.3f;
    float _tierHpBonus[TIER_MAX + 1]     = {1.0f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
    float _tierDamageBonus[TIER_MAX + 1] = {1.0f, 1.0f, 1.0f,  1.0f, 1.0f,  1.0f};
    float _eventBossHpMultUplift     = 4.0f;
    float _eventBossDamageMultUplift = 1.75f;

    // Slice 8b — elite density per tier. Per-mille values (0..1000)
    // so the hot path can roll an integer mod-1000 instead of a
    // float comparison. Default ladder: T1/T2 = 0 (no promotion),
    // T3 = 150 (15%), T4 = 250 (25%), T5 = 400 (40%). Promoted
    // spawns get an additional HP/damage uplift composed on top of
    // the Slice 8 base × tier mult — feel is "1 in 4 mobs in this
    // T4 zone hits like a truck."
    uint32 _eliteDensityPerMille[TIER_MAX + 1] =
        {0, 0, 0, 150, 250, 400};
    float  _eliteHpMultUplift     = 1.5f;
    float  _eliteDamageMultUplift = 1.3f;

    bool  _eventBossLootPoolEnabled      = true;
    float _eventBossLootPurpleMultiplier = 1.0f;

    std::vector<LootPoolEntry> _eventBossLootPool;

    // Per-bundle dedup separate from _uniqueRolledBundles so that a
    // single boss-kill loot bundle can fire BOTH a Slice 4 unique
    // drop AND a Slice 8 event-boss bonus drop without the two
    // paths starving each other.
    std::unordered_set<uint64> _eventBossLootRolledBundles;
    uint64 _eventBossLootRolledBundlesClearedAt = 0;
    // Per-bundle event-boss gold target. Set by TryEventBossDrop
    // (during loot template iteration) so we can pre-seed loot.gold
    // (so the gold pile is visible in the loot window). Re-read by
    // ApplyEventBossGoldUplift in OnPlayerBeforeLootMoney to restore
    // the value if generateMoneyLoot clobbered it (Unit.cpp:14084
    // overwrites loot.gold AFTER FillLoot when creature has non-zero
    // native mingold/maxgold). Cleared on the same TTL as
    // _eventBossLootRolledBundles.
    std::unordered_map<uint64 /*bundleKey*/,
                        uint32 /*goldTarget*/> _eventBossGoldTargets;

    // Slice 9 Pass 1 — class-drop state. _classDropEntries holds the
    // populated cells loaded from `terror_zones_event_boss_class_drops`.
    // _classDropChance[t] is read at LoadConfig from
    // `TerrorZones.Items.DropChance.T<t>`. _classDropRolledBundles is
    // its own dedup set so a single boss kill can fire all three:
    // unique drop + event-boss bonus + class drop.
    std::unordered_set<uint32> _classDropEntries;
    std::unordered_set<uint64> _classDropRolledBundles;
    uint64 _classDropRolledBundlesClearedAt = 0;
    float _classDropChance[TIER_MAX + 1] =
        {0.0f, 0.0f, 0.10f, 0.20f, 0.35f, 0.60f};

    // State. All member containers below are world-thread-only —
    // see the snapshot doc comment above `CombatHotState`. Readers
    // outside the writer thread go through `_combatHot` /
    // `_rotationSnap` / `_eventsSnap` / `_eventBossSpawnSnap`.
    std::vector<PoolEntry> _pool;
    std::unordered_map<uint32, size_t> _poolIndex;
    ActiveRotation _rotation;
    uint64 _nextTickAt = 0;
    uint32 _tickAccumMs = 0;
    // Startup defer: on boot with a stale rotation window and no real
    // players online, hold off on picking zones until the first real
    // player logs in. Keeps targets=0 flat-random picks out of the pool.
    bool _rotationDeferredForFirstLogin = false;

    struct PlayerPref
    {
        bool announceEnabled;
        bool dirty;
        bool loaded;
        // Slice 7 — per-category bitmask. Defaults to 0xFF when the row
        // is missing (never opted out of any category).
        uint8 announceCategories = ANNOUNCE_CATEGORY_ALL;
        // Slice 7 — last empowered zone the player was tracked in. Used
        // to fire the zone-leave line on UpdateZone since AC's
        // `OnPlayerUpdateZone(player, newZone, newArea)` doesn't carry
        // the prior zone. In-memory only (no DB persistence) — the
        // entry path on login re-establishes it.
        uint32 lastEmpoweredZoneId = 0;
        std::string lastEmpoweredZoneName;
    };
    std::unordered_map<uint32 /*guidLow*/, PlayerPref> _prefs;
};

// Slice 9 Pass 1 — class-targeted event-boss drops. 5 playstyle
// archetypes (collapsed from the original 11 armor-class × stat
// combinations); items are class-agnostic and fit any class whose
// playstyle matches. ARCHETYPE_NONE is the fall-through for
// unrecognized class+spec combinations.
enum Archetype : uint8
{
    ARCHETYPE_NONE      = 0,
    ARCHETYPE_STR_DPS   = 1,  // Warrior Arms/Fury, Pally Ret, DK Frost/Unholy
    ARCHETYPE_TANK      = 2,  // Warrior Prot, Pally Prot, DK Blood
    ARCHETYPE_AGI_DPS   = 3,  // Hunter, Rogue, Shaman Enh, Druid Feral
    ARCHETYPE_CASTER    = 4,  // Mage, Warlock, Priest Shadow, Shaman Ele, Druid Bal
    ARCHETYPE_HEALER    = 5,  // Pally Holy, Priest Disc/Holy, Shaman Resto, Druid Resto
    ARCHETYPE_MAX       = ARCHETYPE_HEALER
};

// Twelve drop-eligible slots: 8 armor + 4 jewelry. Indexed 0..11 to
// match the entry encoder directly. Hidden armor (waist/wrists/feet)
// joins the original visible 5; jewelry slots (neck/finger/trinket/
// back) are class-agnostic by 3.3.5a's rules but stat-tagged per
// playstyle so each archetype gets its own jewelry suite.
enum ArmorSlot : uint8
{
    ARMOR_SLOT_HEAD      = 0,
    ARMOR_SLOT_NECK      = 1,
    ARMOR_SLOT_SHOULDERS = 2,
    ARMOR_SLOT_BACK      = 3,
    ARMOR_SLOT_CHEST     = 4,
    ARMOR_SLOT_WRISTS    = 5,
    ARMOR_SLOT_HANDS     = 6,
    ARMOR_SLOT_WAIST     = 7,
    ARMOR_SLOT_LEGS      = 8,
    ARMOR_SLOT_FEET      = 9,
    ARMOR_SLOT_FINGER    = 10,
    ARMOR_SLOT_TRINKET   = 11,
    ARMOR_SLOT_WEAPON    = 12,
    ARMOR_SLOT_COUNT     = 13
};

// Map a player class+spec to a playstyle archetype. `specIndex` is
// the 0-based talent tree the player has the most points in (0..2).
// Returns ARCHETYPE_NONE for unrecognized class IDs (everything
// outside CLASS_WARRIOR..CLASS_DRUID minus the gap at 10) or
// out-of-range specIndex. DK Blood maps to TANK per the spec; spec
// author intentionally chose tank-by-tree over the 3.3.5a
// DPS-by-default convention.
Archetype ArchetypeForClassSpec(uint8 classId, uint8 specIndex);

// Encode (band, tier, archetype, slot) to a custom item entry.
// Returns 0 on out-of-range input. The runtime drop hook uses this
// to look up which entry to inject; the offline Python generator
// (`docs/terror-zones/tools/tz_item_gen/`) computes the SAME number
// for SQL emission. Round-trip with DecodeClassDropEntry is
// unit-tested for all 1,920 cells. New encoding (post 5-playstyle
// collapse + 12-slot expansion):
//   per_band = 4 tiers * 5 archetypes * 12 slots = 240
//   per_tier = 5 archetypes * 12 slots = 60
//   per_archetype = 12 slots
// Reserved range: [700100, 702020).
uint32 EncodeClassDropEntry(uint8 bandIndex, Tier tier,
                             Archetype archetype, ArmorSlot slot);

// Inverse of EncodeClassDropEntry. Returns false (and leaves the
// out params unchanged) when `entry` is outside [700100, 702020)
// or when the decoded axis values fall out of range.
bool DecodeClassDropEntry(uint32 entry,
                           uint8& bandIndex, Tier& tier,
                           Archetype& archetype, ArmorSlot& slot);

} // namespace mod_terror_zones

#endif
