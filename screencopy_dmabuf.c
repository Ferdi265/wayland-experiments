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
#include <linux-dmabuf-unstable-v1.h>
#include <wlr-screencopy-unstable-v1.h>
#include <xf86drm.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <fcntl.h>

typedef struct {
    struct wl_output * proxy;
    size_t id;
    struct wl_list link;
} output_t;

#define PRINT_DRM_FORMAT(drm_format) \
    ((drm_format) >>  0) & 0xff, \
    ((drm_format) >>  8) & 0xff, \
    ((drm_format) >> 16) & 0xff, \
    ((drm_format) >> 24) & 0xff

#define PRINT_WL_SHM_FORMAT(wl_shm_format) PRINT_DRM_FORMAT(\
    (wl_shm_format) == WL_SHM_FORMAT_ARGB8888 ? DRM_FORMAT_ARGB8888 : \
    (wl_shm_format) == WL_SHM_FORMAT_XRGB8888 ? DRM_FORMAT_XRGB8888 : \
    (wl_shm_format) \
)\

typedef struct {
    uint32_t drm_format;
    uint32_t _padding;
    uint64_t modifier;
} dmabuf_format_table_entry_t;

typedef struct {
    struct wl_display * display;
    struct wl_registry * registry;

    struct wl_compositor * compositor;
    struct wl_shm * shm;
    struct wp_viewporter * viewporter;
    struct xdg_wm_base * xdg_wm_base;
    struct zwp_linux_dmabuf_v1 * linux_dmabuf;
    struct zwlr_screencopy_manager_v1 * screencopy;
    uint32_t compositor_id;
    uint32_t shm_id;
    uint32_t viewporter_id;
    uint32_t xdg_wm_base_id;
    uint32_t linux_dmabuf_id;
    uint32_t screencopy_id;
    struct wl_list /*output_t*/ outputs;

    struct wl_shm_pool * shm_pool;
    struct wl_buffer * shm_buffer;
    uint32_t * shm_pixels;
    size_t shm_size;
    uint32_t shm_width;
    uint32_t shm_height;
    int shm_fd;

    struct zwp_linux_dmabuf_feedback_v1 * dmabuf_feedback;
    struct wl_buffer * dmabuf_buffer;
    uint32_t dmabuf_width;
    uint32_t dmabuf_height;

    struct gbm_device * gbm_main_device;
    struct gbm_bo * gbm_bo;
    dmabuf_format_table_entry_t * dmabuf_format_table;
    uint32_t dmabuf_format_table_length;
    bool gbm_tranche_device_is_main_device;

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

    if (ctx->dmabuf_buffer != NULL) wl_buffer_destroy(ctx->dmabuf_buffer);
    if (ctx->dmabuf_feedback != NULL) zwp_linux_dmabuf_feedback_v1_destroy(ctx->dmabuf_feedback);

    if (ctx->dmabuf_format_table != NULL) munmap(ctx->dmabuf_format_table, ctx->dmabuf_format_table_length * sizeof (dmabuf_format_table_entry_t));
    if (ctx->gbm_bo != NULL) gbm_bo_destroy(ctx->gbm_bo);
    if (ctx->gbm_main_device != NULL) gbm_device_destroy(ctx->gbm_main_device);

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
    if (ctx->linux_dmabuf != NULL) zwp_linux_dmabuf_v1_destroy(ctx->linux_dmabuf);
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
    } else if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
        if (ctx->linux_dmabuf != NULL) {
            printf("[!] wl_registry: duplicate linux_dmabuf\n");
            exit_fail(ctx);
        }

        ctx->linux_dmabuf = (struct zwp_linux_dmabuf_v1 *)wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, 4);
        ctx->linux_dmabuf_id = id;
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
    } else if (id == ctx->linux_dmabuf_id) {
        printf("[!] wl_registry: linux_dmabuf disapperared\n");
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

// --- linux dmabuf event handlers ---

static struct gbm_device * create_gbm_device(ctx_t * ctx, drmDevice *device) {
    drmDevice *devices[64];
    char * render_node = NULL;

    int n = drmGetDevices2(0, devices, sizeof(devices) / sizeof(devices[0]));
    for (int i = 0; i < n; ++i) {
        drmDevice *dev = devices[i];
        if (device && !drmDevicesEqual(device, dev)) {
                continue;
        }
        if (!(dev->available_nodes & (1 << DRM_NODE_RENDER)))
            continue;

        render_node = strdup(dev->nodes[DRM_NODE_RENDER]);
        break;
    }

    drmFreeDevices(devices, n);

    if (render_node == NULL) {
        printf("[error] could not find render node\n");
        free(render_node);
        return NULL;
    }
    printf("[info] using render node %s\n", render_node);

    int fd = open(render_node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("[error] could not open render node\n");
        free(render_node);
        return NULL;
    }

    free(render_node);
    return gbm_create_device(fd);
}

static void linux_dmabuf_feedback_main_device(void * data, struct zwp_linux_dmabuf_feedback_v1 * feedback, struct wl_array * device) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[linux_dmabuf_feedback] main device\n");

    if (ctx->gbm_main_device != NULL) {
        gbm_device_destroy(ctx->gbm_main_device);
    }

    if (device->size != sizeof (dev_t)) {
        printf("[error] array size mismatch: %zd != %zd\n", device->size, sizeof (dev_t));
        exit_fail(ctx);
    }

    dev_t device_id;
    memcpy(&device_id, device->data, sizeof (dev_t));

    drmDevice * drm_device;
    if (drmGetDeviceFromDevId(device_id, 0, &drm_device) != 0) {
        printf("[error] failed to open drm device\n");
        return;
    }

    ctx->gbm_main_device = create_gbm_device(ctx, drm_device);
    if (ctx->gbm_main_device == NULL) {
        printf("[error] failed to open gbm device\n");
    }

    drmFreeDevices(&drm_device, 1);
}

