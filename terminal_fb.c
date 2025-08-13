#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdint.h>
#include <stdarg.h>
#include <linux/input.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <poll.h>
#include <dirent.h>

// --- Global Variables (Framebuffer) ---
static int fb_fd = -1;
static char *fbp = NULL;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;
static long int screen_size = 0;
static int bytes_per_pixel = 0;

// --- Screen Dimensions ---
static int screen_width = 0;
static int screen_height = 0;
static int terminal_height = 0;
static int keyboard_y_start = 0;
static int keyboard_height = 0;

// --- Font Definition ---
#define FONT_WIDTH 8
#define FONT_HEIGHT 8

// ... [font_8x8 array remains unchanged] ...

// --- Terminal Buffer ---
static int TERMINAL_MAX_LINES;
static int TERMINAL_MAX_COLS;
static char *terminal_buffer_ptr;
static int current_line = 0;
static int current_col = 0;

// --- Shell and Input Related Global Variables ---
static pid_t shell_pid = -1;
static int pty_master_fd = -1;
static int input_fd = -1;
static volatile sig_atomic_t keep_running = 1;
static volatile sig_atomic_t needs_redraw = 1;

// Touch scaling variables
static int abs_x_min = 0, abs_x_max = 0, abs_y_min = 0, abs_y_max = 0;

// --- KMSG Logging ---
static int kmsg_fd = -1;
static void print_to_kmsg(const char *format, ...) {
    if (kmsg_fd == -1) {
        kmsg_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
        if (kmsg_fd == -1) return;
    }
    char buf[256];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buf, sizeof(buf) - 1, format, args);
    va_end(args);
    if (len > 0) {
        write(kmsg_fd, buf, len);
    }
}

// --- Helper for Framebuffer color packing ---
static inline uint32_t pack_rgb(uint8_t R, uint8_t G, uint8_t B, uint8_t A) {
    if (vinfo.bits_per_pixel == 32) {
        uint32_t p = 0;
        p |= ((uint32_t)(R >> (8 - vinfo.red.length)))   << vinfo.red.offset;
        p |= ((uint32_t)(G >> (8 - vinfo.green.length))) << vinfo.green.offset;
        p |= ((uint32_t)(B >> (8 - vinfo.blue.length)))  << vinfo.blue.offset;
        if (vinfo.transp.length)
            p |= ((uint32_t)(A >> (8 - vinfo.transp.length))) << vinfo.transp.offset;
        return p;
    } else if (vinfo.bits_per_pixel == 16) {
        uint16_t r16 = R >> 3;
        uint16_t g16 = G >> 2;
        uint16_t b16 = B >> 3;
        return (uint32_t)((r16 << 11) | (g16 << 5) | b16);
    }
    return 0;
}

// --- Colors ---
#define COLOR_BLACK          pack_rgb(0, 0, 0, 0xFF)
#define COLOR_WHITE          pack_rgb(255, 255, 255, 0xFF)
#define COLOR_GREEN          pack_rgb(0, 255, 0, 0xFF)
#define COLOR_BLUE           pack_rgb(0, 0, 255, 0xFF)
#define COLOR_GRAY           pack_rgb(100, 100, 100, 0xFF)
#define COLOR_DARK_GRAY      pack_rgb(50, 50, 50, 0xFF)
#define COLOR_TERMINAL_BG    pack_rgb(15, 15, 15, 0xFF)
#define COLOR_TERMINAL_FG    COLOR_GREEN
#define COLOR_KEY_BG         pack_rgb(60, 60, 60, 0xFF)
#define COLOR_KEY_TEXT       pack_rgb(200, 200, 200, 0xFF)
#define COLOR_ERROR_TEXT     pack_rgb(255, 0, 0, 0xFF)

// --- Function Prototypes ---
// ... [function prototypes remain unchanged] ...

