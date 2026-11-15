/* mpclink-cli: test the MPC side without the plugin.
 * Runs on a computer, or on an MPC over SSH (Developer mode).
 *
 *   mpclink-cli [-n "MPC Name"] [-h host[:port]] find
 *   mpclink-cli ... list
 *   mpclink-cli ... send <local file> [remote path]
 *   mpclink-cli ... get <path in Beats folder> [dest dir]
 *   mpclink-cli ... inbox [dest dir]
 */
#include "mpclink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int print_entry(const mpclink_entry *e, void *user) {
    (void)user;
    printf("%10lld  %s\n", e->size, e->path);
    return 0;
}

static int usage(void) {
    fprintf(stderr, "usage: mpclink-cli [-n name] [-h host[:port]] find|list|send FILE [REMOTE]|get PATH [DIR]|inbox [DIR]\n");
    return 2;
}

int main(int argc, char **argv) {
    const char *name = "MPC Test", *host = NULL;
    int i = 1;
    for (; i + 1 < argc && argv[i][0] == '-'; i += 2) {
        if (!strcmp(argv[i], "-n")) name = argv[i + 1];
        else if (!strcmp(argv[i], "-h")) host = argv[i + 1];
        else return usage();
    }
    if (i >= argc) return usage();
    const char *cmd = argv[i++];

    mpclink_ctx ctx;
    mpclink_init(&ctx, name);
    int r;
    if (host) {
        char h[64]; int port = MPCLINK_PORT;
        snprintf(h, sizeof h, "%s", host);
        char *colon = strchr(h, ':');
        if (colon) { *colon = 0; port = atoi(colon + 1); }
        mpclink_set_hub(&ctx, h, port);
    } else if ((r = mpclink_discover(&ctx, 1500)) != MPCLINK_OK) {
        fprintf(stderr, "%s (try -h <Mac IP>)\n", mpclink_strerror(r));
        return 1;
    }

    if (!strcmp(cmd, "find")) {
        r = mpclink_ping(&ctx);
        if (r == MPCLINK_OK) printf("hub \"%s\" at %s:%d\n", ctx.hub_name, ctx.host, ctx.port);
    } else if (!strcmp(cmd, "list")) {
        r = mpclink_list(&ctx, print_entry, NULL);
    } else if (!strcmp(cmd, "send") && i < argc) {
        r = mpclink_upload(&ctx, argv[i], i + 1 < argc ? argv[i + 1] : NULL);
        if (r == MPCLINK_OK) printf("sent %s\n", argv[i]);
    } else if (!strcmp(cmd, "get") && i < argc) {
        char saved[1100];
        r = mpclink_download(&ctx, argv[i], i + 1 < argc ? argv[i + 1] : mpclink_default_dest(), saved, sizeof saved);
        if (r == MPCLINK_OK) printf("saved %s\n", saved);
    } else if (!strcmp(cmd, "inbox")) {
        r = mpclink_poll_inbox(&ctx, i < argc ? argv[i] : mpclink_default_dest());
        if (r >= 0) { printf("received %d file(s)\n", r); r = 0; }
    } else {
        return usage();
    }
    if (r) { fprintf(stderr, "%s\n", mpclink_strerror(r)); return 1; }
    return 0;
}
