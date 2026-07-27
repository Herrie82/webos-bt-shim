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
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>

#include "devinst.h"
#include "hid_parser.h"
#include "uinput_dev.h"
#include "wiimote.h"
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
typedef int  (*passkey_fn)(void *addr, void *pin, unsigned char pinlen);
typedef int  (*ssp_fn)(int accept, void *addr, unsigned int passkey);
typedef int  (*getzone_fn)(int zone);
typedef void (*scpasskey_fn)(void *msg);
typedef void (*fromcsraddr_fn)(void *dst, unsigned int lap, unsigned int uapnap);

static open_fn        real_open;
static send_fn        real_send;
static close_fn       real_close;
static passkey_fn     real_passkey;
static ssp_fn         real_sspaccept;
static getzone_fn     real_getzone;
static scpasskey_fn   real_scpasskeyind;
static scpasskey_fn   real_scssppasskeyind;
static fromcsraddr_fn p_fromcsraddr;

/* GOT slot offsets (r_offset) of intra-libPmBtBsaif calls we must redirect.
 * From `readelf -r libPmBtBsaif.so` (webOS 3.0.5 topaz). The old loader binds
 * these internal PLT calls to the local definition, so LD_PRELOAD symbol
 * interposition is ignored -- we overwrite the GOT slots at runtime instead. */
#define GOT_HidOpenUInput     0xe3794
#define GOT_HidSendToInput    0xe373c
#define GOT_HidCloseUInput    0xe39f8
#define GOT_handleScPasskey   0xe3e54
#define GOT_handleScSspPasskey 0xe3a1c

/* Overwrite one GOT slot at base+offset with newfn, but only if it currently
 * holds expect_real (guards against a wrong offset corrupting the table). */
static void got_patch(void *base, unsigned long off, void *expect_real,
                      void *newfn, const char *name)
{
    void **slot = (void **)((char *)base + off);
    long ps = sysconf(_SC_PAGESIZE);
    void *page = (void *)((uintptr_t)slot & ~((uintptr_t)ps - 1));
    if (mprotect(page, ps, PROT_READ | PROT_WRITE) != 0) {
        shim_log("got_patch %s: mprotect failed", name);
        return;
    }
    /* With lazy binding the slot may still hold the resolver stub (not the real
     * function) until first call; patching before that is fine -- it just means
     * the resolver never runs.  Log old vs expected for visibility, patch either
     * way (the offset is fixed for this exact binary). */
    shim_log("got_patch %s: slot=%p old=%p expect_real=%p -> %p",
             name, (void *)slot, *slot, expect_real, newfn);
    *slot = newfn;
}

#define MAX_MANAGED 8
struct managed {
    void *dev;
    int   fd;
    int   active;
    int   is_wiimote;
    int   handle;
    struct hid_profile prof;
};
static struct managed g_tab[MAX_MANAGED];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* forward decls (our interposers, defined below) so the constructor can take
 * their addresses for GOT patching */
void PmBtBsaifHidOpenUInput(void *dev, void *remdev);
void PmBtBsaifHidSendToInput(void *dev, void *msg);
void PmBtBsaifHidCloseUInput(void *dev);
void handleScPasskeyInd(void *msg);
void handleScSspPasskeyInd(void *msg);

