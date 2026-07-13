#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

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

typedef enum {
	MODE_FULL,
	MODE_CROP,
} ScreenMode;

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
	int x;
	int y;
	int width;
	int height;
} CropRect;

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

typedef struct {
	WidthHeight wh;
	Display* disp;
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
		sv.disp  = NULL;
		sv.error = ERR_IMAGE;
		return sv;
	}

	sv.wh.width  = gwa.width;
	sv.wh.height = gwa.height;
	sv.error     = ERR_OK;
	return sv;
}

CropRect select_crop_area(Display* disp, int screen_width, int screen_height) {
	CropRect rect = {0, 0, 0, 0};
	Window root = RootWindow(disp, DefaultScreen(disp));
	XEvent event;
	int start_x = 0, start_y = 0;
	int end_x = 0, end_y = 0;
	int selection_active = 0;

	/* Create transparent overlay window for input handling */
	XSetWindowAttributes attr;
	attr.background_pixel = BlackPixel(disp, DefaultScreen(disp));
	attr.override_redirect = True;
	attr.save_under = True;
	Window overlay = XCreateWindow(disp, root, 0, 0, screen_width, screen_height,
	                                 0, CopyFromParent, InputOutput, CopyFromParent,
	                                 CWBackPixel | CWOverrideRedirect | CWSaveUnder, &attr);

	XMapWindow(disp, overlay);
	XRaiseWindow(disp, overlay);
	XSelectInput(disp, overlay, PointerMotionMask | ButtonPressMask | ButtonReleaseMask);

	/* Make window transparent */
	XSetWindowAttributes trans_attr;
	trans_attr.background_pixmap = None;
	XChangeWindowAttributes(disp, overlay, CWBackPixmap, &trans_attr);

	/* Create GC for drawing red rectangle */
	GC gc = XCreateGC(disp, root, 0, NULL);

	/* Set red color */
	XColor red_color, dummy;
	XAllocNamedColor(disp, DefaultColormap(disp, DefaultScreen(disp)),
	                  "red", &red_color, &dummy);
	XSetForeground(disp, gc, red_color.pixel);
	XSetLineAttributes(disp, gc, 2, LineSolid, CapButt, JoinBevel);
	XSetFunction(disp, gc, GXxor);

	fprintf(stderr, "Click and drag to select crop area...\n");

	int last_x = 0, last_y = 0, last_w = 0, last_h = 0;

	while (1) {
		XNextEvent(disp, &event);

		if (event.type == ButtonPress) {
			if (!selection_active) {
				start_x = event.xbutton.x;
				start_y = event.xbutton.y;
				selection_active = 1;
				last_x = start_x;
				last_y = start_y;
				last_w = 0;
				last_h = 0;
			}
		} else if (event.type == MotionNotify && selection_active) {
			end_x = event.xmotion.x;
			end_y = event.xmotion.y;

			int x = (start_x < end_x) ? start_x : end_x;
			int y = (start_y < end_y) ? start_y : end_y;
			int w = (start_x < end_x) ? (end_x - start_x) : (start_x - end_x);
			int h = (start_y < end_y) ? (end_y - start_y) : (start_y - end_y);

			/* Erase last rectangle */
			if (last_w > 0 && last_h > 0) {
				XDrawRectangle(disp, root, gc, last_x, last_y, last_w, last_h);
			}

			/* Draw new rectangle */
			if (w > 0 && h > 0) {
				XDrawRectangle(disp, root, gc, x, y, w, h);
			}

			last_x = x;
			last_y = y;
			last_w = w;
			last_h = h;

			XSync(disp, False);
			XFlush(disp);
		} else if (event.type == ButtonRelease) {
			end_x = event.xbutton.x;
			end_y = event.xbutton.y;

			/* Clear the last rectangle */
			if (last_w > 0 && last_h > 0) {
				XDrawRectangle(disp, root, gc, last_x, last_y, last_w, last_h);
			}
			break;
		}
	}

	XDestroyWindow(disp, overlay);
	XFreeGC(disp, gc);
	XSync(disp, False);

	rect.x = (start_x < end_x) ? start_x : end_x;
	rect.y = (start_y < end_y) ? start_y : end_y;
	rect.width = (start_x < end_x) ? (end_x - start_x) : (start_x - end_x);
	rect.height = (start_y < end_y) ? (end_y - start_y) : (start_y - end_y);

	/* Clamp to screen bounds */
	if (rect.x < 0) rect.x = 0;
	if (rect.y < 0) rect.y = 0;
	if (rect.x + rect.width > screen_width)
		rect.width = screen_width - rect.x;
	if (rect.y + rect.height > screen_height)
		rect.height = screen_height - rect.y;

	if (rect.width <= 0 || rect.height <= 0) {
		fprintf(stderr, "Invalid crop area selected\n");
		rect.width = 1;
		rect.height = 1;
	}

	return rect;
}

