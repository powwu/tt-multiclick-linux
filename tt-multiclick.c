#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/extensions/XInput2.h>
#include <libevdev/libevdev.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <libei-1.0/libei.h>
#include <poll.h>
#include <unistd.h>
#include <X11/extensions/XTest.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

/* UI widgets (populated from tt-multiclick.ui via GtkBuilder) */
static GtkWidget *main_window;
static GtkWidget *btn_device, *btn_key1, *btn_key2;
static GtkWidget *btn_start;
static GtkWidget *combo_title, *entry_title;
static GtkWidget *lbl_status;
static GtkWidget *chk_skip_source;
static GtkWidget *chk_skip_source_r;
static GtkWidget *chk_xephyr;
static GtkWidget *chk_gamescope;

/* Runtime state */
static char   device_path[256] = "";
static int    key1_code = BTN_LEFT;
static int    key2_code = BTN_RIGHT;
static int    binding_slot = 0;   /* nonzero while waiting for a key */

static pthread_t      worker_thread;
static volatile int   worker_running = 0;
static int            worker_pipe[2] = {-1, -1};

/* Window discovery via pure Xlib — no subprocesses */

typedef struct { int x, y, w, h; } Geom;
typedef struct { long xid; Geom g; Geom nested_g; char xephyr_display[32]; int gamescope; char ei_socket[128]; struct ei *ei_ctx; struct ei_device *ei_dev; int ei_out_w, ei_out_h; } Win;

static Display *query_dpy;  /* opened once in worker(), shared with find_windows() */


/* Trailing '*' means prefix match, e.g. "Corporate Clash*" */
static int title_matches(const char *name, const char *title) {
    size_t tlen = strlen(title);
    if (tlen && title[tlen-1] == '*')
        return strncmp(name, title, tlen-1) == 0;
    return strcmp(name, title) == 0;
}

/* Returns 0 if the window is unmapped or XGetWindowAttributes fails. */
static int win_screen_geom(Display *dpy, Window w, Geom *g) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(dpy, w, &a)) return 0;
    if (a.map_state != IsViewable) return 0;
    Window child;
    int sx, sy;
    XTranslateCoordinates(dpy, w, DefaultRootWindow(dpy), 0, 0, &sx, &sy, &child);
    g->x = sx; g->y = sy; g->w = a.width; g->h = a.height;
    return 1;
}

/* Connect to a gamescope libei socket and return a ready-to-use pointer device,
 * or NULL on failure. Caller must ei_device_unref + ei_unref when done. */
static struct ei_device *ei_connect(struct ei **ei_out, const char *socket_path) {
    struct ei *ei = ei_new_sender(NULL);
    ei_configure_name(ei, "tt-multiclick");
    ei_setup_backend_socket(ei, socket_path);
    struct ei_device *pointer = NULL;
    struct pollfd pfd = { .fd = ei_get_fd(ei), .events = POLLIN };
    for (int i = 0; i < 20; i++) {
        poll(&pfd, 1, 50);
        ei_dispatch(ei);
        struct ei_event *ev;
        while ((ev = ei_get_event(ei))) {
            enum ei_event_type t = ei_event_get_type(ev);
            if (t == EI_EVENT_SEAT_ADDED)
                ei_seat_bind_capabilities(ei_event_get_seat(ev),
                    EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_POINTER_ABSOLUTE, EI_DEVICE_CAP_BUTTON, NULL);
            else if (t == EI_EVENT_DEVICE_ADDED && !pointer) {
                struct ei_device *d = ei_event_get_device(ev);
                if (ei_device_has_capability(d, EI_DEVICE_CAP_POINTER_ABSOLUTE))
                    pointer = ei_device_ref(d);
            } else if (t == EI_EVENT_DEVICE_RESUMED && pointer) {
                ei_event_unref(ev);
                /* Log region info */
                struct ei_region *r = ei_device_get_region(pointer, 0);
                if (r) fprintf(stderr, "[ei] region: %dx%d+%d+%d\n",
                    ei_region_get_width(r), ei_region_get_height(r),
                    ei_region_get_x(r), ei_region_get_y(r));
                else fprintf(stderr, "[ei] no region\n");
                *ei_out = ei;
                return pointer;
            }
            ei_event_unref(ev);
        }
    }
    if (pointer) ei_device_unref(pointer);
    ei_unref(ei);
    return NULL;
}

/* Send a full click (press+release) or half-click via libei absolute motion. */
static void ei_click(const char *socket_path, const char *nested_display,
                     double x, double y, int button, bool press, bool release,
                     struct ei *cached_ei, struct ei_device *cached_dev) {
    if (!socket_path || !socket_path[0]) { fprintf(stderr, "[ei] no socket path\n"); return; }

    /* Log cursor position before */
    if (nested_display) {
        Display *dpy = XOpenDisplay(nested_display);
        if (dpy) {
            Window root = DefaultRootWindow(dpy), child, dummy;
            int rx, ry, wx, wy; unsigned int mask;
            XQueryPointer(dpy, root, &dummy, &child, &rx, &ry, &wx, &wy, &mask);
            fprintf(stderr, "[ei] click socket=%s x=%.0f y=%.0f btn=%d cur_before=%d,%d\n", socket_path, x, y, button, rx, ry);
            XCloseDisplay(dpy);
        }
    } else {
        fprintf(stderr, "[ei] click socket=%s x=%.0f y=%.0f btn=%d\n", socket_path, x, y, button);
    }

    struct ei *ei = NULL;
    struct ei_device *dev = ei_connect(&ei, socket_path);
    if (!dev) { fprintf(stderr, "[ei] connect failed\n"); return; }

    static uint32_t seq = 1;
    ei_device_start_emulating(dev, seq++);

    /* Send all events without any sleep to minimize emulation window */
    ei_device_pointer_motion_absolute(dev, x, y);
    ei_device_frame(dev, ei_now(ei));
    ei_device_pointer_motion(dev, 0, 0);
    ei_device_frame(dev, ei_now(ei));
    if (press) {
        ei_device_button_button(dev, button, true);
        ei_device_frame(dev, ei_now(ei));
    }
    if (release) {
        ei_device_button_button(dev, button, false);
        ei_device_frame(dev, ei_now(ei));
    }
    ei_dispatch(ei);
    ei_device_unref(dev);
    ei_unref(ei);
}

static void ei_win_close(Win *w) {
    if (w->ei_dev) {
        ei_device_stop_emulating(w->ei_dev);
        ei_dispatch(w->ei_ctx);
        ei_device_unref(w->ei_dev);
        w->ei_dev = NULL;
    }
    if (w->ei_ctx) { ei_unref(w->ei_ctx); w->ei_ctx = NULL; }
}

