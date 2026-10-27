// LANywhere: panel para jugar en línea con el modo inalámbrico local.
//
// Muestra si el servidor responde, los grupos abiertos de todos los juegos (los crea cualquiera con "crear grupo"
// en el modo inalámbrico local) y permite elegir si los grupos propios se ven, o jugar en una sala privada por
// código. Todo pasa por el servicio "lanywhr" del sysmodule; la app no habla con el servidor.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include <turbojpeg.h>
#include "gfx.h"
#include "nacp_titles.h"

#define CONFIG_PATH  "sdmc:/lanywhere/config.ini"
#define MAX_GROUPS   8
#define REFRESH_MS   5000

// --- idioma: español si la consola está en español (de España o de Latinoamérica); si no, inglés ---

static bool g_es;
#define TR(es, en) (g_es ? (es) : (en))

static void detect_language(void)
{
    u64 code = 0;
    SetLanguage lang = SetLanguage_ENUS;
    if (R_SUCCEEDED(setInitialize())) {
        if (R_SUCCEEDED(setGetSystemLanguage(&code)))
            setMakeLanguage(code, &lang);
        setExit();
    }
    g_es = lang == SetLanguage_ES || lang == SetLanguage_ES419;
}

// Los errores del sysmodule llegan en español: traducción de los conocidos.
static const char *tr_error(const char *es)
{
    static const char *const map[][2] = {
        { "la sala no existe", "that room does not exist" },
        { "la sala está llena", "the room is full" },
        { "la sala es de otro juego", "that room is for another game" },
        { "no se pudo conectar con el servidor", "could not connect to the server" },
        { "el servidor no responde", "the server is not responding" },
        { "el servidor no contestó", "the server did not answer" },
        { "el servidor no recibe datos (UDP)", "the server is not receiving data (UDP)" },
        { "se perdió la conexión con el servidor", "lost the connection to the server" },
        { "reconectando con el servidor", "reconnecting to the server" },
        { "el puesto en la sala ya no existe", "your place in the room no longer exists" },
        { "el servidor llegó a su límite de este mes; vuelve el día 1", "the server reached its limit for this month; back on the 1st" },
        { "demasiadas salas; prueba en un momento", "too many rooms; try again in a moment" },
        { "demasiados intentos; prueba en un momento", "too many attempts; try again in a moment" },
    };
    if (g_es)
        return es;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(es, map[i][0]) == 0)
            return map[i][1];
    return es;
}

// --- servicio "lanywhr" ---

typedef struct {
    u8 state, my_index, member_count, padding;
    u32 my_ip;
    char code[8];
    u64 game_id;
    u32 packets_sent, packets_received;
    char error[64];
} LanywhereStatus;
_Static_assert(sizeof(LanywhereStatus) == 0x60, "igual que lanywhere::Status");

typedef struct {
    char code[8];
    u64 game_id;
    u8 players, max_players;
    u8 padding[6];
    char host[40];
} LanywhereGroup;
_Static_assert(sizeof(LanywhereGroup) == 0x40, "igual que lanywhere::GroupSummary");

enum { StateIdle = 0, StateConnecting = 1, StateInRoom = 2, StateError = 3 };

static Service g_lanywhere;
static Service g_lanywhere_list;  // sesión propia del hilo de la lista

static Result lanywhere_create(u64 game_id, const char *nick)
{
    return serviceDispatchIn(&g_lanywhere, 0, game_id,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { nick, strlen(nick) } });
}

static Result lanywhere_join(u64 game_id, const char *code, const char *nick)
{
    return serviceDispatchIn(&g_lanywhere, 1, game_id,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In, SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { code, strlen(code) }, { nick, strlen(nick) } });
}

static void lanywhere_leave(void)
{
    serviceDispatch(&g_lanywhere, 2);
}

static Result lanywhere_status(LanywhereStatus *out)
{
    return serviceDispatchOut(&g_lanywhere, 3, *out);
}

// Apodo con el que te ven los demás (el sysmodule crea uno al azar si no hay).
// El grupo abierto elegido: al buscar grupo en el juego, el sysmodule muestra solo ese (10 minutos).
static void lanywhere_prefer(const char *code)
{
    serviceDispatch(&g_lanywhere, 6,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
        .buffers = { { code, strlen(code) } });
}

static void lanywhere_nickname(char *out, size_t size)
{
    memset(out, 0, size);
    serviceDispatch(&g_lanywhere, 5,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { out, size - 1 } });
}

// Devuelve cuántos grupos dejó en out, o -1 si el servidor no responde.
static s32 lanywhere_list_groups(LanywhereGroup *out, size_t count)
{
    s32 n = -1;
    if (R_FAILED(serviceDispatchOut(&g_lanywhere_list, 4, n,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
            .buffers = { { out, count * sizeof(LanywhereGroup) } })))
        return -1;
    return n;
}

// --- juegos instalados: nombre e icono ---