// --- Framebuffer Initialization ---
static int init_framebuffer() {
    fb_fd = open("/dev/graphics/fb0", O_RDWR);
    if (fb_fd == -1) {
        print_to_kmsg("fb_term: Error: cannot open framebuffer: %s\n", strerror(errno));
        return -1;
    }

    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) == -1) {
        print_to_kmsg("fb_term: Error reading fixed info: %s\n", strerror(errno));
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == -1) {
        print_to_kmsg("fb_term: Error reading variable info: %s\n", strerror(errno));
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    screen_width = vinfo.xres;
    screen_height = vinfo.yres;
    bytes_per_pixel = vinfo.bits_per_pixel / 8;
    screen_size = finfo.smem_len;

    // Calculate screen size properly
    if (screen_size <= 0) {
        screen_size = finfo.line_length * vinfo.yres_virtual;
    }

    print_to_kmsg("fb_term: FB: %s, Res: %dx%d, %d bpp, Size: %ld\n", 
                  finfo.id, vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, screen_size);

    fbp = mmap(0, screen_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fbp == MAP_FAILED) {
        print_to_kmsg("fb_term: mmap failed: %s\n", strerror(errno));
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }

    // Calculate dimensions
    terminal_height = screen_height * 2 / 3;
    keyboard_y_start = terminal_height;
    keyboard_height = screen_height - keyboard_y_start;

    // Calculate terminal buffer dimensions
    TERMINAL_MAX_LINES = terminal_height / FONT_HEIGHT;
    TERMINAL_MAX_COLS = screen_width / FONT_WIDTH;
    
    // Reasonable caps
    if (TERMINAL_MAX_COLS > 200) TERMINAL_MAX_COLS = 200;
    if (TERMINAL_MAX_LINES > 50) TERMINAL_MAX_LINES = 50;

    terminal_buffer_ptr = malloc(TERMINAL_MAX_LINES * (TERMINAL_MAX_COLS + 1));
    if (!terminal_buffer_ptr) {
        print_to_kmsg("fb_term: Failed to allocate terminal buffer\n");
        munmap(fbp, screen_size);
        close(fb_fd);
        fb_fd = -1;
        return -1;
    }
    
    for (int i = 0; i < TERMINAL_MAX_LINES; ++i) {
        memset(get_terminal_line(i), 0, TERMINAL_MAX_COLS + 1);
    }

    print_to_kmsg("fb_term: Terminal: %dx%d chars\n", TERMINAL_MAX_COLS, TERMINAL_MAX_LINES);
    return 0;
}

// --- Framebuffer De-initialization ---
static void deinit_framebuffer() {
    if (fbp && fbp != MAP_FAILED) {
        munmap(fbp, screen_size);
        fbp = NULL;
    }
    if (fb_fd != -1) {
        close(fb_fd);
        fb_fd = -1;
    }
    if (terminal_buffer_ptr) {
        free(terminal_buffer_ptr);
        terminal_buffer_ptr = NULL;
    }
    if (kmsg_fd != -1) {
        close(kmsg_fd);
        kmsg_fd = -1;
    }
}

// --- Drawing Functions ---
// ... [draw_pixel, draw_rectangle, clear_screen remain unchanged] ...

static void draw_char(int x, int y, char c, uint32_t fg_color, uint32_t bg_color) {
    if (c < 0 || c >= 128) c = ' ';
    const unsigned char *bitmap = font_8x8[(unsigned char)c];

    for (int row = 0; row < FONT_HEIGHT; ++row) {
        for (int col = 0; col < FONT_WIDTH; ++col) {
            int px = x + col;
            int py = y + row;
            if (px >= 0 && px < screen_width && py >= 0 && py < screen_height) {
                uint32_t color = (bitmap[row] & (1 << (7 - col))) ? fg_color : bg_color;
                draw_pixel(px, py, color);
            }
        }
    }
}

static void draw_text(int x, int y, const char *text, uint32_t fg_color, uint32_t bg_color) {
    int current_x = x;
    for (int i = 0; text[i] != '\0'; ++i) {
        draw_char(current_x, y, text[i], fg_color, bg_color);
        current_x += FONT_WIDTH;
    }
}

