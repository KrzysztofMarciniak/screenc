#include <X11/Xlib.h>
#include <X11/Xutil.h>
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

typedef struct {
	Display* disp;
	unsigned char* data;
	char* filename;
	int should_free_data;
} CleanupVariables;

void cleanup(CleanupVariables* cv) {
	if (!cv) return;
	if (cv->data && cv->should_free_data) free(cv->data);
	if (cv->filename) free(cv->filename);
	if (cv->disp) XCloseDisplay(cv->disp);
}

typedef struct {
	WidthHeight wh;
	Display* disp;
	unsigned char* data;
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

	XImage* img = XGetImage(sv.disp, root, 0, 0, gwa.width, gwa.height,
	                         AllPlanes, ZPixmap);
	if (!img) {
		fprintf(stderr, "Error: XGetImage failed\n");
		XCloseDisplay(sv.disp);
		sv.disp  = NULL;
		sv.error = ERR_IMAGE;
		return sv;
	}

	/* Extract pixel data from XImage */
	int width = img->width;
	int height = img->height;
	unsigned char* data = malloc(width * height * 3);
	if (!data) {
		fprintf(stderr, "Error: malloc failed for image data\n");
		XDestroyImage(img);
		XCloseDisplay(sv.disp);
		sv.error = ERR_MEMORY;
		return sv;
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

	XDestroyImage(img);

	sv.wh.width  = width;
	sv.wh.height = height;
	sv.data      = data;
	sv.error     = ERR_OK;
	return sv;
}

CropRect select_crop_area(Display* disp, unsigned char* screenshot_data, int screen_width, int screen_height) {
	CropRect rect = {0, 0, 0, 0};
	Window root = RootWindow(disp, DefaultScreen(disp));
	XEvent event;
	int start_x = 0, start_y = 0;
	int end_x = 0, end_y = 0;
	int selection_active = 0;

	/* Create overlay window */
	XSetWindowAttributes attr;
	attr.background_pixel = BlackPixel(disp, DefaultScreen(disp));
	attr.override_redirect = True;
	Window overlay = XCreateWindow(disp, root, 0, 0, screen_width, screen_height,
	                                 0, CopyFromParent, InputOutput, CopyFromParent,
	                                 CWBackPixel | CWOverrideRedirect, &attr);

	XMapWindow(disp, overlay);
	XRaiseWindow(disp, overlay);
	XSelectInput(disp, overlay, PointerMotionMask | ButtonPressMask | ButtonReleaseMask | ExposureMask);

	/* Create a temporary XImage for display (doesn't own data) */
	XImage display_img = {
		.width = screen_width,
		.height = screen_height,
		.xoffset = 0,
		.format = ZPixmap,
		.data = (char*)screenshot_data,
		.byte_order = LSBFirst,
		.bitmap_unit = 8,
		.bitmap_pad = 8,
		.depth = 24,
		.bytes_per_line = screen_width * 3,
		.bits_per_pixel = 24,
		.red_mask = 0xFF0000,
		.green_mask = 0x00FF00,
		.blue_mask = 0x0000FF
	};

	GC gc = XCreateGC(disp, overlay, 0, NULL);

	/* Display the screenshot */
	XPutImage(disp, overlay, gc, &display_img, 0, 0, 0, 0, screen_width, screen_height);

	/* Create GC for drawing red rectangle */
	GC rect_gc = XCreateGC(disp, overlay, 0, NULL);
	XColor red_color, dummy;
	XAllocNamedColor(disp, DefaultColormap(disp, DefaultScreen(disp)),
	                  "red", &red_color, &dummy);
	XSetForeground(disp, rect_gc, red_color.pixel);
	XSetLineAttributes(disp, rect_gc, 2, LineSolid, CapButt, JoinBevel);

	fprintf(stderr, "Click and drag to select crop area...\n");

	while (1) {
		XNextEvent(disp, &event);

		if (event.type == ButtonPress) {
			if (!selection_active) {
				start_x = event.xbutton.x;
				start_y = event.xbutton.y;
				selection_active = 1;
			}
		} else if (event.type == MotionNotify && selection_active) {
			end_x = event.xmotion.x;
			end_y = event.xmotion.y;

			/* Redraw screenshot + rectangle */
			XPutImage(disp, overlay, gc, &display_img, 0, 0, 0, 0, screen_width, screen_height);

			int x = (start_x < end_x) ? start_x : end_x;
			int y = (start_y < end_y) ? start_y : end_y;
			int w = (start_x < end_x) ? (end_x - start_x) : (start_x - end_x);
			int h = (start_y < end_y) ? (end_y - start_y) : (start_y - end_y);

			if (w > 0 && h > 0) {
				XDrawRectangle(disp, overlay, rect_gc, x, y, w, h);
			}
			XSync(disp, False);
		} else if (event.type == ButtonRelease) {
			end_x = event.xbutton.x;
			end_y = event.xbutton.y;
			break;
		}
	}

	XDestroyWindow(disp, overlay);
	XFreeGC(disp, rect_gc);
	XFreeGC(disp, gc);

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

Error extract_crop(unsigned char* full_data, int full_width, unsigned char** crop_data_out, CropRect* crop) {
	int width = crop->width;
	int height = crop->height;
	int start_x = crop->x;
	int start_y = crop->y;

	unsigned char* crop_data = malloc(width * height * 3);
	if (!crop_data) {
		fprintf(stderr, "Error: malloc failed for crop data\n");
		return ERR_MEMORY;
	}

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			unsigned char* src = &full_data[((start_y + y) * full_width + (start_x + x)) * 3];
			unsigned char* dst = &crop_data[(y * width + x) * 3];
			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
		}
	}

	*crop_data_out = crop_data;
	return ERR_OK;
}

typedef struct {
	char* filename;
	Error error;
} FilenameWithError;

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

	CleanupVariables cv = {0};
	cv.should_free_data = 1;

	ScreenshotVariables sv = grab_screenshot();

	if (sv.error != ERR_OK) {
		cleanup(&cv);
		return sv.error;
	}

	cv.disp = sv.disp;
	cv.data = sv.data;
	WidthHeight wh = sv.wh;

	unsigned char* final_data = sv.data;
	WidthHeight final_wh = wh;

	if (mode == MODE_CROP) {
		CropRect crop_rect = select_crop_area(sv.disp, sv.data, wh.width, wh.height);
		
		Error e = extract_crop(sv.data, wh.width, &final_data, &crop_rect);
		if (e != ERR_OK) {
			cleanup(&cv);
			return e;
		}

		final_wh.width = crop_rect.width;
		final_wh.height = crop_rect.height;
	}

	FilenameWithError fwe = make_filename();
	if (fwe.error != ERR_OK) {
		fprintf(stderr, "Error: failed to allocate filename\n");
		if (final_data != sv.data) free(final_data);
		cleanup(&cv);
		return fwe.error;
	}

	char* full_path = get_save_path(fwe.filename);
	if (!full_path) {
		fprintf(stderr, "Error: failed to allocate path\n");
		free(fwe.filename);
		if (final_data != sv.data) free(final_data);
		cleanup(&cv);
		return ERR_MEMORY;
	}

	Error e = save_png(full_path, final_data, final_wh);
	if (e != ERR_OK) {
		fprintf(stderr, "Error saving PNG: %s\n", full_path);
	} else {
		printf("Saved screenshot as '%s'\n", full_path);
	}

	free(full_path);
	free(fwe.filename);
	if (final_data != sv.data) free(final_data);
	cleanup(&cv);
	return e;
}
