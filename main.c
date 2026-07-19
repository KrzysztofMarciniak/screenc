#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <getopt.h>
#include <png.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef _OPENMP
#include <omp.h>
#endif

#define PROGRAM_NAME "screenc"
#define PROGRAM_VERSION "0.2"

typedef enum {
        ERR_OK = 0,
        ERR_DISPLAY,
        ERR_IMAGE,
        ERR_MEMORY_PNG,
        ERR_MEMORY_FILENAME,
        ERR_MEMORY,
        ERR_SAVE_PNG,
        ERR_INVALID_ARGS,
} Error;

typedef struct {
        int x;
        int y;
        int width;
        int height;
} CropRect;

typedef struct {
        int width;
        int height;
} WidthHeight;

static const char* error_string(Error err) {
        switch (err) {
                case ERR_OK:
                        return "success";
                case ERR_DISPLAY:
                        return "cannot open X display";
                case ERR_IMAGE:
                        return "failed to capture screen image";
                case ERR_MEMORY_PNG:
                        return "out of memory (PNG)";
                case ERR_MEMORY_FILENAME:
                        return "out of memory (filename)";
                case ERR_MEMORY:
                        return "out of memory";
                case ERR_SAVE_PNG:
                        return "failed to write PNG file";
                case ERR_INVALID_ARGS:
                        return "invalid arguments";
        }
        return "unknown error";
}

/* Finds how many bits `mask`'s lowest set bit is shifted by, e.g.
 * 0xFF0000 -> 16. Used to pull R/G/B components out of a packed pixel. */
static int mask_shift(unsigned long mask) {
        int shift = 0;
        while (mask != 0 && (mask & 1) == 0) {
                mask >>= 1;
                shift++;
        }
        return shift;
}

Error extract_data(XImage* img, unsigned char** data_out,
                   const CropRect* crop) {
        int width           = crop ? crop->width : img->width;
        int height          = crop ? crop->height : img->height;
        int start_x         = crop ? crop->x : 0;
        int start_y         = crop ? crop->y : 0;
        int bytes_per_pixel = img->bits_per_pixel / 8;

        if (width <= 0 || height <= 0) {
                fprintf(stderr, "Error: empty capture area (%dx%d)\n", width,
                        height);
                return ERR_MEMORY;
        }

        size_t nbytes       = (size_t)width * (size_t)height * 3;
        unsigned char* data = malloc(nbytes);
        if (!data) {
                fprintf(stderr, "Error: malloc failed for image data\n");
                return ERR_MEMORY;
        }

        int r_shift = mask_shift(img->red_mask);
        int g_shift = mask_shift(img->green_mask);
        int b_shift = mask_shift(img->blue_mask);

        /* Fast path: direct buffer access if 24-bit or 32-bit */
        if ((bytes_per_pixel == 3 || bytes_per_pixel == 4) &&
            img->format == ZPixmap) {
                unsigned char* src_base =
                    (unsigned char*)img->data + start_y * img->bytes_per_line;

#ifdef _OPENMP
#pragma omp parallel for collapse(2)
#endif
                for (int y = 0; y < height; y++) {
                        for (int x = 0; x < width; x++) {
                                unsigned char* src_pixel =
                                    src_base + y * img->bytes_per_line +
                                    (start_x + x) * bytes_per_pixel;
                                unsigned long pixel;

                                if (bytes_per_pixel == 4) {
                                        pixel = *(unsigned int*)src_pixel;
                                } else {
                                        pixel = src_pixel[0] |
                                                (src_pixel[1] << 8) |
                                                (src_pixel[2] << 16);
                                }

                                unsigned char* dst = &data[(y * width + x) * 3];
                                dst[0] = (pixel & img->red_mask) >> r_shift;
                                dst[1] = (pixel & img->green_mask) >> g_shift;
                                dst[2] = (pixel & img->blue_mask) >> b_shift;
                        }
                }
        } else {
                /* Fallback: use XGetPixel */
#ifdef _OPENMP
#pragma omp parallel for collapse(2)
#endif
                for (int y = 0; y < height; y++) {
                        for (int x = 0; x < width; x++) {
                                unsigned long pixel =
                                    XGetPixel(img, start_x + x, start_y + y);
                                unsigned char* p = &data[(y * width + x) * 3];
                                p[0] = (pixel & img->red_mask) >> r_shift;
                                p[1] = (pixel & img->green_mask) >> g_shift;
                                p[2] = (pixel & img->blue_mask) >> b_shift;
                        }
                }
        }

        *data_out = data;
        return ERR_OK;
}