static void linux_dmabuf_feedback_format_table(void * data, struct zwp_linux_dmabuf_feedback_v1 * feedback, int fd, uint32_t size) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[linux_dmabuf_feedback] format table %d (%d bytes)\n", fd, size);

    if (size % sizeof (dmabuf_format_table_entry_t) != 0) {
        printf("[error] dmabuf format table is not a whole number of entries\n");
        exit_fail(ctx);
    }

    if (ctx->dmabuf_format_table != NULL) {
        munmap(ctx->dmabuf_format_table, ctx->dmabuf_format_table_length * sizeof (dmabuf_format_table_entry_t));
    }

    ctx->dmabuf_format_table = (dmabuf_format_table_entry_t *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    ctx->dmabuf_format_table_length = size / sizeof (dmabuf_format_table_entry_t);
    if (ctx->dmabuf_format_table == MAP_FAILED) {
        printf("[error] failed to map format table\n");
        ctx->dmabuf_format_table = NULL;
        exit_fail(ctx);
    }
}

static void linux_dmabuf_feedback_tranche_target_device(void * data, struct zwp_linux_dmabuf_feedback_v1 * feedback, struct wl_array * device) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[linux_dmabuf_feedback] tranche target device\n");

    if (device->size != sizeof (dev_t)) {
        printf("[error] array size mismatch: %zd != %zd\n", device->size, sizeof (dev_t));
        exit_fail(ctx);
    }

    dev_t device_id;
    memcpy(&device_id, device->data, sizeof (dev_t));

    drmDevice * drm_device;
    if (drmGetDeviceFromDevId(device_id, 0, &drm_device) != 0) {
        printf("[error] failed to open drm device\n");
        return;
    }

    if (ctx->gbm_main_device != NULL) {
        drmDevice * drm_main_device;
        drmGetDevice2(gbm_device_get_fd(ctx->gbm_main_device), 0, &drm_main_device);
        ctx->gbm_tranche_device_is_main_device = drmDevicesEqual(drm_main_device, drm_device);
        drmFreeDevices(&drm_main_device, 1);

        if (!ctx->gbm_tranche_device_is_main_device) {
            printf("[error] tranche device is not main device\n");
        }
    } else {
        printf("[info] no main device, using tranche device\n");
        ctx->gbm_main_device = create_gbm_device(ctx, drm_device);
        ctx->gbm_tranche_device_is_main_device = ctx->gbm_main_device != NULL;

        if (ctx->gbm_main_device == NULL) {
            printf("[error] failed to open gbm device\n");
        }
    }

    drmFreeDevices(&drm_device, 1);
}