/* Recursively collect matching windows into wins[]. Returns updated count. */
static int collect_windows(Display *dpy, Window w,
                            const char *title, int use_xephyr, int use_gamescope,
                            Win *wins, int n, int max) {
    char *name = NULL;
    if (XFetchName(dpy, w, &name) && name) {
        int is_xephyr = (strncmp(name, "Xephyr on :", 11) == 0);
        int matches = title_matches(name, title);

        if (matches || (use_xephyr && is_xephyr)) {
            Geom g;
            if (n < max && win_screen_geom(dpy, w, &g) && g.w > 1 && g.h > 1) {
                wins[n].xid = (long)w;
                wins[n].g   = g;
                wins[n].xephyr_display[0] = '\0';
                if (use_xephyr && is_xephyr) {
                    /* Extract ":N" from "Xephyr on :N.0 (screen 0)" etc. */
                    const char *p = name + 10; /* points to ":N" */
                    snprintf(wins[n].xephyr_display,
                             sizeof(wins[n].xephyr_display), "%s", p);
                    /* Trim trailing whitespace/parens */
                    char *end = wins[n].xephyr_display;
                    while (*end && *end != ' ' && *end != ')') end++;
                    *end = '\0';
                }
                n++;
            }
        }
        XFree(name);
    }
    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren = 0;
    if (XQueryTree(dpy, w, &root_ret, &parent_ret, &children, &nchildren) && children) {
        for (unsigned int i = 0; i < nchildren && n < max; i++)
            n = collect_windows(dpy, children[i], title, use_xephyr, use_gamescope, wins, n, max);
        XFree(children);
    }
    return n;
}

/*
 * For gamescope mode: scan all X sockets in /tmp/.X11-unix, connect to each
 * (skipping the host display), and search for the game title inside it.
 * Each match gets xephyr_display set to that nested display string so the
 * rest of the code injects into it exactly like Xephyr mode.
 * The host-side geometry is taken from the gamescope SDL window (WM_CLASS=gamescope)
 * whose _GAMESCOPE_WAYLAND_DISPLAY property matches the nested display's wayland socket,
 * or falls back to the full SDL window bounds if no match is found.
 */
static int find_gamescope_windows(const char *title, Win *wins, int max) {
    const char *host_display = getenv("DISPLAY") ?: ":0";
    int n = 0;

    /* Build a list of gamescope SDL windows on the host display keyed by their geometry */
    Display *hdpy = XOpenDisplay(host_display);

    DIR *d = opendir("/tmp/.X11-unix");
    if (!d) { if (hdpy) XCloseDisplay(hdpy); return 0; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < max) {
        if (de->d_name[0] != 'X') continue;
        char disp[32];
        snprintf(disp, sizeof(disp), ":%s", de->d_name + 1);
        if (strcmp(disp, host_display) == 0) continue;

        Display *ndpy = XOpenDisplay(disp);
        if (!ndpy) continue;

        int before = n;
        n = collect_windows(ndpy, DefaultRootWindow(ndpy), title, 0, 0, wins, n, max);
        int out_w = DisplayWidth(ndpy, 0);
        int out_h = DisplayHeight(ndpy, 0);

        for (int i = before; i < n; i++) {
            snprintf(wins[i].xephyr_display, sizeof(wins[i].xephyr_display), "%s", disp);
            wins[i].gamescope = 1;
            wins[i].nested_g = wins[i].g; /* save nested display geometry before host overwrite */
            wins[i].ei_out_w = out_w;
            wins[i].ei_out_h = out_h;

            /* Find the ei socket by scanning /proc for a process on this nested
             * display that has GAMESCOPE_WAYLAND_DISPLAY set in its environment. */
            char runtime[64];
            snprintf(runtime, sizeof(runtime), "/run/user/%d", (int)getuid());
            DIR *pd = opendir("/proc");
            if (pd) {
                struct dirent *pe;
                while ((pe = readdir(pd))) {
                    if (pe->d_name[0] < '1' || pe->d_name[0] > '9') continue;
                    char envpath[64];
                    snprintf(envpath, sizeof(envpath), "/proc/%s/environ", pe->d_name);
                    FILE *f = fopen(envpath, "r");
                    if (!f) continue;
                    char buf[16384]; size_t nr = fread(buf, 1, sizeof(buf)-1, f); fclose(f);
                    buf[nr] = '\0';
                    /* Check DISPLAY matches our nested display */
                    char disp_needle[40];
                    snprintf(disp_needle, sizeof(disp_needle), "DISPLAY=%s", disp);
                    int has_disp = 0;
                    char *p = buf;
                    while (p < buf + nr) {
                        if (strcmp(p, disp_needle) == 0) { has_disp = 1; break; }
                        p += strlen(p) + 1;
                    }
                    if (!has_disp) continue;
                    /* Extract GAMESCOPE_WAYLAND_DISPLAY */
                    p = buf;
                    while (p < buf + nr) {
                        if (strncmp(p, "GAMESCOPE_WAYLAND_DISPLAY=", 26) == 0) {
                            snprintf(wins[i].ei_socket, sizeof(wins[i].ei_socket),
                                     "%s/%s-ei", runtime, p + 26);
                            break;
                        }
                        p += strlen(p) + 1;
                    }
                    if (wins[i].ei_socket[0]) break;
                }
                closedir(pd);
            }

            /* Find the gamescope SDL window on the host by matching _NET_WM_PID
             * to the gamescope process that owns this nested display's ei socket. */
            if (hdpy && wins[i].ei_socket[0]) {
                /* Find the gamescope PID that holds the ei lock for this socket */
                char ei_lock[256];
                snprintf(ei_lock, sizeof(ei_lock), "%s.lock", wins[i].ei_socket);
                pid_t gs_pid = 0;
                DIR *pd2 = opendir("/proc");
                if (pd2) {
                    struct dirent *pe2;
                    while ((pe2 = readdir(pd2))) {
                        if (pe2->d_name[0] < '1' || pe2->d_name[0] > '9') continue;
                        char fdpath[64];
                        snprintf(fdpath, sizeof(fdpath), "/proc/%s/fd", pe2->d_name);
                        DIR *fd2 = opendir(fdpath);
                        if (!fd2) continue;
                        struct dirent *fe;
                        while ((fe = readdir(fd2))) {
                            char lnk[256], tgt[256];
                            snprintf(lnk, sizeof(lnk), "%s/%s", fdpath, fe->d_name);
                            ssize_t n2 = readlink(lnk, tgt, sizeof(tgt)-1);
                            if (n2 > 0) { tgt[n2] = '\0'; if (strcmp(tgt, ei_lock) == 0) { gs_pid = atoi(pe2->d_name); break; } }
                        }
                        closedir(fd2);
                        if (gs_pid) break;
                    }
                    closedir(pd2);
                }

                if (gs_pid) {
                    Atom pid_atom = XInternAtom(hdpy, "_NET_WM_PID", True);
                    Window root = DefaultRootWindow(hdpy), root_ret, parent_ret, *children = NULL;
                    unsigned int nch = 0;
                    if (pid_atom != None && XQueryTree(hdpy, root, &root_ret, &parent_ret, &children, &nch) && children) {
                        for (unsigned int j = 0; j < nch; j++) {
                            Atom actual; int fmt; unsigned long nitems, left;
                            unsigned char *prop = NULL;
                            if (XGetWindowProperty(hdpy, children[j], pid_atom, 0, 1, False,
                                    XA_CARDINAL, &actual, &fmt, &nitems, &left, &prop) == Success && prop) {
                                pid_t wpid = *(pid_t *)prop;
                                XFree(prop);
                                if (wpid == gs_pid) {
                                    Geom g;
                                    if (win_screen_geom(hdpy, children[j], &g) && g.w > 1 && g.h > 1)
                                        wins[i].g = g;
                                    break;
                                }
                            }
                        }
                        XFree(children);
                    }
                }
            }
        }
        XCloseDisplay(ndpy);
    }
    closedir(d);
    if (hdpy) XCloseDisplay(hdpy);
    return n;
}

