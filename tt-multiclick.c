#include <gtk/gtk.h>
#include <libevdev/libevdev.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <libei-1.0/libei.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

/* ── UI widgets ─────────────────────────────────────────────────── */
static GtkWidget *main_window;
static GtkWidget *btn_device, *btn_key1, *btn_key2;
static GtkWidget *btn_start;
static GtkWidget *combo_title, *entry_title;
static GtkWidget *lbl_status;
static GtkWidget *chk_gamescope;
static GtkWidget *chk_skip_source, *chk_skip_source_r;

/* ── State ──────────────────────────────────────────────────────── */
static char   device_path[256] = "";
static int    key1_code = BTN_LEFT;
static int    key2_code = BTN_RIGHT;
static int    binding_slot = 0;   /* nonzero while waiting for a key */

static pthread_t      worker_thread;
static volatile int   worker_running = 0;
static int            worker_pipe[2] = {-1, -1};

/* ── Pure-Xlib window discovery (no subprocesses) ───────────────── */

typedef struct { int x, y, w, h; } Geom;
typedef struct { long xid; Geom g; } Win;

static Display *query_dpy;  /* shared with worker; opened once in worker() */

static int get_cursor_pos(int *x, int *y) {
    Window root = DefaultRootWindow(query_dpy), child;
    int rx, ry, wx, wy; unsigned int mask;
    if (!XQueryPointer(query_dpy, root, &root, &child, &rx, &ry, &wx, &wy, &mask))
        return -1;
    *x = rx; *y = ry;
    return 0;
}

static int get_cursor_pos_hyprctl(int *x, int *y) {
    FILE *f = popen("hyprctl cursorpos 2>/dev/null", "r");
    if (!f) return -1;
    int r = fscanf(f, "%d, %d", x, y);
    pclose(f);
    return (r == 2) ? 0 : -1;
}

/* Match window name against title (trailing * = prefix match). */
static int title_matches(const char *name, const char *title) {
    size_t tlen = strlen(title);
    if (tlen && title[tlen-1] == '*')
        return strncmp(name, title, tlen-1) == 0;
    return strcmp(name, title) == 0;
}

/* Get absolute screen position of a window via XTranslateCoordinates. */
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

/* Recursively walk window tree, collect matches into wins[]. */
static int collect_windows(Display *dpy, Window w,
                            const char *title, Win *wins, int n, int max) {
    /* Check this window's name */
    char *name = NULL;
    if (XFetchName(dpy, w, &name) && name) {
        if (title_matches(name, title)) {
            Geom g;
            if (n < max && win_screen_geom(dpy, w, &g) && g.w > 1 && g.h > 1) {
                wins[n].xid = (long)w;
                wins[n].g   = g;
                n++;
            }
        }
        XFree(name);
    }
    /* Recurse into children */
    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren = 0;
    if (XQueryTree(dpy, w, &root_ret, &parent_ret, &children, &nchildren) && children) {
        for (unsigned int i = 0; i < nchildren && n < max; i++)
            n = collect_windows(dpy, children[i], title, wins, n, max);
        XFree(children);
    }
    return n;
}

static int find_windows(const char *title, Win *wins, int max) {
    return collect_windows(query_dpy, DefaultRootWindow(query_dpy),
                           title, wins, 0, max);
}

/* ── X11 click sender ───────────────────────────────────────────── */

static void send_click(Display *dpy, long xid, int ex, int ey,
                       int rx, int ry, int button) {
    Window win = (Window)xid;
    Window root = DefaultRootWindow(dpy);
    XEvent ev = {0};
    ev.xbutton.display     = dpy;
    ev.xbutton.window      = win;
    ev.xbutton.root        = root;
    ev.xbutton.subwindow   = None;
    ev.xbutton.time        = CurrentTime;
    ev.xbutton.x           = ex;
    ev.xbutton.y           = ey;
    ev.xbutton.x_root      = rx;
    ev.xbutton.y_root      = ry;
    ev.xbutton.state       = 0;
    ev.xbutton.button      = button;
    ev.xbutton.same_screen = True;
    ev.type = ButtonPress;
    XSendEvent(dpy, win, True, ButtonPressMask, &ev);
    ev.type = ButtonRelease;
    XSendEvent(dpy, win, True, ButtonReleaseMask, &ev);
    XFlush(dpy);
}

