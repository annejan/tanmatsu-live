// Tanmatsu live coding instrument.
//
// The whole point of the app is that the keyboard, the display and the audio
// are one loop: you type a pattern, hit eval, and the sound changes without
// the transport stopping. This file owns the editor and the drawing; the sound
// lives in app_audio.c and the voices in components/strudel_dsp.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "app_audio.h"
#include "app_store.h"
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "bsp/power.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "pax_fonts.h"
#include "pax_gfx.h"
#include "pax_text.h"
#include "pax_types.h"

static char const TAG[] = "strudel";

#define ED_MAX_LINES 24
#define ED_MAX_COLS  95

#define COL_BG       0xFF101014
#define COL_PANEL    0xFF1B1B22
#define COL_TEXT     0xFFE6E6EE
#define COL_DIM      0xFF6A6A7A
#define COL_ACCENT   0xFF2FE0A8
#define COL_WARN     0xFFFF5070
#define COL_CURSOR   0xFF2FE0A8
#define COL_STEP_ON  0xFF2FE0A8
#define COL_STEP_OFF 0xFF33333F
#define COL_PLAYHEAD 0xFFFFC24A

// The Tanmatsu function keys carry no legends, they are coloured shapes. The
// footer draws those shapes so the hint matches what is under your fingers.
#define COL_KEY_RED    0xFFE83B3B
#define COL_KEY_ORANGE 0xFFF08A2E
#define COL_KEY_YELLOW 0xFFE8CF3B
#define COL_KEY_GREEN  0xFF3BC96A
#define COL_KEY_BLUE   0xFF3B9BE8
#define COL_KEY_PURPLE 0xFFA96BE8

#define FONT      pax_font_sky_mono
#define FONT_SIZE 16.0f
#define LINE_H    19.0f
#define ED_TOP    30.0f
#define LANE_TOP  340.0f

static pax_buf_t     fb          = {0};
// Panel dimensions, which is what the blit wants. Tanmatsu's ST7701 is mounted
// portrait and the BSP asks for a 270 degree rotation, so these are NOT the
// coordinate space you draw in.
static size_t        disp_w      = 0;
static size_t        disp_h      = 0;
// Logical dimensions after the pax orientation is applied: what every draw
// call below is measured against.
static float         ui_w        = 0.0f;
static float         ui_h        = 0.0f;
static QueueHandle_t input_queue = NULL;
static float         char_w      = 9.0f;

static char  ed[ED_MAX_LINES][ED_MAX_COLS + 1];
static int   ed_lines     = 0;
static int   cur_row      = 0;
static int   cur_col      = 0;
static bool  dirty        = true;
// Partial redraw bookkeeping. Re-rasterising every glyph on every frame is by
// far the most expensive thing the UI does, so a keystroke repaints only the
// lines it touched and leaves the rest of the framebuffer alone.
static bool  need_full    = true;
static int   ed_scroll    = 0;
static int   dirty_row_a  = -1;
static int   dirty_row_b  = -1;
static float ui_draw_ms   = 0.0f;
static float ui_blit_ms   = 0.0f;
static char  status[64]   = "";
static bool  status_error = false;

// Starter tunes. Each is a normal editor buffer, so everything here is
// something you could have typed, and everything here can be edited live.
typedef struct {
    char const*        name;
    char const* const* lines;
    int                count;
} preset_t;

static char const* const tune_house[] = {
    "# orange triangle evaluates . red cross plays . esc exits",
    "bpm 128",
    "delay 1 0.1875 0.42 0.30",
    "",
    "bd      x ~ ~ x ~ ~ x ~",
    "sd      ~ ~ x ~ ~ ~ x ~",
    "hh:0.26 x*8?0.2",
    "oh:0.20 ~ ~ ~ ~ ~ ~ x ~",
    "cp:0.34 x(3,8,3)",
    "",
    "saw:0.26:620:0.55   <c2 g1 c2 bb1> ~ [~ c2] ~",
    "saw:0.11:1500:0.35  [c3 eb3 g3]*2 . <bb2 ab2>",
    "square:0.06:2600    ~ <c5 eb5> ~ g4",
};

// A breakbeat in the amen idiom: kick off the grid, snares on the backbeat
// with ghosts between them, and hats thinned at random so no bar repeats.
static char const* const tune_break[] = {
    "# amen style breakbeat . yellow square and green circle change tempo",
    "bpm 174",
    "delay 1 0.0862 0.35 0.22",
    "",
    "bd      x.....x....x....",
    "sd      ....x..x..x...x.",
    "hh:0.20 x*16?0.12",
    "oh:0.16 .......x........",
    "rim:0.18 ~ ~ [~ x] ~",
    "",
    "saw:0.30:340:0.6   c1 ~ ~ [~ c1] ~ ~ eb1 ~",
    "square:0.05:2800   ~ ~ <g4 bb4> ~",
};