static int find_windows(const char *title, int use_xephyr, int use_gamescope, Win *wins, int max) {
    if (use_gamescope)
        return find_gamescope_windows(title, wins, max);
    return collect_windows(query_dpy, DefaultRootWindow(query_dpy),
                           title, use_xephyr, 0, wins, 0, max);
}

/* Like find_windows but reuses ei connections from cache for gamescope mode */
static int find_windows_cached(const char *title, int use_xephyr, int use_gamescope,
                                Win *wins, int max, Win *cache, int cache_n) {
    int n = find_windows(title, use_xephyr, use_gamescope, wins, max);
    if (use_gamescope && cache) {
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < cache_n; j++) {
                if (strcmp(wins[i].xephyr_display, cache[j].xephyr_display) == 0) {
                    wins[i].ei_ctx = cache[j].ei_ctx;
                    wins[i].ei_dev = cache[j].ei_dev;
                    break;
                }
            }
        }
    }
    return n;
}

/* XSendEvent click — works for non-Xephyr windows that accept synthetic events */

static void send_click(Display *dpy, long xid, int ex, int ey,
                       int rx, int ry, int button) {
    Window win = (Window)xid;
    XEvent ev = {0};
    ev.xbutton.display     = dpy;
    ev.xbutton.window      = win;
    ev.xbutton.root        = DefaultRootWindow(dpy);
    ev.xbutton.time        = CurrentTime;
    ev.xbutton.x           = ex;  ev.xbutton.y      = ey;
    ev.xbutton.x_root      = rx;  ev.xbutton.y_root = ry;
    ev.xbutton.button      = button;
    ev.xbutton.same_screen = True;
    ev.type = ButtonPress;
    XSendEvent(dpy, win, True, ButtonPressMask, &ev);
    ev.type = ButtonRelease;
    XSendEvent(dpy, win, True, ButtonReleaseMask, &ev);
    XFlush(dpy);
}

/* Worker thread — reads raw evdev events and dispatches clicks/drags */

typedef struct { char device[256]; char title[256]; int k1, k2; int skip_source; int skip_source_r; int use_xephyr; int use_gamescope; } WorkerArgs;

/* State for a held button (right-click drag mirroring across windows) */
typedef struct {
    volatile int active;
    int    xbutton;
    int    nw;
    Win    wins[16];
    int    src_idx;
    int    skip_source_r;
    int    external_src;
    Display *dpy;       /* separate Display for motion thread */
    pthread_t tid;
} HeldButton;

static gboolean set_status(gpointer data) {
    gtk_label_set_text(GTK_LABEL(lbl_status), (char *)data);
    free(data);
    return G_SOURCE_REMOVE;
}

static void status_msg(const char *fmt, ...) {
    char *buf = malloc(512);
    va_list ap; va_start(ap, fmt); vsnprintf(buf, 512, fmt, ap); va_end(ap);
    g_idle_add(set_status, buf);
}

/*
 * Mirrors cursor movement to all non-source windows while a button is held.
 * Uses XI2 raw motion events so XWarpPointer calls don't feed back as input.
 * For Xephyr targets, opens their display directly and warps inside it.
 * Near-edge positions are re-centered to prevent the cursor getting stuck.
 */
