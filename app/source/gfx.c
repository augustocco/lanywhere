#include <string.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "gfx.h"

static Framebuffer g_fb;
static u32 *g_px;
static u32 g_stride;  // en píxeles
static FT_Library g_ft;
static FT_Face g_face_std, g_face_ext;
static bool g_have_ext;

bool gfx_init(void)
{
    if (R_FAILED(framebufferCreate(&g_fb, nwindowGetDefault(), GFX_W, GFX_H, PIXEL_FORMAT_RGBA_8888, 2)) ||
        R_FAILED(framebufferMakeLinear(&g_fb)))
        return false;

    PlFontData std, ext;
    if (R_FAILED(plInitialize(PlServiceType_User)) ||
        R_FAILED(plGetSharedFontByType(&std, PlSharedFontType_Standard)) ||
        FT_Init_FreeType(&g_ft) ||
        FT_New_Memory_Face(g_ft, std.address, std.size, 0, &g_face_std))
        return false;
    g_have_ext = R_SUCCEEDED(plGetSharedFontByType(&ext, PlSharedFontType_NintendoExt)) &&
                 !FT_New_Memory_Face(g_ft, ext.address, ext.size, 0, &g_face_ext);
    return true;
}

void gfx_exit(void)
{
    if (g_have_ext)
        FT_Done_Face(g_face_ext);
    FT_Done_Face(g_face_std);
    FT_Done_FreeType(g_ft);
    plExit();
    framebufferClose(&g_fb);
}

void gfx_begin(void)
{
    u32 stride_bytes;
    g_px = framebufferBegin(&g_fb, &stride_bytes);
    g_stride = stride_bytes / 4;
}

void gfx_end(void)
{
    framebufferEnd(&g_fb);
}

// El framebuffer es RGBA_8888 en memoria (R en el byte bajo); los colores de la API son 0xAARRGGBB.
static inline u32 to_fb(u32 c)
{
    return (c & 0xFF00FF00) | (c >> 16 & 0xFF) | (c & 0xFF) << 16;
}

static inline void blend(u32 *dst, u32 argb, u32 extra_alpha)
{
    u32 a = ((argb >> 24) * extra_alpha) / 255;
    if (a == 0)
        return;
    u32 src = to_fb(argb);
    if (a == 255) {
        *dst = src | 0xFF000000;
        return;
    }
    u32 d = *dst, inv = 255 - a;
    u32 r = ((src & 0xFF) * a + (d & 0xFF) * inv) / 255;
    u32 g = ((src >> 8 & 0xFF) * a + (d >> 8 & 0xFF) * inv) / 255;
    u32 b = ((src >> 16 & 0xFF) * a + (d >> 16 & 0xFF) * inv) / 255;
    *dst = 0xFF000000 | b << 16 | g << 8 | r;
}

void gfx_clear_gradient(u32 top, u32 bottom)
{
    for (int y = 0; y < GFX_H; y++) {
        u32 t = (u32)y * 255 / (GFX_H - 1);
        u32 c = 0xFF000000;
        for (int s = 0; s < 24; s += 8) {
            u32 a = top >> s & 0xFF, b = bottom >> s & 0xFF;
            c |= ((a * (255 - t) + b * t) / 255) << s;
        }
        u32 fb = to_fb(c);
        u32 *row = g_px + (size_t)y * g_stride;
        for (int x = 0; x < GFX_W; x++)
            row[x] = fb;
    }
}

void gfx_rect(int x, int y, int w, int h, u32 color)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > GFX_W ? GFX_W : x + w, y1 = y + h > GFX_H ? GFX_H : y + h;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            blend(&g_px[(size_t)yy * g_stride + xx], color, 255);
}

void gfx_round_rect(int x, int y, int w, int h, int r, u32 color)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int yy = 0; yy < h; yy++) {
        int py = y + yy;
        if (py < 0 || py >= GFX_H)
            continue;
        // Distancia al borde redondeado en esta fila: con suavizado de 1 px.
        int dy = yy < r ? r - yy : (yy >= h - r ? yy - (h - r - 1) : 0);
        for (int xx = 0; xx < w; xx++) {
            int px = x + xx;
            if (px < 0 || px >= GFX_W)
                continue;
            int dx = xx < r ? r - xx : (xx >= w - r ? xx - (w - r - 1) : 0);
            u32 cover = 255;
            if (dx && dy) {
                float d = __builtin_sqrtf((float)(dx * dx + dy * dy)) - (float)r;
                if (d >= 1.0f)
                    continue;
                if (d > 0.0f)
                    cover = (u32)((1.0f - d) * 255.0f);
            }
            blend(&g_px[(size_t)py * g_stride + px], color, cover);
        }
    }
}