// Rolling two step bass under a moving arpeggio, the trance end of drum and
// bass. The bass is one line: <> walks the root while * rolls it.
static char const* const tune_liquid[] = {
    "# trancy drum and bass",
    "bpm 174",
    "delay 1 0.1034 0.5 0.34",
    "reverb 0.7 0.5 0.22",
    "room 1 0.4",
    "",
    "bd      x.......x.......",
    "sd      ....x.......x...",
    "hh:0.18 x*16?0.1",
    "oh:0.14 ..........x.....",
    "",
    "saw:0.30:380:0.62  <c1 c1 ab0 bb0>*2",
    "saw:0.12:1700:0.4  [c4 eb4 g4 bb4]*2",
    "square:0.05:3200   ~ <g5 bb5 c6 bb5> ~ eb5",
    "tri:0.08:900       <c3 ab2 bb2 g2>",
};

// Sparse and wet: almost nothing playing, most of the sound is the delay.
static char const* const tune_dub[] = {
    "# dub . mostly delay and reverb . blue cloud panics",
    "bpm 140",
    "delay 1 0.321 0.62 0.55",
    "reverb 0.85 0.35 0.40",
    "room 0 0.25",
    "room 1 0.55",
    "",
    "bd      x.......x.......",
    "sd:0.5  ............x...",
    "hh:0.13 ~x~x~x~x~x~x~x~x",
    "cp:0.3  ~ ~ ~ [~ x]?0.5",
    "",
    "saw:0.24:460:0.55  <c2 ~ g1 ~>",
    "square:0.05:1600   ~ ~ ~ [c4 eb4]?0.4",
};

#define TUNE(v) {#v, v, (int)(sizeof(v) / sizeof((v)[0]))}
static preset_t const presets[] = {
    {"house", tune_house, (int)(sizeof(tune_house) / sizeof(tune_house[0]))},
    {"break", tune_break, (int)(sizeof(tune_break) / sizeof(tune_break[0]))},
    {"liquid", tune_liquid, (int)(sizeof(tune_liquid) / sizeof(tune_liquid[0]))},
    {"dub", tune_dub, (int)(sizeof(tune_dub) / sizeof(tune_dub[0]))},
};
#define PRESET_COUNT ((int)(sizeof(presets) / sizeof(presets[0])))

static int preset_idx = 0;
static int save_slot  = 0;

// A few whole buffer snapshots. Live coding is mostly small edits to something
// that is already playing, so being able to step back is worth the memory.
#define UNDO_DEPTH 8
static char undo_buf[UNDO_DEPTH][ED_MAX_LINES][ED_MAX_COLS + 1];
static int  undo_lines[UNDO_DEPTH];
static int  undo_head  = 0;  // next slot to write
static int  undo_count = 0;

// ---------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------

static void ed_load_preset(int idx) {
    if (PRESET_COUNT == 0) {
        return;
    }
    preset_idx     = ((idx % PRESET_COUNT) + PRESET_COUNT) % PRESET_COUNT;
    preset_t const* t = &presets[preset_idx];

    ed_lines = t->count > ED_MAX_LINES ? ED_MAX_LINES : t->count;
    for (int i = 0; i < ed_lines; i++) {
        snprintf(ed[i], sizeof(ed[i]), "%s", t->lines[i]);
    }
    cur_row   = 4;
    cur_col   = 0;
    need_full = true;
}

// Flattens the editor into one buffer for the parser.
static void ed_text(char* out, size_t len) {
    size_t pos = 0;
    for (int i = 0; i < ed_lines && pos + 2 < len; i++) {
        int n = snprintf(out + pos, len - pos, "%s\n", ed[i]);
        if (n < 0) {
            break;
        }
        pos += (size_t)n;
    }
    out[pos < len ? pos : len - 1] = 0;
}

static void undo_push(void) {
    memcpy(undo_buf[undo_head], ed, sizeof(ed));
    undo_lines[undo_head] = ed_lines;
    undo_head             = (undo_head + 1) % UNDO_DEPTH;
    if (undo_count < UNDO_DEPTH) {
        undo_count++;
    }
}

static bool undo_pop(void) {
    if (undo_count == 0) {
        return false;
    }
    undo_head = (undo_head + UNDO_DEPTH - 1) % UNDO_DEPTH;
    undo_count--;
    memcpy(ed, undo_buf[undo_head], sizeof(ed));
    ed_lines  = undo_lines[undo_head];
    need_full = true;
    return true;
}

