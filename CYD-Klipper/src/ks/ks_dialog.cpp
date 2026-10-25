// KlipperScreen confirm dialog (screen._confirm_send_action): dark card, title, text, Cancel / action.
#include "ks_ui.h"
#include "ks_panels.h"

struct Dlg { lv_obj_t* overlay; void (*ok)(void*); void* user; };

static void close_dlg(Dlg* d) { lv_obj_del(d->overlay); }   // Dlg is freed by the overlay's DELETE handler
static void on_cancel(lv_event_t* e) { close_dlg((Dlg*)lv_event_get_user_data(e)); }
static void on_ok(lv_event_t* e) {
    Dlg* d = (Dlg*)lv_event_get_user_data(e);
    void (*ok)(void*) = d->ok; void* u = d->user;
    close_dlg(d);
    if (ok) ok(u);
}
static void on_del(lv_event_t* e) { delete (Dlg*)lv_event_get_user_data(e); }

void ks_confirm(const char* title, const char* text, const char* ok_label, void (*ok)(void*), void* user) {
    Dlg* d = new Dlg{nullptr, ok, user};
    lv_obj_t* ov = lv_obj_create(lv_layer_top());                  // full-screen dimmer swallowing touches
    d->overlay = ov;
    lv_obj_set_size(ov, 800, 480);
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_60, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ov, on_del, LV_EVENT_DELETE, d);

    lv_obj_t* card = lv_obj_create(ov);                           // .dialog
    lv_obj_set_size(card, 560, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, KS_ACTIVE, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* t = ks_label(card, title, &lv_font_montserrat_28);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP); lv_obj_set_width(t, lv_pct(100));
    if (text && *text) {
        lv_obj_t* x = ks_label(card, text, &lv_font_montserrat_22);
        lv_label_set_long_mode(x, LV_LABEL_LONG_WRAP); lv_obj_set_width(x, lv_pct(100));
    }
    lv_obj_t* row = lv_obj_create(card);
    lv_obj_set_size(row, lv_pct(100), 84);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* b1 = ks_button(row, "cancel", "Cancel", 1, on_cancel, d);
    lv_obj_set_size(b1, 240, 80);
    lv_obj_t* b2 = ks_button(row, "complete", ok_label ? ok_label : "OK", 3, on_ok, d);
    lv_obj_set_size(b2, 240, 80);
}