typedef struct {
    u64 id;
    char name[128];
    Image icon;
    bool installed;
} GameInfo;
static GameInfo g_games[24];
static int g_game_count;

static void decode_icon(Image *img, const unsigned char *jpeg, size_t size)
{
    tjhandle tj = tjInitDecompress();
    int w, h, subsamp, colorspace;
    if (tj && tjDecompressHeader3(tj, jpeg, size, &w, &h, &subsamp, &colorspace) == 0 && w > 0 && h > 0 && w <= 1024 && h <= 1024) {
        img->px = malloc((size_t)w * h * 4);
        // TJPF_BGRA en memoria = 0xAARRGGBB como u32 (little-endian), el formato de gfx.
        if (img->px && tjDecompress2(tj, jpeg, size, (unsigned char *)img->px, w, w * 4, h, TJPF_BGRA, 0) == 0) {
            img->w = w;
            img->h = h;
        } else {
            free(img->px);
            img->px = NULL;
        }
    }
    if (tj)
        tjDestroy(tj);
}

static const GameInfo *game_info(u64 id)
{
    for (int i = 0; i < g_game_count; i++)
        if (g_games[i].id == id)
            return &g_games[i];
    if (g_game_count == (int)(sizeof(g_games) / sizeof(g_games[0])))
        return NULL;

    GameInfo *g = &g_games[g_game_count++];
    memset(g, 0, sizeof(*g));
    g->id = id;
    snprintf(g->name, sizeof(g->name), TR("Juego %016lX", "Game %016lX"), id);
    static NsApplicationControlData control;
    u64 size = 0;
    if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, id, &control, sizeof(control), &size)) &&
        size >= sizeof(control.nacp)) {
        g->installed = true;
        static const int preference[] = { 5, 6, 0, 1 };  // español latino, español, inglés (EE. UU. y RU)
        char name[0x200];
        nacp_name((const unsigned char *)&control.nacp, sizeof(control.nacp), preference, 4, name, sizeof(name));
        if (name[0])
            snprintf(g->name, sizeof(g->name), "%s", name);
        if (size > sizeof(control.nacp))
            decode_icon(&g->icon, control.icon, size - sizeof(control.nacp));
    }
    return g;
}

// --- config.ini: "public=0" oculta los grupos propios ---

static bool config_public(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    bool pub = true;
    char line[128];
    while (f && fgets(line, sizeof(line), f))
        if (strncmp(line, "public=", 7) == 0)
            pub = line[7] != '0';
    if (f)
        fclose(f);
    return pub;
}

// Cambia (o añade) "clave=valor" conservando las demás líneas (server=, port=...).
static void config_set(const char *key, const char *value)
{
    char rest[1024] = "";
    size_t key_len = strlen(key);
    FILE *f = fopen(CONFIG_PATH, "r");
    char line[128];
    while (f && fgets(line, sizeof(line), f))
        if (!(strncmp(line, key, key_len) == 0 && line[key_len] == '=') && line[0] != '\n' &&
            strlen(rest) + strlen(line) < sizeof(rest))
            strcat(rest, line);
    if (f)
        fclose(f);
    mkdir("sdmc:/lanywhere", 0777);
    f = fopen(CONFIG_PATH, "w");
    if (!f)
        return;
    fputs(rest, f);
    if (rest[0] && rest[strlen(rest) - 1] != '\n')
        fputc('\n', f);
    fprintf(f, "%s=%s\n", key, value);
    fclose(f);
}

static void config_set_public(bool pub)
{
    config_set("public", pub ? "1" : "0");
}

// Apodo nuevo: letras, números, espacios y . _ - ; de 2 a 20 caracteres. Devuelve false si no vale.
static bool clean_nickname(const char *in, char *out, size_t size)
{
    size_t o = 0, chars = 0;
    bool space = true;  // sin espacios al principio ni repetidos
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < size && chars < 20; ) {
        if (*p >= 0x80) {  // letra con tilde, ñ...: el carácter UTF-8 entero
            size_t n = *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : 2;
            for (size_t i = 0; i < n && p[i]; i++)
                out[o++] = p[i];
            p += n;
            chars++;
            space = false;
        } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                   *p == '.' || *p == '_' || *p == '-') {
            out[o++] = *p++;
            chars++;
            space = false;
        } else if (*p == ' ' && !space) {
            out[o++] = *p++;
            chars++;
            space = true;
        } else {
            p++;
        }
    }
    while (o > 0 && out[o - 1] == ' ') {
        o--;
        chars--;
    }
    out[o] = '\0';
    return chars >= 2;
}

// --- lista de grupos en un hilo (la consulta al servidor tarda) ---

static Mutex g_list_lock;
static LanywhereGroup g_groups[MAX_GROUPS];
static s32 g_group_count = -2;  // -2: aún no se sabe, -1: sin servidor
static volatile bool g_list_changed;
static volatile bool g_quit;
static UEvent g_refresh;

