#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <omp.h>

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

Error extract_data(XImage* img, unsigned char** data_out, CropRect* crop) {
	int width = crop ? crop->width : img->width;
	int height = crop ? crop->height : img->height;
	int start_x = crop ? crop->x : 0;
	int start_y = crop ? crop->y : 0;
	int bytes_per_pixel = img->bits_per_pixel / 8;

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

	/* Fast path: direct buffer access if 24-bit or 32-bit */
	if ((bytes_per_pixel == 3 || bytes_per_pixel == 4) && img->format == ZPixmap) {
		unsigned char* src_base = (unsigned char*)img->data + start_y * img->bytes_per_line;
		
		#pragma omp parallel for collapse(2)
		for (int y = 0; y < height; y++) {
			for (int x = 0; x < width; x++) {
				unsigned char* src_pixel = src_base + y * img->bytes_per_line + (start_x + x) * bytes_per_pixel;
				unsigned long pixel;
				
				if (bytes_per_pixel == 4) {
					pixel = *(unsigned int*)src_pixel;
				} else {
					pixel = src_pixel[0] | (src_pixel[1] << 8) | (src_pixel[2] << 16);
				}
				
				unsigned char* dst = &data[(y * width + x) * 3];
				dst[0] = (pixel & img->red_mask) >> r_shift;
				dst[1] = (pixel & img->green_mask) >> g_shift;
				dst[2] = (pixel & img->blue_mask) >> b_shift;
			}
		}
	} else {
		/* Fallback: use XGetPixel */
		#pragma omp parallel for collapse(2)
		for (int y = 0; y < height; y++) {
			for (int x = 0; x < width; x++) {
				unsigned long pixel = XGetPixel(img, start_x + x, start_y + y);
				unsigned char* p    = &data[(y * width + x) * 3];
				p[0] = (pixel & img->red_mask) >> r_shift;
				p[1] = (pixel & img->green_mask) >> g_shift;
				p[2] = (pixel & img->blue_mask) >> b_shift;
			}
		}
	}

	*data_out = data;
	return ERR_OK;
}

CropRect select_crop_area(Display* disp, XImage* img, int screen_width, int screen_height) {
	CropRect rect = {0, 0, screen_width, screen_height};
	Window root = RootWindow(disp, DefaultScreen(disp));
	XEvent event;
	int start_x = 0, start_y = 0;
	int end_x = 0, end_y = 0;
	int selection_active = 0;
	Pixmap cached_pixmap = None;

	/* Create overlay window */
	XSetWindowAttributes attr;
	attr.background_pixel = BlackPixel(disp, DefaultScreen(disp));
	attr.override_redirect = True;
	Window overlay = XCreateWindow(disp, root, 0, 0, screen_width, screen_height,
	                                 0, CopyFromParent, InputOutput, CopyFromParent,
	                                 CWBackPixel | CWOverrideRedirect, &attr);

	XMapWindow(disp, overlay);
	XRaiseWindow(disp, overlay);
	XSelectInput(disp, overlay, PointerMotionMask | ButtonPressMask | ButtonReleaseMask);

	GC gc = XCreateGC(disp, overlay, 0, NULL);

	/* Cache the screenshot as a pixmap for fast redraws */
	cached_pixmap = XCreatePixmap(disp, overlay, screen_width, screen_height, DefaultDepth(disp, DefaultScreen(disp)));
	XPutImage(disp, cached_pixmap, gc, img, 0, 0, 0, 0, screen_width, screen_height);

	/* Draw cached pixmap to overlay */
	XCopyArea(disp, cached_pixmap, overlay, gc, 0, 0, screen_width, screen_height, 0, 0);

	/* Create GC for red rectangle */
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

			/* Redraw cached pixmap only, not the full image */
			XCopyArea(disp, cached_pixmap, overlay, gc, 0, 0, screen_width, screen_height, 0, 0);

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

	XFreePixmap(disp, cached_pixmap);
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
	/* Use faster compression level (6 instead of default 9) */
	png_set_compression_level(png_ptr, 6);
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
	int crop_mode = 1;

	/* Parse arguments */
	if (argc > 2) {
		print_usage(argv[0]);
		return ERR_INVALID_ARGS;
	}

	if (argc == 2) {
		if (strcmp(argv[1], "--full") == 0) {
			crop_mode = 0;
		} else if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
			print_usage(argv[0]);
			return ERR_OK;
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[1]);
			print_usage(argv[0]);
			return ERR_INVALID_ARGS;
		}
	}

	/* Open display */
	Display* disp = XOpenDisplay(NULL);
	if (!disp) {
		fprintf(stderr, "Error: Cannot open X display\n");
		return ERR_DISPLAY;
	}

	/* Get screenshot */
	Window root = RootWindow(disp, DefaultScreen(disp));
	XWindowAttributes gwa;
	XGetWindowAttributes(disp, root, &gwa);

	XImage* img = XGetImage(disp, root, 0, 0, gwa.width, gwa.height,
	                         AllPlanes, ZPixmap);
	if (!img) {
		fprintf(stderr, "Error: XGetImage failed\n");
		XCloseDisplay(disp);
		return ERR_IMAGE;
	}

	WidthHeight wh = {gwa.width, gwa.height};
	CropRect crop = {0, 0, gwa.width, gwa.height};

	/* Get crop area if in crop mode */
	if (crop_mode) {
		crop = select_crop_area(disp, img, gwa.width, gwa.height);
		wh.width = crop.width;
		wh.height = crop.height;
	}

	/* Extract pixel data for selected area */
	unsigned char* data = NULL;
	Error e = extract_data(img, &data, crop_mode ? &crop : NULL);
	if (e != ERR_OK) {
		XDestroyImage(img);
		XCloseDisplay(disp);
		return e;
	}

	/* Generate filename and save */
	FilenameWithError fwe = make_filename();
	if (fwe.error != ERR_OK) {
		fprintf(stderr, "Error: failed to allocate filename\n");
		free(data);
		XDestroyImage(img);
		XCloseDisplay(disp);
		return fwe.error;
	}

	char* full_path = get_save_path(fwe.filename);
	if (!full_path) {
		fprintf(stderr, "Error: failed to allocate path\n");
		free(fwe.filename);
		free(data);
		XDestroyImage(img);
		XCloseDisplay(disp);
		return ERR_MEMORY;
	}

	e = save_png(full_path, data, wh);
	if (e != ERR_OK) {
		fprintf(stderr, "Error saving PNG: %s\n", full_path);
	} else {
		printf("Saved screenshot as '%s'\n", full_path);
	}

	free(full_path);
	free(fwe.filename);
	free(data);
	XDestroyImage(img);
	XCloseDisplay(disp);
	return e;
}
