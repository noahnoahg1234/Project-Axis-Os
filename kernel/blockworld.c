/*
 * BlockWorld - a small text-mode block sandbox for AxisOS.
 *
 * An original Minecraft-style game: a 2D side-view world of blocks you can
 * mine and place, with an inventory GUI, crafting, mobs, day/night, health,
 * survival and creative modes, a cheat console, and saved worlds on disk.
 *
 * Controls
 *   A / D      walk left / right        W or Space  jump (fly up in creative)
 *   S          crouch / fly down        Arrows      move the aim cursor
 *   F / Enter  mine or attack           G           place the selected block
 *   1..9       choose a hotbar slot     E           inventory / block picker
 *   C          crafting                 T or /      cheat console
 *   R          eat food                 M           save    Esc/Q  menu
 *   Mouse: move to aim, left-click mines, right-click places.
 */
#include "axis.h"

#define WW 220
#define WH 48
#define VIEW_W 78
#define VIEW_H 21   /* rows 1..21; row 0 status, 22 messages, 23-24 hotbar */

/* ---- blocks ------------------------------------------------------------ */
enum {
    AIR, GRASS, DIRT, STONE, COBBLE, LOG, LEAVES, SAND, WATER, PLANK, GLASS,
    COAL, IRON, GOLD, DIAMOND, TORCH, BEDROCK, SNOW, BRICK, TNT, CRAFT, LADDER,
    FLOWER, SAPLING, WOOL, PUMPKIN, OBSIDIAN, ICE, CACTUS, STICK, PORK, BONE,
    STRING, WHEAT, BLOCK_COUNT
};

typedef struct {
    const char *name;
    char glyph;
    uint8_t fg, bg;
    uint8_t solid;   /* blocks movement */
    uint8_t hardness;/* mining hits (0 = instant, 255 = unbreakable) */
    uint8_t placeable;
    uint8_t item;    /* not a world block (inventory-only item) */
} block_t;

static const block_t B[BLOCK_COUNT] = {
    [AIR]     = {"Air",        ' ',  C_BLACK,  C_BLACK, 0, 255, 0, 0},
    [GRASS]   = {"Grass",      '"',  C_GREEN,  C_LGREEN,1, 1, 1, 0},
    [DIRT]    = {"Dirt",       (char)0xB1, C_BROWN, C_BROWN, 1, 1, 1, 0},
    [STONE]   = {"Stone",      (char)0xB1, C_DGRAY, C_LGRAY, 1, 3, 1, 0},
    [COBBLE]  = {"Cobblestone",(char)0xB0, C_DGRAY, C_LGRAY, 1, 3, 1, 0},
    [LOG]     = {"Wood Log",   (char)0xB1, C_BLACK, C_BROWN, 1, 2, 1, 0},
    [LEAVES]  = {"Leaves",     (char)0xB0, C_GREEN, C_LGREEN,1, 1, 1, 0},
    [SAND]    = {"Sand",       (char)0xB0, C_BROWN, C_YELLOW,1, 1, 1, 0},
    [WATER]   = {"Water",      (char)0xF7, C_WHITE, C_BLUE,  0, 255, 1, 0},
    [PLANK]   = {"Planks",     (char)0xEC, C_BROWN, C_YELLOW,1, 2, 1, 0},
    [GLASS]   = {"Glass",      (char)0xB0, C_WHITE, C_LCYAN, 1, 1, 1, 0},
    [COAL]    = {"Coal Ore",   (char)0x04, C_BLACK, C_LGRAY, 1, 3, 1, 0},
    [IRON]    = {"Iron Ore",   (char)0x04, C_BROWN, C_LGRAY, 1, 4, 1, 0},
    [GOLD]    = {"Gold Ore",   (char)0x04, C_YELLOW,C_LGRAY, 1, 4, 1, 0},
    [DIAMOND] = {"Diamond Ore",(char)0x04, C_LCYAN, C_LGRAY, 1, 5, 1, 0},
    [TORCH]   = {"Torch",      (char)0x7C, C_YELLOW,C_BLACK, 0, 0, 1, 0},
    [BEDROCK] = {"Bedrock",    (char)0xB2, C_BLACK, C_DGRAY, 1, 255, 0, 0},
    [SNOW]    = {"Snow",       (char)0xB0, C_LGRAY, C_WHITE, 1, 1, 1, 0},
    [BRICK]   = {"Bricks",     (char)0xB1, C_LRED,  C_RED,   1, 3, 1, 0},
    [TNT]     = {"TNT",        'T',  C_WHITE, C_RED,   1, 1, 1, 0},
    [CRAFT]   = {"Crafting Table", '#', C_BLACK, C_BROWN, 1, 2, 1, 0},
    [LADDER]  = {"Ladder",     'H',  C_BROWN, C_BLACK, 0, 1, 1, 0},
    [FLOWER]  = {"Flower",     (char)0x0F, C_LRED, C_BLACK, 0, 0, 1, 0},
    [SAPLING] = {"Sapling",    (char)0x05, C_LGREEN,C_BLACK, 0, 0, 1, 0},
    [WOOL]    = {"Wool",       (char)0xB2, C_LGRAY, C_WHITE, 1, 1, 1, 0},
    [PUMPKIN] = {"Pumpkin",    (char)0x0F, C_BLACK, C_BROWN, 1, 2, 1, 0},
    [OBSIDIAN]= {"Obsidian",   (char)0xB2, C_LMAGENTA, C_BLACK, 1, 8, 1, 0},
    [ICE]     = {"Ice",        (char)0xB0, C_WHITE, C_LCYAN, 1, 1, 1, 0},
    [CACTUS]  = {"Cactus",     (char)0xB1, C_BLACK, C_GREEN, 1, 1, 1, 0},
    [STICK]   = {"Stick",      '/',  C_BROWN, C_BLACK, 0, 0, 0, 1},
    [PORK]    = {"Porkchop",   (char)0x05, C_LRED, C_BLACK, 0, 0, 0, 1},
    [BONE]    = {"Bone",       '|',  C_WHITE, C_BLACK, 0, 0, 0, 1},
    [STRING]  = {"String",     '~',  C_WHITE, C_BLACK, 0, 0, 0, 1},
    [WHEAT]   = {"Wheat",      (char)0x0F, C_YELLOW, C_BLACK, 0, 0, 0, 1},
};

