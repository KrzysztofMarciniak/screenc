#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef enum {
        ERR_OK = 0,
        ERR_DISPLAY,
        ERR_IMAGE,
        ERR_MEMORY_PNG,
        ERR_MEMORY_FILENAME,
        ERR_MEMORY,
        ERR_SAVE_PNG,
} Error;

typedef char Filename;

typedef struct {
        Filename* filename;
        Error error;
} FilenameWithError;

typedef struct {
        Display* disp;
        XImage* img;
        unsigned char* data;
        char* filename;
} CleanupVariables;

typedef struct {
    int width;
    int height;
} WidthHeight;

void cleanup(CleanupVariables* cv) {
        if (!cv) return;
        if (cv->data) free(cv->data);
        if (cv->filename) free(cv->filename);
        if (cv->img) XDestroyImage(cv->img);
        if (cv->disp) XCloseDisplay(cv->disp);
}

typedef struct{
    WidthHeight wh;
    Display* dp;
    XImage* img;
    Error error;
} ScreenshotVariables;

ScreenshotVariables grab_screenshot(void) {
    ScreenshotVariables sv = {0};

    sv.disp = XOpenDisplay(NULL);
    if (!sv.disp) {
        fprintf(stderr, "Error: Cannot open X display\n");
        sv.error = ERR_DISPLAY;
        return sv;
    }

    Window root = RootWindow(sv.disp, DefaultScreen(sv.disp));
    XWindowAttributes gwa;
    XGetWindowAttributes(sv.disp, root, &gwa);

    sv.img = XGetImage(sv.disp, root, 0, 0, gwa.width, gwa.height,
                       AllPlanes, ZPixmap);
    if (!sv.img) {
        fprintf(stderr, "Error: XGetImage failed\n");
        XCloseDisplay(sv.disp);
        sv.disp = NULL;
        sv.error = ERR_IMAGE;
        return sv;
    }

    sv.wh.width  = gwa.width;
    sv.wh.height = gwa.height;
    sv.error = ERR_OK;
    return sv;
}

Error extract_data(XImage* img, unsigned char** data_out) {
        int width  = img->width;
        int height = img->height;

        unsigned char* data = malloc(width * height * 3);
        if (!data) {
                fprintf(stderr, "Error: malloc failed for image data\n");
                return ERR_MEMORY;
        }

        int r_shift = 0, g_shift = 0, b_shift = 0;
        unsigned long mask;

        mask = img->red_mask;
        while ((mask & 1) == 0) {
                mask >>= 1;
                r_shift++;
        }
        mask = img->green_mask;
        while ((mask & 1) == 0) {
                mask >>= 1;
                g_shift++;
        }
        mask = img->blue_mask;
        while ((mask & 1) == 0) {
                mask >>= 1;
                b_shift++;
        }

        for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                        unsigned long pixel = XGetPixel(img, x, y);
                        unsigned char* p    = &data[(y * width + x) * 3];
                        p[0] = (pixel & img->red_mask) >> r_shift;
                        p[1] = (pixel & img->green_mask) >> g_shift;
                        p[2] = (pixel & img->blue_mask) >> b_shift;
                }
        }

        *data_out = data;
        return ERR_OK;
}

FilenameWithError make_filename(void) {
        FilenameWithError result = {.filename = NULL, .error = ERR_OK};

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

        result.filename = malloc(len + 1);
        if (!result.filename) {
                result.error = ERR_MEMORY_FILENAME;
                return result;
        }

        snprintf(result.filename, len + 1,
                 "%04d-%02d-%02d_%02d-%02d-%02d.screenshot.png", year, month,
                 day, hour, minute, second);

        return result;
}

Error save_png(const Filename* filename, unsigned char* data, WidthHeight wh) {
        FILE* fp = fopen(filename, "wb");
        if (!fp) {
                perror("fopen");
                return ERR_SAVE_PNG;
        }

        png_structp png_ptr =
            png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
        png_infop info_ptr = png_create_info_struct(png_ptr);
        if (!png_ptr || !info_ptr) {
                fprintf(stderr, "PNG init failed\n");
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
        png_set_IHDR(png_ptr, info_ptr, wh.width, wh.height, 8, PNG_COLOR_TYPE_RGB,
                     PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE,
                     PNG_FILTER_TYPE_BASE);

        png_bytep* row_pointers = malloc(sizeof(png_bytep) * wh.height);
        if (!row_pointers) {
                fprintf(stderr, "Memory error\n");
                png_destroy_write_struct(&png_ptr, &info_ptr);
                fclose(fp);
                return ERR_MEMORY_PNG;
        }

        for (int y = 0; y < wh.height; y++) row_pointers[y] = data + y * wh.width * 3;

        png_set_rows(png_ptr, info_ptr, row_pointers);
        png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, NULL);

        free(row_pointers);
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        return ERR_OK;
}

int main(void) {
    CleanupVariables cv = {0};
    ScreenshotVariables sv = grab_screenshot();

    if (sv.error != ERR_OK) {
        cleanup(&cv);
        return sv.error;
    }

    cv.disp = sv.disp;
    cv.img  = sv.img;
    WidthHeight wh = sv.wh;

    Error e = extract_data(cv.img, &cv.data);
    if (e != ERR_OK) {
        cleanup(&cv);
        return e;
    }

    FilenameWithError fwe = make_filename();
    if (fwe.error != ERR_OK) {
        fprintf(stderr, "Error: failed to allocate filename\n");
        cleanup(&cv);
        return fwe.error;
    }
    cv.filename = fwe.filename;

    e = save_png(cv.filename, cv.data, wh);
    if (e != ERR_OK) {
        fprintf(stderr, "Error saving PNG: %s\n", cv.filename);
    } else {
        printf("Saved screenshot as '%s'\n", cv.filename);
    }

    cleanup(&cv);
    return e;
}
