#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-util.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-egl-core.h>
#include <wayland-egl.h>
#include <xdg-shell.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

typedef struct {
    struct wl_display * display;
    struct wl_registry * registry;

    struct wl_compositor * compositor;
    struct wl_shm * shm;
    struct xdg_wm_base * xdg_wm_base;
    uint32_t compositor_id;
    uint32_t shm_id;
    uint32_t xdg_wm_base_id;

    struct wl_surface * surface;
    struct xdg_surface * xdg_surface;
    struct xdg_toplevel * xdg_toplevel;
    struct wl_egl_window * egl_window;

    uint32_t last_surface_serial;
    bool xdg_surface_configured;
    bool xdg_toplevel_configured;
    bool configured;
    bool closing;

    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLConfig egl_config;
    EGLSurface egl_surface;
    uint32_t width;
    uint32_t height;
    bool egl_initialized;
} ctx_t;

static void cleanup(ctx_t * ctx) {
    printf("[info] cleaning up\n");

    if (ctx->egl_context != EGL_NO_SURFACE) eglDestroyContext(ctx->egl_display, ctx->egl_context);
    if (ctx->egl_surface != EGL_NO_SURFACE) eglDestroySurface(ctx->egl_display, ctx->egl_surface);
    if (ctx->egl_window != EGL_NO_SURFACE) wl_egl_window_destroy(ctx->egl_window);
    if (ctx->egl_display != EGL_NO_DISPLAY) eglTerminate(ctx->egl_display);

    if (ctx->xdg_toplevel != NULL) xdg_toplevel_destroy(ctx->xdg_toplevel);
    if (ctx->xdg_surface != NULL) xdg_surface_destroy(ctx->xdg_surface);
    if (ctx->surface != NULL) wl_surface_destroy(ctx->surface);

    if (ctx->xdg_wm_base != NULL) xdg_wm_base_destroy(ctx->xdg_wm_base);
    if (ctx->shm != NULL) wl_shm_destroy(ctx->shm);
    if (ctx->compositor != NULL) wl_compositor_destroy(ctx->compositor);
    if (ctx->registry != NULL) wl_registry_destroy(ctx->registry);
    if (ctx->display != NULL) wl_display_disconnect(ctx->display);

    free(ctx);
}

static void exit_fail(ctx_t * ctx) {
    cleanup(ctx);
    exit(1);
}

// --- wl_registry event handlers ---

static void registry_event_add(
    void * data, struct wl_registry * registry,
    uint32_t id, const char * interface, uint32_t version
) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[registry][+] id=%08x %s v%d\n", id, interface, version);

    if (strcmp(interface, "wl_compositor") == 0) {
        if (ctx->compositor != NULL) {
            printf("[!] wl_registry: duplicate compositor\n");
            exit_fail(ctx);
        }

        ctx->compositor = (struct wl_compositor *)wl_registry_bind(registry, id, &wl_compositor_interface, 4);
        ctx->compositor_id = id;
    } else if (strcmp(interface, "wl_shm") == 0) {
        if (ctx->compositor != NULL) {
            printf("[!] wl_registry: duplicate shm\n");
            exit_fail(ctx);
        }

        ctx->shm = (struct wl_shm *)wl_registry_bind(registry, id, &wl_shm_interface, 1);
        ctx->shm_id = id;
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        if (ctx->xdg_wm_base != NULL) {
            printf("[!] wl_registry: duplicate xdg_wm_base\n");
            exit_fail(ctx);
        }

        ctx->xdg_wm_base = (struct xdg_wm_base *)wl_registry_bind(registry, id, &xdg_wm_base_interface, 2);
        ctx->xdg_wm_base_id = id;
    }
}

static void registry_event_remove(
    void * data, struct wl_registry * registry,
    uint32_t id
) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[registry][-] id=%08x\n", id);

    if (id == ctx->compositor_id) {
        printf("[!] wl_registry: compositor disapperared\n");
        exit_fail(ctx);
    } else if (id == ctx->shm_id) {
        printf("[!] wl_registry: shm disapperared\n");
        exit_fail(ctx);
    } else if (id == ctx->xdg_wm_base_id) {
        printf("[!] wl_registry: xdg_wm_base disapperared\n");
        exit_fail(ctx);
    }

    (void)registry;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_event_add,
    .global_remove = registry_event_remove
};

// --- xdg_wm_base event handlers ---

static void xdg_wm_base_event_ping(
    void * data, struct xdg_wm_base * xdg_wm_base, uint32_t serial
) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[xdg_wm_base] ping %d\n", serial);
    xdg_wm_base_pong(xdg_wm_base, serial);

    (void)ctx;
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_event_ping
};

// --- configure callbacks ---

static void surface_configure_finished(ctx_t * ctx) {
    printf("[info] acknowledging configure\n");
    xdg_surface_ack_configure(ctx->xdg_surface, ctx->last_surface_serial);

    printf("[info] committing surface\n");
    wl_surface_commit(ctx->surface);

    ctx->xdg_surface_configured = false;
    ctx->xdg_toplevel_configured = false;
    ctx->configured = true;
}

// --- xdg_surface event handlers ---

