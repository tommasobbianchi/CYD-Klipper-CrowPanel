// panels/print.py (Sovol patch/klipperscreen/print.py): G-code file browser.
// Differences forced by the 40 KB LVGL heap (LV_MEM_SIZE): the list is paged (ROWS rows at a time, page buttons) instead of
// one scrolling widget per file; thumbnails are limited to THUMB_MAX_PIX decoded pixels (the 32x32 slicer thumbnail), because
// the PNG decoder allocates in the LVGL heap and a 300x300 thumbnail (360 KB) cannot be decoded there.
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <strings.h>
#include <time.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_cfg.h"
#include "ks_panels.h"
#include "../conf/global_config.h"

#define THUMB_MAX_PIX 2304      // raise only together with LV_MEM_SIZE
#define ROWS 4
#define ROW_H 74

namespace {

struct Entry {
    std::string name;
    bool dir = false;
    double mod = 0;
    uint32_t size = 0;
    float est = -1;             // metadata estimated_time (s)
    bool meta = false;          // metadata fetched
    std::string thumb;          // thumbnail relative_path
    int tw = 0, th = 0;
};

struct Row {
    int idx = -1;
    lv_obj_t *box = nullptr, *info = nullptr;
    uint8_t* png = nullptr;
    lv_img_dsc_t dsc;
    bool thumb_done = false;
};

struct Pr {
    uint32_t id = 0, lid = 0;   // id: row requests, lid: directory request (both from g_gen, so never equal)
    std::vector<Entry> e;
    int page = 0, mi = 0, nrows = 0;
    bool loading = true, failed = false;
    lv_obj_t *list = nullptr, *path = nullptr, *pgl = nullptr;
    lv_obj_t* sortb[3] = {};
    Row rows[ROWS];
};

Pr* g_pr = nullptr;
uint32_t g_gen = 0;
std::string g_dir;              // current directory relative to gcodes ("" = root)
bool g_keep_dir = false;        // set before opening a sub page so that coming back keeps the directory

std::string urlenc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}
std::string join(const std::string& dir, const std::string& n) { return dir.empty() ? n : dir + "/" + n; }
std::string dir_of(const std::string& rel) { size_t k = rel.rfind('/'); return k == std::string::npos ? "" : rel.substr(0, k); }

bool is_gcode(const std::string& n) {
    size_t k = n.rfind('.');
    if (k == std::string::npos) return false;
    const char* x = n.c_str() + k;
    return !strcasecmp(x, ".gcode") || !strcasecmp(x, ".g") || !strcasecmp(x, ".gco");
}
std::string display_name(const Entry& e) {          // os.path.splitext(name)[0].replace("_", " ") for files
    if (e.dir) return e.name;
    std::string s = e.name;
    size_t k = s.rfind('.');
    if (k != std::string::npos && k > 0) s.erase(k);
    std::replace(s.begin(), s.end(), '_', ' ');
    return s;
}
void fmt_size(uint32_t sz, char* o, size_t n) {
    if (sz < 1024u * 1024u) snprintf(o, n, "%.1f kB", sz / 1024.0);
    else if (sz < 1024u * 1024u * 1024u) snprintf(o, n, "%.1f MB", sz / 1048576.0);
    else snprintf(o, n, "%.1f GB", sz / 1073741824.0);
}
void fmt_date(double mod, char* o, size_t n) {
    time_t t = (time_t)mod; struct tm tm; localtime_r(&t, &tm);
    strftime(o, n, ks_cfg_bool("24htime", true) ? "%Y/%m/%d %H:%M" : "%Y/%m/%d %I:%M %p", &tm);
}
void fmt_info(const Entry& e, char* o, size_t n) {
    char d[28]; fmt_date(e.mod, d, sizeof d);
    if (e.dir) { snprintf(o, n, "%s", d); return; }
    char sz[16], et[16] = "";
    fmt_size(e.size, sz, sizeof sz);
    if (e.est >= 1) ks_fmt_duration(e.est, et, sizeof et);
    snprintf(o, n, "%s   %s%s%s", d, sz, et[0] ? "   " : "", et);
}

