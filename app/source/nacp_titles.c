#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include "nacp_titles.h"

#define NACP_SIZE             0x4000
#define NACP_TITLES_FORMAT    0x3215  // 0: 16 títulos tal cual; 1: bloque DEFLATE (u16 tamaño + datos)
#define NACP_PLAIN_TITLES     16

int nacp_titles(const unsigned char *nacp, size_t size, unsigned char *out)
{
    if (!nacp || size < NACP_SIZE)
        return 0;
    if (nacp[NACP_TITLES_FORMAT] != 1) {
        memcpy(out, nacp, NACP_PLAIN_TITLES * NACP_TITLE_SIZE);
        return NACP_PLAIN_TITLES;
    }
    unsigned compressed = nacp[0] | nacp[1] << 8;
    if (compressed == 0 || compressed > NACP_PLAIN_TITLES * NACP_TITLE_SIZE - 2)
        return 0;

    // DEFLATE en bruto (sin cabecera zlib): wbits -15.
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, -15) != Z_OK)
        return 0;
    z.next_in = (Bytef *)nacp + 2;
    z.avail_in = compressed;
    z.next_out = out;
    z.avail_out = NACP_MAX_TITLES * NACP_TITLE_SIZE;
    int rc = inflate(&z, Z_FINISH);
    size_t produced = NACP_MAX_TITLES * NACP_TITLE_SIZE - z.avail_out;
    inflateEnd(&z);
    if ((rc != Z_STREAM_END && rc != Z_OK) || produced < NACP_TITLE_SIZE)
        return 0;
    memset(out + produced, 0, NACP_MAX_TITLES * NACP_TITLE_SIZE - produced);
    return (int)(produced / NACP_TITLE_SIZE);
}

int nacp_name(const unsigned char *nacp, size_t size, const int *preference, int preference_count, char *out,
              size_t out_size)
{
    static unsigned char titles[NACP_MAX_TITLES * NACP_TITLE_SIZE];
    out[0] = '\0';
    int count = nacp_titles(nacp, size, titles);
    for (int i = 0; i < preference_count; i++) {
        if (preference[i] >= count)
            continue;
        const char *t = (const char *)titles + preference[i] * NACP_TITLE_SIZE;
        if (t[0]) {
            snprintf(out, out_size, "%.*s", 0x200, t);
            return i;
        }
    }
    for (int lang = 0; lang < count; lang++) {
        const char *t = (const char *)titles + lang * NACP_TITLE_SIZE;
        if (t[0]) {
            snprintf(out, out_size, "%.*s", 0x200, t);
            break;
        }
    }
    return -1;
}
