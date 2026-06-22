#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <ctype.h>
#include <sys/stat.h>
#include <wayland-client.h>

#include "loginui.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct xdg_wm_base *wm_base;

struct g_output {
    struct wl_output *wl_output;
    uint32_t name;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    uint32_t width, height;
    bool configured;
    struct g_output *next;
};
static struct g_output *outputs;

static struct wl_surface *pv_surface;
static struct xdg_surface *pv_xsurf;
static struct xdg_toplevel *pv_top;
static int pv_w = 900, pv_h = 560;
static bool pv_configured = false;

static bool preview = false;
static bool running = true;

static cairo_surface_t *logo = NULL;

static double g_alpha = 1.0;

static double mono_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ── assets ─────────────────────────────────────────────────────────────── */

static bool try_logo_file(const char *name) {
    if (!name || !name[0]) return false;
    const char *tpl[] = {
        "/opt/local/share/icons/hicolor/scalable/apps/%s.svg",
        "/usr/local/share/icons/hicolor/scalable/apps/%s.svg",
        "/usr/share/icons/hicolor/scalable/apps/%s.svg",
        "/usr/share/pixmaps/%s.svg",
        "/usr/share/pixmaps/%s.png",
        "/usr/share/icons/hicolor/256x256/apps/%s.png",
        NULL
    };
    for (int i = 0; tpl[i]; i++) {
        char p[1024];
        snprintf(p, sizeof p, tpl[i], name);
        if (access(p, R_OK) == 0) { logo = loginui_load_image(p, -1, 256); if (logo) return true; }
    }
    return false;
}

static void os_release_value(const char *key, char *out, size_t n) {
    out[0] = '\0';
    FILE *f = fopen("/etc/os-release", "r");
    if (!f) return;
    char line[512];
    size_t klen = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) != 0 || line[klen] != '=') continue;
        char *v = line + klen + 1;
        while (*v == '"' || *v == '\'') v++;
        char *end = v + strlen(v);
        while (end > v && (end[-1] == '\n' || end[-1] == '"' || end[-1] == '\'' || isspace((unsigned char)end[-1]))) end--;
        *end = '\0';
        snprintf(out, n, "%s", v);
        break;
    }
    fclose(f);
}

static void load_logo(void) {
    char logo_name[128], id[128];
    os_release_value("LOGO", logo_name, sizeof logo_name);
    os_release_value("ID", id, sizeof id);
    if (try_logo_file(logo_name)) return;
    if (try_logo_file(id)) return;
    try_logo_file("emblem-singularity");
}

/* ── render ─────────────────────────────────────────────────────────────── */

static void render_surface(struct wl_surface *surface, int w, int h) {
    cairo_t *cr;
    struct loginui_buffer *b = loginui_create_buffer(shm, w, h, &cr);
    if (!b) return;

    cairo_push_group(cr);
    loginui_render_splash(cr, w, h, NULL, logo, mono_seconds());
    cairo_pop_group_to_source(cr);
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint_with_alpha(cr, g_alpha);
    cairo_restore(cr);

    cairo_destroy(cr);
    wl_surface_attach(surface, b->wl_buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, w, h);
    wl_surface_commit(surface);
}

static void render_all(void) {
    if (preview) {
        if (pv_configured) render_surface(pv_surface, pv_w, pv_h);
        return;
    }
    for (struct g_output *o = outputs; o; o = o->next)
        if (o->configured) render_surface(o->surface, (int)o->width, (int)o->height);
}

/* ── layer surface ──────────────────────────────────────────────────────── */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                            uint32_t serial, uint32_t w, uint32_t h) {
    struct g_output *o = data;
    o->width = w; o->height = h; o->configured = true;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    render_surface(o->surface, (int)w, (int)h);
}
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) { (void)data; (void)ls; running = false; }
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_configure, .closed = layer_closed,
};

static void create_layer_surface(struct g_output *o) {
    if (o->layer_surface || !layer_shell) return;
    o->surface = wl_compositor_create_surface(compositor);
    o->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, o->surface, o->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "splash");
    zwlr_layer_surface_v1_set_anchor(o->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer_surface, -1);
    zwlr_layer_surface_v1_add_listener(o->layer_surface, &layer_surface_listener, o);
    wl_surface_commit(o->surface);
}

