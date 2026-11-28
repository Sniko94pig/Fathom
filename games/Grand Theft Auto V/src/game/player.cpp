#include "game/internal.h"
#include "ingame/log.h"
#include <algorithm>
#include <climits>
#include <cstdio>

namespace gtav {

namespace {

struct Proofs {
    int bullet = 0, fire = 0, explosion = 0, collision = 0, melee = 0, steam = 0, p6 = 0, drown = 0;
};

struct God {
    bool active = false;
    int ped = 0;
    int player = -1;
    bool wasInvincible = false;
    bool haveProofs = false;
    Proofs proofs;
} god;

struct Health {
    bool active = false;
    int ped = 0;
} health;

struct Police {
    bool active = false;
    int player = -1;
} police;

struct Money {
    bool active = false;
    int character = -1;
    int floor = 0;
    bool backed = false;
} money;

const char* kCashStat[3] = {"SP0_TOTAL_CASH", "SP1_TOTAL_CASH", "SP2_TOTAL_CASH"};

bool ReadProofs(int ped, Proofs& p) {
    return n::GET_ENTITY_PROOFS(ped, &p.bullet, &p.fire, &p.explosion, &p.collision, &p.melee, &p.steam, &p.p6, &p.drown) != 0;
}

void WriteProofs(int ped, const Proofs& p) {
    n::SET_ENTITY_PROOFS(ped, p.bullet, p.fire, p.explosion, p.collision, p.melee, p.steam, p.p6, p.drown);
}

void GodRelease() {
    if (!god.active) return;
    if (god.player >= 0 && !MapshotBusy()) n::SET_PLAYER_INVINCIBLE(god.player, god.wasInvincible ? 1 : 0);
    if (god.ped && n::DOES_ENTITY_EXIST(god.ped)) {
        if (god.haveProofs) WriteProofs(god.ped, god.proofs);
        else WriteProofs(god.ped, Proofs{});
        n::SET_DISABLE_HIGH_FALL_DEATH(god.ped, 0);
    }
    god = God{};
}

void GodTick() {
    if (!On("god_mode") || !st.ped) {
        GodRelease();
        return;
    }
    if (god.active && god.ped != st.ped) GodRelease();
    if (!god.active) {
        god.active = true;
        god.ped = st.ped;
        god.player = st.player;
        bool base = false;
        god.wasInvincible = MapshotBaseInvincible(base) ? base : n::GET_PLAYER_INVINCIBLE(st.player) != 0;
        god.haveProofs = ReadProofs(st.ped, god.proofs);
        LOG("god mode on for ped %d (was invincible %d)", st.ped, god.wasInvincible ? 1 : 0);
    }
    n::SET_PLAYER_INVINCIBLE(st.player, 1);
    n::SET_ENTITY_PROOFS(st.ped, 1, 1, 1, 1, 1, 1, 1, 1);
    n::SET_DISABLE_HIGH_FALL_DEATH(st.ped, 1);
    if (st.alive) {
        int hp = n::GET_ENTITY_HEALTH(st.ped), max = n::GET_ENTITY_MAX_HEALTH(st.ped);
        if (max > 0 && hp < max) n::SET_ENTITY_HEALTH(st.ped, max, 0, 0);
    }
}

void HealthRelease() {
    if (!health.active) return;
    if (health.ped && n::DOES_ENTITY_EXIST(health.ped)) n::SET_PED_SUFFERS_CRITICAL_HITS(health.ped, 1);
    health = Health{};
}

void HealthTick() {
    if (!On("unlimited_health") || !st.ped) {
        HealthRelease();
        return;
    }
    if (health.active && health.ped != st.ped) HealthRelease();
    if (!health.active) {
        health.active = true;
        health.ped = st.ped;
    }
    n::SET_PED_SUFFERS_CRITICAL_HITS(st.ped, 0);
    if (!st.alive) return;
    int hp = n::GET_ENTITY_HEALTH(st.ped), max = n::GET_ENTITY_MAX_HEALTH(st.ped);
    if (max > 0 && hp < max) n::SET_ENTITY_HEALTH(st.ped, max, 0, 0);
}

void StaminaTick() {
    if (!On("unlimited_stamina") || st.player < 0 || !st.alive) return;
    n::RESTORE_PLAYER_STAMINA(st.player, 1.0f);
}

void Heal() {
    if (!st.ped || !st.alive) {
        Note("Heal works when your character is alive");
        return;
    }
    int max = n::GET_ENTITY_MAX_HEALTH(st.ped);
    if (max > 0) n::SET_ENTITY_HEALTH(st.ped, max, 0, 0);
    int armor = n::GET_PLAYER_MAX_ARMOUR(st.player);
    n::SET_PED_ARMOUR(st.ped, armor > 0 ? armor : 100);
    n::CLEAR_PED_BLOOD_DAMAGE(st.ped);
    n::RESET_PED_VISIBLE_DAMAGE(st.ped);
}

void GiveStars() {
    int stars = std::clamp((int)std::lround(Value("wanted_level")), 0, 5);
    if (st.player < 0) return;
    if (stars == 0) {
        n::CLEAR_PLAYER_WANTED_LEVEL(st.player);
        Note("Wanted level cleared");
        return;
    }
    int cap = n::GET_MAX_WANTED_LEVEL();
    if (cap < stars) {
        Note("The game allows at most " + std::to_string(cap) + " stars right now");
        if (cap <= 0) return;
        stars = cap;
    }
    n::SET_PLAYER_WANTED_LEVEL(st.player, stars, 0);
    n::SET_PLAYER_WANTED_LEVEL_NOW(st.player, 0);
    Note("Wanted level set to " + std::to_string(stars) + (stars == 1 ? " star" : " stars"));
}

void PoliceRelease() {
    if (!police.active) return;
    if (police.player >= 0) {
        n::SET_POLICE_IGNORE_PLAYER(police.player, 0);
        n::SET_DISPATCH_COPS_FOR_PLAYER(police.player, 1);
    }
    police = Police{};
}

void PoliceTick() {
    if (!On("police_blind") || st.player < 0) {
        PoliceRelease();
        return;
    }
    if (police.active && police.player != st.player) PoliceRelease();
    police.active = true;
    police.player = st.player;
    n::SET_POLICE_IGNORE_PLAYER(st.player, 1);
    n::SET_DISPATCH_COPS_FOR_PLAYER(st.player, 0);
}

bool ReadCash(int character, int& cash) {
    if (character < 0 || character > 2) return false;
    int v = 0;
    if (!n::STAT_GET_INT(Joaat(kCashStat[character]), &v, -1)) return false;
    cash = v;
    return true;
}

bool WriteCash(int character, int cash) {
    if (character < 0 || character > 2) return false;
    return n::STAT_SET_INT(Joaat(kCashStat[character]), std::max(0, cash), 1) != 0;
}

void GiveMoney() {
    if (st.character < 0) {
        Note("Give money works when you play as Michael, Franklin or Trevor");
        return;
    }
    int amount = std::clamp((int)std::lround(Value("money_amount")), 0, 10000000);
    int cash = 0;
    if (!ReadCash(st.character, cash)) {
        Note("Could not read your money");
        return;
    }
    long long next = std::min<long long>((long long)cash + amount, INT_MAX);
    if (!WriteCash(st.character, (int)next)) {
        Note("Could not change your money");
        return;
    }
    if (money.active && money.character == st.character) money.floor = (int)next;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Added $%d, you now have $%lld", amount, next);
    Note(buf);
    LOG("money: character %d %d -> %lld", st.character, cash, next);
}

void MoneyTick() {
    if (!On("unlimited_money") || st.character < 0) {
        money.active = false;
        return;
    }
    if (!money.backed) {
        money.backed = true;
        BackupOnce();
    }
    int cash = 0;
    if (!ReadCash(st.character, cash)) return;
    if (!money.active || money.character != st.character) {
        money.active = true;
        money.character = st.character;
        money.floor = cash;
        return;
    }
    if (cash < money.floor) {
        WriteCash(st.character, money.floor);
        return;
    }
    money.floor = cash;
}

}

bool GodBaseInvincible(bool& value) {
    if (!god.active) return false;
    value = god.wasInvincible;
    return true;
}

void PlayerInit() {
    Ready("god_mode", true);
    Ready("unlimited_health", true);
    Ready("unlimited_stamina", true);
    Ready("heal", true);
    Ready("give_stars", true);
    Ready("police_blind", true);
    Ready("give_money", true);
    Ready("unlimited_money", true);
}

void PlayerTick() {
    GodTick();
    HealthTick();
    StaminaTick();
    PoliceTick();
    MoneyTick();
    if (Pressed("heal")) Heal();
    if (Pressed("give_stars")) GiveStars();
    if (Pressed("give_money")) GiveMoney();
}

void PlayerOff() {
    GodRelease();
    HealthRelease();
    PoliceRelease();
    money.active = false;
}

}