// --- Terminal Logic ---
static char* get_terminal_line(int line_idx) {
    return terminal_buffer_ptr + (line_idx * (TERMINAL_MAX_COLS + 1));
}

static void terminal_scroll_up() {
    for (int i = 0; i < TERMINAL_MAX_LINES - 1; ++i) {
        strcpy(get_terminal_line(i), get_terminal_line(i+1));
    }
    memset(get_terminal_line(TERMINAL_MAX_LINES - 1), 0, TERMINAL_MAX_COLS + 1);
    needs_redraw = 1;
}

static void terminal_putc(char c) {
    if (c == '\n' || c == '\r') {
        if (current_col > 0) {
            get_terminal_line(current_line)[current_col] = '\0';
        }
        current_line++;
        current_col = 0;
        if (current_line >= TERMINAL_MAX_LINES) {
            terminal_scroll_up();
            current_line = TERMINAL_MAX_LINES - 1;
        }
    } else if (c == '\b') {
        if (current_col > 0) {
            current_col--;
            get_terminal_line(current_line)[current_col] = ' ';
        } else if (current_line > 0) {
            current_line--;
            current_col = strlen(get_terminal_line(current_line));
        }
    } else if (c >= 32 && c <= 126) {
        if (current_col < TERMINAL_MAX_COLS) {
            get_terminal_line(current_line)[current_col] = c;
            current_col++;
        }
        if (current_col >= TERMINAL_MAX_COLS) {
            terminal_putc('\n');
        }
    }
    needs_redraw = 1;
}

static void terminal_puts(const char *s) {
    for (int i = 0; s[i] != '\0'; ++i) {
        terminal_putc(s[i]);
    }
}

static void draw_terminal_area() {
    draw_rectangle(0, 0, screen_width, terminal_height, COLOR_TERMINAL_BG);

    int max_lines_on_screen = terminal_height / FONT_HEIGHT;
    int start_line_idx = (current_line > max_lines_on_screen - 1) ? 
                         (current_line - (max_lines_on_screen - 1)) : 0;

    for (int i = 0; i < max_lines_on_screen; ++i) {
        if (start_line_idx + i < TERMINAL_MAX_LINES) {
            draw_text(0, i * FONT_HEIGHT, get_terminal_line(start_line_idx + i),
                      COLOR_TERMINAL_FG, COLOR_TERMINAL_BG);
        }
    }

    // Draw cursor
    if (current_line >= start_line_idx && current_line < start_line_idx + max_lines_on_screen) {
        int cursor_y = (current_line - start_line_idx) * FONT_HEIGHT;
        int cursor_x = current_col * FONT_WIDTH;
        if (cursor_x < screen_width) {
            for (int y = cursor_y; y < cursor_y + FONT_HEIGHT; y++) {
                draw_pixel(cursor_x, y, COLOR_WHITE);
            }
        }
    }
}

// --- Keyboard Layout ---
typedef struct {
    float x, y, w, h;
    const char *label;
    char value;
    const char *pty_sequence;
} KeyboardKey;

#define KEY_BASE_WIDTH_RATIO_W 0.08
#define KEY_BASE_HEIGHT_RATIO_H 0.15
#define KEY_HORIZONTAL_SPACING_RATIO_W 0.01
#define KEY_VERTICAL_SPACING_RATIO_H 0.01