// metadata -> estimated time + the biggest thumbnail that still fits THUMB_MAX_PIX (else the smallest one)
void meta_parse(JsonObjectConst r, float& est, std::string& thumb, int& tw, int& th) {
    est = r["estimated_time"] | -1.0f;
    thumb.clear(); tw = th = 0;
    int best = -1, sw = 0, sh = 0; long sa = 1L << 30;
    std::string spath;
    for (JsonObjectConst t : r["thumbnails"].as<JsonArrayConst>()) {
        int w = t["width"] | 0, h = t["height"] | 0; const char* p = t["relative_path"] | "";
        if (!w || !h || !*p) continue;
        long a = (long)w * h;
        if (a <= THUMB_MAX_PIX && a > best) { best = (int)a; thumb = p; tw = w; th = h; }
        if (a < sa) { sa = a; spath = p; sw = w; sh = h; }
    }
    if (best < 0 && !spath.empty() && sa <= 4L * THUMB_MAX_PIX) { thumb = spath; tw = sw; th = sh; }   // too big otherwise: no thumbnail
}
std::string thumb_url(const std::string& rel, const std::string& thumb) {
    return "/server/files/gcodes/" + urlenc(join(dir_of(rel), thumb));
}

void show_image(lv_obj_t* box, lv_img_dsc_t* dsc, uint8_t* png, size_t len, int w, int h, int target) {
    memset(dsc, 0, sizeof *dsc);
    dsc->header.cf = LV_IMG_CF_RAW_ALPHA;
    dsc->header.w = w; dsc->header.h = h;
    dsc->data_size = len; dsc->data = png;
    lv_obj_clean(box);
    lv_obj_t* im = lv_img_create(box);
    lv_img_set_src(im, dsc);
    int m = w > h ? w : h;
    if (m > 0) lv_img_set_zoom(im, 256 * target / m);
    lv_obj_center(im);
}

// ---- private DELETE (ks_mr only offers GET/POST) ----------------------------------------------------------------------------
struct DelJob { std::string path; volatile int state = 0; };   // 0 running, 1 ok, 2 failed
void load_dir(Pr* p);
void del_task(void* a) {
    DelJob* j = (DelJob*)a;
    PrinterConfiguration* pc = &global_config.printer_config[global_config.printer_index];
    int code = -1;
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient c;
        c.setReuse(false); c.setConnectTimeout(6000); c.setTimeout(8000);
        c.begin("http://" + String(pc->printer_host) + ":" + String(pc->klipper_port) + j->path.c_str());
        if (pc->printer_auth[0]) c.addHeader("X-Api-Key", pc->printer_auth);
        code = c.sendRequest("DELETE");
        c.end();
    }
    j->state = (code >= 200 && code < 300) ? 1 : 2;
    vTaskDelete(nullptr);
}
void del_poll(lv_timer_t* t) {
    DelJob* j = (DelJob*)t->user_data;
    if (!j->state) return;
    if (j->state == 1) { ks_toast("Deleted"); if (g_pr) load_dir(g_pr); }
    else ks_toast("Delete failed");
    lv_timer_del(t);
    delete j;
}
void start_delete(const std::string& rel, bool dir) {
    DelJob* j = new DelJob;
    j->path = dir ? "/server/files/directory?path=" + urlenc("gcodes/" + rel) + "&force=true"
                  : "/server/files/gcodes/" + urlenc(rel);
    if (xTaskCreate(del_task, "ks_del", 8192, j, 1, nullptr) != pdPASS) { delete j; ks_toast("Delete failed"); return; }
    lv_timer_create(del_poll, 200, j);
}

// ---- row requests: metadata then thumbnail, one at a time ------------------------------------------------------------------
void step(Pr* p);
uint32_t tok(Pr* p, int mi) { return (p->id << 4) | (uint32_t)mi; }
Pr* tok_pr(void* u, int& mi) {
    uint32_t t = (uint32_t)(uintptr_t)u;
    mi = t & 15;
    return (g_pr && g_pr->id == (t >> 4)) ? g_pr : nullptr;
}