static uint8_t world[WH][WW];

/* ---- state ------------------------------------------------------------- */
static int px, py;            /* player cell */
static int pvy;              /* jump counter (>0 = rising) */
static int facing = 1;
static int cx, cy;            /* aim cursor */
static int cam;              /* camera left column */
static bool creative, flying;
static int health = 20, food = 20;
static uint32_t wtime;       /* 0..24000 = one day */
static uint32_t seed;
static char world_name[32];
static int hurt_cd, regen_t, hunger_t;
static char msg[80];
static uint32_t msg_t;

typedef struct { uint8_t id, count; } slot_t;
static slot_t inv[36];       /* 0..8 hotbar */
static int hotbar;

typedef struct {
    uint8_t type, alive;
    int x, y, vy;
    int hp, timer, hurt;
} mob_t;
enum { M_PIG, M_COW, M_SHEEP, M_CHICKEN, M_ZOMBIE, M_SKELETON, M_SPIDER, M_CREEPER, MT_COUNT };
static const struct { const char *name; char glyph; uint8_t col; int hp; bool hostile; uint8_t drop, dropn; } MOB[MT_COUNT] = {
    {"Pig",      'p', C_LMAGENTA, 10, false, PORK, 2},
    {"Cow",      'C', C_WHITE,    10, false, PORK, 1},
    {"Sheep",    'S', C_LGRAY,    8,  false, WOOL, 1},
    {"Chicken",  'h', C_YELLOW,   4,  false, PORK, 1},
    {"Zombie",   'Z', C_LGREEN,   16, true,  IRON, 0},
    {"Skeleton", 'K', C_WHITE,    12, true,  BONE, 2},
    {"Spider",   'm', C_DGRAY,    12, true,  STRING, 2},
    {"Creeper",  '&', C_GREEN,    16, true,  COAL, 0},
};
#define MAXMOB 24
static mob_t mobs[MAXMOB];

static uint32_t rng;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int irnd(int n) { return n > 0 ? (int)(rnd() % (uint32_t)n) : 0; }

static bool solid_at(int x, int y) {
    if (x < 0 || x >= WW || y < 0 || y >= WH) return x < 0 || x >= WW; /* walls solid, sky open */
    return B[world[y][x]].solid;
}
static bool is_day(void) { return wtime < 12000; }

static void say(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    msg_t = ticks;
}

/* ---- world generation -------------------------------------------------- */
static int ground[WW];
static void gen_tree(int x, int top) {
    int h = 3 + irnd(3);
    for (int i = 0; i < h; i++) if (top - i >= 0) world[top - i][x] = LOG;
    int ly = top - h;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            int lx = x + dx, yy = ly + dy;
            if (lx > 0 && lx < WW && yy >= 0 && world[yy][lx] == AIR && (dx || dy)) world[yy][lx] = LEAVES;
        }
    for (int dx = -1; dx <= 1; dx++) if (x+dx>0&&x+dx<WW&&ly-1>=0&&world[ly-1][x+dx]==AIR) world[ly-1][x+dx]=LEAVES;
}

static void worldgen(uint32_t sd) {
    rng = sd ? sd : 1;
    memset(world, AIR, sizeof world);
    int base = WH * 55 / 100;
    int h = base;
    for (int x = 0; x < WW; x++) {
        /* wandering heightmap */
        h += irnd(3) - 1;
        if (h < 8) h = 8;
        if (h > WH - 6) h = WH - 6;
        /* smooth-ish */
        ground[x] = h;
    }
    for (int x = 2; x < WW - 2; x++) ground[x] = (ground[x-1]+ground[x]+ground[x+1])/3;
    bool desert = irnd(2);
    for (int x = 0; x < WW; x++) {
        int g = ground[x];
        bool sandy = ((x / 24) % 3 == 0);
        for (int y = g; y < WH; y++) {
            if (y == WH - 1) world[y][x] = BEDROCK;
            else if (y == g) world[y][x] = sandy ? SAND : GRASS;
            else if (y < g + 4) world[y][x] = sandy ? SAND : DIRT;
            else world[y][x] = STONE;
        }
        (void)desert;
    }
    /* ores in stone */
    for (int i = 0; i < WW * 4; i++) {
        int x = irnd(WW), y = ground[x] + 5 + irnd(WH - ground[x] - 7);
        if (y < 0 || y >= WH || world[y][x] != STONE) continue;
        int r = irnd(100);
        world[y][x] = r < 45 ? COAL : r < 72 ? IRON : r < 90 ? GOLD : DIAMOND;
    }
    /* caves: random carve */
    for (int i = 0; i < WW / 3; i++) {
        int x = irnd(WW), y = ground[x] + 4 + irnd(WH - ground[x] - 6), len = 8 + irnd(24);
        int dx = irnd(3) - 1, dy = irnd(3) - 1;
        for (int s = 0; s < len; s++) {
            if (x > 0 && x < WW && y > 0 && y < WH - 1 && world[y][x] != BEDROCK) world[y][x] = AIR;
            if (x+1<WW && y<WH-1 && world[y][x+1]!=BEDROCK) world[y][x+1]=AIR;
            x += dx; y += dy;
            if (irnd(4) == 0) dx = irnd(3) - 1;
            if (irnd(4) == 0) dy = irnd(3) - 1;
            if (x < 1 || x >= WW - 1 || y < ground[x < WW ? x : 0] || y >= WH - 1) break;
        }
    }
    /* water pools in dips */
    for (int x = 1; x < WW - 1; x++) {
        int g = ground[x];
        if (ground[x-1] > g && ground[x+1] > g && world[g][x] != WATER) {
            for (int y = g; y < g + 2 && y < WH; y++) if (world[y][x]==AIR||world[y][x]==GRASS) world[y][x]=WATER;
        }
    }
    /* trees & flowers on grass */
    for (int x = 3; x < WW - 3; x += 1) {
        int g = ground[x];
        if (world[g][x] == GRASS) {
            if (irnd(10) == 0) gen_tree(x, g - 1);
            else if (irnd(8) == 0 && g-1>=0) world[g-1][x] = irnd(2) ? FLOWER : SAPLING;
        }
    }
}

