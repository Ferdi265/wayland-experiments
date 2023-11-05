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
#include <viewporter.h>
#include <xdg-shell.h>
#include <wlr-screencopy-unstable-v1.h>

typedef struct {
    struct wl_output * proxy;
    size_t id;
    struct wl_list link;
} output_t;

typedef struct {
    struct wl_display * display;
    struct wl_registry * registry;

    struct wl_compositor * compositor;
    struct wl_shm * shm;
    struct wp_viewporter * viewporter;
    struct xdg_wm_base * xdg_wm_base;
    struct zwlr_screencopy_manager_v1 * screencopy;
    uint32_t compositor_id;
    uint32_t shm_id;
    uint32_t viewporter_id;
    uint32_t xdg_wm_base_id;
    uint32_t screencopy_id;
    struct wl_list /*output_t*/ outputs;

    struct wl_shm_pool * shm_pool;
    struct wl_buffer * shm_buffer;
    uint32_t * shm_pixels;
    size_t shm_size;
    uint32_t shm_width;
    uint32_t shm_height;
    int shm_fd;

    struct wl_surface * surface;
    struct wp_viewport * viewport;
    struct xdg_surface * xdg_surface;
    struct xdg_toplevel * xdg_toplevel;
    struct zwlr_screencopy_frame_v1 * screencopy_frame;

    uint32_t last_surface_serial;
    uint32_t win_width;
    uint32_t win_height;
    bool xdg_surface_configured;
    bool xdg_toplevel_configured;
    bool configured;
    bool closing;
} ctx_t;

static void cleanup(ctx_t * ctx) {
    printf("[info] cleaning up\n");

    if (ctx->screencopy_frame != NULL) zwlr_screencopy_frame_v1_destroy(ctx->screencopy_frame);
    if (ctx->xdg_toplevel != NULL) xdg_toplevel_destroy(ctx->xdg_toplevel);
    if (ctx->xdg_surface != NULL) xdg_surface_destroy(ctx->xdg_surface);
    if (ctx->viewport != NULL) wp_viewport_destroy(ctx->viewport);
    if (ctx->surface != NULL) wl_surface_destroy(ctx->surface);

    if (ctx->shm_buffer != NULL) wl_buffer_destroy(ctx->shm_buffer);
    if (ctx->shm_pool != NULL) wl_shm_pool_destroy(ctx->shm_pool);
    if (ctx->shm_pixels != NULL) munmap(ctx->shm_pixels, ctx->shm_size);
    if (ctx->shm_fd != -1) close(ctx->shm_fd);

    output_t *output, *output_next;
    wl_list_for_each_safe(output, output_next, &ctx->outputs, link) {
        wl_list_remove(&output->link);
        wl_output_destroy(output->proxy);
        free(output);
    }

    if (ctx->screencopy != NULL) zwlr_screencopy_manager_v1_destroy(ctx->screencopy);
    if (ctx->xdg_wm_base != NULL) xdg_wm_base_destroy(ctx->xdg_wm_base);
    if (ctx->viewporter != NULL) wp_viewporter_destroy(ctx->viewporter);
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
        if (ctx->shm != NULL) {
            printf("[!] wl_registry: duplicate shm\n");
            exit_fail(ctx);
        }

        ctx->shm = (struct wl_shm *)wl_registry_bind(registry, id, &wl_shm_interface, 1);
        ctx->shm_id = id;
    } else if (strcmp(interface, "wp_viewporter") == 0) {
        if (ctx->viewporter != NULL) {
            printf("[!] wl_registry: duplicate viewporter\n");
            exit_fail(ctx);
        }

        ctx->viewporter = (struct wp_viewporter *)wl_registry_bind(registry, id, &wp_viewporter_interface, 1);
        ctx->viewporter_id = id;
    } else if (strcmp(interface, "xdg_wm_base") == 0) {
        if (ctx->xdg_wm_base != NULL) {
            printf("[!] wl_registry: duplicate xdg_wm_base\n");
            exit_fail(ctx);
        }

        ctx->xdg_wm_base = (struct xdg_wm_base *)wl_registry_bind(registry, id, &xdg_wm_base_interface, 2);
        ctx->xdg_wm_base_id = id;
    } else if (strcmp(interface, "zwlr_screencopy_manager_v1") == 0) {
        if (ctx->screencopy != NULL) {
            printf("[!] wl_registry: duplicate screencopy\n");
            exit_fail(ctx);
        }

        ctx->screencopy = (struct zwlr_screencopy_manager_v1 *)wl_registry_bind(registry, id, &zwlr_screencopy_manager_v1_interface, 3);
        ctx->screencopy_id = id;
    } else if (strcmp(interface, "wl_output") == 0) {
        output_t * output = malloc(sizeof (output_t));
        if (output == NULL) {
            printf("[!] wl_registry: failed to allocate output handle\n");
            exit_fail(ctx);
        }

        printf("[info] binding output\n");
        output->proxy = wl_registry_bind(registry, id, &wl_output_interface, 1);
        output->id = id;
        wl_list_insert(&ctx->outputs, &output->link);
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
    } else if (id == ctx->viewporter_id) {
        printf("[!] wl_registry: viewporter disapperared\n");
        exit_fail(ctx);
    } else if (id == ctx->xdg_wm_base_id) {
        printf("[!] wl_registry: xdg_wm_base disapperared\n");
        exit_fail(ctx);
    } else if (id == ctx->screencopy_id) {
        printf("[!] wl_registry: screencopy disapperared\n");
        exit_fail(ctx);
    } else {
        output_t *output, *output_next;
        wl_list_for_each_safe(output, output_next, &ctx->outputs, link) {
            if (output->id == id) {
                wl_list_remove(&output->link);
                wl_output_destroy(output->proxy);
                free(output);
            }
        }
    }

    (void)registry;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_event_add,
    .global_remove = registry_event_remove
};