/* ── Gamescope EIS helpers ───────────────────────────────────────── */

typedef struct {
    long   pid;
    int    at_x, at_y, w, h;   /* hyprctl window geometry */
    int    virt_w, virt_h;     /* gamescope -W/-H virtual size */
    char   ei_socket[64];      /* e.g. "gamescope-2-ei" */
    struct ei        *ei_ctx;
    struct ei_device *ei_dev;
    uint32_t          ei_seq;
    int               wakeup_fd; /* write end of gamescope's internal wakeup pipe */
} GsWin;

/* Parse -W and -H from /proc/<pid>/cmdline. Returns 1 on success. */
static int gs_parse_virtual_size(long pid, int *vw, int *vh) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[4096]; size_t n = fread(buf, 1, sizeof(buf)-1, f); fclose(f);
    buf[n] = '\0';
    /* cmdline is NUL-separated; replace NULs with spaces for easy scanning */
    for (size_t i = 0; i < n; i++) if (!buf[i]) buf[i] = ' ';
    char *p;
    *vw = *vh = 0;
    if ((p = strstr(buf, " -W "))) *vw = atoi(p + 4);
    if ((p = strstr(buf, " -H "))) *vh = atoi(p + 4);
    return (*vw > 0 && *vh > 0);
}

/* Find which gamescope-N-ei socket a PID has open, by matching socket inodes.
   Fills ei_socket like "gamescope-2-ei". Returns 1 on success. */
static int gs_find_ei_socket(long pid, char *ei_socket, size_t len) {
    /* Build table: inode -> instance index from /proc/net/unix */
    typedef struct { unsigned long inode; int idx; } Slot;
    Slot slots[8]; int nslots = 0;

    FILE *f = fopen("/proc/net/unix", "r");
    if (!f) return 0;
    char line[512];
    fgets(line, sizeof(line), f); /* skip header */
    while (fgets(line, sizeof(line), f) && nslots < 8) {
        /* fields: Num RefCount Protocol Flags Type St Inode Path */
        unsigned long inode = 0; char path[256] = "";
        /* sscanf the inode (7th field) and path (8th) */
        if (sscanf(line, "%*s %*s %*s %*s %*s %*s %lu %255s", &inode, path) == 2) {
            /* match /run/user/<uid>/gamescope-N-ei */
            char *base = strrchr(path, '/');
            if (!base) base = path; else base++;
            int idx = -1;
            if (sscanf(base, "gamescope-%d-ei", &idx) == 1 && idx >= 0) {
                slots[nslots].inode = inode;
                slots[nslots].idx   = idx;
                nslots++;
            }
        }
    }
    fclose(f);
    if (!nslots) return 0;

    /* Walk /proc/<pid>/fd looking for a socket whose inode matches */
    char fddir[64]; snprintf(fddir, sizeof(fddir), "/proc/%ld/fd", pid);
    DIR *d = opendir(fddir); if (!d) return 0;
    struct dirent *de; int found = 0;
    while ((de = readdir(d)) && !found) {
        char fdpath[128], target[256];
        snprintf(fdpath, sizeof(fdpath), "%s/%s", fddir, de->d_name);
        ssize_t r = readlink(fdpath, target, sizeof(target)-1);
        if (r <= 0) continue;
        target[r] = '\0';
        unsigned long sock_inode = 0;
        if (sscanf(target, "socket:[%lu]", &sock_inode) != 1) continue;
        for (int i = 0; i < nslots; i++) {
            if (slots[i].inode == sock_inode) {
                snprintf(ei_socket, len, "gamescope-%d-ei", slots[i].idx);
                found = 1; break;
            }
        }
    }
    closedir(d);
    return found;
}

/* Query hyprctl for all gamescope windows matching title_prefix.
   Returns count, fills wins[]. */