/* ---- inventory --------------------------------------------------------- */
static void inv_add(int id, int n) {
    if (creative) return;
    for (int i = 0; i < 36 && n > 0; i++)
        if (inv[i].id == id && inv[i].count > 0 && inv[i].count < 99) {
            int add = 99 - inv[i].count; if (add > n) add = n;
            inv[i].count += add; n -= add;
        }
    for (int i = 0; i < 36 && n > 0; i++)
        if (inv[i].count == 0) { inv[i].id = id; inv[i].count = n > 99 ? 99 : n; n -= inv[i].count; }
    if (n > 0) say("Inventory full!");
}
static int held_id(void) { return creative ? inv[hotbar].id : (inv[hotbar].count ? inv[hotbar].id : AIR); }
static void consume_held(void) {
    if (creative) return;
    if (inv[hotbar].count > 0 && --inv[hotbar].count == 0) inv[hotbar].id = AIR;
}
static int inv_count(int id) { int c = 0; for (int i = 0; i < 36; i++) if (inv[i].id == id) c += inv[i].count; return c; }
static void inv_remove(int id, int n) {
    for (int i = 0; i < 36 && n > 0; i++)
        if (inv[i].id == id && inv[i].count > 0) {
            int t = inv[i].count < n ? inv[i].count : n;
            inv[i].count -= t; n -= t; if (!inv[i].count) inv[i].id = AIR;
        }
}

/* ---- mobs -------------------------------------------------------------- */
static void spawn_mob(int type, int x, int y) {
    for (int i = 0; i < MAXMOB; i++)
        if (!mobs[i].alive) {
            mobs[i] = (mob_t){(uint8_t)type, 1, x, y, 0, MOB[type].hp, irnd(100), 0};
            return;
        }
}
static int surface(int x) {
    if (x < 0 || x >= WW) return WH / 2;
    for (int y = 0; y < WH; y++) if (B[world[y][x]].solid) return y - 1;
    return WH - 2;
}
static void try_spawn(void) {
    int alive = 0; for (int i = 0; i < MAXMOB; i++) alive += mobs[i].alive;
    if (alive >= MAXMOB - 4) return;
    int x = px + (irnd(2) ? 1 : -1) * (20 + irnd(20));
    if (x < 2 || x >= WW - 2) return;
    int y = surface(x);
    if (is_day()) { if (irnd(3)) return; spawn_mob(irnd(4), x, y); }      /* passive by day */
    else { if (irnd(2)) return; spawn_mob(M_ZOMBIE + irnd(4), x, y); }    /* hostile by night */
}

/* ---- physics ----------------------------------------------------------- */
static void try_move(int dx) {
    int nx = px + dx;
    if (nx < 0 || nx >= WW) return;
    if (!solid_at(nx, py)) { px = nx; facing = dx > 0 ? 1 : -1; }
    else if (!solid_at(nx, py - 1) && !solid_at(px, py - 1)) { px = nx; py--; facing = dx > 0 ? 1 : -1; } /* step up */
}
static void physics(void) {
    bool onground = solid_at(px, py + 1);
    bool onladder = world[py][px] == LADDER;
    if (flying) { pvy = 0; return; }
    if (onladder) { pvy = 0; return; }
    if (pvy > 0) { if (!solid_at(px, py - 1)) { py--; pvy--; } else pvy = 0; }
    else if (!onground) { if (py + 1 < WH) py++; }
    if (py >= WH - 1) { py = WH - 2; }
}

static void damage_player(int d) {
    if (creative || hurt_cd > 0) return;
    health -= d; hurt_cd = 8;
    if (health <= 0) {
        say("You died! Respawning...");
        health = 20; food = 20;
        px = WW / 2; py = surface(px);
        for (int i = 0; i < MAXMOB; i++) mobs[i].alive = 0;
    }
}

static void mob_tick(void) {
    for (int i = 0; i < MAXMOB; i++) {
        mob_t *m = &mobs[i];
        if (!m->alive) continue;
        if (m->hurt) m->hurt--;
        /* despawn far away */
        if (m->x < px - 60 || m->x > px + 60) { m->alive = 0; continue; }
        /* gravity */
        if (!solid_at(m->x, m->y + 1) && m->y + 1 < WH) m->y++;
        bool host = MOB[m->type].hostile;
        if (host) {
            int dir = (m->x < px) ? 1 : -1;
            if (irnd(3) != 0) {
                int nx = m->x + dir;
                if (!solid_at(nx, m->y)) m->x = nx;
                else if (!solid_at(nx, m->y - 1)) { m->x = nx; m->y--; } /* jump obstacle */
            }
            if (m->x == px && (m->y == py || m->y == py - 1)) {
                damage_player(m->type == M_CREEPER ? 6 : 2);
                if (m->type == M_CREEPER) m->alive = 0; /* goes boom */
            }
        } else {
            if (irnd(6) == 0) { int dir = irnd(2) ? 1 : -1; if (!solid_at(m->x + dir, m->y)) m->x += dir; }
        }
    }
}