void row_png(bool ok, uint8_t* data, size_t len, void* u) {
    int mi; Pr* p = tok_pr(u, mi);
    if (!p || !ok || !data) { ks_free_bytes(data); if (p) { p->mi++; step(p); } return; }
    Row& r = p->rows[mi];
    Entry& en = p->e[r.idx];
    r.png = data;
    show_image(r.box, &r.dsc, data, len, en.tw, en.th, 64);
    p->mi++;
    step(p);
}
void row_meta(bool ok, JsonDocument& d, void* u) {
    int mi; Pr* p = tok_pr(u, mi);
    if (!p) return;
    Entry& en = p->e[p->rows[mi].idx];
    en.meta = true;
    if (ok) {
        meta_parse(d["result"].as<JsonObjectConst>(), en.est, en.thumb, en.tw, en.th);
        char b[96]; fmt_info(en, b, sizeof b);
        if (p->rows[mi].info) lv_label_set_text(p->rows[mi].info, b);
    }
    step(p);
}
void step(Pr* p) {
    while (p->mi < p->nrows) {
        Row& r = p->rows[p->mi];
        Entry& en = p->e[r.idx];
        if (en.dir) { p->mi++; continue; }
        std::string rel = join(g_dir, en.name);
        if (!en.meta) {
            std::string u = "/server/files/metadata?filename=" + urlenc(rel);
            ks_get_async(u.c_str(), row_meta, (void*)(uintptr_t)tok(p, p->mi));
            return;
        }
        if (!r.thumb_done && !en.thumb.empty()) {
            r.thumb_done = true;
            std::string u = thumb_url(rel, en.thumb);
            ks_get_bytes_async(u.c_str(), row_png, (void*)(uintptr_t)tok(p, p->mi));
            return;
        }
        p->mi++;
    }
}

// ---- detail / print dialog ---------------------------------------------------------------------------------------------------
struct Dlg {
    lv_obj_t *ov = nullptr, *box = nullptr, *info = nullptr;
    uint8_t* png = nullptr;
    lv_img_dsc_t dsc;
    uint32_t tok = 0;
    Entry en;
    std::string rel;
};
Dlg* g_dlg = nullptr;
std::string g_del_path; bool g_del_dir = false;
std::string g_ren_src;
bool g_ren_open = false;
lv_obj_t* g_ren_ta = nullptr;

void dlg_close() { if (g_dlg && g_dlg->ov) lv_obj_del(g_dlg->ov); }
void dlg_gone(lv_event_t* e) {
    Dlg* d = (Dlg*)lv_event_get_user_data(e);
    if (d->png) ks_free_bytes(d->png);
    if (g_dlg == d) g_dlg = nullptr;
    delete d;
}
void dlg_info_text(Dlg* d) {
    char b[160], t[16] = "", date[28], sz[16];
    fmt_date(d->en.mod, date, sizeof date);
    if (d->en.dir) snprintf(b, sizeof b, "Modified: %s", date);
    else {
        fmt_size(d->en.size, sz, sizeof sz);
        if (d->en.est >= 1) ks_fmt_duration(d->en.est, t, sizeof t);
        snprintf(b, sizeof b, "Uploaded: %s\nSize: %s%s%s", date, sz, t[0] ? "\nPrint Time: " : "", t);
    }
    lv_label_set_text(d->info, b);
}
void dlg_png(bool ok, uint8_t* data, size_t len, void* u) {
    if (!g_dlg || g_dlg->tok != (uint32_t)(uintptr_t)u || !ok || !data) { ks_free_bytes(data); return; }
    g_dlg->png = data;
    show_image(g_dlg->box, &g_dlg->dsc, data, len, g_dlg->en.tw, g_dlg->en.th, 140);
}
void dlg_fetch_thumb() {
    if (g_dlg->en.thumb.empty()) return;
    std::string u = thumb_url(g_dlg->rel, g_dlg->en.thumb);
    ks_get_bytes_async(u.c_str(), dlg_png, (void*)(uintptr_t)g_dlg->tok);
}
void dlg_meta(bool ok, JsonDocument& d, void* u) {
    if (!g_dlg || g_dlg->tok != (uint32_t)(uintptr_t)u) return;
    g_dlg->en.meta = true;
    if (!ok) return;
    meta_parse(d["result"].as<JsonObjectConst>(), g_dlg->en.est, g_dlg->en.thumb, g_dlg->en.tw, g_dlg->en.th);
    dlg_info_text(g_dlg);
    dlg_fetch_thumb();
}

void start_cb(bool ok, JsonDocument&, void*) { if (ok) ks_home(); else ks_toast("Could not start the print"); }
void do_print(lv_event_t*) {
    if (!g_dlg) return;
    std::string path = "/printer/print/start?filename=" + urlenc(g_dlg->rel);
    dlg_close();
    ks_post_async(path.c_str(), "{}", start_cb, nullptr);   // job_status takes over once print_stats says "printing"
}
void del_ok(void*) { start_delete(g_del_path, g_del_dir); }
void do_delete(lv_event_t*) {
    if (!g_dlg) return;
    g_del_path = g_dlg->rel; g_del_dir = g_dlg->en.dir;
    dlg_close();
    ks_confirm(g_del_dir ? "Delete Directory?" : "Delete File?", ("gcodes/" + g_del_path).c_str(), "Delete", del_ok, nullptr);
}