// Replaces the whole buffer, used when a set is loaded.
static void ed_set_text(char const* text) {
    ed_lines = 0;
    while (*text && ed_lines < ED_MAX_LINES) {
        char const* eol = strchr(text, '\n');
        size_t      len = eol ? (size_t)(eol - text) : strlen(text);
        if (len > ED_MAX_COLS) {
            len = ED_MAX_COLS;
        }
        memcpy(ed[ed_lines], text, len);
        ed[ed_lines][len] = 0;
        ed_lines++;
        if (!eol) {
            break;
        }
        text = eol + 1;
    }
    if (ed_lines == 0) {
        ed_lines = 1;
        ed[0][0] = 0;
    }
    cur_row   = 0;
    cur_col   = 0;
    need_full = true;
}

static void ed_clamp(void) {
    if (ed_lines < 1) {
        ed_lines = 1;
        ed[0][0] = 0;
    }
    if (cur_row < 0) {
        cur_row = 0;
    }
    if (cur_row >= ed_lines) {
        cur_row = ed_lines - 1;
    }
    int len = (int)strlen(ed[cur_row]);
    if (cur_col > len) {
        cur_col = len;
    }
    if (cur_col < 0) {
        cur_col = 0;
    }
}

// True when the cursor sits inside the step grid part of a line, that is
// after the head token, in a run of grid characters. Typing there should
// replace the step under the cursor rather than push everything sideways:
// changing a hit to an accent is one keystroke, not backspace and retype.
static bool in_grid_region(void) {
    char const* line = ed[cur_row];
    if (line[0] == '#' || line[0] == 0) {
        return false;
    }
    // Find the end of the head token and the start of the pattern
    int i = 0;
    while (line[i] && line[i] != ' ' && line[i] != '\t') {
        i++;
    }
    while (line[i] == ' ' || line[i] == '\t') {
        i++;
    }
    int start = i;
    if (start == 0 || line[start] == 0) {
        return false;
    }
    // The pattern must be a grid: only step characters, no spaces
    for (int k = start; line[k]; k++) {
        char ch = line[k];
        bool ok = ch == 'x' || ch == 'X' || ch == '.' || ch == '~' || ch == '-' || ch == '_' ||
                  (ch >= '0' && ch <= '9');
        if (!ok) {
            return false;
        }
    }
    return cur_col >= start && cur_col < (int)strlen(line);
}

static void ed_overtype_char(char c) {
    ed[cur_row][cur_col] = c;
    cur_col++;
    dirty = true;
}

static void ed_insert_char(char c) {
    int len = (int)strlen(ed[cur_row]);
    if (len >= ED_MAX_COLS) {
        return;
    }
    memmove(&ed[cur_row][cur_col + 1], &ed[cur_row][cur_col], (size_t)(len - cur_col + 1));
    ed[cur_row][cur_col] = c;
    cur_col++;
    dirty = true;
}

static void ed_backspace(void) {
    if (cur_col > 0) {
        int len = (int)strlen(ed[cur_row]);
        memmove(&ed[cur_row][cur_col - 1], &ed[cur_row][cur_col], (size_t)(len - cur_col + 1));
        cur_col--;
    } else if (cur_row > 0) {
        // Join with the line above, keeping whatever fits
        int prev = (int)strlen(ed[cur_row - 1]);
        int room = ED_MAX_COLS - prev;
        if (room > 0) {
            strncat(ed[cur_row - 1], ed[cur_row], (size_t)room);
        }
        for (int i = cur_row; i < ed_lines - 1; i++) {
            memcpy(ed[i], ed[i + 1], sizeof(ed[i]));
        }
        ed_lines--;
        cur_row--;
        cur_col = prev;
    }
    dirty = true;
}

static void ed_delete(void) {
    int len = (int)strlen(ed[cur_row]);
    if (cur_col < len) {
        memmove(&ed[cur_row][cur_col], &ed[cur_row][cur_col + 1], (size_t)(len - cur_col));
        dirty = true;
    } else if (cur_row + 1 < ed_lines) {
        cur_col++;
        ed_backspace();
        cur_col--;
    }
}

static void ed_newline(void) {
    if (ed_lines >= ED_MAX_LINES) {
        return;
    }
    undo_push();
    for (int i = ed_lines; i > cur_row + 1; i--) {
        memcpy(ed[i], ed[i - 1], sizeof(ed[i]));
    }
    ed_lines++;
    // Through a scratch copy: source and destination are both inside ed, and
    // the compiler will not accept a direct copy between them.
    char tail[ED_MAX_COLS + 1];
    snprintf(tail, sizeof(tail), "%s", &ed[cur_row][cur_col]);
    ed[cur_row][cur_col] = 0;
    memcpy(ed[cur_row + 1], tail, sizeof(tail));
    cur_row++;
    cur_col = 0;
    dirty   = true;
}