// Fixed keyboard layout with non-overlapping keys
static KeyboardKey keyboard_layout[] = {
    // Row 1
    {0.0f, 0.0f, 1.0f, 1.0f, "Up", 0, "\x1b[A"},
    {1.1f, 0.0f, 1.0f, 1.0f, "Down", 0, "\x1b[B"},
    {2.2f, 0.0f, 1.0f, 1.0f, "Left", 0, "\x1b[D"},
    {3.3f, 0.0f, 1.0f, 1.0f, "Right", 0, "\x1b[C"},
    {4.4f, 0.0f, 1.5f, 1.0f, "Ctrl", 0, "\x01"},
    {6.0f, 0.0f, 1.5f, 1.0f, "Exit", 0, NULL},

    // Row 2
    {0.0f, 1.1f, 1.0f, 1.0f, "1", '1', NULL}, 
    {1.1f, 1.1f, 1.0f, 1.0f, "2", '2', NULL},
    {2.2f, 1.1f, 1.0f, 1.0f, "3", '3', NULL},
    {3.3f, 1.1f, 1.0f, 1.0f, "4", '4', NULL},
    {4.4f, 1.1f, 1.0f, 1.0f, "5", '5', NULL},
    {5.5f, 1.1f, 1.0f, 1.0f, "6", '6', NULL},
    {6.6f, 1.1f, 1.0f, 1.0f, "7", '7', NULL},
    {7.7f, 1.1f, 1.0f, 1.0f, "8", '8', NULL},
    {8.8f, 1.1f, 1.0f, 1.0f, "9", '9', NULL},
    {9.9f, 1.1f, 1.0f, 1.0f, "0", '0', NULL},

    // Row 3
    {0.5f, 2.2f, 1.0f, 1.0f, "Q", 'q', NULL},
    {1.6f, 2.2f, 1.0f, 1.0f, "W", 'w', NULL},
    {2.7f, 2.2f, 1.0f, 1.0f, "E", 'e', NULL},
    {3.8f, 2.2f, 1.0f, 1.0f, "R", 'r', NULL},
    {4.9f, 2.2f, 1.0f, 1.0f, "T", 't', NULL},
    {6.0f, 2.2f, 1.0f, 1.0f, "Y", 'y', NULL},
    {7.1f, 2.2f, 1.0f, 1.0f, "U", 'u', NULL},
    {8.2f, 2.2f, 1.0f, 1.0f, "I", 'i', NULL},
    {9.3f, 2.2f, 1.0f, 1.0f, "O", 'o', NULL},
    {10.4f, 2.2f, 1.0f, 1.0f, "P", 'p', NULL},

    // Row 4
    {1.0f, 3.3f, 1.0f, 1.0f, "A", 'a', NULL},
    {2.1f, 3.3f, 1.0f, 1.0f, "S", 's', NULL},
    {3.2f, 3.3f, 1.0f, 1.0f, "D", 'd', NULL},
    {4.3f, 3.3f, 1.0f, 1.0f, "F", 'f', NULL},
    {5.4f, 3.3f, 1.0f, 1.0f, "G", 'g', NULL},
    {6.5f, 3.3f, 1.0f, 1.0f, "H", 'h', NULL},
    {7.6f, 3.3f, 1.0f, 1.0f, "J", 'j', NULL},
    {8.7f, 3.3f, 1.0f, 1.0f, "K", 'k', NULL},
    {9.8f, 3.3f, 1.0f, 1.0f, "L", 'l', NULL},

    // Row 5
    {1.5f, 4.4f, 1.0f, 1.0f, "Z", 'z', NULL},
    {2.6f, 4.4f, 1.0f, 1.0f, "X", 'x', NULL},
    {3.7f, 4.4f, 1.0f, 1.0f, "C", 'c', NULL},
    {4.8f, 4.4f, 1.0f, 1.0f, "V", 'v', NULL},
    {5.9f, 4.4f, 1.0f, 1.0f, "B", 'b', NULL},
    {7.0f, 4.4f, 1.0f, 1.0f, "N", 'n', NULL},
    {8.1f, 4.4f, 1.0f, 1.0f, "M", 'm', NULL},

    // Row 6
    {0.0f, 5.5f, 1.5f, 1.0f, "Shift", 0, NULL},
    {1.6f, 5.5f, 4.0f, 1.0f, "Space", ' ', NULL},
    {5.7f, 5.5f, 1.5f, 1.0f, "Bks", '\b', NULL},
    {7.3f, 5.5f, 2.0f, 1.0f, "Ent", '\n', NULL}
};
#define NUM_KEYBOARD_KEYS (sizeof(keyboard_layout) / sizeof(KeyboardKey)