static void linux_dmabuf_feedback_tranche_flags(void * data, struct zwp_linux_dmabuf_feedback_v1 * feedback, enum zwp_linux_dmabuf_feedback_v1_tranche_flags flags) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[linux_dmabuf_feedback] tranche flags: { %s }\n",
        flags & ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SCANOUT ? "scanout" : ""
    );
}

static void linux_dmabuf_feedback_tranche_formats(void * data, struct zwp_linux_dmabuf_feedback_v1 * feedback, struct wl_array * indices) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[linux_dmabuf_feedback] tranche formats\n");

    if (indices->size % sizeof (uint16_t) != 0) {
        printf("[error] index array size is not a whole number of indices\n");
        exit_fail(ctx);
    }

    if (ctx->dmabuf_format_table == NULL) {
        printf("[error] missing format table\n");
        return;
    }

    if (!ctx->gbm_tranche_device_is_main_device) {
        printf("[error] tranche device is not main device\n");
        return;
    }

    uint16_t * index;
    wl_array_for_each(index, indices) {
        if (*index >= ctx->dmabuf_format_table_length) {
            printf("[error] format index is out of bounds of format table (%d >= %d)\n", *index, ctx->dmabuf_format_table_length);
            exit_fail(ctx);
        }

        uint32_t drm_format = ctx->dmabuf_format_table[*index].drm_format;
        uint64_t modifier = ctx->dmabuf_format_table[*index].modifier;
        printf("[info] format modifier pair: %c%c%c%c %lx\n", PRINT_DRM_FORMAT(drm_format), modifier);
    }
}

static void linux_dmabuf_feedback_tranche_done(void * data, struct zwp_linux_dmabuf_feedback_v1 * feedback) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[linux_dmabuf_feedback] tranche done\n");

    ctx->gbm_tranche_device_is_main_device = false;
}

static void linux_dmabuf_feedback_done(void * data, struct zwp_linux_dmabuf_feedback_v1 * feedback) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[linux_dmabuf_feedback] done\n");
}

static const struct zwp_linux_dmabuf_feedback_v1_listener linux_dmabuf_feedback_listener = {
    .main_device = linux_dmabuf_feedback_main_device,
    .format_table = linux_dmabuf_feedback_format_table,
    .tranche_target_device = linux_dmabuf_feedback_tranche_target_device,
    .tranche_flags = linux_dmabuf_feedback_tranche_flags,
    .tranche_formats = linux_dmabuf_feedback_tranche_formats,
    .tranche_done = linux_dmabuf_feedback_tranche_done,
    .done = linux_dmabuf_feedback_done
};

// --- zwlr_screencopy_frame_v1 event handlers ---

static void zwlr_screencopy_frame_buffer_shm(void * data, struct zwlr_screencopy_frame_v1 * frame, enum wl_shm_format format, uint32_t width, uint32_t height, uint32_t stride) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[zwlr_screencopy_frame] buffer shm %dx%d+%d@%c%c%c%c\n", width, height, stride, PRINT_WL_SHM_FORMAT(format));

    printf("[info] ignoring (no shm support)\n");
}