static void list_thread(void *arg)
{
    (void)arg;
    while (!g_quit) {
        LanywhereGroup groups[MAX_GROUPS];
        s32 n = lanywhere_list_groups(groups, MAX_GROUPS);
        mutexLock(&g_list_lock);
        if (n != g_group_count || memcmp(groups, g_groups, sizeof(groups)) != 0)
            g_list_changed = true;
        memcpy(g_groups, groups, sizeof(groups));
        g_group_count = n;
        mutexUnlock(&g_list_lock);
        waitSingle(waiterForUEvent(&g_refresh), REFRESH_MS * 1000000ULL);
    }
}

// --- pantalla ---

#define C_BG_TOP    RGBA(0x1B, 0x22, 0x36, 0xFF)
#define C_BG_BOTTOM RGBA(0x0F, 0x12, 0x1C, 0xFF)
#define C_TEXT      RGBA(0xF2, 0xF3, 0xF7, 0xFF)
#define C_DIM       RGBA(0x9A, 0xA0, 0xB0, 0xFF)
#define C_FAINT     RGBA(0x6B, 0x72, 0x85, 0xFF)
#define C_LINE      RGBA(0xFF, 0xFF, 0xFF, 0x1C)
#define C_CARD      RGBA(0xFF, 0xFF, 0xFF, 0x0F)
#define C_PANEL     RGBA(0xFF, 0xFF, 0xFF, 0x0A)
#define C_ACCENT    RGBA(0x8B, 0x5C, 0xF6, 0xFF)  // violeta de LANywhere (el del icono)
#define C_OK        RGBA(0x34, 0xC7, 0x59, 0xFF)
#define C_WARN      RGBA(0xF5, 0xA5, 0x24, 0xFF)
#define C_ERR       RGBA(0xF0, 0x4A, 0x4A, 0xFF)
#define C_SEAT_OFF  RGBA(0xFF, 0xFF, 0xFF, 0x26)
#define C_CODE      RGBA(0xFF, 0xD6, 0x4A, 0xFF)

#define MARGIN      56
#define LIST_W      760
#define SIDE_X      (MARGIN + LIST_W + 32)
#define SIDE_W      (GFX_W - SIDE_X - MARGIN)
#define FOOTER_Y    (GFX_H - 72)

// Zonas táctiles del último fotograma: tocar una equivale a pulsar su botón del mando.
typedef struct { int x, y, w, h; u64 button; } TouchZone;
static TouchZone g_zones[16];
#define TOUCH_CARD  (1ULL << 62)  // tocar la tarjeta del grupo n devuelve TOUCH_CARD | n
static int g_zone_count;

// Botón del mando según la letra que se dibuja ("−" para Minus).
static u64 button_mask(const char *label)
{
    switch (label[0]) {
    case 'A': return HidNpadButton_A;
    case 'B': return HidNpadButton_B;
    case 'X': return HidNpadButton_X;
    case 'Y': return HidNpadButton_Y;
    case '+': return HidNpadButton_Plus;
    default:  return HidNpadButton_Minus;
    }
}

static void add_zone(int x, int y, int w, int h, u64 button)
{
    if (g_zone_count < (int)(sizeof(g_zones) / sizeof(g_zones[0])))
        g_zones[g_zone_count++] = (TouchZone){ x, y, w, h, button };
}

// Botón del mando que corresponde a un toque nuevo en la pantalla (al levantar el dedo), o 0.
static u64 touch_button(void)
{
    static bool was_down;
    static u32 last_x, last_y;
    HidTouchScreenState ts = {0};
    bool down = hidGetTouchScreenStates(&ts, 1) && ts.count > 0;
    u64 pressed = 0;
    if (down) {
        last_x = ts.touches[0].x;
        last_y = ts.touches[0].y;
    } else if (was_down) {
        for (int i = 0; i < g_zone_count; i++) {
            const TouchZone *z = &g_zones[i];
            if ((int)last_x >= z->x && (int)last_x < z->x + z->w && (int)last_y >= z->y && (int)last_y < z->y + z->h)
                pressed = z->button;
        }
    }
    was_down = down;
    return pressed;
}

typedef struct {
    LanywhereStatus st;
    LanywhereGroup groups[MAX_GROUPS];
    s32 count;
    bool pub;
    bool private_room;
    char nickname[33];
    char message[128];
    int sel;  // grupo elegido en la lista, o -1
} View;

// Capturas sin mando: si aparece sdmc:/lanywhere/captura.flag (por ejemplo, copiado por FTP), el siguiente
// fotograma se guarda como sdmc:/lanywhere/captura-N.png y el archivo se borra.
#define CAPTURE_FLAG "sdmc:/lanywhere/captura.flag"
static bool g_capture_pending;

static bool capture_requested(void)
{
    struct stat st;
    return stat(CAPTURE_FLAG, &st) == 0;
}

