/*
 * shim.c -- LD_PRELOAD interposers for the three HID->uinput bridge functions in
 * libPmBtBsaif.so.  Because those symbols are called through the library's own
 * PLT/GOT (see docs/ARCHITECTURE.md ?6), preloading same-named symbols captures
 * every call, including the internal one from PmBtBsaifHandleHidhPrim.
 *
 *   PmBtBsaifHidOpenUInput(dev, remdev)  -- create the input node
 *   PmBtBsaifHidSendToInput(dev, msg)    -- translate a report
 *   PmBtBsaifHidCloseUInput(dev)         -- tear down
 *
 * Policy:
 *   - keyboards (and anything with no mappable non-keyboard fields) are handed
 *     straight to the original implementation via RTLD_NEXT, so stock keyboard
 *     behaviour is preserved byte-for-byte;
 *   - mice / gamepads (which the stock code classifies but then DROPS) are taken
 *     over: we build a properly-capable uinput node from the device's own HID
 *     report descriptor and translate every report ourselves.
 *
 * WEBOS_BT_SHIM_DUMP=1 logs each device's descriptor and every raw report,
 * regardless of who ends up handling it -- the on-device validation tool.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "devinst.h"
#include "hid_parser.h"
#include "uinput_dev.h"
#include "log.h"

/* remote-device info block offsets (param_2 of OpenUInput) */
#define REMDEV_VENDOR  0x04
#define REMDEV_PRODUCT 0x06
#define REMDEV_VERSION 0x08
#define REMDEV_NAME    0x12

#define EXPORT __attribute__((visibility("default")))

typedef void (*open_fn)(void *dev, void *remdev);
typedef void (*send_fn)(void *dev, void *msg);
typedef void (*close_fn)(void *dev);

static open_fn  real_open;
static send_fn  real_send;
static close_fn real_close;

#define MAX_MANAGED 8
struct managed {
    void *dev;
    int   fd;
    int   active;
    struct hid_profile prof;
};
static struct managed g_tab[MAX_MANAGED];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

__attribute__((constructor))
static void shim_init(void)
{
    const char *d = getenv("WEBOS_BT_SHIM_DUMP");
    g_shim_dump = (d && d[0] == '1');
    real_open  = (open_fn)  dlsym(RTLD_NEXT, "PmBtBsaifHidOpenUInput");
    real_send  = (send_fn)  dlsym(RTLD_NEXT, "PmBtBsaifHidSendToInput");
    real_close = (close_fn) dlsym(RTLD_NEXT, "PmBtBsaifHidCloseUInput");
    /* LD_PRELOAD is inherited by every child of BluetoothMonitor (incl. its
     * /bin/sh helpers), but only PmBtEngine actually links libPmBtBsaif.  Stay
     * quiet elsewhere so the log isn't flooded with meaningless load lines. */
    if (real_open || real_send || real_close)
        shim_log("webos-bt-shim loaded (dump=%d, real open=%p send=%p close=%p)",
                 g_shim_dump, (void *)real_open, (void *)real_send, (void *)real_close);
}

static struct managed *find(void *dev)
{
    int i;
    for (i = 0; i < MAX_MANAGED; i++)
        if (g_tab[i].active && g_tab[i].dev == dev)
            return &g_tab[i];
    return NULL;
}
static struct managed *alloc_slot(void *dev)
{
    int i;
    for (i = 0; i < MAX_MANAGED; i++)
        if (!g_tab[i].active) {
            memset(&g_tab[i], 0, sizeof(g_tab[i]));
            g_tab[i].dev = dev;
            g_tab[i].fd = -1;
            g_tab[i].active = 1;
            return &g_tab[i];
        }
    return NULL;
}

/* ---------------------------------------------------------------- */