static CropRect normalize_rect(int x0, int y0, int x1, int y1, int screen_width,
                               int screen_height) {
        CropRect rect;
        rect.x      = (x0 < x1) ? x0 : x1;
        rect.y      = (y0 < y1) ? y0 : y1;
        rect.width  = (x0 < x1) ? (x1 - x0) : (x0 - x1);
        rect.height = (y0 < y1) ? (y1 - y0) : (y0 - y1);

        if (rect.x < 0) rect.x = 0;
        if (rect.y < 0) rect.y = 0;
        if (rect.x + rect.width > screen_width)
                rect.width = screen_width - rect.x;
        if (rect.y + rect.height > screen_height)
                rect.height = screen_height - rect.y;

        if (rect.width <= 0 || rect.height <= 0) {
                fprintf(stderr, "Invalid crop area selected\n");
                rect.width  = 1;
                rect.height = 1;
        }
        return rect;
}

CropRect select_crop_area(Display* disp, XImage* img, int screen_width,
                          int screen_height) {
        Window root = RootWindow(disp, DefaultScreen(disp));
        XEvent event;
        int start_x = 0, start_y = 0;
        int end_x = 0, end_y = 0;
        bool selection_active = false;

        /* Create overlay window */
        XSetWindowAttributes attr;
        attr.background_pixel  = BlackPixel(disp, DefaultScreen(disp));
        attr.override_redirect = True;
        Window overlay =
            XCreateWindow(disp, root, 0, 0, screen_width, screen_height, 0,
                          CopyFromParent, InputOutput, CopyFromParent,
                          CWBackPixel | CWOverrideRedirect, &attr);

        XMapWindow(disp, overlay);
        XRaiseWindow(disp, overlay);
        XSelectInput(disp, overlay,
                     PointerMotionMask | ButtonPressMask | ButtonReleaseMask);

        GC gc = XCreateGC(disp, overlay, 0, NULL);

        /* Cache the screenshot as a pixmap for fast redraws */
        Pixmap cached_pixmap =
            XCreatePixmap(disp, overlay, screen_width, screen_height,
                          DefaultDepth(disp, DefaultScreen(disp)));
        XPutImage(disp, cached_pixmap, gc, img, 0, 0, 0, 0, screen_width,
                  screen_height);

        /* Draw cached pixmap to overlay */
        XCopyArea(disp, cached_pixmap, overlay, gc, 0, 0, screen_width,
                  screen_height, 0, 0);

        /* Create GC for red rectangle */
        GC rect_gc = XCreateGC(disp, overlay, 0, NULL);
        XColor red_color, dummy;
        if (XAllocNamedColor(disp, DefaultColormap(disp, DefaultScreen(disp)),
                             "red", &red_color, &dummy)) {
                XSetForeground(disp, rect_gc, red_color.pixel);
        } else {
                /* Fall back if "red" can't be allocated (e.g. exhausted
                 * colormap on an 8-bit visual) */
                XSetForeground(disp, rect_gc,
                               WhitePixel(disp, DefaultScreen(disp)));
        }
        XSetLineAttributes(disp, rect_gc, 2, LineSolid, CapButt, JoinBevel);

        fprintf(stderr, "Click and drag to select crop area...\n");

        while (1) {
                XNextEvent(disp, &event);

                if (event.type == ButtonPress) {
                        if (!selection_active) {
                                start_x          = event.xbutton.x;
                                start_y          = event.xbutton.y;
                                selection_active = true;
                        }
                } else if (event.type == MotionNotify && selection_active) {
                        end_x = event.xmotion.x;
                        end_y = event.xmotion.y;

                        /* Redraw cached pixmap only, not the full image */
                        XCopyArea(disp, cached_pixmap, overlay, gc, 0, 0,
                                  screen_width, screen_height, 0, 0);

                        int x = (start_x < end_x) ? start_x : end_x;
                        int y = (start_y < end_y) ? start_y : end_y;
                        int w = (start_x < end_x) ? (end_x - start_x)
                                                  : (start_x - end_x);
                        int h = (start_y < end_y) ? (end_y - start_y)
                                                  : (start_y - end_y);

                        if (w > 0 && h > 0) {
                                XDrawRectangle(disp, overlay, rect_gc, x, y, w,
                                               h);
                        }
                        XSync(disp, False);
                } else if (event.type == ButtonRelease) {
                        end_x = event.xbutton.x;
                        end_y = event.xbutton.y;
                        break;
                }
        }

        XFreePixmap(disp, cached_pixmap);
        XDestroyWindow(disp, overlay);
        XFreeGC(disp, rect_gc);
        XFreeGC(disp, gc);

        return normalize_rect(start_x, start_y, end_x, end_y, screen_width,
                              screen_height);
}