Error extract_data(XImage* img, unsigned char** data_out, CropRect* crop) {
	int width  = crop ? crop->width : img->width;
	int height = crop ? crop->height : img->height;
	int start_x = crop ? crop->x : 0;
	int start_y = crop ? crop->y : 0;

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
			unsigned long pixel = XGetPixel(img, start_x + x, start_y + y);
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

char* get_save_path(const char* filename) {
	/* Try current directory first */
	if (access(".", W_OK) == 0) {
		size_t len = strlen(filename) + 1;
		char* path = malloc(len);
		if (path) {
			strcpy(path, filename);
			return path;
		}
	}

	/* Fall back to home directory */
	const char* home = getenv("HOME");
	if (home) {
		size_t len = strlen(home) + strlen(filename) + 2;
		char* path = malloc(len);
		if (path) {
			snprintf(path, len, "%s/%s", home, filename);
			return path;
		}
	}

	/* Last resort: just use the filename */
	size_t len = strlen(filename) + 1;
	char* path = malloc(len);
	if (path) {
		strcpy(path, filename);
	}
	return path;
}

Error save_png(const char* filepath, unsigned char* data, WidthHeight wh) {
	FILE* fp = fopen(filepath, "wb");
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
	png_set_IHDR(png_ptr, info_ptr, wh.width, wh.height, 8,
	             PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
	             PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

	png_bytep* row_pointers = malloc(sizeof(png_bytep) * wh.height);
	if (!row_pointers) {
		fprintf(stderr, "Memory error\n");
		png_destroy_write_struct(&png_ptr, &info_ptr);
		fclose(fp);
		return ERR_MEMORY_PNG;
	}

	for (int y = 0; y < wh.height; y++)
		row_pointers[y] = data + y * wh.width * 3;

	png_set_rows(png_ptr, info_ptr, row_pointers);
	png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, NULL);

	free(row_pointers);
	png_destroy_write_struct(&png_ptr, &info_ptr);
	fclose(fp);
	return ERR_OK;
}

void print_usage(const char* program_name) {
	fprintf(stderr, "Usage: %s [--full]\n", program_name);
	fprintf(stderr, "  (no args) Crop mode (default) - select area with mouse\n");
	fprintf(stderr, "  --full    Capture full screen\n");
}

int main(int argc, char* argv[]) {
	ScreenMode mode = MODE_CROP;

	/* Parse arguments */
	if (argc > 2) {
		print_usage(argv[0]);
		return ERR_INVALID_ARGS;
	}

	if (argc == 2) {
		if (strcmp(argv[1], "--full") == 0) {
			mode = MODE_FULL;
		} else if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
			print_usage(argv[0]);
			return ERR_OK;
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[1]);
			print_usage(argv[0]);
			return ERR_INVALID_ARGS;
		}
	}

	CleanupVariables cv    = {0};
	ScreenshotVariables sv = grab_screenshot();

	if (sv.error != ERR_OK) {
		cleanup(&cv);
		return sv.error;
	}

	cv.disp        = sv.disp;
	cv.img         = sv.img;
	WidthHeight wh = sv.wh;

	CropRect crop_rect = {0};
	if (mode == MODE_CROP) {
		crop_rect = select_crop_area(sv.disp, wh.width, wh.height);
		wh.width = crop_rect.width;
		wh.height = crop_rect.height;
	}

	Error e = extract_data(cv.img, &cv.data, mode == MODE_CROP ? &crop_rect : NULL);
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

	char* full_path = get_save_path(fwe.filename);
	if (!full_path) {
		fprintf(stderr, "Error: failed to allocate path\n");
		free(fwe.filename);
		cleanup(&cv);
		return ERR_MEMORY;
	}

	e = save_png(full_path, cv.data, wh);
	if (e != ERR_OK) {
		fprintf(stderr, "Error saving PNG: %s\n", full_path);
	} else {
		printf("Saved screenshot as '%s'\n", full_path);
	}

	free(full_path);
	free(fwe.filename);
	cleanup(&cv);
	return e;
}
