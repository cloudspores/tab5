#include "launcher.h"
#include "theme.h"
#include "topbar.h"
#include "settings.h"
#include <cstring>
#include <cstdio>
#include "esp_log.h"

static const char *TAG = "launcher";

namespace {
const App *apps[8]; int napps = 0;
lv_obj_t *home_scr = nullptr;
lv_obj_t *card_status[8];
const App *visible = nullptr;

void card_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const App *a = (const App *)lv_event_get_user_data(e);
    launcher::open(a->id);
}

void build_home()
{
    using namespace theme;
    home_scr = screen();
    topbar::create(home_scr, "tab5");
    const int top = PAD + topbar::HEIGHT + GAP;
    const int cols = 3, rows = 2;
    const int cw = (W - 2 * PAD - (cols - 1) * GAP) / cols;
    const int ch = (H - top - PAD - (rows - 1) * GAP) / rows;
    for (int i = 0; i < napps && i < cols * rows; i++) {
        const App *a = apps[i];
        int x = PAD + (i % cols) * (cw + GAP), y = top + (i / cols) * (ch + GAP);
        lv_obj_t *p = panel(home_scr, x, y, cw, ch);
        lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(p, card_cb, LV_EVENT_ALL, (void *)a);
        char num[4]; snprintf(num, sizeof num, "%02d", i + 1);
        module_label(p, num, "APP");
        if (a->icon) {
            lv_obj_t *ic = label(p, a->icon, &lv_font_montserrat_24, INK);
            lv_obj_align(ic, LV_ALIGN_TOP_RIGHT, 0, -2);
        }
        lv_obj_t *name = label(p, a->name, &familjen_bold_52, INK);
        lv_obj_set_style_text_font(name, &familjen_bold_52, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, -6);
        lv_obj_t *desc = label(p, a->desc, &familjen_medium_24, MID);
        lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(desc, cw - 36);
        lv_obj_align_to(desc, name, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
        card_status[i] = label(p, "TAP TO OPEN", &jbmono_14, MID);
        lv_obj_align(card_status[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_t *k = keycap(p, LV_SYMBOL_RIGHT, 44, 32, false, card_cb, (void *)a);
        lv_obj_set_style_text_font(lv_obj_get_child(k, 0), &lv_font_montserrat_14, 0);
        lv_obj_align(k, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    }
}
}

namespace launcher {

void add(const App *app) { if (napps < 8) apps[napps++] = app; }

void init()
{
    theme::lock();
    build_home();
    theme::unlock();
    topbar::on_home(home);
}

void refresh()
{
    theme::lock();
    for (int i = 0; i < napps; i++) {
        const char *s = apps[i]->status ? apps[i]->status() : nullptr;
        lv_label_set_text(card_status[i], s && *s ? s : "TAP TO OPEN");
        lv_obj_set_style_text_color(card_status[i], lv_color_hex(s && *s ? theme::ORANGE : theme::MID), 0);
    }
    theme::unlock();
}

void home()
{
    if (visible && visible->exit) visible->exit();
    visible = nullptr;
    refresh();
    theme::lock();
    lv_screen_load(home_scr);
    theme::unlock();
    settings::set_str("app", "home");
    ESP_LOGI(TAG, "home");
}

void open(const char *id)
{
    for (int i = 0; i < napps; i++) {
        if (strcmp(apps[i]->id, id)) continue;
        if (visible == apps[i]) return;
        if (!apps[i]->screen) { ESP_LOGW(TAG, "%s has no screen yet", id); return; }
        if (visible && visible->exit) visible->exit();
        theme::lock();
        lv_obj_t *scr = apps[i]->screen();
        lv_screen_load(scr);
        theme::unlock();
        visible = apps[i];
        if (apps[i]->enter) apps[i]->enter();
        settings::set_str("app", id);
        ESP_LOGI(TAG, "open %s", id);
        return;
    }
    ESP_LOGW(TAG, "no app %s", id);
}

const char *current() { return visible ? visible->id : "home"; }

}