char* make_filename(void) {
        time_t now   = time(NULL);
        struct tm* t = localtime(&now);

        int year   = t->tm_year + 1900;
        int month  = t->tm_mon + 1;
        int day    = t->tm_mday;
        int hour   = t->tm_hour;
        int minute = t->tm_min;
        int second = t->tm_sec;

        int len =
            snprintf(NULL, 0, "%04d-%02d-%02d_%02d-%02d-%02d.screenshot.png",
                     year, month, day, hour, minute, second);
        if (len < 0) return NULL;

        char* filename = malloc((size_t)len + 1);
        if (!filename) return NULL;

        snprintf(filename, (size_t)len + 1,
                 "%04d-%02d-%02d_%02d-%02d-%02d.screenshot.png", year, month,
                 day, hour, minute, second);
        return filename;
}

static char* dup_string(const char* s) {
        size_t len = strlen(s) + 1;
        char* copy = malloc(len);
        if (copy) memcpy(copy, s, len);
        return copy;
}

char* get_save_path(const char* filename) {
        /* Try current directory first */
        if (access(".", W_OK) == 0) {
                return dup_string(filename);
        }

        /* Fall back to home directory */
        const char* home = getenv("HOME");
        if (home) {
                size_t len = strlen(home) + strlen(filename) + 2;
                char* path = malloc(len);
                if (path) snprintf(path, len, "%s/%s", home, filename);
                return path;
        }

        /* Last resort: just use the filename */
        return dup_string(filename);
}

Error save_png(const char* filepath, const unsigned char* data,
               WidthHeight wh) {
        FILE* fp = fopen(filepath, "wb");
        if (!fp) {
                perror("fopen");
                return ERR_SAVE_PNG;
        }

        png_structp png_ptr =
            png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
        png_infop info_ptr = png_ptr ? png_create_info_struct(png_ptr) : NULL;
        if (!png_ptr || !info_ptr) {
                fprintf(stderr, "PNG init failed\n");
                if (png_ptr) png_destroy_write_struct(&png_ptr, NULL);
                fclose(fp);
                return ERR_MEMORY_PNG;
        }

        if (setjmp(png_jmpbuf(png_ptr))) {
                fprintf(stderr, "PNG write error\n");
                png_destroy_write_struct(&png_ptr, &info_ptr);
                fclose(fp);
                return ERR_SAVE_PNG;
        }

        png_init_io(png_ptr, fp);
        /* Use faster compression level (6 instead of default 9) */
        png_set_compression_level(png_ptr, 6);
        png_set_IHDR(png_ptr, info_ptr, wh.width, wh.height, 8,
                     PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                     PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

        png_bytep* row_pointers = malloc(sizeof(png_bytep) * (size_t)wh.height);
        if (!row_pointers) {
                fprintf(stderr, "Memory error\n");
                png_destroy_write_struct(&png_ptr, &info_ptr);
                fclose(fp);
                return ERR_MEMORY_PNG;
        }

        for (int y = 0; y < wh.height; y++) {
                /* libpng's row_pointers isn't const-qualified even though it
                 * only reads the rows on write; the cast is safe. */
                row_pointers[y] = (png_bytep)(data + (size_t)y * wh.width * 3);
        }

        png_set_rows(png_ptr, info_ptr, row_pointers);
        png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, NULL);

        free(row_pointers);
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        return ERR_OK;
}

