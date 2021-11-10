#include <stdio.h>
#include <stdlib.h>
#include <wayland-client.h>

int main(void) {
    struct wl_display * display = wl_display_connect(NULL);
    if (display == NULL) {
        printf("[!] wl_display_connect\n");
        exit(1);
    }

    wl_display_disconnect(display);
}