static int gs_find_windows(const char *title_prefix, GsWin *wins, int max) {
    FILE *f = popen("hyprctl clients -j 2>/dev/null", "r");
    if (!f) return 0;

    /* Read entire output */
    char *buf = NULL; size_t bufsz = 0, total = 0;
    char tmp[4096]; size_t r;
    while ((r = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        buf = realloc(buf, total + r + 1);
        memcpy(buf + total, tmp, r);
        total += r;
    }
    pclose(f);
    if (!buf) return 0;
    buf[total] = '\0';

    int count = 0;
    /* Minimal JSON array parser: find each { ... } object */
    char *p = buf;
    while (count < max && (p = strchr(p, '{'))) {
        /* Find matching closing brace (not nested deeply in this JSON) */
        int depth = 0; char *obj_start = p;
        do {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            p++;
        } while (*p && depth > 0);
        /* p now points just past the closing } */
        char obj[2048];
        size_t obj_len = (size_t)(p - obj_start);
        if (obj_len >= sizeof(obj)) continue;
        memcpy(obj, obj_start, obj_len);
        obj[obj_len] = '\0';

        /* Check class == "gamescope" */
        if (!strstr(obj, "\"gamescope\"")) continue;

        /* Extract title */
        char title[256] = "";
        char *tp = strstr(obj, "\"title\"");
        if (tp) {
            tp = strchr(tp + 7, '"'); /* opening quote of value */
            if (tp) {
                tp++;
                char *te = strchr(tp, '"');
                if (te) { size_t tl = (size_t)(te-tp); if (tl < sizeof(title)) { memcpy(title, tp, tl); title[tl] = '\0'; } }
            }
        }

        /* Match title prefix */
        size_t pfx_len = strlen(title_prefix);
        if (pfx_len && title_prefix[pfx_len-1] == '*') {
            if (strncmp(title, title_prefix, pfx_len-1) != 0) continue;
        } else {
            if (strcmp(title, title_prefix) != 0) continue;
        }

        /* Extract pid */
        long pid = 0;
        char *pidp = strstr(obj, "\"pid\"");
        if (pidp) pid = atol(strchr(pidp + 5, ':') + 1);
        if (!pid) continue;

        /* Extract at:[x,y] */
        int ax = 0, ay = 0;
        char *atp = strstr(obj, "\"at\"");
        if (atp) sscanf(atp, "\"at\": [ %d, %d ]", &ax, &ay);

        /* Extract size:[w,h] */
        int sw = 0, sh = 0;
        char *szp = strstr(obj, "\"size\"");
        if (szp) sscanf(szp, "\"size\": [ %d, %d ]", &sw, &sh);

        if (!sw || !sh) continue;

        int vw, vh;
        if (!gs_parse_virtual_size(pid, &vw, &vh)) continue;

        char ei_sock[64];
        if (!gs_find_ei_socket(pid, ei_sock, sizeof(ei_sock))) continue;

        wins[count].pid    = pid;
        wins[count].at_x   = ax;
        wins[count].at_y   = ay;
        wins[count].w      = sw;
        wins[count].h      = sh;
        wins[count].virt_w = vw;
        wins[count].virt_h = vh;
        strncpy(wins[count].ei_socket, ei_sock, sizeof(wins[count].ei_socket)-1);
        count++;
    }
    free(buf);
    return count;
}

/* Find the write end of gamescope's internal wakeup pipe.
   Strategy: find a pipe where the read end is monitored in an epoll,
   then open the write end via /proc/<pid>/fd. */
static int gs_find_wakeup_fd(long pid) {
    char fddir[64];
    snprintf(fddir, sizeof(fddir), "/proc/%ld/fd", pid);

    /* Step 1: collect all pipe inodes with a writable fd */
    typedef struct { unsigned long inode; int fd_num; } PipeW;
    PipeW writefds[64]; int nw = 0;

    DIR *d = opendir(fddir); if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) && nw < 64) {
        if (!isdigit(de->d_name[0])) continue;
        char path[128], target[128];
        snprintf(path, sizeof(path), "%s/%s", fddir, de->d_name);
        ssize_t r = readlink(path, target, sizeof(target)-1);
        if (r <= 0) continue; target[r] = '\0';
        unsigned long inode = 0;
        if (sscanf(target, "pipe:[%lu]", &inode) != 1) continue;
        char fipath[128]; snprintf(fipath, sizeof(fipath), "/proc/%ld/fdinfo/%s", pid, de->d_name);
        FILE *f = fopen(fipath, "r"); if (!f) continue;
        char line[64]; unsigned int flags = 0;
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "flags: %o", &flags) == 1) break;
        fclose(f);
        if ((flags & 3) == 1) { writefds[nw].inode = inode; writefds[nw].fd_num = atoi(de->d_name); nw++; }
    }
    closedir(d);

    /* Step 2: find an epoll that monitors the read end of one of those pipes */
    d = opendir(fddir); if (!d) return -1;
    int result = -1;
    while ((de = readdir(d)) && result < 0) {
        if (!isdigit(de->d_name[0])) continue;
        char path[128], target[128];
        snprintf(path, sizeof(path), "%s/%s", fddir, de->d_name);
        ssize_t r = readlink(path, target, sizeof(target)-1);
        if (r <= 0) continue; target[r] = '\0';
        if (!strstr(target, "eventpoll")) continue;
        char fipath[128]; snprintf(fipath, sizeof(fipath), "/proc/%ld/fdinfo/%s", pid, de->d_name);
        FILE *f = fopen(fipath, "r"); if (!f) continue;
        char line[256];
        while (fgets(line, sizeof(line), f) && result < 0) {
            int tfd = -1;
            if (sscanf(line, "tfd: %d", &tfd) != 1) continue;
            char tpath[128], ttarget[128];
            snprintf(tpath, sizeof(tpath), "/proc/%ld/fd/%d", pid, tfd);
            ssize_t tr = readlink(tpath, ttarget, sizeof(ttarget)-1);
            if (tr <= 0) continue; ttarget[tr] = '\0';
            unsigned long tinode = 0;
            if (sscanf(ttarget, "pipe:[%lu]", &tinode) != 1) continue;
            for (int i = 0; i < nw; i++) {
                if (writefds[i].inode == tinode) {
                    char wpath[128];
                    snprintf(wpath, sizeof(wpath), "/proc/%ld/fd/%d", pid, writefds[i].fd_num);
                    result = open(wpath, O_WRONLY | O_NONBLOCK);
                    break;
                }
            }
        }
        fclose(f);
    }
    closedir(d);
    return result;
}