static void draw_keyboard_area() {
    draw_rectangle(0, keyboard_y_start, screen_width, keyboard_height, COLOR_DARK_GRAY);

    float key_width_unit = screen_width * KEY_BASE_WIDTH_RATIO_W;
    float key_height_unit = keyboard_height * KEY_BASE_HEIGHT_RATIO_H;
    float h_spacing_unit = screen_width * KEY_HORIZONTAL_SPACING_RATIO_W;
    float v_spacing_unit = keyboard_height * KEY_VERTICAL_SPACING_RATIO_H;

    for (int i = 0; i < NUM_KEYBOARD_KEYS; ++i) {
        KeyboardKey *key = &keyboard_layout[i];
        int key_x = (int)(key->x * (key_width_unit + h_spacing_unit));
        int key_y = keyboard_y_start + (int)(key->y * (key_height_unit + v_spacing_unit));
        int key_w = (int)(key->w * key_width_unit);
        int key_h = (int)(key->h * key_height_unit);

        // Ensure keys stay within bounds
        if (key_x + key_w > screen_width) key_w = screen_width - key_x;
        if (key_y + key_h > screen_height) key_h = screen_height - key_y;
        if (key_w <= 0 || key_h <= 0) continue;

        draw_rectangle(key_x, key_y, key_w, key_h, COLOR_KEY_BG);
        draw_rectangle(key_x + 2, key_y + 2, key_w - 4, key_h - 4, COLOR_BLACK);

        // Center text in key
        int text_x = key_x + (key_w - (int)(strlen(key->label) * FONT_WIDTH)) / 2;
        int text_y = key_y + (key_h - FONT_HEIGHT) / 2;
        if (text_x < 0) text_x = 0;
        if (text_y < keyboard_y_start) text_y = keyboard_y_start;
        
        draw_text(text_x, text_y, key->label, COLOR_KEY_TEXT, COLOR_BLACK);
    }
}

// --- Input Handling ---
static int query_abs_range(int fd, int code, int *minv, int *maxv) {
    struct input_absinfo ai;
    if (ioctl(fd, EVIOCGABS(code), &ai) == -1) return -1;
    *minv = ai.minimum;
    *maxv = ai.maximum;
    return 0;
}

static inline int scale(int v, int minv, int maxv, int out_max) {
    if (maxv == minv) return 0;
    return (int)((long)(v - minv) * out_max / (maxv - minv));
}

static int init_first_touch_device() {
    DIR *d = opendir("/dev/input");
    if (!d) {
        print_to_kmsg("fb_term: Cannot open /dev/input: %s\n", strerror(errno));
        return -1;
    }
    
    struct dirent *de;
    char path[128];
    int found = -1;

    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "event", 5) == 0) {
            snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;

            unsigned long evbits[(EV_MAX+7)/8] = {0};
            if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) == -1) {
                close(fd);
                continue;
            }

            if ((evbits[EV_KEY/8] & (1 << (EV_KEY%8))) &&
                (evbits[EV_ABS/8] & (1 << (EV_ABS%8)))) {
                
                // Try multi-touch axes first
                if (query_abs_range(fd, ABS_MT_POSITION_X, &abs_x_min, &abs_x_max) == 0 &&
                    query_abs_range(fd, ABS_MT_POSITION_Y, &abs_y_min, &abs_y_max) == 0) {
                    found = fd;
                    print_to_kmsg("fb_term: Using multi-touch device: %s\n", path);
                    break;
                }
                // Try single-touch axes
                else if (query_abs_range(fd, ABS_X, &abs_x_min, &abs_x_max) == 0 &&
                         query_abs_range(fd, ABS_Y, &abs_y_min, &abs_y_max) == 0) {
                    found = fd;
                    print_to_kmsg("fb_term: Using single-touch device: %s\n", path);
                    break;
                }
            }
            close(fd);
        }
    }
    closedir(d);

    if (found > 0) {
        input_fd = found;
        print_to_kmsg("fb_term: Touch ranges: X[%d-%d] Y[%d-%d]\n", 
                      abs_x_min, abs_x_max, abs_y_min, abs_y_max);
        return 0;
    }
    
    print_to_kmsg("fb_term: No touch device found\n");
    return -1;
}