EXPORT void PmBtBsaifHidOpenUInput(void *dev, void *remdev)
{
    uint16_t desclen;
    const uint8_t *desc;
    struct hid_profile prof;
    char descr[128];
    struct input_id id;
    const char *name;
    struct managed *m;
    int fd;

    desclen = U16(dev, DEV_RDESC_LEN);
    desc    = FIELD(dev, DEV_RDESC);

    if (g_shim_dump) {
        shim_dbg("OpenUInput dev=%p subclass=0x%02x rdesc_len=%u",
                 dev, U8(dev, DEV_SUBCLASS), desclen);
        if (desclen > 0 && desclen <= DEV_RDESC_MAX)
            shim_hexdump("report-descriptor", desc, desclen);
    }

    /* No usable descriptor -> let the stock keyboard path handle it. */
    if (desclen == 0 || desclen > DEV_RDESC_MAX) {
        shim_log("no HID descriptor (len=%u) -> delegating to stock OpenUInput", desclen);
        if (real_open) real_open(dev, remdev);
        return;
    }

    if (hid_parse(desc, desclen, &prof) != 0) {
        shim_log("hid_parse failed -> delegating");
        if (real_open) real_open(dev, remdev);
        return;
    }
    hid_profile_describe(&prof, descr, sizeof(descr));
    shim_log("device: %s", descr);

    /* Keyboards / nothing-we-map -> stock behaviour untouched. */
    if (!prof.is_mouse && !prof.is_gamepad) {
        shim_log("keyboard/other -> delegating to stock OpenUInput");
        if (real_open) real_open(dev, remdev);
        return;
    }
    if (prof.nfields == 0) {
        shim_log("no mappable fields -> delegating");
        if (real_open) real_open(dev, remdev);
        return;
    }

    /* We take over this device. */
    memset(&id, 0, sizeof(id));
    id.bustype = BUS_BLUETOOTH;
    id.vendor  = U16(remdev, REMDEV_VENDOR);
    id.product = U16(remdev, REMDEV_PRODUCT);
    id.version = U16(remdev, REMDEV_VERSION);
    name = (const char *)FIELD(remdev, REMDEV_NAME);

    fd = uinput_create(&prof, name, &id);
    if (fd < 0) {
        shim_log("uinput_create failed -> delegating to stock (input will drop)");
        if (real_open) real_open(dev, remdev);
        return;
    }

    pthread_mutex_lock(&g_lock);
    m = find(dev);
    if (m) { uinput_destroy(m->fd); m->active = 0; }
    m = alloc_slot(dev);
    if (m) { m->fd = fd; m->prof = prof; }
    pthread_mutex_unlock(&g_lock);

    if (!m) { shim_log("managed table full -> closing node"); uinput_destroy(fd); return; }

    /* Keep the device struct consistent: mark uinput up, record fd. */
    U8(dev, DEV_UINPUT_FLAG) = 1;
    U32(dev, DEV_UINPUT_FD)  = (uint32_t)fd;
    shim_log("took over dev=%p as %s (fd=%d)", dev,
             prof.is_gamepad ? "gamepad" : "mouse", fd);
}

EXPORT void PmBtBsaifHidSendToInput(void *dev, void *msg)
{
    struct managed *m;
    uint8_t  rtype;
    uint16_t rlen;
    const unsigned char *rptr;

    rtype = U8(msg, MSG_REPORT_TYPE);
    rlen  = U16(msg, MSG_REPORT_LEN);
    rptr  = (const unsigned char *)PTR(msg, MSG_REPORT_PTR);

    if (g_shim_dump) {
        shim_dbg("SendToInput dev=%p type=%u len=%u", dev, rtype, rlen);
        if (rtype == REPORT_TYPE_INPUT && rptr && rlen)
            shim_hexdump("report", rptr, rlen);
    }

    pthread_mutex_lock(&g_lock);
    m = find(dev);
    pthread_mutex_unlock(&g_lock);

    if (m) {
        if (rtype == REPORT_TYPE_INPUT && rptr && rlen)
            uinput_emit_report(m->fd, &m->prof, rptr, rlen);
        return;                       /* managed: never fall through to stock */
    }

    if (real_send) real_send(dev, msg);   /* keyboards, consumer remote */
}

EXPORT void PmBtBsaifHidCloseUInput(void *dev)
{
    struct managed *m;

    pthread_mutex_lock(&g_lock);
    m = find(dev);
    if (m) {
        int fd = m->fd;
        m->active = 0;
        pthread_mutex_unlock(&g_lock);
        shim_log("closing managed dev=%p fd=%d", dev, fd);
        uinput_destroy(fd);
        U8(dev, DEV_UINPUT_FLAG) = 0;
        return;
    }
    pthread_mutex_unlock(&g_lock);

    if (real_close) real_close(dev);
}