__attribute__((constructor))
static void shim_init(void)
{
    const char *d = getenv("WEBOS_BT_SHIM_DUMP");
    g_shim_dump = (d && d[0] == '1');
    real_open      = (open_fn)    dlsym(RTLD_NEXT, "PmBtBsaifHidOpenUInput");
    real_send      = (send_fn)    dlsym(RTLD_NEXT, "PmBtBsaifHidSendToInput");
    real_close     = (close_fn)   dlsym(RTLD_NEXT, "PmBtBsaifHidCloseUInput");
    real_passkey   = (passkey_fn) dlsym(RTLD_NEXT, "PmBtBsaifPassKey");
    real_sspaccept = (ssp_fn)     dlsym(RTLD_NEXT, "PmBtBsaifSspAccept");
    real_getzone   = (getzone_fn) dlsym(RTLD_NEXT, "PmBtDbgGetZoneState");
    real_scpasskeyind    = (scpasskey_fn)   dlsym(RTLD_NEXT, "handleScPasskeyInd");
    real_scssppasskeyind = (scpasskey_fn)   dlsym(RTLD_NEXT, "handleScSspPasskeyInd");
    p_fromcsraddr        = (fromcsraddr_fn) dlsym(RTLD_DEFAULT, "PmBtHelpFromCsrAddrCpy");

    if (real_open || real_send || real_close) {
        shim_log("webos-bt-shim loaded (dump=%d, real open=%p send=%p close=%p)",
                 g_shim_dump, (void *)real_open, (void *)real_send, (void *)real_close);

        /* This loader binds libPmBtBsaif's calls to its OWN functions locally,
         * so LD_PRELOAD can't intercept them.  Overwrite the GOT slots so the
         * intra-library PLT calls land in our interposers instead. */
        {
            Dl_info info;
            if (dladdr((void *)real_open, &info) && info.dli_fbase) {
                void *base = info.dli_fbase;
                shim_log("GOT-patching libPmBtBsaif at base=%p", base);
                got_patch(base, GOT_HidOpenUInput,  (void *)real_open,  (void *)PmBtBsaifHidOpenUInput,  "HidOpenUInput");
                got_patch(base, GOT_HidSendToInput, (void *)real_send,  (void *)PmBtBsaifHidSendToInput, "HidSendToInput");
                got_patch(base, GOT_HidCloseUInput, (void *)real_close, (void *)PmBtBsaifHidCloseUInput, "HidCloseUInput");
                got_patch(base, GOT_handleScPasskey,    (void *)real_scpasskeyind,    (void *)handleScPasskeyInd,    "handleScPasskeyInd");
                got_patch(base, GOT_handleScSspPasskey, (void *)real_scssppasskeyind, (void *)handleScSspPasskeyInd, "handleScSspPasskeyInd");
            } else {
                shim_log("dladdr failed -- cannot GOT-patch; intra-lib hooks inactive");
            }
        }
    }
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
    name    = (const char *)FIELD(remdev, REMDEV_NAME);

    if (g_shim_dump) {
        const uint8_t *bd = FIELD(dev, DEV_BDADDR);
        shim_dbg("OpenUInput dev=%p name='%s' bd=%02x:%02x:%02x:%02x:%02x:%02x "
                 "subclass=0x%02x id=%u rdesc_len=%u", dev, name,
                 bd[0],bd[1],bd[2],bd[3],bd[4],bd[5],
                 U8(dev, DEV_SUBCLASS), U8(dev, DEV_ID), desclen);
        if (desclen > 0 && desclen <= DEV_RDESC_MAX)
            shim_hexdump("report-descriptor", desc, desclen);
    }

    /* ---- Wii Remote: dedicated path (custom protocol, not standard HID) ---- */
    {
        uint8_t written[6];
        if (wiimote_name_matches(name) ||
            wiimote_is_nintendo(FIELD(dev, DEV_BDADDR), written)) {
            int handle = U8(dev, DEV_ID);
            int wfd = wiimote_create_uinput(name,
                          U16(remdev, REMDEV_VENDOR), U16(remdev, REMDEV_PRODUCT));
            if (wfd < 0) { if (real_open) real_open(dev, remdev); return; }

            pthread_mutex_lock(&g_lock);
            m = find(dev);
            if (m) { uinput_destroy(m->fd); m->active = 0; }
            m = alloc_slot(dev);
            if (m) { m->fd = wfd; m->is_wiimote = 1; m->handle = handle; }
            pthread_mutex_unlock(&g_lock);
            if (!m) { uinput_destroy(wfd); return; }

            U8(dev, DEV_UINPUT_FLAG) = 1;
            U32(dev, DEV_UINPUT_FD)  = (uint32_t)wfd;
            shim_log("took over dev=%p as Wii Remote (fd=%d handle=%d)", dev, wfd, handle);
            wiimote_start_reporting(handle);
            return;
        }
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
        if (rtype == REPORT_TYPE_INPUT && rptr && rlen) {
            if (m->is_wiimote) wiimote_decode(m->fd, rptr, rlen);
            else               uinput_emit_report(m->fd, &m->prof, rptr, rlen);
        }
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

/* Interpose the legacy PIN response.  A Wii Remote paired via 1+2 wants a PIN
 * equal to its own BD_ADDR reversed -- 6 raw bytes that can't be typed into the
 * pairing dialog.  When we see a Nintendo address here, substitute the correct
 * PIN (the user can type any dummy value in the dialog).  Everything else is
 * passed through untouched. */
EXPORT int PmBtBsaifPassKey(void *addr, void *pin, unsigned char pinlen)
{
    uint8_t written[6];

    if (g_shim_dump && addr) {
        const uint8_t *a = (const uint8_t *)addr;
        shim_dbg("PassKey addr=%02x:%02x:%02x:%02x:%02x:%02x pinlen=%u",
                 a[0],a[1],a[2],a[3],a[4],a[5], pinlen);
    }

    if (addr && wiimote_is_nintendo((const uint8_t *)addr, written)) {
        uint8_t wpin[6];
        wiimote_make_pin(written, wpin);
        shim_log("wiimote: injecting PIN for %02x:%02x:%02x:%02x:%02x:%02x",
                 written[0],written[1],written[2],written[3],written[4],written[5]);
        if (real_passkey) return real_passkey(addr, wpin, 6);
    }
    if (real_passkey) return real_passkey(addr, pin, pinlen);
    return 0;
}

/* Interpose the SSP acceptance path.  A Wii Remote Plus does SSP (not legacy
 * PIN), so the pairing response comes through here rather than PmBtBsaifPassKey.
 * A Wiimote is no-input/no-output -> the correct association model is Just
 * Works, so for a Nintendo address we force accept=1.  We also always log which
 * SSP model + address the stack chose, to see how the negotiation resolved. */
EXPORT int PmBtBsaifSspAccept(int accept, void *addr, unsigned int passkey)
{
    int is_nin = 0;
    uint8_t written[6];

    if (addr) is_nin = wiimote_is_nintendo((const uint8_t *)addr, written);

    if (addr) {
        const uint8_t *a = (const uint8_t *)addr;
        shim_log("SspAccept accept=%d passkey=%u nintendo=%d addr=%02x:%02x:%02x:%02x:%02x:%02x",
                 accept, passkey, is_nin, a[0],a[1],a[2],a[3],a[4],a[5]);
    }
    if (is_nin && !accept) {
        shim_log("wiimote: forcing SSP accept (Just Works)");
        accept = 1;
    }
    return real_sspaccept ? real_sspaccept(accept, addr, passkey) : 0;
}

/* Interpose the debug-zone gate.  The security (4) / GAP (3) / HIDH (27) trace
 * is compiled in but off by default, hiding the pairing negotiation.  In dump
 * mode, force those zones verbose so bt.log shows the full SSP/authentication
 * handshake.  Everything else defers to the real zone state. */
EXPORT int PmBtDbgGetZoneState(int zone)
{
    if (g_shim_dump && (zone == 3 || zone == 4 || zone == 0x1b))
        return 5;                       /* > any "if (N < level)" log threshold */
    return real_getzone ? real_getzone(zone) : 0;
}

/* Interpose the legacy PIN *request* handler.  The stock flow raises a UI dialog
 * and only sends the PIN after the user types it -- far too slow for a Wii
 * Remote, whose PIN-request window is short, so bonding fails 0x18 before the
 * response is ever sent.  Here we answer instantly: for a Nintendo address we
 * compute the address-derived PIN and send it straight to the stack via
 * PmBtBsaifPassKey (a direct CsrPutMessage, no engine queue), skipping the
 * dialog entirely.  Non-Nintendo devices keep the normal flow. */
EXPORT void handleScPasskeyInd(void *msg)
{
    void *ind = msg ? *(void **)((char *)msg + 4) : 0;
    shim_log("handleScPasskeyInd FIRED msg=%p ind=%p", msg, ind);

    if (ind && real_passkey) {
        const uint8_t *p = (const uint8_t *)ind;
        uint8_t written[6];
        int off;
        if (g_shim_dump) shim_hexdump("passkey-ind", p, 0x30);

        /* The BD_ADDR is embedded in the indication struct in some CSR-specific
         * layout; rather than hardcode it, scan for a Nintendo OUI (either byte
         * orientation) and lock onto the 6 bytes wherever they sit. */
        for (off = 0; off + 6 <= 0x30; off++) {
            if (wiimote_is_nintendo(p + off, written)) {
                uint8_t pin[6];
                wiimote_make_pin(written, pin);
                shim_log("wiimote: found addr at ind+%d = %02x:%02x:%02x:%02x:%02x:%02x; "
                         "auto-answering legacy PIN instantly", off,
                         written[0],written[1],written[2],written[3],written[4],written[5]);
                /* PmBtBsaifPassKey expects the address in written (PmBtStrToAddr)
                 * order; PIN is that address reversed. */
                real_passkey(written, pin, 6);
                return;                 /* skip the slow dialog entirely */
            }
        }

        /* Fallback: address may be stored CSR-encoded (lap u24 + uap + nap) at
         * ind+8 / ind+0xc, not as 6 contiguous bytes.  Try both packings. */
        {
            uint32_t a = *(uint32_t *)(p + 8), b = *(uint32_t *)(p + 0xc);
            uint32_t lap = a & 0xffffff, uap, nap;
            int k;
            for (k = 0; k < 2; k++) {
                uint8_t cand[6], w2[6];
                if (k == 0) { uap = b & 0xff; nap = (b >> 8)  & 0xffff; }
                else        { uap = b & 0xff; nap = (b >> 16) & 0xffff; }
                cand[0] = (nap >> 8) & 0xff; cand[1] = nap & 0xff; cand[2] = uap;
                cand[3] = (lap >> 16) & 0xff; cand[4] = (lap >> 8) & 0xff; cand[5] = lap & 0xff;
                if (wiimote_is_nintendo(cand, w2)) {
                    uint8_t pin[6];
                    wiimote_make_pin(w2, pin);
                    shim_log("wiimote: CSR-decoded addr (pack%d) %02x:%02x:%02x:%02x:%02x:%02x; "
                             "auto-answering PIN", k,
                             w2[0],w2[1],w2[2],w2[3],w2[4],w2[5]);
                    real_passkey(w2, pin, 6);
                    return;
                }
            }
        }
    }
    if (real_scpasskeyind) real_scpasskeyind(msg);
}

/* SSP passkey indication.  The Wii Remote uses legacy PIN (above), but interpose
 * this too so we can see if any device takes the SSP path here. */
EXPORT void handleScSspPasskeyInd(void *msg)
{
    void *ind = msg ? *(void **)((char *)msg + 4) : 0;
    shim_log("handleScSspPasskeyInd FIRED msg=%p ind=%p", msg, ind);
    if (g_shim_dump && ind) shim_hexdump("ssp-passkey-ind", (const unsigned char *)ind, 0x30);
    if (real_scssppasskeyind) real_scssppasskeyind(msg);
}