static void zwlr_screencopy_frame_buffer_dmabuf(void * data, struct zwlr_screencopy_frame_v1 * frame, uint32_t format, uint32_t width, uint32_t height) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[zwlr_screencopy_frame] buffer dmabuf %dx%d@%c%c%c%c\n", width, height, PRINT_DRM_FORMAT(format));

    if (ctx->dmabuf_buffer != NULL) {
        wl_buffer_destroy(ctx->dmabuf_buffer);
    }

    if (ctx->gbm_bo != NULL) {
        gbm_bo_destroy(ctx->gbm_bo);
    }

    ctx->dmabuf_width = width;
    ctx->dmabuf_height = height;

    uint64_t modifiers[] = { DRM_FORMAT_MOD_LINEAR, I915_FORMAT_MOD_X_TILED, I915_FORMAT_MOD_Y_TILED, I915_FORMAT_MOD_Y_TILED_CCS, DRM_FORMAT_MOD_INVALID };
    ctx->gbm_bo = gbm_bo_create_with_modifiers2(ctx->gbm_main_device,
        width, height, format,
        modifiers, sizeof modifiers / sizeof (uint64_t),
        GBM_BO_USE_RENDERING
    );

    struct zwp_linux_buffer_params_v1 * params = zwp_linux_dmabuf_v1_create_params(ctx->linux_dmabuf);
    printf("[info] dmabuf %dx%d@%c%c%c%c with modifier %lx\n", width, height, PRINT_DRM_FORMAT(format), gbm_bo_get_modifier(ctx->gbm_bo));

    for (int plane = 0; plane < gbm_bo_get_plane_count(ctx->gbm_bo); plane++) {
        zwp_linux_buffer_params_v1_add(
            params,
            gbm_bo_get_fd_for_plane(ctx->gbm_bo, plane),
            plane,
            gbm_bo_get_offset(ctx->gbm_bo, plane),
            gbm_bo_get_stride_for_plane(ctx->gbm_bo, plane),
            gbm_bo_get_modifier(ctx->gbm_bo) >> 32,
            gbm_bo_get_modifier(ctx->gbm_bo)
        );
        printf("[info] plane %d: offset %d, stride %d\n", plane, gbm_bo_get_offset(ctx->gbm_bo, plane), gbm_bo_get_stride_for_plane(ctx->gbm_bo, plane));
    }

    ctx->dmabuf_buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, format, 0);
    zwp_linux_buffer_params_v1_destroy(params);
}

static void zwlr_screencopy_frame_buffer_done(void * data, struct zwlr_screencopy_frame_v1 * frame) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[zwlr_screencopy_frame] buffer_done\n");

    zwlr_screencopy_frame_v1_copy(frame, ctx->dmabuf_buffer);
}

static void zwlr_screencopy_frame_flags(void * data, struct zwlr_screencopy_frame_v1 * frame, enum zwlr_screencopy_frame_v1_flags flags) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[zwlr_screencopy_frame] flags\n");
}

static void zwlr_screencopy_frame_ready(void * data, struct zwlr_screencopy_frame_v1 * frame, uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec) {
    ctx_t * ctx = (ctx_t *)data;
    printf("[zwlr_screencopy_frame] ready\n");

    printf("[info] attaching buffer to surface\n");
    wl_surface_attach(ctx->surface, ctx->dmabuf_buffer, 0, 0);

    printf("[info] setting source viewport\n");
    wp_viewport_set_source(ctx->viewport, 0, 0, wl_fixed_from_int(ctx->dmabuf_width), wl_fixed_from_int(ctx->dmabuf_height));

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
    ctx->linux_dmabuf = NULL;
    ctx->linux_dmabuf_id = 0;
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

    ctx->dmabuf_feedback = NULL;
    ctx->dmabuf_buffer = NULL;

    ctx->gbm_main_device = NULL;
    ctx->gbm_bo = NULL;
    ctx->dmabuf_format_table = NULL;
    ctx->dmabuf_format_table_length = 0;
    ctx->gbm_tranche_device_is_main_device = false;

    ctx->surface = NULL;
    ctx->xdg_surface = NULL;
    ctx->xdg_toplevel = NULL;
    ctx->screencopy_frame = NULL;

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
    } else if (ctx->linux_dmabuf == NULL) {
        printf("[!] wl_registry: no linux_dmabuf found\n");
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

    printf("[info] getting surface dmabuf feedback\n");
    ctx->dmabuf_feedback = zwp_linux_dmabuf_v1_get_surface_feedback(ctx->linux_dmabuf, ctx->surface);

    printf("[info] creating linux dmabuf feedback listener\n");
    zwp_linux_dmabuf_feedback_v1_add_listener(ctx->dmabuf_feedback, &linux_dmabuf_feedback_listener, (void *)ctx);

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