static void *motion_thread(void *arg) {
    HeldButton *h = arg;
    Display *dpy = h->dpy;

    /* Keep one Display open per target for the thread lifetime */
    Display *xdpys[16] = {0};
    for (int i = 0; i < h->nw; i++) {
        if (i == h->src_idx && h->skip_source_r) continue;
            xdpys[i] = XOpenDisplay(h->wins[i].xephyr_display);
    }

    /* If source is a Xephyr window, read deltas from inside it.
     * If source is the tt-multiclick window itself, open a fresh host connection —
     * XQueryPointer is unaffected by grabs on other connections. */
    Display *src_dpy = (!h->external_src && h->wins[h->src_idx].xephyr_display[0])
                       ? XOpenDisplay(h->wins[h->src_idx].xephyr_display)
                       : XOpenDisplay(NULL);
    if (!src_dpy) return NULL;
    Window src_root = DefaultRootWindow(src_dpy);

    /* XI2 raw motion events are not generated by XWarpPointer, so this
     * won't feed back into itself when we warp the cursor. */
    {
        int xi_opcode, ev, err;
        if (XQueryExtension(src_dpy, "XInputExtension", &xi_opcode, &ev, &err)) {
            XIEventMask mask;
            unsigned char bits[XIMaskLen(XI_RawMotion)] = {0};
            mask.deviceid = XIAllMasterDevices;
            mask.mask     = bits;
            mask.mask_len = sizeof(bits);
            XISetMask(bits, XI_RawMotion);
            XISelectEvents(src_dpy, src_root, &mask, 1);
            XFlush(src_dpy);
        }
    }

    while (h->active) {
        /* Poll at 1ms rather than blocking forever so h->active is checked promptly */
        if (!XPending(src_dpy)) {
            struct timespec ts2 = {0, 1000000}; /* 1ms poll */
            nanosleep(&ts2, NULL);
            continue;
        }

        XEvent xev;
        XNextEvent(src_dpy, &xev);
        if (xev.type != GenericEvent) continue;

        XGenericEventCookie *cookie = &xev.xcookie;
        if (!XGetEventData(src_dpy, cookie)) continue;

        int dx = 0, dy = 0;
        if (cookie->evtype == XI_RawMotion) {
            XIRawEvent *raw = cookie->data;
            double *val = raw->raw_values;
            int k = 0;
            for (int a = 0; a < raw->valuators.mask_len * 8; a++) {
                if (!XIMaskIsSet(raw->valuators.mask, a)) continue;
                if (a == 0) dx = (int)val[k];
                if (a == 1) dy = (int)val[k];
                k++;
            }
        }
        XFreeEventData(src_dpy, cookie);

        if (dx == 0 && dy == 0) continue;

        for (int i = 0; i < h->nw && h->active; i++) {
            if (i == h->src_idx && h->skip_source_r) continue;
            if (h->wins[i].xephyr_display[0]) {
                if (!xdpys[i]) continue;
                Window xroot = DefaultRootWindow(xdpys[i]), xchild;
                int cx, cy, wxe, wye; unsigned int msk;
                XQueryPointer(xdpys[i], xroot, &xroot, &xchild, &cx, &cy, &wxe, &wye, &msk);
                int sw = DisplayWidth(xdpys[i], 0), sh = DisplayHeight(xdpys[i], 0);
                int nx = cx + dx, ny = cy + dy;
                /* Re-center if within 64px of any edge — prevents the cursor
                 * getting stuck against the Xephyr window border. */
                if (nx < 64 || nx > sw-64 || ny < 64 || ny > sh-64) {
                    nx = sw/2; ny = sh/2;
                }
                XWarpPointer(xdpys[i], None, xroot, 0,0,0,0, nx, ny);
                XFlush(xdpys[i]);
            } else if (!h->external_src) {
                Window hroot = DefaultRootWindow(dpy), hchild;
                int hx, hy, hwx, hwy; unsigned int hmsk;
                XQueryPointer(dpy, hroot, &hroot, &hchild, &hx, &hy, &hwx, &hwy, &hmsk);
                XWarpPointer(dpy, None, hroot, 0,0,0,0, hx+dx, hy+dy);
                XFlush(dpy);
            }
        }
    }

    if (src_dpy) XCloseDisplay(src_dpy);

    for (int i = 0; i < h->nw; i++)
        if (xdpys[i]) XCloseDisplay(xdpys[i]);

    return NULL;
}