static void save_capture_if_requested(void)
{
    if (!g_capture_pending)
        return;
    g_capture_pending = false;
    char path[64];
    struct stat st;
    for (int n = 1; n < 1000; n++) {
        snprintf(path, sizeof(path), "sdmc:/lanywhere/captura-%d.png", n);
        if (stat(path, &st) != 0)
            break;
    }
    gfx_save_png(path);
    remove(CAPTURE_FLAG);
}

static void status_pill(const View *v)
{
    const char *text = TR("Conectado", "Connected");
    u32 dot = C_OK;
    if (v->count == -2) {
        text = TR("Conectando…", "Connecting…");
        dot = C_WARN;
    } else if (v->count < 0) {
        text = TR("Sin conexión", "Offline");
        dot = C_ERR;
    }
    int tw = gfx_text_width(22, text);
    int w = 18 + 14 + 10 + tw + 20, x = GFX_W - MARGIN - w, y = 42;
    gfx_round_rect(x, y, w, 40, 20, C_CARD);
    gfx_round_rect(x + 18, y + 13, 14, 14, 7, dot);
    gfx_text(x + 42, y + 8, 22, C_TEXT, text);

    // Apodo: lo que ven los demás jugadores (nunca el nombre de usuario de la consola).
    if (v->nickname[0]) {
        int nw = gfx_text_width(22, v->nickname);
        int lw = gfx_text_width(16, TR("Tu apodo", "Your nickname"));
        int pw = 20 + (nw > lw ? nw : lw) + 20, px = x - 16 - pw;
        gfx_round_rect(px, y - 8, pw, 56, 18, C_CARD);
        gfx_text(px + 20, y - 4, 16, C_DIM, TR("Tu apodo", "Your nickname"));
        gfx_text(px + 20, y + 16, 22, C_CODE, v->nickname);
    }
}

static void seats(int x, int y, int players, int max)
{
    if (max <= 0 || max > 8)
        max = 8;
    for (int i = 0; i < max; i++)
        gfx_round_rect(x + i * 22, y, 16, 16, 8, i < players ? C_OK : C_SEAT_OFF);
}

static void group_card(int x, int y, int w, const LanywhereGroup *g, bool selected)
{
    const int h = 96;
    if (selected) {
        gfx_round_rect(x - 4, y - 4, w + 8, h + 8, 20, C_ACCENT);
        gfx_round_rect(x, y, w, h, 16, C_BG_TOP);
    }
    gfx_round_rect(x, y, w, h, 16, C_CARD);
    const GameInfo *info = game_info(g->game_id);
    if (info && info->icon.px) {
        gfx_image(&info->icon, x + 14, y + 14, 68, 68);
    } else {
        gfx_round_rect(x + 14, y + 14, 68, 68, 12, RGBA(0x3A, 0x40, 0x50, 0xFF));
        gfx_text(x + 36, y + 30, 30, C_DIM, "?");
    }
    int tx = x + 100, right = x + w - 24;
    char host[80];
    snprintf(host, sizeof(host), TR("Grupo de %s", "%s's group"), g->host[0] ? g->host : TR("alguien", "someone"));
    gfx_text_fit(tx, y + 16, 26, C_TEXT, info ? info->name : TR("Juego", "Game"), right - 190 - tx);
    gfx_text_fit(tx, y + 54, 20, C_DIM, host, right - 190 - tx);

    char count[16];
    snprintf(count, sizeof(count), "%u/%u", g->players, g->max_players);
    int cw = gfx_text_width(28, count);
    gfx_text(right - cw, y + 14, 28, C_TEXT, count);
    seats(right - 8 * 22 + 6, y + 58, g->players, g->max_players);
}