static void print_usage(const char* prog) {
        fprintf(stderr,
                "Usage: %s [OPTION]\n"
                "Take a PNG screenshot.\n"
                "\n"
                "  -f, --full          capture the full screen (default: "
                "interactive crop)\n"
                "  -o, --output PATH   save to PATH instead of an "
                "auto-generated name\n"
                "  -h, --help          show this help and exit\n"
                "  -v, --version       show version and exit\n",
                prog);
}

int main(int argc, char* argv[]) {
        bool crop_mode          = true;
        const char* output_path = NULL;

        static const struct option long_opts[] = {
            {"full", no_argument, NULL, 'f'},
            {"output", required_argument, NULL, 'o'},
            {"help", no_argument, NULL, 'h'},
            {"version", no_argument, NULL, 'v'},
            {NULL, 0, NULL, 0},
        };

        int opt;
        while ((opt = getopt_long(argc, argv, "fo:hv", long_opts, NULL)) !=
               -1) {
                switch (opt) {
                        case 'f':
                                crop_mode = false;
                                break;
                        case 'o':
                                output_path = optarg;
                                break;
                        case 'h':
                                print_usage(argv[0]);
                                return ERR_OK;
                        case 'v':
                                printf("%s %s\n", PROGRAM_NAME,
                                       PROGRAM_VERSION);
                                return ERR_OK;
                        default:
                                print_usage(argv[0]);
                                return ERR_INVALID_ARGS;
                }
        }
        if (optind < argc) {
                fprintf(stderr, "%s: unexpected argument '%s'\n", argv[0],
                        argv[optind]);
                print_usage(argv[0]);
                return ERR_INVALID_ARGS;
        }

        /* Open display */
        Display* disp = XOpenDisplay(NULL);
        if (!disp) {
                fprintf(stderr, "%s: %s\n", argv[0], error_string(ERR_DISPLAY));
                return ERR_DISPLAY;
        }

        /* Get screenshot */
        Window root = RootWindow(disp, DefaultScreen(disp));
        XWindowAttributes gwa;
        XGetWindowAttributes(disp, root, &gwa);

        XImage* img = XGetImage(disp, root, 0, 0, gwa.width, gwa.height,
                                AllPlanes, ZPixmap);
        if (!img) {
                fprintf(stderr, "%s: %s\n", argv[0], error_string(ERR_IMAGE));
                XCloseDisplay(disp);
                return ERR_IMAGE;
        }

        WidthHeight wh = {gwa.width, gwa.height};
        CropRect crop  = {0, 0, gwa.width, gwa.height};

        /* Get crop area if in crop mode */
        if (crop_mode) {
                crop      = select_crop_area(disp, img, gwa.width, gwa.height);
                wh.width  = crop.width;
                wh.height = crop.height;
        }

        /* Extract pixel data for selected area */
        unsigned char* data = NULL;
        Error e = extract_data(img, &data, crop_mode ? &crop : NULL);

        /* Pixel data now lives in `data`; the X connection isn't needed for
         * the rest of the program (filename/path/PNG work is all local). */
        XDestroyImage(img);
        XCloseDisplay(disp);

        if (e != ERR_OK) {
                fprintf(stderr, "%s: %s\n", argv[0], error_string(e));
                return e;
        }

        /* Generate filename (unless an explicit --output path was given) */
        char* generated_name = output_path ? NULL : make_filename();
        if (!output_path && !generated_name) {
                fprintf(stderr, "%s: %s\n", argv[0],
                        error_string(ERR_MEMORY_FILENAME));
                free(data);
                return ERR_MEMORY_FILENAME;
        }

        char* full_path = output_path ? dup_string(output_path)
                                      : get_save_path(generated_name);
        free(generated_name);
        if (!full_path) {
                fprintf(stderr, "%s: %s\n", argv[0], error_string(ERR_MEMORY));
                free(data);
                return ERR_MEMORY;
        }

        e = save_png(full_path, data, wh);
        if (e != ERR_OK) {
                fprintf(stderr, "%s: %s: %s\n", argv[0], error_string(e),
                        full_path);
        } else {
                printf("Saved screenshot as '%s'\n", full_path);
        }

        free(full_path);
        free(data);
        return e;
}