/* ---- actions ----------------------------------------------------------- */
static bool in_reach(int x, int y) {
    int dx = x - px, dy = y - py;
    int r = creative ? 10 : 5;
    return dx*dx + dy*dy <= r*r;
}
static mob_t *mob_at(int x, int y) {
    for (int i = 0; i < MAXMOB; i++) if (mobs[i].alive && mobs[i].x == x && mobs[i].y == y) return &mobs[i];
    return NULL;
}
static void do_mine(void) {
    if (!in_reach(cx, cy)) { say("Too far away."); return; }
    mob_t *m = mob_at(cx, cy);
    if (m) {
        int dmg = creative ? 99 : 3 + irnd(3);
        m->hp -= dmg; m->hurt = 3;
        if (m->hp <= 0) {
            m->alive = 0;
            if (MOB[m->type].drop && MOB[m->type].dropn) inv_add(MOB[m->type].drop, 1 + irnd(MOB[m->type].dropn));
            say("Defeated a %s!", MOB[m->type].name);
        }
        return;
    }
    int b = world[cy][cx];
    if (b == AIR || B[b].hardness == 255) { if (b != AIR) say("%s can't be mined.", B[b].name); return; }
    world[cy][cx] = AIR;
    /* drops */
    int drop = b;
    if (b == GRASS) drop = DIRT;
    else if (b == STONE) drop = COBBLE;
    else if (b == COAL) drop = COAL;
    else if (b == LEAVES) { drop = (irnd(10)==0)? SAPLING : -1; }
    else if (b == FLOWER || b == SAPLING) drop = b;
    if (drop >= 0) inv_add(drop, 1);
    /* water/sand fall-ish: leave simple */
}
static void do_place(void) {
    int id = held_id();
    if (id == AIR || B[id].item) { say("Nothing to place."); return; }
    if (!in_reach(cx, cy)) { say("Too far away."); return; }
    if (world[cy][cx] != AIR && world[cy][cx] != WATER) return;
    if (mob_at(cx, cy)) return;
    if (cx == px && cy == py && B[id].solid) return;
    world[cy][cx] = (uint8_t)id;
    consume_held();
    if (id == TNT) {
        /* fun: tnt blows a crater */
        for (int dy = -3; dy <= 3; dy++) for (int dx = -3; dx <= 3; dx++) {
            int x = cx+dx, y = cy+dy;
            if (x>0&&x<WW&&y>0&&y<WH-1&&dx*dx+dy*dy<=9&&B[world[y][x]].hardness!=255) world[y][x]=AIR;
        }
        say("BOOM!");
    }
}

/* ---- rendering --------------------------------------------------------- */
static uint8_t sky_attr(int worldy, int col) {
    bool night = !is_day();
    uint8_t bg = night ? C_BLACK : (worldy < WH/4 ? C_LBLUE : C_LCYAN);
    /* stars at night */
    if (night && ((col * 7 + worldy * 13) % 29) == 0) return ATTR(C_WHITE, bg);
    return ATTR(bg, bg);
}
static void draw_cell(int sx, int sy, int wx, int wy) {
    if (wx < 0 || wx >= WW || wy < 0 || wy >= WH) { scr_put(sx, sy, ' ', ATTR(C_BLACK, C_BLACK)); return; }
    int id = world[wy][wx];
    if (id == AIR) {
        /* sun/moon */
        uint8_t a = sky_attr(wy, wx);
        scr_put(sx, sy, (a & 0x0F) == C_WHITE && (a>>4)==C_BLACK ? (char)0xF9 : ' ', a);
        return;
    }
    uint8_t a = ATTR(B[id].fg, B[id].bg);
    if (!is_day() && id != TORCH) {
        /* dim surface at night a touch: keep simple, torches glow */
    }
    scr_put(sx, sy, B[id].glyph, a);
}

static void render(void) {
    cam = px - VIEW_W / 2;
    if (cam < 0) cam = 0;
    if (cam > WW - VIEW_W) cam = WW - VIEW_W;
    int top = py - VIEW_H / 2;
    if (top < 0) top = 0;
    if (top > WH - VIEW_H) top = WH - VIEW_H;
    /* world */
    for (int sy = 0; sy < VIEW_H; sy++)
        for (int sx = 0; sx < VIEW_W; sx++)
            draw_cell(sx + 1, sy + 1, cam + sx, top + sy);
    /* mobs */
    for (int i = 0; i < MAXMOB; i++) {
        mob_t *m = &mobs[i];
        if (!m->alive) continue;
        int sx = m->x - cam, sy = m->y - top;
        if (sx >= 0 && sx < VIEW_W && sy >= 0 && sy < VIEW_H)
            scr_put(sx + 1, sy + 1, MOB[m->type].glyph, ATTR(m->hurt ? C_LRED : MOB[m->type].col, C_BLACK));
    }
    /* cursor */
    int csx = cx - cam, csy = cy - top;
    if (csx >= 0 && csx < VIEW_W && csy >= 0 && csy < VIEW_H) {
        char g = world[cy][cx] != AIR ? B[world[cy][cx]].glyph : (char)0xFB;
        uint8_t fg = in_reach(cx, cy) ? C_WHITE : C_DGRAY;
        scr_put(csx + 1, csy + 1, g == ' ' ? (char)0xFB : g, ATTR(fg, C_MAGENTA));
    }
    /* player */
    int psx = px - cam, psy = py - top;
    scr_put(psx + 1, psy + 1, facing > 0 ? (char)0x02 : (char)0x02, ATTR(hurt_cd ? C_LRED : C_YELLOW, C_BLACK));
    /* status bar */
    scr_fill(0, 0, SCREEN_W, 1, ' ', ATTR(C_BLACK, C_LGRAY));
    int hearts = (health + 1) / 2;
    char hb[24]; int o = 0;
    for (int i = 0; i < 10; i++) hb[o++] = i < hearts ? (char)0x03 : (char)0xFA;
    hb[o] = 0;
    scr_printf(1, 0, ATTR(C_LRED, C_LGRAY), "%s", hb);
    scr_printf(14, 0, ATTR(C_BROWN, C_LGRAY), "Food %2d", food);
    scr_printf(24, 0, ATTR(C_BLACK, C_LGRAY), "%s", creative ? "CREATIVE" : "SURVIVAL");
    scr_printf(36, 0, ATTR(C_BLACK, C_LGRAY), "XY %d,%d", px, py);
    scr_printf(50, 0, ATTR(C_BLACK, C_LGRAY), "%s %02d:%02d", is_day() ? "Day" : "Night",
               (wtime * 24 / 24000) % 24, (wtime * 1440 / 24000) % 60);
    scr_printf(66, 0, ATTR(C_BLACK, C_LGRAY), "%.12s", world_name);
    /* message line */
    scr_fill(0, VIEW_H + 1, SCREEN_W, 1, ' ', ATTR(C_WHITE, C_BLACK));
    if (msg[0] && ticks - msg_t < 3 * TICK_HZ) scr_puts(1, VIEW_H + 1, msg, ATTR(C_YELLOW, C_BLACK));
    else scr_puts(1, VIEW_H + 1, "A/D move  W jump  arrows aim  F mine  G place  E inv  C craft  T cheats  Q menu", ATTR(C_DGRAY, C_BLACK));
    /* hotbar */
    scr_fill(0, 23, SCREEN_W, 2, ' ', ATTR(C_WHITE, C_DGRAY));
    for (int i = 0; i < 9; i++) {
        int x = 2 + i * 8;
        uint8_t sel = i == hotbar ? C_YELLOW : C_LGRAY;
        scr_put(x - 1, 23, i == hotbar ? (char)0x10 : ' ', ATTR(C_YELLOW, C_DGRAY));
        scr_printf(x, 23, ATTR(sel, C_DGRAY), "%d", i + 1);
        int id = inv[i].id;
        if (creative || inv[i].count) {
            scr_put(x + 2, 23, B[id].glyph == ' ' ? '.' : B[id].glyph, ATTR(B[id].fg, B[id].bg));
            if (!creative) scr_printf(x + 4, 23, ATTR(C_WHITE, C_DGRAY), "%d", inv[i].count);
        }
    }
    int hid = held_id();
    scr_printf(2, 24, ATTR(C_WHITE, C_DGRAY), "Holding: %-16s", hid == AIR ? "(nothing)" : B[hid].name);
}