static int gs_ei_connect(GsWin *g) {
    char socket_path[128];
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg) xdg = "/run/user/1000";
    snprintf(socket_path, sizeof(socket_path), "%s/%s", xdg, g->ei_socket);

    struct ei *ei = ei_new_sender(NULL);
    if (!ei) return 0;
    ei_configure_name(ei, "tt-multiclick");
    if (ei_setup_backend_socket(ei, socket_path) < 0) { ei_unref(ei); return 0; }

    int ei_fd = ei_get_fd(ei);
    struct ei_device *abs_dev = NULL;

    for (int iter = 0; iter < 200 && !abs_dev; iter++) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(ei_fd, &rfds);
        struct timeval tv = {0, 10000};
        if (select(ei_fd+1, &rfds, NULL, NULL, &tv) <= 0) continue;
        ei_dispatch(ei);
        struct ei_event *ev;
        while ((ev = ei_get_event(ei))) {
            enum ei_event_type t = ei_event_get_type(ev);
            if (t == EI_EVENT_SEAT_ADDED)
                ei_seat_bind_capabilities(ei_event_get_seat(ev),
                    EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_POINTER_ABSOLUTE,
                    EI_DEVICE_CAP_BUTTON, NULL);
            else if (t == EI_EVENT_DEVICE_ADDED) {
                struct ei_device *d = ei_event_get_device(ev);
                if (ei_device_has_capability(d, EI_DEVICE_CAP_POINTER_ABSOLUTE) &&
                    ei_device_has_capability(d, EI_DEVICE_CAP_BUTTON))
                    abs_dev = ei_device_ref(d);
            } else if (t == EI_EVENT_DEVICE_RESUMED && abs_dev) {
                ei_event_unref(ev);
                g->ei_ctx = ei;
                g->ei_dev = abs_dev;
                g->ei_seq = 1;
                g->wakeup_fd = gs_find_wakeup_fd(g->pid);
                fprintf(stderr, "[ei] connected to %s wakeup_fd=%d\n", socket_path, g->wakeup_fd);
                return 1;
            } else if (t == EI_EVENT_DISCONNECT) {
                ei_event_unref(ev); goto fail;
            }
            ei_event_unref(ev);
        }
    }