static void xdg_surface_event_configure(
    void * data, struct xdg_surface * xdg_surface, uint32_t serial
) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[xdg_surface] configure %d\n", serial);

    ctx->last_surface_serial = serial;
    ctx->xdg_surface_configured = true;
    if (ctx->xdg_surface_configured && ctx->xdg_toplevel_configured) {
        surface_configure_finished(ctx);
    }
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_event_configure,
};

// --- xdg_toplevel event handlers ---

static void xdg_toplevel_event_configure(
    void * data, struct xdg_toplevel * xdg_toplevel,
    int32_t width, int32_t height, struct wl_array * states
) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[xdg_toplevel] configure width=%d, height=%d\n", width, height);

    printf("[xdg_toplevel] states = {");
    enum xdg_toplevel_state * state;
    wl_array_for_each(state, states) {
        switch (*state) {
            case XDG_TOPLEVEL_STATE_MAXIMIZED:
                printf("maximized");
                break;
            case XDG_TOPLEVEL_STATE_FULLSCREEN:
                printf("fullscreen");
                break;
            case XDG_TOPLEVEL_STATE_RESIZING:
                printf("resizing");
                break;
            case XDG_TOPLEVEL_STATE_ACTIVATED:
                printf("activated");
                break;
            case XDG_TOPLEVEL_STATE_TILED_LEFT:
                printf("tiled-left");
                break;
            case XDG_TOPLEVEL_STATE_TILED_RIGHT:
                printf("tiled-right");
                break;
            case XDG_TOPLEVEL_STATE_TILED_TOP:
                printf("tiled-top");
                break;
            case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
                printf("tiled-bottom");
                break;
            default:
                printf("%d", *state);
                break;
        }
        printf(", ");
    }
    printf("}\n");

    if (width == 0) width = 100;
    if (height == 0) height = 100;
    if (ctx->egl_initialized && (width != ctx->width || height != ctx->height)) {
        ctx->width = width;
        ctx->height = height;

        printf("[info] resizing EGL window\n");
        wl_egl_window_resize(ctx->egl_window, width, height, 0, 0);

        printf("[info] clearing frame\n");
        glClearColor(1.0, 1.0, 0.0, 1.0);
        glClear(GL_COLOR_BUFFER_BIT);
        glFlush();

        if (eglSwapBuffers(ctx->egl_display, ctx->egl_surface) != EGL_TRUE) {
            printf("[!] eglSwapBuffers: failed to swap buffers\n");
            exit_fail(ctx);
        }
    }

    ctx->xdg_toplevel_configured = true;
    if (ctx->xdg_surface_configured && ctx->xdg_toplevel_configured) {
        surface_configure_finished(ctx);
    }
}

static void xdg_toplevel_event_close(
    void * data, struct xdg_toplevel * xdg_toplevel
) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[xdg_surface] close\n");

    printf("[info] closing\n");
    ctx->closing = true;

    (void)ctx;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_event_configure,
    .close = xdg_toplevel_event_close
};

// --- egl initialization ---