static void *worker(void *arg) {
    WorkerArgs *a = arg;

    int fd = open(a->device, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { status_msg("Cannot open device."); free(a); return NULL; }

    struct libevdev *dev = NULL;
    if (libevdev_new_from_fd(fd, &dev) < 0) {
        status_msg("libevdev init failed."); close(fd); free(a); return NULL;
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { status_msg("Cannot open X display."); goto done; }
    query_dpy = dpy;

    status_msg("Running — listening on %s", a->device);

    HeldButton held[2] = {0};

    /* Persistent ei connections for gamescope mode — opened once, kept for worker lifetime */
    Win ei_cache[16] = {0};
    int ei_cache_n = 0;
    if (a->use_gamescope) {
        ei_cache_n = find_gamescope_windows(a->title, ei_cache, 16);
        for (int i = 0; i < ei_cache_n; i++) {
            if (ei_cache[i].ei_socket[0]) {
                ei_cache[i].ei_dev = ei_connect(&ei_cache[i].ei_ctx, ei_cache[i].ei_socket);
                fprintf(stderr, "[ei_cache] display=%s socket=%s dev=%s host_g=%dx%d+%d+%d nested_g=%dx%d\n",
                    ei_cache[i].xephyr_display, ei_cache[i].ei_socket,
                    ei_cache[i].ei_dev ? "OK" : "FAILED",
                    ei_cache[i].g.w, ei_cache[i].g.h, ei_cache[i].g.x, ei_cache[i].g.y,
                    ei_cache[i].nested_g.w, ei_cache[i].nested_g.h);
            }
        }
    }

    struct input_event ev;
    fd_set rfds;

    while (worker_running) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        FD_SET(worker_pipe[0], &rfds);
        int nfds = (fd > worker_pipe[0] ? fd : worker_pipe[0]) + 1;
        struct timeval tv = {0, 50000};
        if (select(nfds, &rfds, NULL, NULL, &tv) <= 0) continue;
        if (FD_ISSET(worker_pipe[0], &rfds)) break;
        if (!FD_ISSET(fd, &rfds)) continue;

        while (1) {
            int rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            if (rc == -EAGAIN) break;
            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                while (libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev) >= 0);
                break;
            }
            if (rc < 0) break;

            if (ev.type != EV_KEY) continue;
            int slot = (ev.code == a->k1) ? 0 : (ev.code == a->k2) ? 1 : -1;
            if (slot < 0) continue;

            if (ev.value == 1) { /* press */
                if (held[slot].active) continue;

                Win wins[16];
                int nw = find_windows_cached(a->title, a->use_xephyr, a->use_gamescope, wins, 16, ei_cache, ei_cache_n);
                if (nw < 2) continue;

                Window root = DefaultRootWindow(dpy), child;
                int px, py, wx, wy; unsigned int mask;
                XQueryPointer(dpy, root, &root, &child, &px, &py, &wx, &wy, &mask);

                int src_idx = -1;
                for (int i = 0; i < nw; i++) {
                    Geom *g = &wins[i].g;
                    if (px >= g->x && px < g->x+g->w && py >= g->y && py < g->y+g->h)
                        if (src_idx < 0 || g->w*g->h < wins[src_idx].g.w*wins[src_idx].g.h)
                            src_idx = i;
                }
                /* For gamescope, if no host window matched, fall back to wins[0] */
                if (src_idx < 0 && nw > 0 && wins[0].gamescope) src_idx = 0;
                fprintf(stderr, "[src] px=%d py=%d src_idx=%d display=%s g=%dx%d+%d+%d\n",
                    px, py, src_idx,
                    src_idx >= 0 ? wins[src_idx].xephyr_display : "none",
                    src_idx >= 0 ? wins[src_idx].g.w : 0, src_idx >= 0 ? wins[src_idx].g.h : 0,
                    src_idx >= 0 ? wins[src_idx].g.x : 0, src_idx >= 0 ? wins[src_idx].g.y : 0);
                /* src_idx == -1: cursor is over the tt-multiclick window, not a game window.
                 * Still forward to all windows; use wins[0] as the reference for relative coords. */
                int external_src = (src_idx < 0);
                if (external_src) src_idx = 0;

                double rel_x, rel_y;
                if (wins[src_idx].gamescope) {
                    /* Read cursor directly from the source nested display */
                    Display *ndpy = XOpenDisplay(wins[src_idx].xephyr_display);
                    if (ndpy) {
                        Window nr, nc; int nrx, nry, nwx, nwy; unsigned int nm;
                        XQueryPointer(ndpy, DefaultRootWindow(ndpy), &nr, &nc, &nrx, &nry, &nwx, &nwy, &nm);
                        rel_x = (double)nrx / wins[src_idx].nested_g.w;
                        rel_y = (double)nry / wins[src_idx].nested_g.h;
                        XCloseDisplay(ndpy);
                    } else { rel_x = rel_y = 0; }
                } else {
                    rel_x = (double)(px - wins[src_idx].g.x) / wins[src_idx].g.w;
                    rel_y = (double)(py - wins[src_idx].g.y) / wins[src_idx].g.h;
                }

                if (slot == 0) {
                    int right_held = held[1].active;
                    if (right_held) {
                        /* Right is already held: send ButtonPress now (hold mode, release comes later) */
                        for (int i = 0; i < nw; i++) {
                            if (!external_src && i == src_idx && a->skip_source) continue;
                            Geom *g = &wins[i].g;
                            int ew = wins[i].gamescope ? wins[i].nested_g.w : g->w;
                            int eh = wins[i].gamescope ? wins[i].nested_g.h : g->h;
                            int dx = (int)(rel_x * ew), dy = (int)(rel_y * eh);
                            if (dx < 0) dx = 0; else if (dx >= ew) dx = ew-1;
                            if (dy < 0) dy = 0; else if (dy >= eh) dy = eh-1;
                            if (wins[i].xephyr_display[0]) {
                                if (wins[i].gamescope) {
                                    ei_click(wins[i].ei_socket, wins[i].xephyr_display,
                                        dx, dy, 0x110, true, false, wins[i].ei_ctx, wins[i].ei_dev);
                                } else {
                                    Display *xdpy = XOpenDisplay(wins[i].xephyr_display);
                                    if (xdpy) {
                                        Window xroot = DefaultRootWindow(xdpy);
                                        XWarpPointer(xdpy, None, xroot, 0,0,0,0, dx, dy);
                                        XTestFakeButtonEvent(xdpy, 1, True, CurrentTime);
                                        XFlush(xdpy);
                                        XCloseDisplay(xdpy);
                                    }
                                }
                            } else {
                                XEvent bev = {0};
                                bev.xbutton.display = dpy; bev.xbutton.window = (Window)wins[i].xid;
                                bev.xbutton.root = DefaultRootWindow(dpy); bev.xbutton.time = CurrentTime;
                                bev.xbutton.x = dx; bev.xbutton.y = dy;
                                bev.xbutton.x_root = g->x+dx; bev.xbutton.y_root = g->y+dy;
                                bev.xbutton.button = 1; bev.xbutton.same_screen = True;
                                bev.type = ButtonPress;
                                XSendEvent(dpy, (Window)wins[i].xid, True, ButtonPressMask, &bev);
                                XFlush(dpy);
                            }
                        }
                        held[slot].nw      = nw;
                        held[slot].src_idx = src_idx;
                        held[slot].external_src = external_src;
                        held[slot].skip_source_r = a->skip_source;
                        for (int i = 0; i < nw; i++) held[slot].wins[i] = wins[i];
                        held[slot].xbutton = 1; /* hold mode */
                        held[slot].active = 1;
                    } else {
                        /* Right not held: defer the click until release so we capture
                         * the final cursor position rather than the press position. */
                        held[slot].nw      = nw;
                        held[slot].src_idx = src_idx;
                        held[slot].external_src = external_src;
                        held[slot].skip_source_r = a->skip_source;
                        for (int i = 0; i < nw; i++) held[slot].wins[i] = wins[i];
                        /* store rel coords for use at release time */
                        held[slot].xbutton = 0; /* 0 = deferred click, sent on release */
                        held[slot].active = 1;
                    }
                } else {
                    /* Right click: warp + XTest press into each window, then start motion thread. */
                    held[slot].xbutton = 3;
                    held[slot].nw      = nw;
                    held[slot].src_idx     = src_idx;
                    held[slot].skip_source_r = a->skip_source_r;
                    held[slot].external_src  = external_src;
                    for (int i = 0; i < nw; i++) held[slot].wins[i] = wins[i];

                    for (int i = 0; i < nw; i++) {
                        if (!external_src && i == src_idx && a->skip_source_r) continue;
                        Geom *g = &wins[i].g;
                        int ew2 = wins[i].gamescope ? wins[i].nested_g.w : g->w;
                        int eh2 = wins[i].gamescope ? wins[i].nested_g.h : g->h;
                        int dx = (int)(rel_x * ew2), dy = (int)(rel_y * eh2);
                        if (dx < 0) dx = 0; else if (dx >= ew2) dx = ew2-1;
                        if (dy < 0) dy = 0; else if (dy >= eh2) dy = eh2-1;
                        if (wins[i].xephyr_display[0]) {
                                /* Xephyr/gamescope: inject into its own display so the game sees it */
                            if (wins[i].gamescope) {
                                ei_click(wins[i].ei_socket, wins[i].xephyr_display,
                                    dx, dy, 0x111, true, false, wins[i].ei_ctx, wins[i].ei_dev);
                            } else {
                                Display *xdpy = XOpenDisplay(wins[i].xephyr_display);
                                if (xdpy) {
                                    Window xroot = DefaultRootWindow(xdpy);
                                    XWarpPointer(xdpy, None, xroot, 0,0,0,0, dx, dy);
                                    XTestFakeButtonEvent(xdpy, 3, True, CurrentTime);
                                    XFlush(xdpy);
                                    XCloseDisplay(xdpy);
                                }
                            }
                        } else {
                            XWarpPointer(dpy, None, DefaultRootWindow(dpy), 0,0,0,0, g->x+dx, g->y+dy);
                            XFlush(dpy);
                            XTestFakeButtonEvent(dpy, 3, True, CurrentTime);
                            XFlush(dpy);
                        }
                    }
                    /* Warp back to source so the user's cursor doesn't jump */
                    if (!wins[src_idx].xephyr_display[0]) {
                        XWarpPointer(dpy, None, DefaultRootWindow(dpy), 0,0,0,0, px, py);
                        XFlush(dpy);
                    }

                    held[slot].dpy = XOpenDisplay(NULL);
                    held[slot].active = 1;

                    /* In external-source mode, grab the pointer to our window so the
                     * host cursor is confined and XQueryPointer returns real deltas. */
                    if (external_src) {
                        /* Grab the pointer so XQueryPointer on this connection
                         * returns real deltas even though the cursor is over another window. */
                        Window xroot = DefaultRootWindow(held[slot].dpy);
                        XGrabPointer(held[slot].dpy, xroot, False,
                            PointerMotionMask | ButtonReleaseMask,
                            GrabModeAsync, GrabModeAsync,
                            None, None, CurrentTime);
                        XFlush(held[slot].dpy);
                    }

                    pthread_create(&held[slot].tid, NULL, motion_thread, &held[slot]);
                }

            } else if (ev.value == 0) { /* release */
                if (!held[slot].active) continue;
                held[slot].active = 0;

                if (slot == 0) {
                    if (held[slot].xbutton == 0) {
                        /* Deferred click+release: send now using current cursor position */
                        Window root = DefaultRootWindow(dpy), child;
                        int px, py, wx, wy; unsigned int mask;
                        XQueryPointer(dpy, root, &root, &child, &px, &py, &wx, &wy, &mask);
                        Geom *sg = &held[slot].wins[held[slot].src_idx].g;
                        double rel_x, rel_y;
                        if (held[slot].wins[held[slot].src_idx].gamescope) {
                            Display *ndpy = XOpenDisplay(held[slot].wins[held[slot].src_idx].xephyr_display);
                            if (ndpy) {
                                Window nr, nc; int nrx, nry, nwx, nwy; unsigned int nm;
                                XQueryPointer(ndpy, DefaultRootWindow(ndpy), &nr, &nc, &nrx, &nry, &nwx, &nwy, &nm);
                                rel_x = (double)nrx / held[slot].wins[held[slot].src_idx].nested_g.w;
                                rel_y = (double)nry / held[slot].wins[held[slot].src_idx].nested_g.h;
                                XCloseDisplay(ndpy);
                            } else { rel_x = rel_y = 0; }
                        } else {
                            rel_x = (double)(px - sg->x) / sg->w;
                            rel_y = (double)(py - sg->y) / sg->h;
                        }
                        for (int i = 0; i < held[slot].nw; i++) {
                            if (!held[slot].external_src && i == held[slot].src_idx && held[slot].skip_source_r) continue;
                            Geom *g = &held[slot].wins[i].g;
                            int ew3 = held[slot].wins[i].gamescope ? held[slot].wins[i].nested_g.w : g->w;
                            int eh3 = held[slot].wins[i].gamescope ? held[slot].wins[i].nested_g.h : g->h;
                            int dx = (int)(rel_x * ew3), dy = (int)(rel_y * eh3);
                            if (dy < 0) dy = 0; else if (dy >= eh3) dy = eh3-1;
                            if (held[slot].wins[i].xephyr_display[0]) {
                                if (held[slot].wins[i].gamescope) {
                                    ei_click(held[slot].wins[i].ei_socket, held[slot].wins[i].xephyr_display,
                                        dx, dy, 0x110, true, true, held[slot].wins[i].ei_ctx, held[slot].wins[i].ei_dev);
                                } else {
                                    Display *xdpy = XOpenDisplay(held[slot].wins[i].xephyr_display);
                                    if (xdpy) {
                                        Window xroot = DefaultRootWindow(xdpy);
                                        XWarpPointer(xdpy, None, xroot, 0,0,0,0, dx, dy);
                                        XTestFakeButtonEvent(xdpy, 1, True, CurrentTime);
                                        XTestFakeButtonEvent(xdpy, 1, False, CurrentTime);
                                        XFlush(xdpy);
                                        XCloseDisplay(xdpy);
                                    }
                                }
                            } else {
                                XEvent bev = {0};
                                bev.xbutton.display = dpy; bev.xbutton.window = (Window)held[slot].wins[i].xid;
                                bev.xbutton.root = DefaultRootWindow(dpy); bev.xbutton.time = CurrentTime;
                                bev.xbutton.x = dx; bev.xbutton.y = dy;
                                bev.xbutton.x_root = g->x+dx; bev.xbutton.y_root = g->y+dy;
                                bev.xbutton.button = 1; bev.xbutton.same_screen = True;
                                bev.type = ButtonPress;
                                XSendEvent(dpy, (Window)held[slot].wins[i].xid, True, ButtonPressMask, &bev);
                                bev.type = ButtonRelease;
                                XSendEvent(dpy, (Window)held[slot].wins[i].xid, True, ButtonReleaseMask, &bev);
                                XFlush(dpy);
                            }
                        }
                    } else {
                        /* Hold release: send ButtonRelease to each window */
                        for (int i = 0; i < held[slot].nw; i++) {
                            if (!held[slot].external_src && i == held[slot].src_idx && held[slot].skip_source_r) continue;
                            if (held[slot].wins[i].xephyr_display[0]) {
                                Display *xdpy = XOpenDisplay(held[slot].wins[i].xephyr_display);
                                if (xdpy) {
                                    XTestFakeButtonEvent(xdpy, 1, False, CurrentTime);
                                    XFlush(xdpy);
                                    XCloseDisplay(xdpy);
                                }
                            } else {
                                XEvent bev = {0};
                                bev.xbutton.display = dpy; bev.xbutton.window = (Window)held[slot].wins[i].xid;
                                bev.xbutton.root = DefaultRootWindow(dpy); bev.xbutton.time = CurrentTime;
                                bev.xbutton.button = 1; bev.xbutton.same_screen = True;
                                bev.type = ButtonRelease;
                                XSendEvent(dpy, (Window)held[slot].wins[i].xid, True, ButtonReleaseMask, &bev);
                                XFlush(dpy);
                            }
                        }
                    }
                } else {
                pthread_join(held[slot].tid, NULL);

                /* Warp+release in each non-source window */
                Window root = DefaultRootWindow(dpy), child;
                int px, py, wx, wy; unsigned int mask;
                XQueryPointer(dpy, root, &root, &child, &px, &py, &wx, &wy, &mask);
                Geom *sg = &held[slot].wins[held[slot].src_idx].g;
                double rel_x, rel_y;
                if (held[slot].wins[held[slot].src_idx].gamescope) {
                    Display *ndpy = XOpenDisplay(held[slot].wins[held[slot].src_idx].xephyr_display);
                    if (ndpy) {
                        Window nr, nc; int nrx, nry, nwx, nwy; unsigned int nm;
                        XQueryPointer(ndpy, DefaultRootWindow(ndpy), &nr, &nc, &nrx, &nry, &nwx, &nwy, &nm);
                        rel_x = (double)nrx / held[slot].wins[held[slot].src_idx].nested_g.w;
                        rel_y = (double)nry / held[slot].wins[held[slot].src_idx].nested_g.h;
                        XCloseDisplay(ndpy);
                    } else { rel_x = rel_y = 0; }
                } else {
                    rel_x = (double)(px - sg->x) / sg->w;
                    rel_y = (double)(py - sg->y) / sg->h;
                }

                for (int i = 0; i < held[slot].nw; i++) {
                    if (i == held[slot].src_idx && held[slot].skip_source_r) continue;
                    Geom *g = &held[slot].wins[i].g;
                    int ew4 = held[slot].wins[i].gamescope ? held[slot].wins[i].nested_g.w : g->w;
                    int eh4 = held[slot].wins[i].gamescope ? held[slot].wins[i].nested_g.h : g->h;
                    int dx = (int)(rel_x * ew4), dy = (int)(rel_y * eh4);
                    if (dx < 0) dx = 0; else if (dx >= ew4) dx = ew4-1;
                    if (dy < 0) dy = 0; else if (dy >= eh4) dy = eh4-1;
                    if (held[slot].wins[i].xephyr_display[0]) {
                        if (held[slot].wins[i].gamescope) {
                            ei_click(held[slot].wins[i].ei_socket, held[slot].wins[i].xephyr_display,
                                dx, dy, 0x111, false, true, held[slot].wins[i].ei_ctx, held[slot].wins[i].ei_dev);
                        } else {
                            Display *xdpy = XOpenDisplay(held[slot].wins[i].xephyr_display);
                            if (xdpy) {
                                XWarpPointer(xdpy, None, DefaultRootWindow(xdpy), 0,0,0,0, dx, dy);
                                XTestFakeButtonEvent(xdpy, 3, False, CurrentTime);
                                XFlush(xdpy);
                                XCloseDisplay(xdpy);
                            }
                        }
                    } else {
                        XWarpPointer(dpy, None, DefaultRootWindow(dpy), 0,0,0,0, g->x+dx, g->y+dy);
                        XFlush(dpy);
                        XTestFakeButtonEvent(dpy, 3, False, CurrentTime);
                        XFlush(dpy);
                    }
                }
                /* Warp back to source only if source is not Xephyr */
                if (!held[slot].wins[held[slot].src_idx].xephyr_display[0]) {
                    XWarpPointer(dpy, None, DefaultRootWindow(dpy), 0,0,0,0, px, py);
                    XFlush(dpy);
                }

                if (held[slot].external_src) {
                    XUngrabPointer(held[slot].dpy, CurrentTime);
                    XFlush(held[slot].dpy);
                }

                XCloseDisplay(held[slot].dpy);
                held[slot].dpy = NULL;
                } /* end slot != 0 */
            }
        }
    }

    /* Clean up held buttons */
    for (int s = 0; s < 2; s++) {
        if (!held[s].active) continue;
        held[s].active = 0;
        if (s == 0) {
            for (int i = 0; i < held[s].nw; i++) {
                if (held[s].wins[i].xephyr_display[0]) {
                    Display *xdpy = XOpenDisplay(held[s].wins[i].xephyr_display);
                    if (xdpy) { XTestFakeButtonEvent(xdpy, 1, False, CurrentTime); XFlush(xdpy); XCloseDisplay(xdpy); }
                } else {
                    XEvent bev = {0}; bev.type = ButtonRelease;
                    bev.xbutton.display = dpy; bev.xbutton.window = (Window)held[s].wins[i].xid;
                    bev.xbutton.button = 1; bev.xbutton.same_screen = True; bev.xbutton.time = CurrentTime;
                    XSendEvent(dpy, (Window)held[s].wins[i].xid, True, ButtonReleaseMask, &bev);
                }
            }
            XFlush(dpy);
        } else {
            pthread_join(held[s].tid, NULL);
            for (int i = 0; i < held[s].nw; i++) {
                if (i == held[s].src_idx && held[s].skip_source_r) continue;
                XTestFakeButtonEvent(dpy, 3, False, CurrentTime);
            }
            XFlush(dpy);
            if (held[s].dpy) XCloseDisplay(held[s].dpy);
        }
    }

    XCloseDisplay(dpy);