fail:
    if (abs_dev) ei_device_unref(abs_dev);
    ei_unref(ei);
    return 0;
}

static void gs_ei_disconnect(GsWin *g) {
    if (g->ei_dev) { ei_device_unref(g->ei_dev); g->ei_dev = NULL; }
    if (g->ei_ctx) { ei_unref(g->ei_ctx);         g->ei_ctx = NULL; }
    if (g->wakeup_fd >= 0) { close(g->wakeup_fd); g->wakeup_fd = -1; }
}

/* Send a click on an already-connected GsWin. */
static void gs_send_button(GsWin *g, double vx, double vy, int button, bool press, bool release) {
    if (!g->ei_ctx || !g->ei_dev) return;
    struct ei *ei = g->ei_ctx;
    struct ei_device *dev = g->ei_dev;

    ei_device_start_emulating(dev, g->ei_seq++);
    ei_device_pointer_motion_absolute(dev, vx, vy);
    ei_device_frame(dev, ei_now(ei));
    ei_device_pointer_motion(dev, 0, 0);
    ei_device_frame(dev, ei_now(ei));
    if (press) {
        ei_device_button_button(dev, (uint32_t)button, true);
        ei_device_frame(dev, ei_now(ei));
    }
    if (release) {
        ei_device_button_button(dev, (uint32_t)button, false);
        ei_device_frame(dev, ei_now(ei));
    }
    ei_device_stop_emulating(dev);
    /* Send a tiny relative nudge to flush gamescope's input queue */
    ei_device_start_emulating(dev, g->ei_seq++);
    ei_device_pointer_motion(dev, 0, 1);
    ei_device_frame(dev, ei_now(ei));
    ei_device_pointer_motion(dev, 0, -1);
    ei_device_frame(dev, ei_now(ei));
    ei_device_stop_emulating(dev);
    ei_dispatch(ei);
    struct ei_event *ev; while ((ev = ei_get_event(ei))) ei_event_unref(ev);
    /* Wake gamescope's main loop so it calls eis_dispatch and processes our events */
    if (g->wakeup_fd >= 0) {
        if (write(g->wakeup_fd, "\0", 1) < 0) {
            close(g->wakeup_fd);
            g->wakeup_fd = gs_find_wakeup_fd(g->pid);
            if (g->wakeup_fd >= 0) write(g->wakeup_fd, "\0", 1);
        }
    }
}

static void gs_send_click(GsWin *g, double vx, double vy, int button) {
    gs_send_button(g, vx, vy, button, true, true);
}



/* ── Worker thread ──────────────────────────────────────────────── */