void ren_cb(bool ok, JsonDocument&, void*) {
    if (!ok) { ks_toast("Rename failed"); return; }
    ks_toast("Renamed");
    if (g_ren_open) ks_back();
}
void rename_save(lv_event_t*) {
    if (!g_ren_ta) return;
    std::string dst = lv_textarea_get_text(g_ren_ta);
    ks_keyboard_hide();
    if (dst.empty() || dst == g_ren_src) { ks_back(); return; }
    JsonDocument d;
    d["source"] = "gcodes/" + g_ren_src;
    d["dest"] = "gcodes/" + dst;
    String body; serializeJson(d, body);
    ks_post_async("/server/files/move", body.c_str(), ren_cb, nullptr);
}
void rename_init(lv_obj_t* c) {                       // show_rename(): "Rename/Move:" entry + Save
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_style_pad_row(c, 14, 0);
    ks_label(c, "Rename/Move:", &ks_font_22);
    g_ren_ta = ks_textarea(c, "", 640);
    lv_textarea_set_text(g_ren_ta, g_ren_src.c_str());
    g_ren_open = true;
    lv_obj_add_event_cb(g_ren_ta, [](lv_event_t*) { g_ren_open = false; g_ren_ta = nullptr; }, LV_EVENT_DELETE, nullptr);
    lv_obj_t* b = ks_button_sized(c, "complete", 40, "Save", 3, rename_save, nullptr);
    lv_obj_set_size(b, 220, 84);
}
void do_rename(lv_event_t*) {
    if (!g_dlg) return;
    g_ren_src = g_dlg->rel;
    dlg_close();
    g_keep_dir = true;
    ks_open("Rename", rename_init);
}

void open_dialog(const Entry& en) {
    if (g_dlg) dlg_close();
    Dlg* d = new Dlg;
    g_dlg = d;
    d->en = en;
    d->rel = join(g_dir, en.name);
    d->tok = ++g_gen;
    lv_obj_t* ov = lv_obj_create(lv_layer_top());
    d->ov = ov;
    lv_obj_set_size(ov, 800, 480);
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_60, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ov, dlg_gone, LV_EVENT_DELETE, d);

    lv_obj_t* card = lv_obj_create(ov);
    lv_obj_set_size(card, 720, 330);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, KS_ACTIVE, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    d->box = ks_card(card, 150, 150);
    ks_img(d->box, en.dir ? "folder" : "file", 72, lv_color_white());
    lv_obj_center(lv_obj_get_child(d->box, 0));
    lv_obj_align(d->box, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t* txt = ks_card(card, 500, 160);
    lv_obj_align(txt, LV_ALIGN_TOP_LEFT, 166, 0);
    lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(txt, 8, 0);
    lv_obj_t* nm = ks_label(txt, en.name.c_str(), &ks_font_28);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(nm, lv_pct(100));
    d->info = ks_label(txt, "", &ks_font_22);
    lv_obj_set_style_text_color(d->info, lv_color_hex(0x9AA5B1), 0);
    dlg_info_text(d);

    lv_obj_t* row = ks_card(card, lv_pct(100), 88);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    auto add = [&](const char* icon, const char* label, int color, lv_event_cb_t cb) {
        lv_obj_t* b = ks_button_sized(row, icon, 40, label, color, cb, nullptr);
        lv_obj_set_size(b, 158, 84);
    };
    add("cancel", "Cancel", 1, [](lv_event_t*) { dlg_close(); });
    add("files", "Rename", 2, do_rename);
    add("delete", "Delete", 1, do_delete);
    if (!en.dir && is_gcode(en.name)) add("print", "Print", 3, do_print);

    if (!en.dir) {
        if (!en.meta) {
            std::string u = "/server/files/metadata?filename=" + urlenc(d->rel);
            ks_get_async(u.c_str(), dlg_meta, (void*)(uintptr_t)d->tok);
        } else dlg_fetch_thumb();
    }
}

// ---- list ---------------------------------------------------------------------------------------------------------------------
void clear_rows(Pr* p) {
    for (Row& r : p->rows) { if (r.png) ks_free_bytes(r.png); r = Row(); }
}
void show_page(Pr* p);