done:
    /* Close any persistent ei connections */
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < held[s].nw; i++)
            held[s].wins[i].ei_ctx = NULL; /* owned by cache, don't double-free */
    for (int i = 0; i < ei_cache_n; i++)
        ei_win_close(&ei_cache[i]);
    libevdev_free(dev);
    close(fd);
    free(a);
    return NULL;
}

/* Key-binding capture */

static void finish_binding(int slot, int code) {
    const char *evname = libevdev_event_code_get_name(EV_KEY, code);
    char label[64];
    snprintf(label, sizeof(label), "%s", evname ? evname : "?");
    if (slot == 1) { key1_code = code; gtk_button_set_label(GTK_BUTTON(btn_key1), label); }
    else           { key2_code = code; gtk_button_set_label(GTK_BUTTON(btn_key2), label); }
    binding_slot = 0;
}

static gboolean on_binding_key_press(GtkWidget *w, GdkEventKey *ev, gpointer unused) {
    if (!binding_slot) return FALSE;
    const char *name = gdk_keyval_name(ev->keyval);
    if (!name) return FALSE;

    int code = 0;
    /* Try KEY_<NAME> lookup */
    char evname[64];
    snprintf(evname, sizeof(evname), "KEY_%s", name);
    for (char *p = evname+4; *p; p++) *p = toupper((unsigned char)*p);
    int c = libevdev_event_code_from_name(EV_KEY, evname);
    if (c >= 0) code = c;

    if (!code) return FALSE;
    finish_binding(binding_slot, code);
    return TRUE;
}