typedef struct { char device[256]; char title[256]; int k1, k2; int gamescope; int skip_source; int skip_source_r; } WorkerArgs;

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

    /* Open persistent EIS connections for gamescope mode */
    GsWin gswins[16]; int nw_gs = 0;
    if (a->gamescope) {
        nw_gs = gs_find_windows(a->title, gswins, 16);
        for (int i = 0; i < nw_gs; i++)
            gs_ei_connect(&gswins[i]);
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

        int rc;
        while ((rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) >= 0 ||
               rc == LIBEVDEV_READ_STATUS_SYNC) {
            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                while (libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev) >= 0);
                continue;
            }

        if (ev.type != EV_KEY) continue;
        if (ev.value == 2) continue; /* ignore repeat */
        int xbutton = (ev.code == a->k1) ? 1 : (ev.code == a->k2) ? 2 : 0;
        if (!xbutton) continue;
        /* left click: fire on release only; right click: fire on press and release */
        if (xbutton == 1 && ev.value != 0) continue;

        int px, py;
        if (a->gamescope) {
            if (get_cursor_pos_hyprctl(&px, &py) < 0) continue;
        } else {
            if (get_cursor_pos(&px, &py) < 0) continue;
        }

        if (a->gamescope) {
            fprintf(stderr, "[gs] cursor=(%d,%d) button=%d cached %d windows\n", px, py, xbutton, nw_gs);
            if (nw_gs < 2) continue;

            GsWin *src = NULL;
            for (int i = 0; i < nw_gs; i++) {
                GsWin *g = &gswins[i];
                fprintf(stderr, "[gs]   win[%d] pid=%ld at=(%d,%d) size=%dx%d virt=%dx%d sock=%s\n",
                        i, g->pid, g->at_x, g->at_y, g->w, g->h, g->virt_w, g->virt_h, g->ei_socket);
                if (px >= g->at_x && px < g->at_x + g->w &&
                    py >= g->at_y && py < g->at_y + g->h)
                    if (!src || g->w * g->h < src->w * src->h) src = g;
            }
            if (!src) { fprintf(stderr, "[gs] cursor not inside any window\n"); continue; }

            double rel_x = (double)(px - src->at_x) / src->w;
            double rel_y = (double)(py - src->at_y) / src->h;
            fprintf(stderr, "[gs] src pid=%ld rel=(%.4f,%.4f)\n", src->pid, rel_x, rel_y);

            for (int i = 0; i < nw_gs; i++) {
                GsWin *g = &gswins[i];
                if (g->pid == src->pid && (xbutton == 1 ? a->skip_source : a->skip_source_r)) continue;
                double vx = rel_x * g->virt_w;
                double vy = rel_y * g->virt_h;
                int btn = xbutton == 1 ? BTN_LEFT : BTN_RIGHT;
                if (xbutton == 2)
                    gs_send_button(g, vx, vy, btn, ev.value == 1, ev.value == 0);
                else
                    gs_send_click(g, vx, vy, btn);
                struct timespec ts = {0, 10000000};
                nanosleep(&ts, NULL);
            }
        } else {
            Win wins[16];
            int nw = find_windows(a->title, wins, 16);
            if (nw < 2) continue;

            Win *src = NULL;
            for (int i = 0; i < nw; i++) {
                Geom *g = &wins[i].g;
                if (px >= g->x && px < g->x+g->w && py >= g->y && py < g->y+g->h)
                    if (!src || g->w*g->h < src->g.w*src->g.h) src = &wins[i];
            }
            if (!src) continue;

            double rel_x = (double)(px - src->g.x) / src->g.w;
            double rel_y = (double)(py - src->g.y) / src->g.h;

            for (int i = 0; i < nw; i++) {
                Geom *g = &wins[i].g;
                int dx = (int)(rel_x * g->w);
                int dy = (int)(rel_y * g->h);
                if (dx < 0) dx = 0; else if (dx >= g->w) dx = g->w-1;
                if (dy < 0) dy = 0; else if (dy >= g->h) dy = g->h-1;
                send_click(dpy, wins[i].xid, dx, dy, g->x+dx, g->y+dy, xbutton);
                struct timespec ts = {0, 10000000};
                nanosleep(&ts, NULL);
            }
        }
        } /* end while libevdev_next_event */
    }

    XCloseDisplay(dpy);
done:
    for (int i = 0; i < nw_gs; i++) gs_ei_disconnect(&gswins[i]);
    libevdev_free(dev);
    close(fd);
    free(a);
    return NULL;
}

/* ── Key-binding capture ────────────────────────────────────────── */

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

/* ── Title combo ────────────────────────────────────────────────── */

static void on_title_combo_changed(GtkComboBox *cb, gpointer unused) {
    int idx = gtk_combo_box_get_active(cb);
    gtk_widget_set_sensitive(entry_title, idx == 2);
    if (idx == 0) gtk_entry_set_text(GTK_ENTRY(entry_title), "Toontown Rewritten");
    else if (idx == 1) gtk_entry_set_text(GTK_ENTRY(entry_title), "Corporate Clash*");
}

/* ── Device picker ──────────────────────────────────────────────── */

typedef struct { GtkWidget *tv; GtkListStore *store; GtkWidget *chk_byid; } DevicePickerCtx;

static void populate_device_list(GtkListStore *store, int use_byid) {
    gtk_list_store_clear(store);
    /* use_byid: 1=by-id, 0=by-path, 2=raw /dev/input */
    const char *dir = use_byid == 1 ? "/dev/input/by-id"
                    : use_byid == 0 ? "/dev/input/by-path"
                    : "/dev/input";
    DIR *d = opendir(dir);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (use_byid == 2 && strncmp(de->d_name, "event", 5) != 0) continue;
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        struct libevdev *dev = NULL;
        if (libevdev_new_from_fd(fd, &dev) == 0) {
            GtkTreeIter it;
            gtk_list_store_append(store, &it);
            gtk_list_store_set(store, &it, 0, path, 1, libevdev_get_name(dev), -1);
            libevdev_free(dev);
        }
        close(fd);
    }
    if (d) closedir(d);
}

