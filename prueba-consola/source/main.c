// lanywhere-prueba: control de salas de LANywhere para las pruebas (antes de la app definitiva).
//
// Crea una sala o se une a una por código usando el servicio "lanywhr" del sysmodule. La sala sigue activa al salir
// de la app: luego se abre el juego y su modo inalámbrico local (crear o buscar grupo) va por el servidor.

#include <stdio.h>
#include <string.h>
#include <switch.h>

typedef struct {
    u8 state, my_index, member_count, padding;
    u32 my_ip;
    char code[8];
    u64 game_id;
    u32 packets_sent, packets_received;
    char error[64];
} LanywhereStatus;
_Static_assert(sizeof(LanywhereStatus) == 0x60, "igual que lanywhere::Status");

enum { StateIdle = 0, StateConnecting = 1, StateInRoom = 2, StateError = 3 };

static Service g_lanywhere;

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

static Result lanywhere_status(LanywhereStatus *out)
{
    return serviceDispatchOut(&g_lanywhere, 3, *out);
}

static void lanywhere_leave(void)
{
    serviceDispatch(&g_lanywhere, 2);
}

static const char *ip_str(u32 ip, char *buf)
{
    sprintf(buf, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return buf;
}

static bool ask_code(char *out, size_t size)
{
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0)))
        return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, "Codigo de la sala");
    swkbdConfigSetStringLenMax(&kbd, 6);
    swkbdConfigSetStringLenMin(&kbd, 6);
    Result rc = swkbdShow(&kbd, out, size);
    swkbdClose(&kbd);
    for (char *p = out; *p; p++)
        if (*p >= 'a' && *p <= 'z')
            *p -= 32;
    return R_SUCCEEDED(rc) && strlen(out) == 6;
}

static void show(void)
{
    LanywhereStatus st = {0};
    char a[20];
    lanywhere_status(&st);
    printf("\x1b[2J\x1b[1;1H");
    printf("LANywhere: salas (prueba)\n\n");
    switch (st.state) {
    case StateInRoom:
        printf("En la sala  %s   (%u jugadores)\n", st.code, st.member_count);
        printf("Mi IP virtual: %s\n", ip_str(st.my_ip, a));
        printf("Paquetes: %u enviados, %u recibidos\n\n", st.packets_sent, st.packets_received);
        printf("Pulsa + y abre el juego: su modo local (crear o buscar\n");
        printf("grupo) va por la sala.\n\n");
        break;
    case StateError:
        printf("Error: %s\n\n", st.error);
        break;
    default:
        printf("Sin sala.\n\n");
        break;
    }
    printf("A  crear sala\nY  unirse a una sala\nX  salir de la sala\n+  cerrar (la sala sigue activa)\n");
    consoleUpdate(NULL);
}

int main(int argc, char **argv)
{
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    Result rc = smGetService(&g_lanywhere, "lanywhr");
    if (R_FAILED(rc)) {
        printf("El sysmodule de LANywhere no esta activo (0x%X).\nPulsa + para salir.\n", rc);
        consoleUpdate(NULL);
        while (appletMainLoop()) {
            padUpdate(&pad);
            if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
                break;
            svcSleepThread(16000000ULL);
        }
        consoleExit(NULL);
        return 0;
    }

    u64 last = 0;
    show();
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus)
            break;
        if (down & HidNpadButton_A) {
            printf("\nCreando sala...\n");
            consoleUpdate(NULL);
            lanywhere_create(0, "consola");
        }
        if (down & HidNpadButton_Y) {
            char code[16] = {0};
            if (ask_code(code, sizeof(code))) {
                printf("\nUniendose a %s...\n", code);
                consoleUpdate(NULL);
                lanywhere_join(0, code, "consola");
            }
        }
        if (down & HidNpadButton_X)
            lanywhere_leave();

        u64 now = armTicksToNs(armGetSystemTick()) / 1000000;
        if (down || now - last >= 1000) {
            last = now;
            show();
        }
        svcSleepThread(16000000ULL);
    }

    serviceClose(&g_lanywhere);
    consoleExit(NULL);
    return 0;
}