static gboolean on_binding_button_press(GtkWidget *w, GdkEventButton *ev, gpointer unused) {
    if (!binding_slot) return FALSE;
    /* Ignore the click that triggered the binding button itself */
    static guint32 ignore_time = 0;
    if (ev->time == ignore_time) return FALSE;

    int codes[] = {0, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE, BTN_EXTRA};
    int code = (ev->button < 6) ? codes[ev->button] : 0;
    if (!code) return FALSE;
    finish_binding(binding_slot, code);
    return TRUE;
}

static void on_bind_key1(GtkButton *b, gpointer unused) {
    binding_slot = 1;
    gtk_button_set_label(b, "Press a key or button…");
}

static void on_bind_key2(GtkButton *b, gpointer unused) {
    binding_slot = 2;
    gtk_button_set_label(b, "Press a key or button…");
}

/* Title combo */

static void on_title_combo_changed(GtkComboBox *cb, gpointer unused) {
    int idx = gtk_combo_box_get_active(cb);
    gtk_widget_set_sensitive(entry_title, idx == 2);
    if (idx == 0) gtk_entry_set_text(GTK_ENTRY(entry_title), "Toontown Rewritten");
    else if (idx == 1) gtk_entry_set_text(GTK_ENTRY(entry_title), "Corporate Clash*");
}

