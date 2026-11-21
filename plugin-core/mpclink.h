/*
 * mpclink: the MPC side of GPP Link.
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

#ifdef __cplusplus
extern "C" {
#endif

#define MPCLINK_PORT           47800
#define MPCLINK_DISCOVERY_PORT 47801

typedef struct {
    char host[64];      /* hub IP, e.g. "192.168.1.20" */
    int  port;          /* hub HTTP port */
    char hub_name[64];  /* the Mac's name, for the screen */
    char device[41];    /* this MPC's name, e.g. "MPC Live II" */
    int  timeout_ms;    /* per socket operation */
    volatile int cancel;/* set non-zero from another thread to stop a transfer early */
} mpclink_ctx;

typedef enum {
    MPCLINK_OK = 0,
    MPCLINK_ERR_NETWORK = -1,  /* can't reach the hub */
    MPCLINK_ERR_HTTP = -2,     /* hub refused the request */
    MPCLINK_ERR_IO = -3,       /* local file problem */
    MPCLINK_ERR_NOT_FOUND = -4,/* no hub answered discovery */
    MPCLINK_ERR_ARG = -5,
    MPCLINK_ERR_CANCELLED = -6 /* ctx->cancel was set */
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

/* The same, to a named destination on the Mac: NULL = "From MPC/<device>/",
 * "gpp" = "Get Producers Paid/" (drafts for the Get Producers Paid app),
 * "gpp-stems" = "Get Producers Paid/Stems/". */
int mpclink_upload_to(mpclink_ctx *ctx, const char *local_file, const char *remote_path, const char *dest);

/* Download one shared file into dest_dir (created if missing).
 * Never overwrites: "kick.wav" becomes "kick (2).wav". The full
 * local path is written to saved_to (may be NULL). */
int mpclink_download(mpclink_ctx *ctx, const char *path, const char *dest_dir,
                     char *saved_to, size_t saved_to_len);

/* Stems folders in the Beats folder on the Mac (folders with 2+ audio files),
 * newest first. e->size is the number of audio files in the folder. */
int mpclink_list_stems(mpclink_ctx *ctx, mpclink_entry_cb cb, void *user);

/* Ask the Mac to copy a stems folder into "Get Producers Paid/Stems/". The files
 * are copied on the Mac; nothing passes through the MPC. *files (may be NULL)
 * gets the number of files copied. */
int mpclink_send_stems(mpclink_ctx *ctx, const char *folder, int *files);

/* Fetch a beat for playback: the hub returns it as plain WAV (16-bit stereo,
 * 44.1 kHz, 44-byte header) whatever the original format. On success *out is
 * a malloc'd buffer the caller frees and *out_len its size. Refuses anything
 * over max_bytes. */
int mpclink_fetch_preview(mpclink_ctx *ctx, const char *path, unsigned char **out, size_t *out_len, size_t max_bytes);

/* The same for a file on this MPC (one it received): sends it to the computer,
 * which returns it as 16-bit stereo 44.1 kHz WAV. Nothing is kept there. */
int mpclink_preview_file(mpclink_ctx *ctx, const char *local_file, unsigned char **out, size_t *out_len, size_t max_bytes);

/* Has the phone page cued a beat for this MPC's Mac Beat picker? Writes its
 * path to buf ("" when nothing is cued). Each cue is returned once. */
int mpclink_take_cue(mpclink_ctx *ctx, char *buf, size_t buf_len);

/* Fetch everything queued for this MPC from the phone page.
 * Returns the number of files received (>= 0) or an error. */
int mpclink_poll_inbox(mpclink_ctx *ctx, const char *dest_dir);

/* The MPC's own internal drive, the one the MPC browser and the computer call
 * "MPC User" ("Force User" on a Force), or NULL when none is found. Tries the
 * internal drive's mount on each generation: /nvme (Live III, XL), /storage
 * (Gen2 without NVMe), /sdcard (Gen1 and Force). A candidate counts only if it
 * is its own writable mount, not an empty folder on the read-only system disk.
 * MPCLINK_USER_DRIVE in the environment overrides it (tests). */
const char *mpclink_user_drive(void);

/* Where received files go on this MPC: "<internal drive>/From Computer". */
const char *mpclink_default_dest(void);

const char *mpclink_strerror(int status);

#ifdef __cplusplus
}
#endif

#endif