static void groups_column(const View *v)
{
    int x = MARGIN, y = 128;
    gfx_text(x, y, 28, C_TEXT, TR("Grupos abiertos ahora", "Open groups right now"));
    if (v->count > 0) {
        char n[8];
        snprintf(n, sizeof(n), "%d", v->count);
        int tw = gfx_text_width(28, TR("Grupos abiertos ahora", "Open groups right now")) + 14;
        int nw = gfx_text_width(20, n);
        gfx_round_rect(x + tw, y + 4, nw + 20, 30, 15, C_ACCENT);
        gfx_text(x + tw + 10, y + 8, 20, C_TEXT, n);
    }
    y += 52;

    if (v->count == -2) {
        gfx_text(x, y + 10, 22, C_DIM, TR("Buscando grupos…", "Looking for groups…"));
        return;
    }
    if (v->count < 0) {
        gfx_round_rect(x, y, LIST_W, 170, 18, C_PANEL);
        gfx_text(x + 28, y + 30, 26, C_TEXT, TR("No hay conexión con el servidor", "No connection to the server"));
        gfx_text(x + 28, y + 76, 20, C_DIM, TR("Revisa que la consola esté conectada a internet por Wi-Fi.", "Check that the console is connected to the internet over Wi-Fi."));
        gfx_text(x + 28, y + 106, 20, C_DIM, TR("La lista se vuelve a intentar sola cada pocos segundos.", "The list retries by itself every few seconds."));
        return;
    }
    if (v->count == 0) {
        gfx_round_rect(x, y, LIST_W, 200, 18, C_PANEL);
        gfx_text(x + 28, y + 30, 26, C_TEXT, TR("Todavía no hay grupos abiertos", "No open groups yet"));
        gfx_text(x + 28, y + 76, 20, C_DIM, TR("¡Sé el primero! Abre un juego, entra en el modo inalámbrico local", "Be the first! Open a game, go to its local wireless mode"));
        gfx_text(x + 28, y + 104, 20, C_DIM, TR("y crea un grupo: aparecerá aquí para jugadores de todo el mundo.", "and create a group: it will show up here for players everywhere."));
        gfx_text(x + 28, y + 146, 20, C_FAINT, TR("Los grupos llenos o que ya empezaron no se muestran.", "Full groups and groups that already started are not shown."));
        return;
    }
    int shown = v->count < 4 ? v->count : 4;
    for (int i = 0; i < shown; i++) {
        group_card(x, y + i * 108, LIST_W, &v->groups[i], v->sel == i);
        add_zone(x, y + i * 108, LIST_W, 96, TOUCH_CARD | (u64)i);
    }
    if (v->count > shown) {
        char more[64];
        snprintf(more, sizeof(more), TR("y %d más: búscalos desde el juego", "and %d more: find them from the game"), v->count - shown);
        gfx_text(x + 8, y + shown * 108 + 4, 20, C_DIM, more);
    }
}

static void step(int x, int y, int n, const char *text)
{
    char num[4];
    snprintf(num, sizeof(num), "%d", n);
    gfx_round_rect(x, y, 30, 30, 15, C_ACCENT);
    gfx_text(x + 15 - gfx_text_width(18, num) / 2, y + 5, 18, C_TEXT, num);
    gfx_text_fit(x + 44, y + 3, 20, C_TEXT, text, SIDE_W - 44 - 40);
}

static void toggle(int x, int y, bool on)
{
    gfx_round_rect(x, y, 56, 30, 15, on ? C_OK : C_SEAT_OFF);
    gfx_round_rect(on ? x + 29 : x + 3, y + 3, 24, 24, 12, C_TEXT);
}

static void side_column(const View *v)
{
    int x = SIDE_X, y = 128;

    gfx_round_rect(x, y, SIDE_W, 192, 18, C_PANEL);
    gfx_text(x + 24, y + 20, 24, C_TEXT, TR("Cómo jugar", "How to play"));
    step(x + 24, y + 66, 1, TR("Abre el juego", "Open the game"));
    step(x + 24, y + 106, 2, TR("Entra en inalámbrico local", "Go to local wireless"));
    step(x + 24, y + 146, 3, TR("Crea un grupo o únete a uno", "Create a group or join one"));
    y += 192 + 20;

    const int room_h = v->private_room ? 172 : v->st.state == StateError && v->st.error[0] ? 176 : 140;
    gfx_round_rect(x, y, SIDE_W, room_h, 18, C_PANEL);
    gfx_text(x + 24, y + 20, 24, C_TEXT, TR("Sala privada", "Private room"));
    if (v->private_room) {
        // Código grande, con espacio entre letras para dictarlo.
        int cx = x + 24;
        for (int i = 0; v->st.code[i] && i < 6; i++) {
            char c[2] = { v->st.code[i], 0 };
            gfx_round_rect(cx, y + 64, 48, 60, 10, RGBA(0xFF, 0xFF, 0xFF, 0x14));
            gfx_text(cx + 24 - gfx_text_width(36, c) / 2, y + 72, 36, C_CODE, c);
            cx += 56;
        }
        char who[64];
        if (v->st.member_count == 1)
            snprintf(who, sizeof(who), "%s", TR("Solo tú: pásale el código", "Just you: share the code"));
        else
            snprintf(who, sizeof(who), TR("%u jugadores en la sala", "%u players in the room"), v->st.member_count);
        gfx_text_fit(x + 24, y + 134, 20, C_DIM, who, SIDE_W - 48);
    } else if (v->st.state == StateInRoom) {
        gfx_text_fit(x + 24, y + 66, 20, C_DIM, TR("Ahora estás en un grupo abierto.", "You are in an open group right now."), SIDE_W - 48);
    } else {
        gfx_text_fit(x + 24, y + 66, 20, C_DIM, TR("Para jugar solo con un amigo:", "To play only with a friend:"), SIDE_W - 48);
        gfx_text_fit(x + 24, y + 94, 20, C_DIM, TR("crea una sala y pásale el código.", "create a room and share the code."), SIDE_W - 48);
        if (v->st.state == StateError && v->st.error[0])
            gfx_text_fit(x + 24, y + 136, 20, C_ERR, tr_error(v->st.error), SIDE_W - 48);
    }
    y += room_h + 20;

    gfx_round_rect(x, y, SIDE_W, 96, 18, C_PANEL);
    gfx_text(x + 24, y + 18, 22, C_TEXT, TR("Mis grupos visibles", "My groups are visible"));
    gfx_text_fit(x + 24, y + 54, 18, C_DIM, v->private_room ? TR("Solo en la sala privada", "Only in the private room") :
                 v->pub ? TR("Para todo el mundo", "To everyone") : TR("Solo en la red local", "Only on the local network"), SIDE_W - 120);
    toggle(x + SIDE_W - 24 - 56, y + 33, v->pub && !v->private_room);
    if (!v->private_room)
        add_zone(x, y, SIDE_W, 96, HidNpadButton_X);  // el panel entero cambia la visibilidad
}

