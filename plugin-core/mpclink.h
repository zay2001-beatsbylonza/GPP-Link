/*
 * mpclink: the MPC side of MPC Link.
 *
 * Plain C99 + POSIX sockets, no other dependencies, so it builds for both
 * MPC targets (Gen1 armv7, Gen2 aarch64) and links into the VST2 plugin.
 *
 * Every call here does network or disk work and can take seconds.
 * The plugin must call them from its own worker thread, never from
 * the audio thread or the UI/parameter callbacks.
 */
#ifndef MPCLINK_H
#define MPCLINK_H

#include <stddef.h>

#define MPCLINK_PORT           47800
#define MPCLINK_DISCOVERY_PORT 47801

typedef struct {
    char host[64];      /* hub IP, e.g. "192.168.1.20" */
    int  port;          /* hub HTTP port */
    char hub_name[64];  /* the Mac's name, for the screen */
    char device[41];    /* this MPC's name, e.g. "MPC Live II" */
    int  timeout_ms;    /* per socket operation */
} mpclink_ctx;

typedef enum {
    MPCLINK_OK = 0,
    MPCLINK_ERR_NETWORK = -1,  /* can't reach the hub */
    MPCLINK_ERR_HTTP = -2,     /* hub refused the request */
    MPCLINK_ERR_IO = -3,       /* local file problem */
    MPCLINK_ERR_NOT_FOUND = -4,/* no hub answered discovery */
    MPCLINK_ERR_ARG = -5
} mpclink_status;

/* One file in the Beats folder. path is relative to it, '/'-separated. */
typedef struct {
    long long size;
    long long mtime;
    char path[512];
} mpclink_entry;

/* Set up a context. device: this MPC's name (letters, digits, space _ . -). */
void mpclink_init(mpclink_ctx *ctx, const char *device);

/* Find the hub by UDP broadcast. Fills host/port/hub_name. */
int mpclink_discover(mpclink_ctx *ctx, int wait_ms);

/* Or point at it directly (when broadcasts are blocked on the network). */
void mpclink_set_hub(mpclink_ctx *ctx, const char *host, int port);

/* Quick check that the hub is up (also registers this MPC with it). */
int mpclink_ping(mpclink_ctx *ctx);

/* List shared files, newest first. Calls cb for each; return non-zero from cb to stop. */
typedef int (*mpclink_entry_cb)(const mpclink_entry *e, void *user);
int mpclink_list(mpclink_ctx *ctx, mpclink_entry_cb cb, void *user);

/* Send a local file to the Mac. remote_path may include folders
 * ("Song_[ProjectData]/kick.wav"); NULL means just the file name.
 * It lands in <Beats>/From MPC/<device>/... and never overwrites. */
int mpclink_upload(mpclink_ctx *ctx, const char *local_file, const char *remote_path);

/* Download one shared file into dest_dir (created if missing).
 * Never overwrites: "kick.wav" becomes "kick (2).wav". The full
 * local path is written to saved_to (may be NULL). */
int mpclink_download(mpclink_ctx *ctx, const char *path, const char *dest_dir,
                     char *saved_to, size_t saved_to_len);

/* Fetch everything queued for this MPC from the phone page.
 * Returns the number of files received (>= 0) or an error. */
int mpclink_poll_inbox(mpclink_ctx *ctx, const char *dest_dir);

/* Where received files go on this MPC: "<internal drive>/From Computer". */
const char *mpclink_default_dest(void);

const char *mpclink_strerror(int status);

#endif