void sort_fill(Pr* p, int i) {
    lv_obj_t* b = p->sortb[i];
    int v = ks_cfg_int("print_sort", 3);
    lv_obj_clean(b);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(b, 4, 0);
    static const char* const N[3] = {"Name", "Date", "Size"};
    ks_label(b, N[i], &ks_font_22);
    if ((v >> 1) == i) ks_img(b, (v & 1) ? "arrow-down" : "arrow-up", 40, lv_color_white());   // arrow-up = ascending, like Python
}
void sort_entries(Pr* p) {
    int v = ks_cfg_int("print_sort", 3), f = v >> 1; bool desc = v & 1;
    size_t start = (!p->e.empty() && p->e[0].name == "..") ? 1 : 0;
    std::stable_sort(p->e.begin() + start, p->e.end(), [&](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;                    // folders first
        int ff = (a.dir && f == 2) ? 0 : f;                  // folders have no meaningful size
        int c;
        if (ff == 1) c = (a.mod > b.mod) - (a.mod < b.mod);
        else if (ff == 2) c = (a.size > b.size) - (a.size < b.size);
        else c = 0;
        if (c == 0) c = strcasecmp(a.name.c_str(), b.name.c_str());
        return desc ? c > 0 : c < 0;
    });
}
void sort_click(lv_event_t* e) {
    Pr* p = g_pr; if (!p) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int v = ks_cfg_int("print_sort", 3);
    int nv = ((v >> 1) == i) ? (i * 2 + ((v & 1) ^ 1)) : i * 2;   // same key toggles direction, new key starts ascending
    ks_cfg_set_int("print_sort", nv);
    sort_entries(p);
    p->page = 0;
    for (int k = 0; k < 3; k++) sort_fill(p, k);
    show_page(p);
}
void refresh_click(lv_event_t*) { if (g_pr) load_dir(g_pr); }
void page_click(lv_event_t* e) {
    Pr* p = g_pr; if (!p) return;
    p->page += (int)(intptr_t)lv_event_get_user_data(e);
    show_page(p);
}

void row_click(lv_event_t* e) {
    Pr* p = g_pr; if (!p) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)p->e.size()) return;
    const Entry en = p->e[idx];                              // copy: load_dir() below replaces the vector
    if (en.dir) {
        if (en.name == "..") g_dir = dir_of(g_dir);
        else g_dir = join(g_dir, en.name);
        p->page = 0;
        load_dir(p);
    } else open_dialog(en);
}
void row_long(lv_event_t* e) {                               // folder: rename / delete
    Pr* p = g_pr; if (!p) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < (int)p->e.size()) open_dialog(p->e[idx]);
}