static void draw(const View *v)
{
    gfx_begin();
    gfx_clear_gradient(C_BG_TOP, C_BG_BOTTOM);
    g_zone_count = 0;

    gfx_text(MARGIN, 36, 40, C_TEXT, "LANywhere");
    gfx_text(MARGIN, 86, 20, C_DIM, TR("Tu multijugador local, ahora en línea", "Your local multiplayer, online"));
    status_pill(v);

    groups_column(v);
    side_column(v);

    gfx_rect(0, FOOTER_Y, GFX_W, 1, C_LINE);
    if (v->message[0])
        gfx_text_fit(MARGIN, FOOTER_Y + 20, 20, C_CODE, v->message, 560);
    // Botones a la derecha, como en el menú de la consola.
    const char *buttons[5], *labels[5];
    int n = 0;
    if (v->private_room) {
        buttons[n] = "B"; labels[n++] = TR("Salir de la sala", "Leave room");
    } else if (v->st.state != StateInRoom) {
        buttons[n] = "A"; labels[n++] = TR("Crear sala", "Create room");
        buttons[n] = "Y"; labels[n++] = TR("Unirse con código", "Join with code");
    }
    if (!v->private_room) {
        buttons[n] = "X"; labels[n++] = v->pub ? TR("Ocultar", "Hide") : TR("Mostrar", "Show");
    }
    buttons[n] = "\u2212"; labels[n++] = TR("Apodo", "Nickname");
    if (v->sel >= 0) {  // grupo elegido en la lista: solo jugar o volver
        n = 0;
        buttons[n] = "A"; labels[n++] = TR("Jugar en este grupo", "Play in this group");
        buttons[n] = "B"; labels[n++] = TR("Volver", "Back");
    }
    buttons[n] = "+"; labels[n++] = TR("Salir", "Exit");
    int total = 0;
    for (int i = 0; i < n; i++)
        total += 30 + 10 + gfx_text_width(22, labels[i]) + (i ? 28 : 0);
    int x = GFX_W - MARGIN - total;
    for (int i = 0; i < n; i++) {
        int w = gfx_button(x, FOOTER_Y + 20, buttons[i], labels[i]);
        add_zone(x - 10, FOOTER_Y + 8, w + 20, 56, button_mask(buttons[i]));  // algo más grande que el dibujo, para el dedo
        x += w + 28;
    }

    save_capture_if_requested();
    gfx_end();
}

static void draw_busy(const char *text)
{
    gfx_begin();
    gfx_clear_gradient(C_BG_TOP, C_BG_BOTTOM);
    int w = gfx_text_width(28, text);
    gfx_text((GFX_W - w) / 2, GFX_H / 2 - 20, 28, C_TEXT, text);
    gfx_end();
}

static bool ask_code(char *out, size_t size)
{
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0)))
        return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, TR("Código de la sala", "Room code"));
    swkbdConfigSetGuideText(&kbd, TR("6 letras o números", "6 letters or numbers"));
    swkbdConfigSetStringLenMax(&kbd, 6);
    swkbdConfigSetStringLenMin(&kbd, 6);
    Result rc = swkbdShow(&kbd, out, size);
    swkbdClose(&kbd);
    for (char *p = out; *p; p++)
        if (*p >= 'a' && *p <= 'z')
            *p -= 32;
    return R_SUCCEEDED(rc) && strlen(out) == 6;
}

static bool ask_nickname(const char *current, char *out, size_t size)
{
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0)))
        return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, TR("Tu apodo", "Your nickname"));
    swkbdConfigSetSubText(&kbd, TR("Es lo que verán los demás. No uses tu nombre real.", "This is what others will see. Don't use your real name."));
    swkbdConfigSetGuideText(&kbd, TR("De 2 a 20 letras o números", "2 to 20 letters or numbers"));
    swkbdConfigSetInitialText(&kbd, current);
    swkbdConfigSetStringLenMax(&kbd, 20);
    Result rc = swkbdShow(&kbd, out, size);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc);
}