/* ---- screens ----------------------------------------------------------- */
int tui_prompt(const char *title, const char *label, char *buf, size_t n);

static void inventory_screen(void) {
    static uint16_t sv[SCREEN_W * SCREEN_H];
    scr_save(sv);
    int sel = 0;
    /* creative: show all placeable blocks; survival: show inventory */
    int list[BLOCK_COUNT], nl = 0;
    if (creative) { for (int i = 1; i < BLOCK_COUNT; i++) if (B[i].placeable || !B[i].item) list[nl++] = i; }
    for (;;) {
        scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_WHITE, C_BLACK));
        scr_box(14, 2, 52, 20, ATTR(C_WHITE, C_BLUE), creative ? " Blocks (creative) " : " Inventory ");
        if (creative) {
            for (int i = 0; i < nl; i++) {
                int col = i % 2, row = i / 2;
                int x = 17 + col * 25, y = 4 + row;
                if (y > 20) break;
                uint8_t a = i == sel ? ATTR(C_BLACK, C_LGRAY) : ATTR(C_WHITE, C_BLUE);
                scr_fill(x - 1, y, 24, 1, ' ', a);
                scr_put(x, y, B[list[i]].glyph, ATTR(B[list[i]].fg, B[list[i]].bg));
                scr_printf(x + 2, y, a, "%s", B[list[i]].name);
            }
            scr_puts(16, 21, "Enter: put in slot   Arrows: move   Esc: close", ATTR(C_LCYAN, C_BLUE));
        } else {
            for (int i = 0; i < 36; i++) {
                int x = 17 + (i % 9) * 5, y = 4 + (i / 9) * 2;
                uint8_t a = i == sel ? ATTR(C_BLACK, C_LGRAY) : ATTR(C_WHITE, C_BLUE);
                scr_fill(x - 1, y, 4, 1, ' ', a);
                if (inv[i].count) { scr_put(x, y, B[inv[i].id].glyph, ATTR(B[inv[i].id].fg, B[inv[i].id].bg)); scr_printf(x + 1, y, a, "%d", inv[i].count); }
                else scr_put(x, y, (char)0xFA, a);
            }
            scr_puts(16, 12, "Top row (1-9) is the hotbar.", ATTR(C_LCYAN, C_BLUE));
            if (inv[sel].count) scr_printf(16, 14, ATTR(C_YELLOW, C_BLUE), "Selected: %s x%d", B[inv[sel].id].name, inv[sel].count);
            scr_puts(16, 21, "Enter: move to hotbar slot 1   Arrows: move   Esc: close", ATTR(C_LCYAN, C_BLUE));
        }
        int k = kbd_getkey();
        if (k == KEY_ESC || k == 'e' || k == 'E' || k == 'q') break;
        int cols = creative ? 2 : 9, cnt = creative ? nl : 36;
        if (k == KEY_LEFT && sel > 0) sel--;
        else if (k == KEY_RIGHT && sel < cnt - 1) sel++;
        else if (k == KEY_UP && sel >= cols) sel -= cols;
        else if (k == KEY_DOWN && sel + cols < cnt) sel += cols;
        else if (k == '\n') {
            if (creative) { inv[hotbar].id = list[sel]; inv[hotbar].count = 1; say("Selected %s", B[list[sel]].name); break; }
            else if (inv[sel].count) { slot_t t = inv[0]; inv[0] = inv[sel]; inv[sel] = t; }
        }
    }
    scr_restore(sv);
}