static void on_byid_toggled(GtkToggleButton *tb, gpointer data) {
    DevicePickerCtx *ctx = data;
    /* cycle: by-id -> by-path -> raw */
    int cur = gtk_combo_box_get_active(GTK_COMBO_BOX(ctx->chk_byid));
    populate_device_list(ctx->store, cur);
}

static void on_device_response(GtkDialog *dlg, int response, gpointer data) {
    if (response == GTK_RESPONSE_OK) {
        DevicePickerCtx *ctx = data;
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(ctx->tv));
        GtkTreeModel *model; GtkTreeIter iter;
        if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
            char *path = NULL;
            gtk_tree_model_get(model, &iter, 0, &path, -1);
            strncpy(device_path, path, sizeof(device_path)-1);
            gtk_button_set_label(GTK_BUTTON(btn_device), path);
            gtk_widget_set_sensitive(btn_start, TRUE);
            g_free(path);
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
    gtk_window_set_default_size(GTK_WINDOW(dlg), 600, 340);

    GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);

    GtkWidget *chk = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(chk), "by-id");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(chk), "by-path");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(chk), "raw /dev/input");
    gtk_combo_box_set_active(GTK_COMBO_BOX(chk), 0);

    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
        gtk_tree_view_column_new_with_attributes("Path",
            gtk_cell_renderer_text_new(), "text", 0, NULL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
        gtk_tree_view_column_new_with_attributes("Name",
            gtk_cell_renderer_text_new(), "text", 1, NULL));

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), tv);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_pack_start(GTK_BOX(content), chk, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(content), sw,  TRUE,  TRUE,  0);

    DevicePickerCtx *ctx = g_new(DevicePickerCtx, 1);
    ctx->tv = tv; ctx->store = store; ctx->chk_byid = chk;

    populate_device_list(store, TRUE);

    g_signal_connect(chk, "changed", G_CALLBACK(on_byid_toggled), ctx);
    g_signal_connect(dlg, "response", G_CALLBACK(on_device_response), ctx);
    g_signal_connect_swapped(dlg, "destroy", G_CALLBACK(g_free), ctx);

    gtk_widget_show_all(dlg);
}

/* ── Start / Stop ───────────────────────────────────────────────── */

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
        a->gamescope = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_gamescope));
        a->skip_source   = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_skip_source));
        a->skip_source_r = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_skip_source_r));
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

/* ── Main ───────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    char ui_path[512];
    char exe[512] = ""; ssize_t el = readlink("/proc/self/exe", exe, sizeof(exe)-1);
    if (el > 0) { exe[el] = '\0'; char *sl = strrchr(exe, '/'); if (sl) *sl = '\0'; }
    snprintf(ui_path, sizeof(ui_path), "%s/tt-multiclick.ui", el > 0 ? exe : ".");
    GtkBuilder *builder = gtk_builder_new_from_file(ui_path);
    main_window = GTK_WIDGET(gtk_builder_get_object(builder, "main_window"));
    btn_device  = GTK_WIDGET(gtk_builder_get_object(builder, "btn_device"));
    btn_key1    = GTK_WIDGET(gtk_builder_get_object(builder, "btn_key1"));
    btn_key2    = GTK_WIDGET(gtk_builder_get_object(builder, "btn_key2"));
    btn_start   = GTK_WIDGET(gtk_builder_get_object(builder, "btn_start"));
    combo_title = GTK_WIDGET(gtk_builder_get_object(builder, "combo_title"));
    entry_title = GTK_WIDGET(gtk_builder_get_object(builder, "entry_title"));
    lbl_status  = GTK_WIDGET(gtk_builder_get_object(builder, "lbl_status"));
    chk_gamescope = GTK_WIDGET(gtk_builder_get_object(builder, "chk_gamescope"));
    chk_skip_source   = GTK_WIDGET(gtk_builder_get_object(builder, "chk_skip_source"));
    chk_skip_source_r = GTK_WIDGET(gtk_builder_get_object(builder, "chk_skip_source_r"));
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