/* Device picker */

static void on_device_response(GtkDialog *dlg, int response, gpointer data) {
    if (response == GTK_RESPONSE_OK) {
        GtkTreeView *tv = GTK_TREE_VIEW(data);
        GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
        GtkTreeModel *model; GtkTreeIter iter;
        if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
            char *byid = NULL, *name = NULL;
            gtk_tree_model_get(model, &iter, 0, &byid, 1, &name, -1);
            strncpy(device_path, byid, sizeof(device_path)-1);
            char label[300];
            snprintf(label, sizeof(label), "%s", byid);
            gtk_button_set_label(GTK_BUTTON(btn_device), label);
            gtk_widget_set_sensitive(btn_start, TRUE);
            g_free(byid); g_free(name);
        }
    }
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

static void on_pick_device(GtkButton *b, gpointer unused) {
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Select Input Device", GTK_WINDOW(main_window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_OK",     GTK_RESPONSE_OK, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 600, 300);

    /* col 0 = by-id path, col 1 = friendly name */
    GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);

    DIR *d = opendir("/dev/input/by-id");
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        /* only event nodes, skip -kbd / -mouse ambiguity by taking all */
        char byid[256];
        snprintf(byid, sizeof(byid), "/dev/input/by-id/%s", de->d_name);
        int fd = open(byid, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        struct libevdev *dev = NULL;
        if (libevdev_new_from_fd(fd, &dev) == 0) {
            GtkTreeIter it;
            gtk_list_store_append(store, &it);
            gtk_list_store_set(store, &it,
                0, byid,
                1, libevdev_get_name(dev), -1);
            libevdev_free(dev);
        }
        close(fd);
    }
    if (d) closedir(d);

    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
        gtk_tree_view_column_new_with_attributes("By-ID Path",
            gtk_cell_renderer_text_new(), "text", 0, NULL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
        gtk_tree_view_column_new_with_attributes("Name",
            gtk_cell_renderer_text_new(), "text", 1, NULL));

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
                       sw, TRUE, TRUE, 0);
    gtk_widget_show_all(dlg);
    g_signal_connect(dlg, "response", G_CALLBACK(on_device_response), tv);
}

/* Start / Stop */

static gboolean on_worker_stopped(gpointer data) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn_start), FALSE);
    gtk_button_set_label(GTK_BUTTON(btn_start), "Start");
    status_msg("Stopped.");
    return G_SOURCE_REMOVE;
}

static void *worker_wrapper(void *arg) {
    worker(arg);
    worker_running = 0;
    g_idle_add(on_worker_stopped, NULL);
    return NULL;
}

static void on_toggle_start(GtkToggleButton *tb, gpointer unused) {
    if (gtk_toggle_button_get_active(tb)) {
        gtk_button_set_label(GTK_BUTTON(tb), "Stop");
        worker_running = 1;
        pipe(worker_pipe);
        WorkerArgs *a = malloc(sizeof(*a));
        strncpy(a->device, device_path, sizeof(a->device)-1);
        strncpy(a->title, gtk_entry_get_text(GTK_ENTRY(entry_title)), sizeof(a->title)-1);
        a->k1 = key1_code;
        a->k2 = key2_code;
        a->skip_source   = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_skip_source));
        a->skip_source_r = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_skip_source_r));
        a->use_xephyr    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_xephyr));
        a->use_gamescope = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_gamescope));
        pthread_create(&worker_thread, NULL, worker_wrapper, a);
    } else {
        worker_running = 0;
        if (worker_pipe[1] >= 0) write(worker_pipe[1], "x", 1);
        pthread_join(worker_thread, NULL);
        close(worker_pipe[0]); close(worker_pipe[1]);
        worker_pipe[0] = worker_pipe[1] = -1;
        gtk_button_set_label(GTK_BUTTON(tb), "Start");
        status_msg("Stopped.");
    }
}

/* Main */

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    GtkBuilder *builder =
        gtk_builder_new_from_file(DATADIR "/tt-multiclick/tt-multiclick.ui");
    main_window = GTK_WIDGET(gtk_builder_get_object(builder, "main_window"));
    btn_device  = GTK_WIDGET(gtk_builder_get_object(builder, "btn_device"));
    btn_key1    = GTK_WIDGET(gtk_builder_get_object(builder, "btn_key1"));
    btn_key2    = GTK_WIDGET(gtk_builder_get_object(builder, "btn_key2"));
    btn_start   = GTK_WIDGET(gtk_builder_get_object(builder, "btn_start"));
    combo_title = GTK_WIDGET(gtk_builder_get_object(builder, "combo_title"));
    entry_title = GTK_WIDGET(gtk_builder_get_object(builder, "entry_title"));
    lbl_status  = GTK_WIDGET(gtk_builder_get_object(builder, "lbl_status"));
    chk_skip_source   = GTK_WIDGET(gtk_builder_get_object(builder, "chk_skip_source"));
    chk_skip_source_r = GTK_WIDGET(gtk_builder_get_object(builder, "chk_skip_source_r"));
    chk_xephyr        = GTK_WIDGET(gtk_builder_get_object(builder, "chk_xephyr"));
    chk_gamescope     = GTK_WIDGET(gtk_builder_get_object(builder, "chk_gamescope"));
    g_object_unref(builder);

    gtk_widget_add_events(main_window, GDK_BUTTON_PRESS_MASK);

    gtk_button_set_label(GTK_BUTTON(btn_key1),
        libevdev_event_code_get_name(EV_KEY, key1_code) ?: "BTN_LEFT");
    gtk_button_set_label(GTK_BUTTON(btn_key2),
        libevdev_event_code_get_name(EV_KEY, key2_code) ?: "BTN_RIGHT");

    g_signal_connect(btn_device,  "clicked", G_CALLBACK(on_pick_device),   NULL);
    g_signal_connect(btn_key1,    "clicked", G_CALLBACK(on_bind_key1),     NULL);
    g_signal_connect(btn_key2,    "clicked", G_CALLBACK(on_bind_key2),     NULL);
    g_signal_connect(btn_start,   "toggled", G_CALLBACK(on_toggle_start),  NULL);
    g_signal_connect(combo_title, "changed", G_CALLBACK(on_title_combo_changed), NULL);
    g_signal_connect(main_window, "key-press-event",    G_CALLBACK(on_binding_key_press),    NULL);
    g_signal_connect(main_window, "button-press-event", G_CALLBACK(on_binding_button_press), NULL);
    g_signal_connect(main_window, "destroy", G_CALLBACK(gtk_main_quit),    NULL);

    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_title), 0);

    gtk_widget_show_all(main_window);
    gtk_main();
    return 0;
}