/* small set of recipes */
static void craft_screen(void) {
    static const struct { const char *name; uint8_t in1, n1, in2, n2, out, outn; } R[] = {
        {"Planks (from 1 log)",        LOG, 1, AIR, 0, PLANK, 4},
        {"Sticks (from 2 planks)",     PLANK, 2, AIR, 0, STICK, 4},
        {"Crafting Table (4 planks)",  PLANK, 4, AIR, 0, CRAFT, 1},
        {"Torch (coal + stick)",       COAL, 1, STICK, 1, TORCH, 4},
        {"Ladder (7 sticks)",          STICK, 7, AIR, 0, LADDER, 3},
        {"Glass (smelt sand)",         SAND, 1, AIR, 0, GLASS, 1},
        {"TNT (sand + coal x4)",       SAND, 4, COAL, 4, TNT, 1},
        {"Bricks (4 cobble)",          COBBLE, 4, AIR, 0, BRICK, 2},
    };
    int n = sizeof R / sizeof *R;
    static uint16_t sv[SCREEN_W * SCREEN_H];
    scr_save(sv);
    int sel = 0;
    for (;;) {
        scr_fill(0, 0, SCREEN_W, SCREEN_H, ' ', ATTR(C_WHITE, C_BLACK));
        scr_box(18, 3, 44, 18, ATTR(C_WHITE, C_BROWN), " Crafting ");
        for (int i = 0; i < n; i++) {
            if (R[i].outn == 0) continue;
            bool can = creative || (inv_count(R[i].in1) >= R[i].n1 && (R[i].in2 == AIR || inv_count(R[i].in2) >= R[i].n2));
            uint8_t a = i == sel ? ATTR(C_BLACK, C_LGRAY) : ATTR(can ? C_WHITE : C_DGRAY, C_BROWN);
            scr_fill(20, 5 + i, 40, 1, ' ', a);
            scr_printf(21, 5 + i, a, "%s", R[i].name);
        }
        scr_puts(20, 20, "Enter: craft   Arrows: choose   Esc: close", ATTR(C_YELLOW, C_BROWN));
        int k = kbd_getkey();
        if (k == KEY_ESC || k == 'c' || k == 'C' || k == 'q') break;
        if (k == KEY_UP) { do { sel = (sel + n - 1) % n; } while (R[sel].outn == 0); }
        else if (k == KEY_DOWN) { do { sel = (sel + 1) % n; } while (R[sel].outn == 0); }
        else if (k == '\n') {
            if (R[sel].outn == 0) continue;
            bool can = creative || (inv_count(R[sel].in1) >= R[sel].n1 && (R[sel].in2 == AIR || inv_count(R[sel].in2) >= R[sel].n2));
            if (!can) { say("Not enough materials."); continue; }
            if (!creative) { inv_remove(R[sel].in1, R[sel].n1); if (R[sel].in2 != AIR) inv_remove(R[sel].in2, R[sel].n2); }
            inv_add(R[sel].out, R[sel].outn);
            say("Crafted %s x%d", B[R[sel].out].name, R[sel].outn);
        }
    }
    scr_restore(sv);
}

static int find_block_name(const char *s) {
    for (int i = 1; i < BLOCK_COUNT; i++) {
        char low[24]; strlcpy(low, B[i].name, sizeof low); str_lower(low);
        if (strcasestr(B[i].name, s) || !strcasecmp(low, s)) return i;
    }
    if (isdigit(s[0])) { int n = atoi(s); if (n > 0 && n < BLOCK_COUNT) return n; }
    return -1;
}
static void console(void) {
    char line[80] = "";
    scr_fill(0, VIEW_H + 1, SCREEN_W, 1, ' ', ATTR(C_WHITE, C_BLACK));
    scr_puts(1, VIEW_H + 1, "/", ATTR(C_LGREEN, C_BLACK));
    con_set_cursor(2, VIEW_H + 1);
    con_show_cursor(true);
    if (tui_input(2, VIEW_H + 1, 70, line, sizeof line, ATTR(C_WHITE, C_BLACK)) != 1) { con_show_cursor(false); return; }
    con_show_cursor(false);
    char *argv[6]; int argc = 0;
    for (char *t = strtok_simple(line); t && argc < 6; t = strtok_simple(NULL)) argv[argc++] = t;
    if (!argc) return;
    const char *c = argv[0];
    if (*c == '/') c++;
    if (!strcasecmp(c, "help")) say("creative survival give tp time heal fly spawn killall seed");
    else if (!strcasecmp(c, "creative")) { creative = true; health = 20; say("Creative mode on"); }
    else if (!strcasecmp(c, "survival")) { creative = flying = false; say("Survival mode on"); }
    else if (!strcasecmp(c, "gamemode") && argc > 1) { creative = (argv[1][0]=='c'||argv[1][0]=='1'); if(creative)health=20; else flying=false; say("Gamemode %s", creative?"creative":"survival"); }
    else if (!strcasecmp(c, "give") && argc > 1) { int b = find_block_name(argv[1]); if (b > 0) { int n = argc > 2 ? atoi(argv[2]) : 1; bool cr=creative; creative=false; inv_add(b, n?n:1); creative=cr; say("Gave %d %s", n?n:1, B[b].name); } else say("Unknown block"); }
    else if (!strcasecmp(c, "tp") && argc > 2) { px = atoi(argv[1]); py = atoi(argv[2]); if(px<0)px=0;if(px>=WW)px=WW-1;if(py<0)py=0;if(py>=WH)py=WH-1; say("Teleported"); }
    else if (!strcasecmp(c, "time") && argc > 1) { if (!strcasecmp(argv[1],"day")) wtime=1000; else if(!strcasecmp(argv[1],"night"))wtime=14000; else wtime=(uint32_t)atoi(argv[1])%24000; say("Time set"); }
    else if (!strcasecmp(c, "heal")) { health = 20; food = 20; say("Healed"); }
    else if (!strcasecmp(c, "fly")) { flying = !flying; say("Fly %s", flying?"on":"off"); }
    else if (!strcasecmp(c, "spawn") && argc > 1) { for(int t=0;t<MT_COUNT;t++) if(strcasestr(MOB[t].name,argv[1])){ spawn_mob(t, px+facing*3, py); say("Spawned %s",MOB[t].name); break; } }
    else if (!strcasecmp(c, "killall")) { for (int i=0;i<MAXMOB;i++) mobs[i].alive=0; say("Cleared mobs"); }
    else if (!strcasecmp(c, "seed")) say("Seed: %u", seed);
    else say("Unknown command - try /help");
}