/* ── preview window ─────────────────────────────────────────────────────── */

static void xdg_surface_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(xs, serial);
    pv_configured = true;
    render_surface(pv_surface, pv_w, pv_h);
}
static const struct xdg_surface_listener xdg_surface_listener = { .configure = xdg_surface_configure };

static void xdg_top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states) {
    (void)data; (void)t; (void)states;
    if (w > 0) pv_w = w;
    if (h > 0) pv_h = h;
}
static void xdg_top_close(void *data, struct xdg_toplevel *t) { (void)data; (void)t; running = false; }
static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_top_configure, .close = xdg_top_close,
};

static void wm_base_ping(void *d, struct xdg_wm_base *b, uint32_t serial) { (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_base_listener = { .ping = wm_base_ping };

static void create_preview_window(void) {
    pv_surface = wl_compositor_create_surface(compositor);
    pv_xsurf = xdg_wm_base_get_xdg_surface(wm_base, pv_surface);
    xdg_surface_add_listener(pv_xsurf, &xdg_surface_listener, NULL);
    pv_top = xdg_surface_get_toplevel(pv_xsurf);
    xdg_toplevel_add_listener(pv_top, &xdg_toplevel_listener, NULL);
    xdg_toplevel_set_title(pv_top, "Singularity Splash (preview)");
    wl_surface_commit(pv_surface);
}

/* ── registry ───────────────────────────────────────────────────────────── */

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version) {
    (void)data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        struct g_output *o = calloc(1, sizeof(*o));
        o->name = name;
        o->wl_output = wl_registry_bind(reg, name, &wl_output_interface, version < 3 ? version : 3);
        o->next = outputs;
        outputs = o;
    }
}
static void reg_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg;
    struct g_output **pp = &outputs;
    while (*pp) {
        if ((*pp)->name == name) {
            struct g_output *dead = *pp;
            *pp = dead->next;
            if (dead->layer_surface) zwlr_layer_surface_v1_destroy(dead->layer_surface);
            if (dead->surface) wl_surface_destroy(dead->surface);
            if (dead->wl_output) wl_output_destroy(dead->wl_output);
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}
static const struct wl_registry_listener registry_listener = { reg_global, reg_remove };

/* ── main ───────────────────────────────────────────────────────────────── */

static bool ready_flag_present(const char *path) {
    return path && access(path, F_OK) == 0;
}

int main(int argc, char **argv) {
    const double TIMEOUT_S = 30.0;
    const double FADE_S = 0.25;

    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--preview") == 0) preview = true;
    if (getenv("SINGULARITY_SPLASH_PREVIEW")) preview = true;

    load_logo();

    char ready_path[512] = "";
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0]) snprintf(ready_path, sizeof ready_path, "%s/singularity-shell-ready", rt);

    display = wl_display_connect(NULL);
    if (!display) { fprintf(stderr, "splash: cannot connect to Wayland display\n"); return 1; }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || (!preview && !layer_shell) || (preview && !wm_base)) {
        fprintf(stderr, "splash: compositor missing required globals\n");
        return 1;
    }

    if (preview) create_preview_window();
    else for (struct g_output *o = outputs; o; o = o->next) create_layer_surface(o);
    wl_display_roundtrip(display);

    int wfd = wl_display_get_fd(display);
    double start_t = mono_seconds();
    bool fading = false;
    double fade_t0 = 0.0;

    while (running) {
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        wl_display_flush(display);

        struct pollfd pfd = { wfd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 33);
        if (pr > 0 && (pfd.revents & POLLIN)) wl_display_read_events(display);
        else wl_display_cancel_read(display);
        wl_display_dispatch_pending(display);

        double t = mono_seconds();
        if (!preview) {
            if (!fading && (ready_flag_present(ready_path) || (t - start_t) > TIMEOUT_S)) {
                fading = true; fade_t0 = t;
            }
            if (fading) {
                double f = (t - fade_t0) / FADE_S;
                g_alpha = 1.0 - f;
                if (g_alpha <= 0.0) { g_alpha = 0.0; running = false; }
            }
        }

        render_all();
    }

    wl_display_roundtrip(display);
    return 0;
}