// --- zwlr_screencopy_frame_v1 event handlers ---

static void resize_shm_buffer(ctx_t * ctx, enum wl_shm_format format, uint32_t width, uint32_t height, uint32_t stride) {
    uint32_t bytes_per_pixel = width / stride;
    uint32_t size = stride * height;

    if (size > ctx->shm_size) {
        if (ctx->shm_buffer != NULL) {
            printf("[info] destroying old shm buffer\n");
            wl_buffer_destroy(ctx->shm_buffer);
            ctx->shm_buffer = NULL;
        }

        printf("[info] resizing shm file\n");
        if (ftruncate(ctx->shm_fd, size) == -1) {
            printf("[!] ftruncate: failed to resize shm file\n");
            exit_fail(ctx);
        }

        printf("[info] remapping shm file\n");
        void * new_pixels = mremap(ctx->shm_pixels, ctx->shm_size, size, MREMAP_MAYMOVE);
        if (new_pixels == MAP_FAILED) {
            printf("[!] mremap: failed to remap shm file\n");
            exit_fail(ctx);
        }
        ctx->shm_pixels = (uint32_t *)new_pixels;
        ctx->shm_size = size;

        printf("[info] resizing shm pool\n");
        wl_shm_pool_resize(ctx->shm_pool, size);
    }

    ctx->shm_width = width;
    ctx->shm_height = height;

    if (ctx->shm_buffer != NULL) {
        printf("[info] destroying old shm buffer\n");
        wl_buffer_destroy(ctx->shm_buffer);
        ctx->shm_buffer = NULL;
    }

    printf("[info] creating shm buffer\n");
    ctx->shm_buffer = wl_shm_pool_create_buffer(ctx->shm_pool, 0, width, height, stride, format);
    if (ctx->shm_buffer == NULL) {
        printf("[!] wl_shm_pool: failed to create buffer\n");
        exit_fail(ctx);
    }
}