/* ---- worlds (save/load) ------------------------------------------------ */
static int worlds_dir(bool create) {
    int d = fs_lookup(FS_ROOT, "Worlds");
    if (d < 0 && create) d = fs_create(FS_ROOT, "Worlds", FT_DIR);
    return d;
}
typedef struct __attribute__((packed)) {
    char magic[4]; uint8_t ver; char name[32]; uint32_t seed, wtime;
    uint16_t px, py; uint8_t creative, health, food, hotbar;
} wsave_t;

static bool world_save(void) {
    int d = worlds_dir(true);
    if (d < 0) { say("Cannot save (no disk)"); return false; }
    uint32_t cap = sizeof(wsave_t) + sizeof inv + WW * WH + 512;
    uint8_t *buf = kmalloc(cap);
    if (!buf) return false;
    wsave_t h; memset(&h, 0, sizeof h);
    memcpy(h.magic, "AXWL", 4); h.ver = 1; strlcpy(h.name, world_name, 32);
    h.seed = seed; h.wtime = wtime; h.px = px; h.py = py; h.creative = creative; h.health = health; h.food = food; h.hotbar = hotbar;
    uint32_t o = 0;
    memcpy(buf, &h, sizeof h); o = sizeof h;
    memcpy(buf + o, inv, sizeof inv); o += sizeof inv;
    /* RLE blocks */
    uint8_t *w = (uint8_t *)world;
    for (uint32_t i = 0; i < WW * WH;) {
        uint8_t v = w[i]; uint32_t run = 1;
        while (i + run < WW * WH && w[i + run] == v && run < 255) run++;
        buf[o++] = (uint8_t)run; buf[o++] = v; i += run;
    }
    char fn[48]; snprintf(fn, sizeof fn, "/Worlds/%s.wld", world_name);
    int id = fs_mkfile(fn, FS_ROOT);
    int r = id < 0 ? id : fs_write(id, buf, o);
    kfree(buf);
    if (r < 0) { say("Save failed"); return false; }
    if (cfg.autosave) fs_save();
    say("World saved.");
    return true;
}
static bool world_load(const char *name) {
    char fn[48]; snprintf(fn, sizeof fn, "/Worlds/%s.wld", name);
    int id = fs_resolve(fn, FS_ROOT);
    if (id < 0 || fs_nodes[id].type != FT_FILE) return false;
    const uint8_t *buf = fs_nodes[id].data; uint32_t len = fs_nodes[id].size;
    if (len < sizeof(wsave_t) + sizeof inv) return false;
    wsave_t h; memcpy(&h, buf, sizeof h);
    if (memcmp(h.magic, "AXWL", 4)) return false;
    strlcpy(world_name, h.name, 32); seed = h.seed; wtime = h.wtime;
    px = h.px; py = h.py; creative = h.creative; health = h.health; food = h.food; hotbar = h.hotbar;
    uint32_t o = sizeof h;
    memcpy(inv, buf + o, sizeof inv); o += sizeof inv;
    uint8_t *w = (uint8_t *)world; uint32_t wi = 0;
    while (o + 1 < len && wi < WW * WH) {
        uint8_t run = buf[o++], v = buf[o++];
        while (run-- && wi < WW * WH) w[wi++] = v;
    }
    for (int i = 0; i < MAXMOB; i++) mobs[i].alive = 0;
    return true;
}

static void default_inv(void) {
    memset(inv, 0, sizeof inv);
    if (creative) {
        int starter[9] = {GRASS, STONE, LOG, PLANK, GLASS, TORCH, SAND, BRICK, TNT};
        for (int i = 0; i < 9; i++) inv[i].id = starter[i];
    } else {
        inv[0] = (slot_t){TORCH, 10}; inv[1] = (slot_t){PLANK, 20};
    }
}

/* world-select / new-world menu; returns true to start, false to quit */
static bool world_menu(void) {
    int d = worlds_dir(false);
    for (;;) {
        char names[16][32]; int nw = 0;
        if (d >= 0) {
            int kids[FS_MAX_NODES]; int k = fs_children(d, kids, FS_MAX_NODES, false);
            for (int i = 0; i < k && nw < 16; i++) {
                const char *nm = fs_nodes[kids[i]].name; int l = strlen(nm);
                if (l > 4 && !strcasecmp(nm + l - 4, ".wld")) { strlcpy(names[nw], nm, 28); names[nw][l-4] = 0; nw++; }
            }
        }
        const char *items[20]; int ni = 0;
        items[ni++] = "+ Create a new world";
        int first_world = ni;
        for (int i = 0; i < nw; i++) items[ni++] = names[i];
        items[ni++] = "Quit BlockWorld";
        static int sel = 0; if (sel >= ni) sel = ni - 1;
        scr_fill(0, 0, SCREEN_W, SCREEN_H, (char)0xB0, ATTR(C_GREEN, C_BLACK));
        scr_fill(0, 2, SCREEN_W, 3, ' ', ATTR(C_WHITE, C_GREEN));
        scr_puts(28, 3, "B L O C K W O R L D", ATTR(C_WHITE, C_GREEN));
        scr_puts(24, 5, "a small block sandbox for AxisOS", ATTR(C_LGREEN, C_BLACK));
        scr_box(24, 7, 32, 13, ATTR(C_WHITE, C_BLACK), " Worlds ");
        for (int i = 0; i < ni; i++) {
            uint8_t a = i == sel ? ATTR(C_BLACK, C_LGREEN) : ATTR(C_WHITE, C_BLACK);
            scr_fill(26, 9 + i, 28, 1, ' ', a);
            scr_printf(27, 9 + i, a, "%s", items[i]);
        }
        scr_puts(20, 22, "Up/Down choose   Enter select   D delete world   Esc quit", ATTR(C_DGRAY, C_BLACK));
        int k = kbd_getkey();
        if (k == KEY_ESC || k == 'q') return false;
        else if (k == KEY_UP) sel = (sel + ni - 1) % ni;
        else if (k == KEY_DOWN) sel = (sel + 1) % ni;
        else if ((k == 'd' || k == 'D') && sel >= first_world && sel < first_world + nw) {
            char fn[48]; snprintf(fn, sizeof fn, "/Worlds/%s.wld", names[sel - first_world]);
            int id = fs_resolve(fn, FS_ROOT); if (id >= 0) fs_delete(id, false);
            if (cfg.autosave) fs_save();
        } else if (k == '\n') {
            if (sel == 0) { /* new world */
                char nm[28] = ""; char sd[16] = "";
                if (!tui_prompt("New world", "Name (e.g. MyWorld):", nm, sizeof nm)) continue;
                if (!nm[0]) strcpy(nm, "World");
                if (!fs_valid_name(nm)) { say("Bad name"); continue; }
                tui_prompt("New world", "Seed (blank = random):", sd, sizeof sd);
                bool cr = tui_confirm("Game mode", "Play in Creative mode? (No = Survival)");
                strlcpy(world_name, nm, 32);
                seed = sd[0] ? (uint32_t)strtol(sd, NULL, 0) : (ticks * 2654435761u + 12345u);
                creative = cr; flying = false; health = 20; food = 20; wtime = 1000; hotbar = 0;
                worldgen(seed); px = WW / 2; py = surface(px); default_inv();
                return true;
            } else if (sel == ni - 1) return false;
            else {
                if (world_load(items[sel])) return true;
                say("Could not load world");
            }
        }
    }
}