void build_row(Pr* p, int slot, int idx) {
    Entry& en = p->e[idx];
    Row& r = p->rows[slot];
    r.idx = idx;
    lv_obj_t* row = lv_obj_create(p->list);
    lv_obj_set_size(row, lv_pct(100), ROW_H);
    lv_obj_set_style_bg_color(row, KS_BG, 0);
    lv_obj_set_style_bg_color(row, KS_ACTIVE, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, KS_ACTIVE, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, row_click, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
    if (en.dir && en.name != "..") lv_obj_add_event_cb(row, row_long, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)idx);

    r.box = ks_card(row, 64, 64);
    lv_obj_align(r.box, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* ic = ks_img(r.box, en.dir ? (en.name == ".." ? "back" : "folder") : "file", 56, lv_color_white());
    lv_obj_center(ic);

    lv_obj_t* nm = ks_label(row, display_name(en).c_str(), &ks_font_22);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nm, 520);
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 76, 4);
    r.info = ks_label(row, "", &ks_font_18);
    lv_obj_set_style_text_color(r.info, lv_color_hex(0x9AA5B1), 0);
    lv_label_set_long_mode(r.info, LV_LABEL_LONG_DOT);
    lv_obj_set_width(r.info, 520);
    lv_obj_align(r.info, LV_ALIGN_TOP_LEFT, 76, 38);
    if (en.name != "..") { char b[96]; fmt_info(en, b, sizeof b); lv_label_set_text(r.info, b); }

    if (en.dir ? en.name != ".." : is_gcode(en.name)) {      // "load" opens a folder, "print" opens the print dialog
        lv_obj_t* b = ks_button(row, en.dir ? "load" : "print", nullptr, 3, row_click, (void*)(intptr_t)idx);
        lv_obj_set_size(b, 84, ROW_H - 8);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

void show_page(Pr* p) {
    lv_obj_clean(p->list);
    clear_rows(p);
    p->id = ++g_gen;                                         // drops answers still in flight for the old page
    int n = (int)p->e.size(), pages = (n + ROWS - 1) / ROWS;
    if (pages < 1) pages = 1;
    if (p->page >= pages) p->page = pages - 1;
    if (p->page < 0) p->page = 0;
    p->nrows = 0;
    for (int i = 0; i < ROWS; i++) {
        int idx = p->page * ROWS + i;
        if (idx >= n) break;
        build_row(p, i, idx);
        p->nrows++;
    }
    if (n == 0) {
        lv_obj_t* m = ks_label(p->list, p->loading ? "Loading..." : p->failed ? "Could not read the file list" : "No files", &ks_font_28);
        lv_obj_set_style_text_color(m, lv_color_hex(0x9AA5B1), 0);
    }
    char b[24]; snprintf(b, sizeof b, "%d / %d", p->page + 1, pages);
    lv_label_set_text(p->pgl, b);
    char pb[160]; snprintf(pb, sizeof pb, "  /%s", g_dir.c_str());
    lv_label_set_text(p->path, pb);
    p->mi = 0;
    step(p);
}

void dir_cb(bool ok, JsonDocument& d, void* u) {
    Pr* p = g_pr;
    if (!p || p->lid != (uint32_t)(uintptr_t)u) return;
    p->loading = false;
    p->failed = !ok;
    p->e.clear();
    if (ok) {
        if (!g_dir.empty()) { Entry up; up.name = ".."; up.dir = true; p->e.push_back(up); }
        size_t cap = 200;
        for (JsonObjectConst o : d["result"]["dirs"].as<JsonArrayConst>()) {
            const char* n = o["dirname"] | "";
            if (!*n || n[0] == '.' || p->e.size() >= cap) continue;
            Entry en; en.name = n; en.dir = true; en.mod = o["modified"] | 0.0; p->e.push_back(std::move(en));
        }
        for (JsonObjectConst o : d["result"]["files"].as<JsonArrayConst>()) {
            const char* n = o["filename"] | "";
            if (!*n || n[0] == '.' || p->e.size() >= cap) continue;
            Entry en; en.name = n; en.mod = o["modified"] | 0.0; en.size = o["size"] | 0u; p->e.push_back(std::move(en));
        }
        sort_entries(p);
    }
    show_page(p);
}
void load_dir(Pr* p) {
    p->loading = true;
    p->lid = ++g_gen;
    p->id = ++g_gen;
    p->e.clear();
    show_page(p);
    std::string u = "/server/files/directory?path=" + urlenc(join("gcodes", g_dir));
    ks_get_async(u.c_str(), dir_cb, (void*)(uintptr_t)p->lid);
}

void pr_gone(lv_event_t* e) {
    Pr* p = (Pr*)lv_event_get_user_data(e);
    clear_rows(p);
    if (g_pr == p) g_pr = nullptr;
    delete p;
}

}   // namespace

void ks_panel_print(lv_obj_t* c) {
    if (!g_keep_dir) g_dir.clear();                          // activate(): back to the gcodes root
    g_keep_dir = false;
    Pr* p = new Pr;
    g_pr = p;
    lv_obj_t* root = ks_card(c, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(root, pr_gone, LV_EVENT_DELETE, p);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* bar = ks_card(root, lv_pct(100), 64);          // sort buttons + refresh + page up / down
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < 3; i++) {
        p->sortb[i] = ks_button(bar, nullptr, nullptr, i + 1, sort_click, (void*)(intptr_t)i);
        lv_obj_set_size(p->sortb[i], 124, 64);
        sort_fill(p, i);
    }
    lv_obj_set_size(ks_button(bar, "refresh", nullptr, 4, refresh_click, nullptr), 84, 64);
    lv_obj_set_size(ks_button(bar, "arrow-up", nullptr, 0, page_click, (void*)(intptr_t)-1), 84, 64);
    lv_obj_set_size(ks_button(bar, "arrow-down", nullptr, 0, page_click, (void*)(intptr_t)1), 84, 64);

    lv_obj_t* pr = ks_card(root, lv_pct(100), 32);           // current path + page counter
    p->path = ks_label(pr, "", &ks_font_22);
    lv_obj_align(p->path, LV_ALIGN_LEFT_MID, 0, 0);
    p->pgl = ks_label(pr, "", &ks_font_22);
    lv_obj_align(p->pgl, LV_ALIGN_RIGHT_MID, -8, 0);

    p->list = ks_list(root);
    lv_obj_set_height(p->list, 320);
    lv_obj_clear_flag(p->list, LV_OBJ_FLAG_SCROLLABLE);      // paged: a drag must not scroll half a row

    load_dir(p);
}