static void zwlr_screencopy_frame_buffer_shm(void * data, struct zwlr_screencopy_frame_v1 * frame, enum wl_shm_format format, uint32_t width, uint32_t height, uint32_t stride) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[zwlr_screencopy_frame] buffer shm %dx%d+%d@%d\n", width, height, stride, format);

    printf("[info] creating screencopy texture\n");
    resize_shm_buffer(ctx, format, width, height, stride);
}

static void zwlr_screencopy_frame_buffer_dmabuf(void * data, struct zwlr_screencopy_frame_v1 * frame, uint32_t format, uint32_t width, uint32_t height) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[zwlr_screencopy_frame] buffer dmabuf %dx%d@%d\n", width, height, format);

    printf("[info] ignoring (no dmabuf support)\n");
}

static void zwlr_screencopy_frame_buffer_done(void * data, struct zwlr_screencopy_frame_v1 * frame) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[zwlr_screencopy_frame] buffer_done\n");

    zwlr_screencopy_frame_v1_copy(frame, ctx->shm_buffer);
}

static void zwlr_screencopy_frame_flags(void * data, struct zwlr_screencopy_frame_v1 * frame, enum zwlr_screencopy_frame_v1_flags flags) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[zwlr_screencopy_frame] flags\n");
}

static void zwlr_screencopy_frame_ready(void * data, struct zwlr_screencopy_frame_v1 * frame, uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[zwlr_screencopy_frame] ready\n");

    printf("[info] attaching buffer to surface\n");
    wl_surface_attach(ctx->surface, ctx->shm_buffer, 0, 0);

    printf("[info] setting source viewport\n");
    wp_viewport_set_source(ctx->viewport, 0, 0, wl_fixed_from_int(ctx->shm_width), wl_fixed_from_int(ctx->shm_height));

    printf("[info] committing surface\n");
    wl_surface_commit(ctx->surface);
}

static void zwlr_screencopy_frame_failed(void * data, struct zwlr_screencopy_frame_v1 * frame) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[zwlr_screencopy_frame] failed\n");
}

static const struct zwlr_screencopy_frame_v1_listener zwlr_screencopy_frame_listener = {
    .buffer = zwlr_screencopy_frame_buffer_shm,
    .linux_dmabuf = zwlr_screencopy_frame_buffer_dmabuf,
    .buffer_done = zwlr_screencopy_frame_buffer_done,
    .flags = zwlr_screencopy_frame_flags,
    .ready = zwlr_screencopy_frame_ready,
    .failed = zwlr_screencopy_frame_failed
};

// --- wl_surface event handlers ---

static void wl_surface_enter(void * data, struct wl_surface * surface, struct wl_output * output) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[wl_surface] enter\n");

    if (ctx->screencopy_frame != NULL) {
        zwlr_screencopy_frame_v1_destroy(ctx->screencopy_frame);
    }

    ctx->screencopy_frame = zwlr_screencopy_manager_v1_capture_output(ctx->screencopy, 0, output);
    zwlr_screencopy_frame_v1_add_listener(ctx->screencopy_frame, &zwlr_screencopy_frame_listener, (void *)ctx);
}