static int find_key_at_coords(int x, int y) {
    if (y < keyboard_y_start) return -1;
    y -= keyboard_y_start;

    float kw = screen_width * KEY_BASE_WIDTH_RATIO_W;
    float kh = keyboard_height * KEY_BASE_HEIGHT_RATIO_H;
    float hs = screen_width * KEY_HORIZONTAL_SPACING_RATIO_W;
    float vs = keyboard_height * KEY_VERTICAL_SPACING_RATIO_H;

    for (int i = 0; i < NUM_KEYBOARD_KEYS; ++i) {
        KeyboardKey *k = &keyboard_layout[i];
        int kx = (int)(k->x * (kw + hs));
        int ky = (int)(k->y * (kh + vs));
        int kwd = (int)(k->w * kw);
        int kht = (int)(k->h * kh);

        if (x >= kx && x < kx + kwd &&
            y >= ky && y < ky + kht) {
            return i;
        }
    }
    return -1;
}

static void handle_input_event() {
    struct input_event ev;
    static int raw_x = -1, raw_y = -1;
    static int touching = 0;

    ssize_t rd;
    while ((rd = read(input_fd, &ev, sizeof(ev)) == sizeof(ev)) {
        switch (ev.type) {
            case EV_ABS:
                if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) raw_x = ev.value;
                else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) raw_y = ev.value;
                break;
            case EV_KEY:
                if (ev.code == BTN_TOUCH) touching = ev.value;
                break;
            case EV_SYN:
                if (ev.code == SYN_REPORT) {
                    if (touching && raw_x != -1 && raw_y != -1) {
                        int px = scale(raw_x, abs_x_min, abs_x_max, screen_width);
                        int py = scale(raw_y, abs_y_min, abs_y_max, screen_height);
                        int key_idx = find_key_at_coords(px, py);
                        if (key_idx != -1) {
                            KeyboardKey *k = &keyboard_layout[key_idx];
                            if (k->value) {
                                send_to_shell(k->value);
                            } else if (k->pty_sequence) {
                                send_string_to_shell(k->pty_sequence);
                            } else if (strcmp(k->label, "Exit") == 0) {
                                keep_running = 0;
                            }
                            needs_redraw = 1;
                        }
                        raw_x = raw_y = -1;
                    } else if (!touching) {
                        raw_x = raw_y = -1;
                    }
                }
                break;
        }
    }
    
    if (rd == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        print_to_kmsg("fb_term: Input read error: %s\n", strerror(errno));
        keep_running = 0;
    }
}

// --- Shell Interaction (PTY) ---
static int start_shell() {
    int mfd = posix_openpt(O_RDWR | O_NOCTTY);
    if (mfd < 0) {
        print_to_kmsg("fb_term: posix_openpt failed: %s\n", strerror(errno));
        return -1;
    }
    
    if (grantpt(mfd) == -1 || unlockpt(mfd) == -1) {
        print_to_kmsg("fb_term: grantpt/unlockpt failed: %s\n", strerror(errno));
        close(mfd);
        return -1;
    }
    
    char *slavename = ptsname(mfd);
    if (!slavename) {
        print_to_kmsg("fb_term: ptsname failed: %s\n", strerror(errno));
        close(mfd);
        return -1;
    }

    pty_master_fd = mfd;
    fcntl(pty_master_fd, F_SETFL, O_NONBLOCK);

    shell_pid = fork();
    if (shell_pid < 0) {
        print_to_kmsg("fb_term: fork failed: %s\n", strerror(errno));
        close(mfd);
        return -1;
    }
    
    if (shell_pid == 0) {
        close(mfd);
        setsid();
        
        int sfd = open(slavename, O_RDWR | O_NOCTTY);
        if (sfd < 0) {
            _exit(1);
        }
        
        dup2(sfd, STDIN_FILENO);
        dup2(sfd, STDOUT_FILENO);
        dup2(sfd, STDERR_FILENO);
        if (sfd > STDERR_FILENO) close(sfd);
        
        char *const argv[] = {"/system/bin/sh", NULL};
        char *const envp[] = {
            "TERM=linux",
            "PATH=/system/bin:/sbin:/bin:/usr/bin:/vendor/bin",
            "HOME=/root",
            NULL
        };
        execve("/system/bin/sh", argv, envp);
        _exit(127);
    }
    
    print_to_kmsg("fb_term: Shell started pid=%d\n", shell_pid);
    terminal_puts("Welcome!\n$ ");
    needs_redraw = 1;
    return 0;
}