// Abre el juego del grupo elegido y le dice al sysmodule que, al buscar grupo en el juego, muestre ese. Devuelve
// true si el sistema va a abrir el juego (la app debe terminar).
static bool play_group(View *v)
{
    const LanywhereGroup *g = &v->groups[v->sel];
    const GameInfo *info = game_info(g->game_id);
    if (!info || !info->installed) {
        snprintf(v->message, sizeof(v->message), "%s", TR("No tienes este juego instalado.", "You don't have this game installed."));
        return false;
    }
    lanywhere_prefer(g->code);
    char busy[200];
    snprintf(busy, sizeof(busy), TR("Abriendo %s…", "Opening %s…"), info->name);
    draw_busy(busy);
    if (R_FAILED(appletRequestLaunchApplication(g->game_id, NULL))) {
        snprintf(v->message, sizeof(v->message), "%s", TR("Abre el juego desde el menú: en «Buscar grupo» te espera este grupo.",
                                                          "Open the game from the menu: this group will be waiting in \"Search for groups\"."));
        return false;
    }
    return true;
}

static void no_service(PadState *pad)
{
    gfx_begin();
    gfx_clear_gradient(C_BG_TOP, C_BG_BOTTOM);
    gfx_text(MARGIN, 36, 40, C_TEXT, "LANywhere");
    gfx_round_rect(MARGIN, 140, GFX_W - 2 * MARGIN, 200, 18, C_PANEL);
    gfx_text(MARGIN + 28, 170, 28, C_TEXT, TR("Falta el componente de LANywhere", "The LANywhere component is missing"));
    gfx_text(MARGIN + 28, 222, 22, C_DIM, TR("Copia la carpeta «atmosphere» del paquete en la raíz de la tarjeta SD", "Copy the \"atmosphere\" folder from the package to the root of the SD card"));
    gfx_text(MARGIN + 28, 254, 22, C_DIM, TR("y reinicia la consola.", "and restart the console."));
    gfx_rect(0, FOOTER_Y, GFX_W, 1, C_LINE);
    g_zone_count = 0;
    int w = gfx_button(GFX_W - MARGIN - 120, FOOTER_Y + 20, "+", TR("Salir", "Exit"));
    add_zone(GFX_W - MARGIN - 130, FOOTER_Y + 8, w + 20, 56, HidNpadButton_Plus);
    gfx_end();
    while (appletMainLoop()) {
        padUpdate(pad);
        if ((padGetButtonsDown(pad) | touch_button()) & HidNpadButton_Plus)
            break;
        svcSleepThread(33000000ULL);
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    hidInitializeTouchScreen();
    detect_language();
    if (!gfx_init())
        return 1;

    if (R_FAILED(smGetService(&g_lanywhere, "lanywhr"))) {
        no_service(&pad);
        gfx_exit();
        return 0;
    }
    nsInitialize();
    const char *nick = "";  // el sysmodule usa el apodo de config.ini

    mutexInit(&g_list_lock);
    ueventCreate(&g_refresh, true);
    Thread thread;
    bool thread_ok = R_SUCCEEDED(smGetService(&g_lanywhere_list, "lanywhr")) &&
                     R_SUCCEEDED(threadCreate(&thread, list_thread, NULL, NULL, 0x8000, 0x2C, -2)) &&
                     R_SUCCEEDED(threadStart(&thread));

    View v = {0};
    v.sel = -1;
    lanywhere_nickname(v.nickname, sizeof(v.nickname));
    View last = {0};
    u64 last_draw = 0, last_capture_check = 0;
    bool dirty = true;
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 touch = touch_button();
        int card = -1;  // tarjeta de grupo tocada
        if (touch & TOUCH_CARD) {
            card = (int)(touch & 0xFF);
            touch = 0;
        }
        u64 down = padGetButtonsDown(&pad) | touch;
        if (down & HidNpadButton_Plus)
            break;

        lanywhere_status(&v.st);
        mutexLock(&g_list_lock);
        v.count = g_group_count;
        memcpy(v.groups, g_groups, sizeof(v.groups));
        if (g_list_changed)
            dirty = true;
        g_list_changed = false;
        mutexUnlock(&g_list_lock);
        v.pub = config_public();
        v.private_room = v.st.state == StateInRoom && v.st.game_id == 0;

        // Grupos de la lista: se eligen con el dedo o con las flechas; A (o tocar otra vez el elegido) abre su juego.
        // Dentro de una sala no se puede entrar en un grupo abierto (el juego buscaría solo en la sala).
        const int shown = v.count > 0 ? (v.count < 4 ? v.count : 4) : 0;
        const bool can_pick = shown > 0 && v.st.state != StateInRoom;
        if (!can_pick)
            v.sel = -1;
        else if (v.sel >= shown)
            v.sel = shown - 1;
        if (card >= 0 && card < shown) {
            dirty = true;
            if (!can_pick)
                snprintf(v.message, sizeof(v.message), "%s", TR("Sal de la sala privada para unirte a un grupo abierto.", "Leave the private room to join an open group."));
            else if (v.sel == card)
                down |= HidNpadButton_A;
            else
                v.sel = card;
        }
        if (can_pick && (down & (HidNpadButton_AnyDown)))
            v.sel = v.sel < 0 ? 0 : (v.sel + 1 < shown ? v.sel + 1 : v.sel);
        if (can_pick && v.sel >= 0 && (down & (HidNpadButton_AnyUp)))
            v.sel--;

        if (v.sel >= 0 && (down & HidNpadButton_B)) {
            v.sel = -1;
        } else if (v.sel >= 0 && (down & HidNpadButton_A)) {
            if (play_group(&v))
                break;  // el sistema abre el juego: la app termina
        } else if (v.private_room && (down & HidNpadButton_B)) {
            lanywhere_leave();
            snprintf(v.message, sizeof(v.message), "%s", TR("Saliste de la sala privada.", "You left the private room."));
        } else if (!v.private_room && v.st.state != StateInRoom && (down & HidNpadButton_A)) {
            draw_busy(TR("Creando la sala…", "Creating the room…"));
            v.message[0] = '\0';
            if (R_FAILED(lanywhere_create(0, nick)))
                snprintf(v.message, sizeof(v.message), "%s", TR("No se pudo crear la sala. ¿Hay internet?", "Could not create the room. Is there internet?"));
            else
                snprintf(v.message, sizeof(v.message), "%s", TR("Sala creada: pásale el código a tu amigo.", "Room created: share the code with your friend."));
        } else if (!v.private_room && v.st.state != StateInRoom && (down & HidNpadButton_Y)) {
            char code[16] = {0};
            if (ask_code(code, sizeof(code))) {
                char busy[64];
                snprintf(busy, sizeof(busy), TR("Entrando en la sala %s…", "Joining room %s…"), code);
                draw_busy(busy);
                v.message[0] = '\0';
                if (R_FAILED(lanywhere_join(0, code, nick)))
                    snprintf(v.message, sizeof(v.message), TR("No se pudo entrar en la sala %s.", "Could not join room %s."), code);
                else
                    snprintf(v.message, sizeof(v.message), TR("Estás en la sala %s. Ya puedes abrir el juego.", "You are in room %s. You can open the game now."), code);
            }
        } else if (down & HidNpadButton_Minus) {
            char typed[64] = {0}, clean[64];
            if (ask_nickname(v.nickname, typed, sizeof(typed))) {
                if (clean_nickname(typed, clean, sizeof(clean))) {
                    config_set("apodo", clean);
                    lanywhere_nickname(v.nickname, sizeof(v.nickname));
                    snprintf(v.message, sizeof(v.message), "%s", v.st.state == StateInRoom ?
                             TR("Apodo cambiado: se usará en la próxima sala.", "Nickname changed: it will be used in the next room.") :
                             TR("Apodo cambiado.", "Nickname changed."));
                } else {
                    snprintf(v.message, sizeof(v.message), "%s", TR("Ese apodo no vale: usa de 2 a 20 letras o números.", "That nickname is not valid: use 2 to 20 letters or numbers."));
                }
            }
        } else if (!v.private_room && (down & HidNpadButton_X)) {
            config_set_public(!v.pub);
            v.pub = !v.pub;
            snprintf(v.message, sizeof(v.message), "%s", v.pub ? TR("Tus grupos se verán en todo el mundo.", "Your groups will be visible to everyone.") :
                                                           TR("Tus grupos solo se verán en la red local.", "Your groups will only be visible on the local network."));
        }
        if (down & (HidNpadButton_A | HidNpadButton_Y | HidNpadButton_B)) {
            lanywhere_status(&v.st);
            v.private_room = v.st.state == StateInRoom && v.st.game_id == 0;
            ueventSignal(&g_refresh);
        }

        // Solo se redibuja si algo cambió (o cada pocos segundos): dibujar en software gasta batería.
        u64 now = armTicksToNs(armGetSystemTick()) / 1000000;
        bool status_changed = v.st.state != last.st.state || v.st.member_count != last.st.member_count ||
                              strcmp(v.st.code, last.st.code) != 0 || strcmp(v.st.error, last.st.error) != 0;
        if (down || status_changed || v.pub != last.pub || strcmp(v.nickname, last.nickname) != 0 ||
            strcmp(v.message, last.message) != 0)
            dirty = true;
        if (now / 1000 != last_capture_check) {
            last_capture_check = now / 1000;
            if (capture_requested()) {
                g_capture_pending = true;
                dirty = true;
            }
        }
        if (dirty || now - last_draw >= 5000) {
            draw(&v);
            last = v;
            last_draw = now;
            dirty = false;
        }
        svcSleepThread(33000000ULL);
    }

    g_quit = true;
    ueventSignal(&g_refresh);
    if (thread_ok) {
        threadWaitForExit(&thread);
        threadClose(&thread);
    }
    serviceClose(&g_lanywhere_list);
    for (int i = 0; i < g_game_count; i++)
        free(g_games[i].icon.px);
    nsExit();
    serviceClose(&g_lanywhere);
    gfx_exit();
    return 0;
}