void gfx_image(const Image *img, int x, int y, int w, int h)
{
    if (!img || !img->px)
        return;
    for (int yy = 0; yy < h; yy++) {
        int py = y + yy;
        if (py < 0 || py >= GFX_H)
            continue;
        const u32 *src = img->px + (size_t)(yy * img->h / h) * img->w;
        for (int xx = 0; xx < w; xx++) {
            int px = x + xx;
            if (px >= 0 && px < GFX_W)
                blend(&g_px[(size_t)py * g_stride + px], src[xx * img->w / w], 255);
        }
    }
}

// --- Texto -----------------------------------------------------------------------------------------------

static u32 next_codepoint(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    u32 c = *p++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra)
        c &= 0x3F >> extra;
    while (extra-- && (*p & 0xC0) == 0x80)
        c = c << 6 | (*p++ & 0x3F);
    *s = (const char *)p;
    return c;
}

// Los caracteres de uso privado (símbolos de los botones) solo están en la fuente extendida del sistema.
static FT_Face face_for(u32 cp)
{
    if (g_have_ext && cp >= 0xE000 && cp <= 0xF8FF && FT_Get_Char_Index(g_face_ext, cp))
        return g_face_ext;
    return g_face_std;
}

// Dibuja (si draw) o mide el texto; y es la línea base superior aproximada (arriba del texto).
static int render_text(int x, int y, int size, u32 color, const char *s, bool draw)
{
    int pen = x;
    FT_Set_Pixel_Sizes(g_face_std, 0, size);
    if (g_have_ext)
        FT_Set_Pixel_Sizes(g_face_ext, 0, size);
    int baseline = y + size * 4 / 5;
    while (*s) {
        u32 cp = next_codepoint(&s);
        FT_Face face = face_for(cp);
        if (FT_Load_Char(face, cp, draw ? FT_LOAD_RENDER : FT_LOAD_DEFAULT))
            continue;
        FT_GlyphSlot g = face->glyph;
        if (draw) {
            const FT_Bitmap *bm = &g->bitmap;
            for (unsigned row = 0; row < bm->rows; row++) {
                int py = baseline - g->bitmap_top + (int)row;
                if (py < 0 || py >= GFX_H)
                    continue;
                for (unsigned col = 0; col < bm->width; col++) {
                    int px = pen + g->bitmap_left + (int)col;
                    u8 cover = bm->buffer[row * bm->pitch + col];
                    if (cover && px >= 0 && px < GFX_W)
                        blend(&g_px[(size_t)py * g_stride + px], color, cover);
                }
            }
        }
        pen += g->advance.x >> 6;
    }
    return pen - x;
}

int gfx_text(int x, int y, int size, u32 color, const char *utf8)
{
    return render_text(x, y, size, color, utf8, true);
}

int gfx_text_width(int size, const char *utf8)
{
    return render_text(0, 0, size, 0, utf8, false);
}

void gfx_text_fit(int x, int y, int size, u32 color, const char *utf8, int max_w)
{
    if (gfx_text_width(size, utf8) <= max_w) {
        gfx_text(x, y, size, color, utf8);
        return;
    }
    char buf[512];
    size_t len = strlen(utf8);
    if (len >= sizeof(buf) - 4)
        len = sizeof(buf) - 4;
    memcpy(buf, utf8, len);
    while (len > 0) {
        // Retroceder un carácter UTF-8 completo.
        do { len--; } while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80);
        memcpy(buf + len, "...", 4);
        if (gfx_text_width(size, buf) <= max_w)
            break;
    }
    gfx_text(x, y, size, color, buf);
}

int gfx_button(int x, int y, const char *button, const char *label)
{
    const int d = 30;
    int bw = gfx_text_width(18, button);
    int pill = bw + 14 > d ? bw + 14 : d;  // "+" y letras caben en un círculo; textos largos, en una píldora
    gfx_round_rect(x, y, pill, d, d / 2, RGBA(0xEE, 0xEE, 0xF2, 0xFF));
    gfx_text(x + (pill - bw) / 2, y + 5, 18, RGBA(0x1B, 0x1D, 0x24, 0xFF), button);
    int w = gfx_text(x + pill + 10, y + 3, 22, RGBA(0xEE, 0xEE, 0xF2, 0xFF), label);
    return pill + 10 + w;
}