static const struct wl_surface_listener wl_surface_listener = {
    .enter = wl_surface_enter
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

static void surface_configure_resize(ctx_t * ctx, uint32_t width, uint32_t height) {
    if (ctx->shm_buffer == NULL) {
        printf("[info] creating fallback texture\n");
        resize_shm_buffer(ctx, WL_SHM_FORMAT_XRGB8888, 1, 1, 4);

        printf("[info] drawing grey pixels\n");
        memset(ctx->shm_pixels, 0xcc, ctx->shm_size);

        printf("[info] attaching buffer to surface\n");
        wl_surface_attach(ctx->surface, ctx->shm_buffer, 0, 0);

        printf("[info] setting source viewport\n");
        wp_viewport_set_source(ctx->viewport, 0, 0, wl_fixed_from_int(1), wl_fixed_from_int(1));
    }

    printf("[info] setting destination viewport\n");
    wp_viewport_set_destination(ctx->viewport, width, height);
}

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

    if (width != ctx->win_width || height != ctx->win_height) {
        ctx->win_width = width;
        ctx->win_height = height;
        surface_configure_resize(ctx, width, height);
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

int main(void) {
    printf("[info] allocating context\n");
    ctx_t * ctx = malloc(sizeof (ctx_t));
    ctx->display = NULL;
    ctx->registry = NULL;

    ctx->compositor = NULL;
    ctx->compositor_id = 0;
    ctx->shm = NULL;
    ctx->shm_id = 0;
    ctx->viewporter = NULL;
    ctx->viewporter_id = 0;
    ctx->xdg_wm_base = NULL;
    ctx->xdg_wm_base_id = 0;
    ctx->screencopy = NULL;
    ctx->screencopy_id = 0;
    wl_list_init(&ctx->outputs);

    ctx->shm_pool = NULL;
    ctx->shm_buffer = NULL;
    ctx->shm_pixels = NULL;
    ctx->shm_size = 0;
    ctx->shm_width = 0;
    ctx->shm_height = 0;
    ctx->shm_fd = -1;

    ctx->surface = NULL;
    ctx->xdg_surface = NULL;
    ctx->xdg_toplevel = NULL;

    ctx->last_surface_serial = 0;
    ctx->win_width = 0;
    ctx->win_height = 0;
    ctx->xdg_surface_configured = false;
    ctx->xdg_toplevel_configured = false;
    ctx->configured = false;
    ctx->closing = false;

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
    } else if (ctx->viewporter == NULL) {
        printf("[!] wl_registry: no viewporter found\n");
        exit_fail(ctx);
    } else if (ctx->xdg_wm_base == NULL) {
        printf("[!] wl_registry: no xdg_wm_base found\n");
        exit_fail(ctx);
    } else if (ctx->screencopy == NULL) {
        printf("[!] wl_registry: no screencopy found\n");
        exit_fail(ctx);
    }

    printf("[info] creating shm file\n");
    ctx->shm_fd = memfd_create("wl_shm_buffer", 0);
    if (ctx->shm_fd == -1) {
        printf("[!] memfd_create: failed to create shm file\n");
        exit_fail(ctx);
    }

    printf("[info] resizing shm file\n");
    ctx->shm_size = 1;
    if (ftruncate(ctx->shm_fd, ctx->shm_size) == -1) {
        printf("[!] ftruncate: failed to resize shm file\n");
        exit_fail(ctx);
    }

    printf("[info] mapping shm file\n");
    ctx->shm_pixels = (uint32_t *)mmap(NULL, ctx->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->shm_fd, 0);
    if (ctx->shm_pixels == MAP_FAILED) {
        ctx->shm_pixels = NULL;
        printf("[!] mmap: failed to map shm file\n");
        exit_fail(ctx);
    }

    printf("[info] creating shm pool\n");
    ctx->shm_pool = wl_shm_create_pool(ctx->shm, ctx->shm_fd, ctx->shm_size);
    if (ctx->shm_pool == NULL) {
        printf("[!] wl_shm: failed to create shm pool\n");
        exit_fail(ctx);
    }

    printf("[info] creating surface\n");
    ctx->surface = wl_compositor_create_surface(ctx->compositor);
    if (ctx->surface == NULL) {
        printf("[!] wl_compositor: failed to create surface\n");
        exit_fail(ctx);
    }

    printf("[info] creating wl_surface listener\n");
    wl_surface_add_listener(ctx->surface, &wl_surface_listener, (void *)ctx);

    printf("[info] creating viewport\n");
    ctx->viewport = wp_viewporter_get_viewport(ctx->viewporter, ctx->surface);
    if (ctx->viewport == NULL) {
        printf("[!] wl_compositor: failed to create viewport\n");
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

    printf("[info] entering event loop\n");
    while (wl_display_dispatch(ctx->display) != -1 && !ctx->closing) {}
    printf("[info] exiting event loop\n");

    cleanup(ctx);
}