int blockworld_run(void) {
    static uint16_t saved[SCREEN_W * SCREEN_H];
    scr_save(saved);
    con_show_cursor(false);
    if (!world_menu()) { scr_restore(saved); con_show_cursor(true); return 0; }
    cx = px + facing; cy = py;
    uint32_t last_phys = 0, last_mob = 0, last_spawn = 0, last_save = ticks;
    bool running = true;
    while (running) {
        render();
        /* input (non-blocking, drain) */
        int k;
        while ((k = kbd_trygetkey()) >= 0) {
            if (k == 'q' || k == 'Q' || k == KEY_ESC) {
                if (tui_confirm("Leave", "Save and return to the menu?")) world_save();
                running = false; break;
            }
            else if (k == 'a' || k == 'A') { try_move(-1); if (cx > px + 6) cx = px; }
            else if (k == 'd' || k == 'D') { try_move(1); if (cx < px - 6) cx = px; }
            else if (k == 'w' || k == 'W' || k == ' ') { if (flying) { if (!solid_at(px, py-1)) py--; } else if (solid_at(px, py+1) || world[py][px]==LADDER) pvy = 3; }
            else if (k == 's' || k == 'S') { if (flying && !solid_at(px, py+1)) py++; else if (world[py][px]==LADDER && !solid_at(px,py+1)) py++; }
            else if (k == KEY_UP) { if (cy > 0) cy--; }
            else if (k == KEY_DOWN) { if (cy < WH - 1) cy++; }
            else if (k == KEY_LEFT) { if (cx > 0) cx--; }
            else if (k == KEY_RIGHT) { if (cx < WW - 1) cx++; }
            else if (k == 'f' || k == 'F' || k == '\n') do_mine();
            else if (k == 'g' || k == 'G') do_place();
            else if (k >= '1' && k <= '9') hotbar = k - '1';
            else if (k == 'e' || k == 'E') inventory_screen();
            else if (k == 'c' || k == 'C') craft_screen();
            else if (k == 't' || k == 'T' || k == '/') console();
            else if (k == 'm' || k == 'M') world_save();
            else if (k == 'r' || k == 'R') { if (!creative && food < 20 && inv_count(PORK)) { inv_remove(PORK,1); food = food+6>20?20:food+6; health = health+4>20?20:health+4; say("Yum!"); } }
            else if (k == 'z' || k == 'Z') flying = creative ? !flying : flying; /* creative fly toggle */
        }
        /* mouse: aim + click */
        if (mouse_present()) {
            int mxp, myp; mouse_pos(&mxp, &myp);
            if (myp >= 1 && myp <= VIEW_H && mxp >= 1 && mxp <= VIEW_W) {
                cx = cam + (mxp - 1); cy = (py - VIEW_H/2 < 0 ? 0 : (py - VIEW_H/2 > WH-VIEW_H ? WH-VIEW_H : py-VIEW_H/2)) + (myp - 1);
                uint8_t b = mouse_buttons();
                static uint8_t pb;
                if ((b & 1)) do_mine();
                if ((b & 2) && !(pb & 2)) do_place();
                pb = b;
            }
        }
        /* timed updates */
        if (ticks - last_phys >= 4) { last_phys = ticks; physics(); if (hurt_cd) hurt_cd--;
            /* keep cursor near player by default when idle */
        }
        if (ticks - last_mob >= 8) { last_mob = ticks; mob_tick(); }
        if (ticks - last_spawn >= 40) { last_spawn = ticks; try_spawn(); }
        /* time + hunger + regen */
        wtime += 2; if (wtime >= 24000) wtime = 0;
        if (!creative) {
            if (++hunger_t > 600) { hunger_t = 0; if (food > 0) food--; else damage_player(1); }
            if (health < 20 && food >= 18 && ++regen_t > 200) { regen_t = 0; health++; }
        }
        if (ticks - last_save >= 60 * TICK_HZ) { last_save = ticks; world_save(); }
        /* drown / cactus / lava-ish hazards */
        if (world[py][px] == WATER) { if (++hunger_t % 50 == 0) {} }
        if (world[py][px] == CACTUS) damage_player(1);
        cpu_hlt();
    }
    scr_restore(saved);
    con_show_cursor(true);
    return 0;
}