static void send_to_shell(char c) {
    if (pty_master_fd != -1 && write(pty_master_fd, &c, 1) == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            keep_running = 0;
        }
    }
}

static void send_string_to_shell(const char *s) {
    if (pty_master_fd != -1 && write(pty_master_fd, s, strlen(s)) == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            keep_running = 0;
        }
    }
}

static void read_from_shell() {
    if (pty_master_fd == -1) return;

    char buf[256];
    ssize_t n_read;
    int data_read = 0;

    while ((n_read = read(pty_master_fd, buf, sizeof(buf) - 1)) > 0) {
        data_read = 1;
        for (ssize_t i = 0; i < n_read; ++i) {
            terminal_putc(buf[i]);
        }
    }

    if (n_read == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        keep_running = 0;
    } else if (n_read == 0) {
        keep_running = 0;
    }

    if (data_read) needs_redraw = 1;
}

// --- Cleanup and Signal Handling ---
static void cleanup_and_exit() {
    print_to_kmsg("fb_term: Cleaning up\n");
    
    if (shell_pid > 0) {
        kill(shell_pid, SIGTERM);
        int status;
        waitpid(shell_pid, &status, 0);
    }
    
    if (pty_master_fd != -1) close(pty_master_fd);
    if (input_fd != -1) close(input_fd);
    
    deinit_framebuffer();
}

static void sigint_handler(int signum) {
    keep_running = 0;
}

// --- Main Function ---
int main(int argc, char *argv[]) {
    signal(SIGINT, sigint_handler);

    if (init_framebuffer() == -1) {
        fprintf(stderr, "Framebuffer init failed\n");
        return 1;
    }

    if (init_first_touch_device() == -1) {
        clear_screen(COLOR_BLACK);
        draw_text(50, screen_height/2 - 20, "ERROR: No Touchscreen!", COLOR_ERROR_TEXT, COLOR_BLACK);
        draw_text(20, screen_height/2 + 20, "Check /dev/input/event* permissions", COLOR_ERROR_TEXT, COLOR_BLACK);
        draw_keyboard_area();
        needs_redraw = 0;
    } else if (start_shell() == -1) {
        clear_screen(COLOR_BLACK);
        draw_text(50, screen_height/2 - 20, "ERROR: Shell Failed!", COLOR_ERROR_TEXT, COLOR_BLACK);
        draw_text(20, screen_height/2 + 20, "Check /system/bin/sh exists", COLOR_ERROR_TEXT, COLOR_BLACK);
        draw_keyboard_area();
        needs_redraw = 0;
    }

    while (keep_running) {
        if (input_fd != -1) {
            handle_input_event();
        }
        
        if (pty_master_fd != -1) {
            read_from_shell();
        }

        if (needs_redraw) {
            clear_screen(COLOR_BLACK);
            draw_terminal_area();
            draw_keyboard_area();
            needs_redraw = 0;
        }

        // Check if shell exited
        if (shell_pid != -1) {
            int status;
            if (waitpid(shell_pid, &status, WNOHANG) == shell_pid) {
                keep_running = 0;
            }
        }

        usleep(10000);
    }

    cleanup_and_exit();
    return 0;
}
