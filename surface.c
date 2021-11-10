#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>

typedef struct {
    struct wl_display * display;
    struct wl_registry * registry;

    struct wl_compositor * compositor;
    uint32_t compositor_id;

    struct wl_surface * surface;
} ctx_t;

static void cleanup(ctx_t * ctx) {
    printf("[info] cleaning up\n");
    if (ctx->registry != NULL) wl_registry_destroy(ctx->registry);
    if (ctx->compositor != NULL) wl_compositor_destroy(ctx->compositor);
    if (ctx->surface != NULL) wl_surface_destroy(ctx->surface);
    if (ctx->display != NULL) wl_display_disconnect(ctx->display);
    free(ctx);
}

static void exit_fail(ctx_t * ctx) {
    cleanup(ctx);
    exit(1);
}

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

        ctx->compositor = (struct wl_compositor *)wl_registry_bind(registry, id, &wl_compositor_interface, 1);
        ctx->compositor_id = id;
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
    }

    (void)registry;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_event_add,
    .global_remove = registry_event_remove
};

int main(void) {
    printf("[info] allocating context\n");
    ctx_t * ctx = malloc(sizeof (ctx_t));
    ctx->display = NULL;
    ctx->registry = NULL;
    ctx->compositor = NULL;
    ctx->compositor_id = 0;
    ctx->surface = NULL;

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
    }

    printf("[info] creating surface\n");
    ctx->surface = wl_compositor_create_surface(ctx->compositor);
    if (ctx->surface == NULL) {
        printf("[!] wl_compositor: failed to create surface\n");
        exit_fail(ctx);
    }

    cleanup(ctx);
}