static void do_eval(void) {
    static char text[ED_MAX_LINES * (ED_MAX_COLS + 2)];
    ed_text(text, sizeof(text));
    app_audio_eval(text);
    char const* err = app_audio_get_error();
    need_full       = true;
    status_error    = err[0] != 0;
    if (status_error) {
        snprintf(status, sizeof(status), "%s", err);
    } else {
        snprintf(status, sizeof(status), "%d parts playing", app_audio_get_part_count());
    }
    dirty = true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void blit(void) {
    esp_err_t res = bsp_display_blit(0, 0, disp_w, disp_h, pax_buf_get_pixels(&fb));
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "Failed to blit: %d", res);
    }
}

static void draw_header(void) {
    pax_simple_rect(&fb, COL_PANEL, 0, 0, ui_w, 26);
    pax_draw_text(&fb, COL_ACCENT, FONT, FONT_SIZE, 6, 5, "TANMATSU LIVE");

    char info[96];
    snprintf(info, sizeof(info), "%s  %3.0f bpm  %2dv  dsp %2.0f%%  ui %.0f+%.0fms",
             app_audio_is_playing() ? "PLAY" : "STOP", app_audio_get_bpm(), app_audio_get_voices(),
             app_audio_get_load() * 100.0f, (double)ui_draw_ms, (double)ui_blit_ms);
    pax_vec2f size = pax_text_size(FONT, FONT_SIZE, info);
    pax_draw_text(&fb, app_audio_is_playing() ? COL_TEXT : COL_DIM, FONT, FONT_SIZE, ui_w - size.x - 6, 5, info);
}

static int ed_visible_rows(void) {
    return (int)((LANE_TOP - ED_TOP - 4) / LINE_H);
}

static int ed_first_visible(void) {
    int visible = ed_visible_rows();
    return cur_row >= visible ? cur_row - visible + 1 : 0;
}

// One editor line, clearing its own strip first so it can be repainted alone.
static void draw_editor_row(int row) {
    int first = ed_scroll;
    int slot  = row - first;
    if (slot < 0 || slot >= ed_visible_rows() || row < 0 || row >= ed_lines) {
        return;
    }
    float y = ED_TOP + (float)slot * LINE_H;

    pax_col_t bg = (row == cur_row) ? 0xFF1A1A24 : COL_BG;
    pax_simple_rect(&fb, bg, 0, y - 2, ui_w, LINE_H);

    bool        is_part = app_audio_get_line_steps(row) > 0;
    char const* txt     = ed[row];
    pax_col_t   col     = COL_TEXT;
    if (txt[0] == '#') {
        col = COL_DIM;
    } else if (is_part) {
        pax_simple_rect(&fb, COL_ACCENT, 0, y, 2, LINE_H - 4);
    }
    pax_draw_text(&fb, col, FONT, FONT_SIZE, 8, y, txt);

    if (row == cur_row) {
        pax_simple_rect(&fb, COL_CURSOR, 8 + (float)cur_col * char_w, y, 2, FONT_SIZE);
    }
}

static void draw_editor(void) {
    int visible = ed_visible_rows();
    for (int i = 0; i < visible && ed_scroll + i < ed_lines; i++) {
        draw_editor_row(ed_scroll + i);
    }
}

static void draw_lanes(void) {
    pax_simple_rect(&fb, COL_BG, 0, LANE_TOP - 2, ui_w, ui_h - 22.0f - LANE_TOP + 2.0f);

    int parts = app_audio_get_part_count();
    if (parts <= 0) {
        return;
    }
    int64_t step      = app_audio_get_step();
    float   y         = LANE_TOP;
    float   lane_h    = 12.0f;
    int     max_parts = (int)((ui_h - 24.0f - LANE_TOP) / lane_h);
    if (parts > max_parts) {
        parts = max_parts;
    }

    for (int p = 0; p < parts; p++) {
        char name[16];
        app_audio_get_part_name(p, name, sizeof(name));
        pax_draw_text(&fb, COL_DIM, FONT, 12, 6, y, name);

        int   n  = app_audio_get_part_steps(p);
        float x0 = 78.0f;
        float w  = (ui_w - x0 - 10.0f) / (float)(n > 0 ? n : 1);
        if (w > 22.0f) {
            w = 22.0f;
        }
        int cur = n > 0 ? (int)(((step - 1) % n + n) % n) : 0;
        for (int i = 0; i < n; i++) {
            bool      on  = app_audio_get_part_step_on(p, i);
            bool      now = app_audio_is_playing() && i == cur;
            pax_col_t c   = now ? COL_PLAYHEAD : (on ? COL_STEP_ON : COL_STEP_OFF);
            pax_simple_rect(&fb, c, x0 + (float)i * w, y, w - 2.0f, on ? 9.0f : 4.0f);
        }
        y += lane_h;
    }
}