void init_egl(ctx_t * ctx) {
    printf("[info] creating EGL display\n");
    ctx->egl_display = eglGetDisplay((EGLNativeDisplayType)ctx->display);
    if (ctx->egl_display == EGL_NO_DISPLAY) {
        printf("[!] eglGetDisplay: failed to create EGL display\n");
        exit_fail(ctx);
    }

    EGLint major, minor;
    printf("[info] initializing EGL display\n");
    if (eglInitialize(ctx->egl_display, &major, &minor) != EGL_TRUE) {
        printf("[!] eglGetDisplay: failed to initialize EGL display\n");
        exit_fail(ctx);
    }
    printf("[info] initialized EGL %d.%d\n", major, minor);

    EGLint num_configs;
    printf("[info] getting number of EGL configs\n");
    if (eglGetConfigs(ctx->egl_display, NULL, 0, &num_configs) != EGL_TRUE) {
        printf("[!] eglGetConfigs: failed to get number of EGL configs\n");
        exit_fail(ctx);
    }

    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    printf("[info] getting EGL config\n");
    if (eglChooseConfig(ctx->egl_display, config_attribs, &ctx->egl_config, 1, &num_configs) != EGL_TRUE) {
        printf("[!] eglChooseConfig: failed to get EGL config\n");
        exit_fail(ctx);
    }

    if (ctx->width == 0) ctx->width = 100;
    if (ctx->height == 0) ctx->height = 100;
    printf("[info] creating EGL window\n");
    ctx->egl_window = wl_egl_window_create(ctx->surface, ctx->width, ctx->height);
    if (ctx->egl_window == EGL_NO_SURFACE) {
        printf("[!] wl_egl_window: failed to create EGL window\n");
        exit_fail(ctx);
    }

    printf("[info] creating EGL surface\n");
    ctx->egl_surface = eglCreateWindowSurface(ctx->egl_display, ctx->egl_config, ctx->egl_window, NULL);

    EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    printf("[info] creating EGL context\n");
    ctx->egl_context = eglCreateContext(ctx->egl_display, ctx->egl_config, EGL_NO_CONTEXT, context_attribs);
    if (ctx->egl_context == EGL_NO_CONTEXT) {
        printf("[!] eglCreateContext: failed to create EGL context\n");
        exit_fail(ctx);
    }

    printf("[info] activating EGL context\n");
    if (eglMakeCurrent(ctx->egl_display, ctx->egl_surface, ctx->egl_surface, ctx->egl_context) != EGL_TRUE) {
        printf("[!] eglMakeCurrent: failed to activate EGL context\n");
        exit_fail(ctx);
    }

    printf("[info] clearing frame\n");
    glClearColor(1.0, 1.0, 0.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    glFlush();

    if (eglSwapBuffers(ctx->egl_display, ctx->egl_surface) != EGL_TRUE) {
        printf("[!] eglSwapBuffers: failed to swap buffers\n");
        exit_fail(ctx);
    }

    ctx->egl_initialized = true;
}

int main(void) {
    printf("[info] allocating context\n");
    ctx_t * ctx = malloc(sizeof (ctx_t));
    ctx->display = NULL;
    ctx->registry = NULL;

    ctx->compositor = NULL;
    ctx->compositor_id = 0;
    ctx->xdg_wm_base = NULL;
    ctx->xdg_wm_base_id = 0;

    ctx->surface = NULL;
    ctx->xdg_surface = NULL;
    ctx->xdg_toplevel = NULL;
    ctx->egl_window = EGL_NO_SURFACE;

    ctx->last_surface_serial = 0;
    ctx->xdg_surface_configured = false;
    ctx->xdg_toplevel_configured = false;
    ctx->configured = false;
    ctx->closing = false;

    ctx->egl_display = EGL_NO_DISPLAY;
    ctx->egl_context = EGL_NO_CONTEXT;
    ctx->egl_surface = EGL_NO_SURFACE;
    ctx->width = 1;
    ctx->height = 1;
    ctx->egl_initialized = false;

    if (ctx == NULL) {
        printf("[!] malloc: allocating context failed\n");
        exit_fail(ctx);
    }

    printf("[info] connecting to display\n");
    ctx->display = wl_display_connect(NULL);
    if (ctx->display == NULL) {
        printf("[!] wl_display: connect failed\n");
        exit_fail(ctx);
    }

    printf("[info] getting registry\n");
    ctx->registry = wl_display_get_registry(ctx->display);
    wl_registry_add_listener(ctx->registry, &registry_listener, (void *)ctx);

    printf("[info] waiting for events\n");
    wl_display_roundtrip(ctx->display);

    printf("[info] checking if protocols found\n");
    if (ctx->compositor == NULL) {
        printf("[!] wl_registry: no compositor found\n");
        exit_fail(ctx);
    } else if (ctx->shm == NULL) {
        printf("[!] wl_registry: no shm found\n");
        exit_fail(ctx);
    } else if (ctx->xdg_wm_base == NULL) {
        printf("[!] wl_registry: no xdg_wm_base found\n");
        exit_fail(ctx);
    }

    printf("[info] creating surface\n");
    ctx->surface = wl_compositor_create_surface(ctx->compositor);
    if (ctx->surface == NULL) {
        printf("[!] wl_compositor: failed to create surface\n");
        exit_fail(ctx);
    }

    printf("[info] creating xdg_wm_base listener\n");
    xdg_wm_base_add_listener(ctx->xdg_wm_base, &xdg_wm_base_listener, (void *)ctx);

    printf("[info] creating xdg_surface\n");
    ctx->xdg_surface = xdg_wm_base_get_xdg_surface(ctx->xdg_wm_base, ctx->surface);
    if (ctx->xdg_surface == NULL) {
        printf("[!] xdg_wm_base: failed to create xdg_surface\n");
        exit_fail(ctx);
    }
    xdg_surface_add_listener(ctx->xdg_surface, &xdg_surface_listener, (void *)ctx);

    printf("[info] creating xdg_toplevel\n");
    ctx->xdg_toplevel = xdg_surface_get_toplevel(ctx->xdg_surface);
    if (ctx->xdg_toplevel == NULL) {
        printf("[!] xdg_surface: failed to create xdg_toplevel\n");
        exit_fail(ctx);
    }
    xdg_toplevel_add_listener(ctx->xdg_toplevel, &xdg_toplevel_listener, (void *)ctx);

    printf("[info] setting xdg_toplevel properties\n");
    xdg_toplevel_set_app_id(ctx->xdg_toplevel, "example");
    xdg_toplevel_set_title(ctx->xdg_toplevel, "example window");

    printf("[info] committing surface to trigger configure events\n");
    wl_surface_commit(ctx->surface);

    printf("[info] waiting for events\n");
    wl_display_roundtrip(ctx->display);

    printf("[info] checking if surface configured\n");
    if (!ctx->configured) {
        printf("[!] xdg_surface: surface not configured\n");
        exit_fail(ctx);
    }

    printf("[info] initializing EGL\n");
    init_egl(ctx);

    printf("[info] entering event loop\n");
    while (wl_display_dispatch(ctx->display) != -1 && !ctx->closing) {}
    printf("[info] exiting event loop\n");

    cleanup(ctx);
}