// One function key, drawn as the shape actually printed on that key.
static void draw_key_glyph(int fkey, float x, float y, float s) {
    float cx = x + s * 0.5f;
    float cy = y + s * 0.5f;
    switch (fkey) {
        case 1:  // red cross
            for (float o = -1.0f; o <= 1.0f; o += 1.0f) {
                pax_simple_line(&fb, COL_KEY_RED, x + o, y, x + s + o, y + s);
                pax_simple_line(&fb, COL_KEY_RED, x + s + o, y, x + o, y + s);
            }
            break;
        case 2:  // orange triangle
            pax_simple_tri(&fb, COL_KEY_ORANGE, cx, y, x + s, y + s, x, y + s);
            break;
        case 3:  // yellow square
            pax_simple_rect(&fb, COL_KEY_YELLOW, x + 1, y + 1, s - 2, s - 2);
            break;
        case 4:  // green circle
            pax_simple_circle(&fb, COL_KEY_GREEN, cx, cy, s * 0.5f);
            break;
        case 5: {  // blue cloud
            float r = s * 0.27f;
            pax_simple_circle(&fb, COL_KEY_BLUE, x + r + 1.0f, cy + r * 0.4f, r);
            pax_simple_circle(&fb, COL_KEY_BLUE, cx, cy - r * 0.35f, r * 1.15f);
            pax_simple_circle(&fb, COL_KEY_BLUE, x + s - r - 1.0f, cy + r * 0.4f, r);
            pax_simple_rect(&fb, COL_KEY_BLUE, x + r * 0.6f, cy + r * 0.2f, s - r * 1.2f, r);
            break;
        }
        case 6:  // purple diamond
            pax_simple_tri(&fb, COL_KEY_PURPLE, cx, y, x + s, cy, x, cy);
            pax_simple_tri(&fb, COL_KEY_PURPLE, cx, y + s, x + s, cy, x, cy);
            break;
        default:
            break;
    }
}

static void draw_footer(void) {
    static char const* const labels[6] = {"play", "eval", "bpm-", "bpm+", "panic", "tune"};

    float bar_y = ui_h - 22.0f;
    pax_simple_rect(&fb, COL_PANEL, 0, bar_y, ui_w, 22);

    float gs = 11.0f;
    float x  = 6.0f;
    for (int i = 0; i < 6; i++) {
        draw_key_glyph(i + 1, x, bar_y + 5.0f, gs);
        x += gs + 4.0f;
        pax_draw_text(&fb, COL_DIM, FONT, 13, x, bar_y + 4.0f, labels[i]);
        x += pax_text_size(FONT, 13, labels[i]).x + 12.0f;
    }

    char const* tail = in_grid_region() ? "OVR  ^space cycle step  ^m mute  ^z undo  ^s save  ^o open"
                                        : "esc exit  ^m mute  ^d dup  ^k kill  ^z undo  ^s save  ^o open";
    pax_draw_text(&fb, COL_DIM, FONT, 13, x, bar_y + 4.0f, tail);
    x += pax_text_size(FONT, 13, tail).x + 12.0f;

    // The status only gets drawn when it does not collide with the legend
    if (status[0]) {
        pax_vec2f size = pax_text_size(FONT, 13, status);
        float     sx   = ui_w - size.x - 6.0f;
        if (sx > x) {
            pax_draw_text(&fb, status_error ? COL_WARN : COL_TEXT, FONT, 13, sx, bar_y + 4.0f, status);
        }
    }
}

static void render(void) {
    if (pax_buf_get_width(&fb) == 0) {
        return;
    }
    int64_t t0    = esp_timer_get_time();
    int     first = ed_first_visible();
    if (first != ed_scroll) {
        ed_scroll = first;
        need_full = true;  // everything shifted, nothing can be reused
    }

    if (need_full) {
        pax_background(&fb, COL_BG);
        draw_editor();
        draw_footer();
        need_full = false;
    } else {
        // These two clear their own strips, and both change every frame anyway
        draw_editor_row(dirty_row_a);
        if (dirty_row_b != dirty_row_a) {
            draw_editor_row(dirty_row_b);
        }
    }
    draw_header();
    draw_lanes();
    dirty_row_a = dirty_row_b = -1;
    int64_t t1                = esp_timer_get_time();
    blit();
    int64_t t2 = esp_timer_get_time();

    // Smoothed, so the reading is legible instead of flickering
    ui_draw_ms += ((float)(t1 - t0) / 1000.0f - ui_draw_ms) * 0.25f;
    ui_blit_ms += ((float)(t2 - t1) / 1000.0f - ui_blit_ms) * 0.25f;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static void handle_navigation(bsp_input_event_args_navigation_t const* nav) {
    if (!nav->state) {
        return;
    }
    bool ctrl = (nav->modifiers & BSP_INPUT_MODIFIER_CTRL) != 0;

    switch (nav->key) {
        case BSP_INPUT_NAVIGATION_KEY_LEFT:
            if (cur_col > 0) {
                cur_col--;
            } else if (cur_row > 0) {
                cur_row--;
                cur_col = (int)strlen(ed[cur_row]);
            }
            dirty = true;
            break;
        case BSP_INPUT_NAVIGATION_KEY_RIGHT:
            if (cur_col < (int)strlen(ed[cur_row])) {
                cur_col++;
            } else if (cur_row + 1 < ed_lines) {
                cur_row++;
                cur_col = 0;
            }
            dirty = true;
            break;
        case BSP_INPUT_NAVIGATION_KEY_UP:
            cur_row--;
            dirty = true;
            break;
        case BSP_INPUT_NAVIGATION_KEY_DOWN:
            cur_row++;
            dirty = true;
            break;
        case BSP_INPUT_NAVIGATION_KEY_HOME:
            cur_col = 0;
            dirty   = true;
            break;
        case BSP_INPUT_NAVIGATION_KEY_END:
            cur_col = (int)strlen(ed[cur_row]);
            dirty   = true;
            break;
        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE:
            ed_backspace();
            break;
        case BSP_INPUT_NAVIGATION_KEY_RETURN:
            // Ctrl+Enter evaluates, which is the habit every live coder has
            if (ctrl) {
                do_eval();
            } else {
                ed_newline();
            }
            break;
        case BSP_INPUT_NAVIGATION_KEY_TAB:
            for (int i = 0; i < 4; i++) {
                ed_insert_char(' ');
            }
            break;
        case BSP_INPUT_NAVIGATION_KEY_F1:
            app_audio_set_playing(!app_audio_is_playing());
            dirty = true;
            break;
        case BSP_INPUT_NAVIGATION_KEY_F2:
            do_eval();
            if (!app_audio_is_playing()) {
                app_audio_set_playing(true);
            }
            break;
        case BSP_INPUT_NAVIGATION_KEY_F3:
            app_audio_set_bpm(app_audio_get_bpm() - 2.0f);
            dirty = true;
            break;
        case BSP_INPUT_NAVIGATION_KEY_F4:
            app_audio_set_bpm(app_audio_get_bpm() + 2.0f);
            dirty = true;
            break;
        case BSP_INPUT_NAVIGATION_KEY_F5:
            app_audio_set_playing(false);
            snprintf(status, sizeof(status), "panic");
            status_error = false;
            dirty        = true;
            break;
        case BSP_INPUT_NAVIGATION_KEY_F6:
            ed_load_preset(preset_idx + 1);
            do_eval();
            snprintf(status, sizeof(status), "tune: %s", presets[preset_idx].name);
            status_error = false;
            break;
        case BSP_INPUT_NAVIGATION_KEY_VOLUME_UP:
            app_audio_set_master(app_audio_get_master() + 0.05f);
            dirty = true;
            break;
        case BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN:
            app_audio_set_master(app_audio_get_master() - 0.05f);
            dirty = true;
            break;
        case BSP_INPUT_NAVIGATION_KEY_ESC:
            // The launcher uses ESC for back, and Tanmatsu has no key above F6
            app_audio_set_playing(false);
            bsp_device_restart_to_launcher();
            break;
        default:
            break;
    }
    ed_clamp();
}

// Commenting a line is how you mute a part without losing it, which during a
// set matters more than any other edit.
// Space on a grid steps forward through rest, hit and accent. Holding a
// position and cycling it is how you audition a change without deciding what
// the character should be first.
static bool cycle_step(void) {
    if (!in_grid_region()) {
        return false;
    }
    char* at = &ed[cur_row][cur_col];
    switch (*at) {
        case '.':
        case '~':
        case '-':
        case '_': *at = 'x'; break;
        case 'x': *at = 'X'; break;
        default: *at = '.'; break;
    }
    dirty = true;
    return true;
}

static void toggle_comment(void) {
    undo_push();
    char* line = ed[cur_row];
    if (line[0] == '#') {
        size_t n = strlen(line);
        size_t k = 1;
        while (k < n && line[k] == ' ') {
            k++;
        }
        memmove(line, line + k, n - k + 1);
        if (cur_col >= (int)k) {
            cur_col -= (int)k;
        }
    } else {
        size_t n = strlen(line);
        if (n + 2 > ED_MAX_COLS) {
            return;
        }
        memmove(line + 2, line, n + 1);
        line[0] = '#';
        line[1] = ' ';
        cur_col += 2;
    }
    need_full = true;
}

static void duplicate_line(void) {
    if (ed_lines >= ED_MAX_LINES) {
        return;
    }
    undo_push();
    for (int i = ed_lines; i > cur_row + 1; i--) {
        memcpy(ed[i], ed[i - 1], sizeof(ed[i]));
    }
    memcpy(ed[cur_row + 1], ed[cur_row], sizeof(ed[cur_row]));
    ed_lines++;
    cur_row++;
    need_full = true;
}

static void kill_line(void) {
    undo_push();
    if (ed_lines <= 1) {
        ed[0][0] = 0;
        cur_col  = 0;
        need_full = true;
        return;
    }
    for (int i = cur_row; i < ed_lines - 1; i++) {
        memcpy(ed[i], ed[i + 1], sizeof(ed[i]));
    }
    ed_lines--;
    if (cur_row >= ed_lines) {
        cur_row = ed_lines - 1;
    }
    cur_col   = 0;
    need_full = true;
}

static void do_save(void) {
    static char text[ED_MAX_LINES * (ED_MAX_COLS + 2)];
    if (!app_store_available()) {
        snprintf(status, sizeof(status), "no storage");
        status_error = true;
        dirty        = true;
        return;
    }
    ed_text(text, sizeof(text));
    bool ok      = app_store_save(save_slot, text);
    status_error = !ok;
    if (ok) {
        snprintf(status, sizeof(status), "saved %s slot %d", app_store_where(), save_slot);
    } else {
        snprintf(status, sizeof(status), "could not save slot %d", save_slot);
    }
    dirty = true;
}

static void do_load(void) {
    static char text[ED_MAX_LINES * (ED_MAX_COLS + 2)];
    if (!app_store_available()) {
        snprintf(status, sizeof(status), "no storage");
        status_error = true;
        dirty        = true;
        return;
    }
    if (!app_store_load(save_slot, text, sizeof(text))) {
        snprintf(status, sizeof(status), "slot %d is empty", save_slot);
        status_error = true;
        dirty        = true;
        return;
    }
    ed_set_text(text);
    do_eval();
    snprintf(status, sizeof(status), "loaded slot %d", save_slot);
    status_error = false;
}

static void handle_keyboard(bsp_input_event_args_keyboard_t const* kb) {
    // The BSP picks .ascii from the base and shift columns only, so a key
    // reached through AltGr reports the wrong character there while .utf8
    // carries the right one. Trust .utf8 whenever it is a single byte, and
    // drop anything multi byte since the buffer and the parser are ASCII.
    char c = kb->ascii;

    // Ctrl shortcuts never reach the buffer as text
    if (kb->modifiers & BSP_INPUT_MODIFIER_CTRL) {
        char lower = (char)(c | 0x20);
        if (lower == 's') {
            do_save();
        } else if (lower == 'o') {
            do_load();
        } else if (lower == 'd') {
            duplicate_line();
            dirty = true;
        } else if (lower == 'k') {
            kill_line();
            do_eval();
        } else if (lower == 'z') {
            if (undo_pop()) {
                ed_clamp();
                do_eval();
                snprintf(status, sizeof(status), "undo");
                status_error = false;
            }
            dirty = true;
        } else if (c == ' ') {
            if (cycle_step()) {
                do_eval();
            }
        } else if (c == '/' || lower == 'm') {
            toggle_comment();
            do_eval();
        } else if (c >= '1' && c <= '8') {
            save_slot = c - '1';
            snprintf(status, sizeof(status), "slot %d%s", save_slot,
                     app_store_exists(save_slot) ? " (in use)" : " (empty)");
            status_error = false;
            dirty        = true;
        }
        return;
    }

    if (kb->utf8[0] != 0) {
        if ((unsigned char)kb->utf8[0] >= 0x80) {
            return;
        }
        if (kb->utf8[1] == 0) {
            c = kb->utf8[0];
        }
    }
    if (c == '\b') {
        ed_backspace();
        return;
    }
    if (c == 127) {
        ed_delete();
        return;
    }
    if (c == '\n' || c == '\r') {
        ed_newline();
        return;
    }
    if (c >= 32 && c < 127) {
        if (c != ' ' && in_grid_region()) {
            ed_overtype_char(c);
        } else {
            ed_insert_char(c);
        }
    }
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

static pax_buf_type_t pax_format_for(bsp_display_color_format_t fmt) {
    switch (fmt) {
        case BSP_DISPLAY_COLOR_FORMAT_8_332RGB:
            return PAX_BUF_8_332RGB;
        case BSP_DISPLAY_COLOR_FORMAT_16_565RGB:
            return PAX_BUF_16_565RGB;
        case BSP_DISPLAY_COLOR_FORMAT_16_4444ARGB:
            return PAX_BUF_16_4444ARGB;
        case BSP_DISPLAY_COLOR_FORMAT_32_8888ARGB:
            return PAX_BUF_32_8888ARGB;
        case BSP_DISPLAY_COLOR_FORMAT_8_GREY:
            return PAX_BUF_8_GREY;
        case BSP_DISPLAY_COLOR_FORMAT_1_GREY:
            return PAX_BUF_1_GREY;
        case BSP_DISPLAY_COLOR_FORMAT_24_888RGB:
        default:
            return PAX_BUF_24_888RGB;
    }
}

static pax_orientation_t pax_orientation_for(bsp_display_rotation_t rot) {
    switch (rot) {
        case BSP_DISPLAY_ROTATION_90:
            return PAX_O_ROT_CCW;
        case BSP_DISPLAY_ROTATION_180:
            return PAX_O_ROT_HALF;
        case BSP_DISPLAY_ROTATION_270:
            return PAX_O_ROT_CW;
        case BSP_DISPLAY_ROTATION_0:
        default:
            return PAX_O_UPRIGHT;
    }
}

void app_main(void) {
    gpio_install_isr_service(0);

    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);

    bsp_configuration_t const cfg = {
        .display =
            {
                // Half the bytes of 24 bit for the same picture: the blit and
                // the background clear are the bulk of a frame's cost, and
                // they share PSRAM bandwidth with the audio task.
                .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_16_565RGB,
                .num_fbs                = 1,
            },
    };
    ESP_ERROR_CHECK(bsp_device_initialize(&cfg));

    bsp_display_color_format_t color_format = 0;
    bsp_display_endianness_t   endianness   = 0;
    res                                     = bsp_display_get_parameters(&disp_w, &disp_h, &color_format, &endianness);
    if (res == ESP_OK) {
        pax_buf_init(&fb, NULL, disp_w, disp_h, pax_format_for(color_format));
        pax_buf_reversed(&fb, endianness == BSP_DISPLAY_ENDIAN_BIG);
        pax_buf_set_orientation(&fb, pax_orientation_for(bsp_display_get_default_rotation()));
        // After the rotation these are the drawing bounds, 800x480 on Tanmatsu,
        // where the panel itself reports 480x800.
        ui_w   = (float)pax_buf_get_width(&fb);
        ui_h   = (float)pax_buf_get_height(&fb);
        char_w = pax_text_size(FONT, FONT_SIZE, "0").x;
        if (char_w < 1.0f) {
            char_w = 9.0f;
        }
    } else {
        ESP_LOGW(TAG, "No display: %d", res);
    }

    ESP_ERROR_CHECK(bsp_input_get_queue(&input_queue));
    bsp_led_set_mode(false);
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
    bsp_input_set_backlight_brightness(60);

    ESP_ERROR_CHECK(app_audio_start());

    // Storage is optional: without it the instrument still plays, it just
    // cannot keep anything.
    app_store_init();

    ed_load_preset(0);
    do_eval();
    app_audio_set_playing(true);
    render();

    int64_t last_draw = 0;
    while (1) {
        bsp_input_event_t event;
        // Wait for the first event, then take everything else already queued
        // before drawing. Holding a key or typing fast then costs one redraw
        // instead of one per character.
        int               prev_row   = cur_row;
        int               prev_lines = ed_lines;
        if (xQueueReceive(input_queue, &event, pdMS_TO_TICKS(50)) == pdTRUE) {
            do {
                switch (event.type) {
                    case INPUT_EVENT_TYPE_NAVIGATION:
                        handle_navigation(&event.args_navigation);
                        break;
                    case INPUT_EVENT_TYPE_KEYBOARD:
                        handle_keyboard(&event.args_keyboard);
                        break;
                    default:
                        break;
                }
            } while (xQueueReceive(input_queue, &event, 0) == pdTRUE);

            // Inserting or removing a line moves everything below it
            if (ed_lines != prev_lines) {
                need_full = true;
            } else {
                dirty_row_a = prev_row;
                dirty_row_b = cur_row;
            }
        }

        int64_t now = esp_timer_get_time();
        // The lanes only need to keep up with the playhead, not the editor
        if (dirty || (app_audio_is_playing() && now - last_draw > 100000)) {
            dirty     = false;
            last_draw = now;
            render();
        }
    }
}
