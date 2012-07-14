/*
 * mudraw -- command line tool for drawing pdf/xps/cbz documents
 */

#include "fitz.h"

/* SumatraPDF: add support for GDI+ draw device */
#ifdef _WIN32
#include <windows.h>
#define GDI_PLUS_BMP_RENDERER
#else
#include <sys/time.h>
#endif

enum { TEXT_PLAIN = 1, TEXT_HTML = 2, TEXT_XML = 3 };

static char *output = NULL;
static float resolution = 72;
static int res_specified = 0;
static float rotation = 0;

static int showxml = 0;
static int showtext = 0;
static int showtime = 0;
static int showmd5 = 0;
static int showoutline = 0;
static int savealpha = 0;
static int uselist = 1;
static int alphabits = 8;
static float gamma_value = 1;
static int invert = 0;
static int width = 0;
static int height = 0;
static int fit = 0;

static fz_text_sheet *sheet = NULL;
static fz_colorspace *colorspace;
static char *filename;

static struct {
	int count, total;
	int min, max;
	int minpage, maxpage;
} timing;

static void usage(void)
{
	fprintf(stderr,
		"usage: mudraw [options] input [pages]\n"
		"\t-o -\toutput filename (%%d for page number)\n"
#ifdef GDI_PLUS_BMP_RENDERER
		"\t\tsupported formats: pgm, ppm, pam, png, pbm, tga, bmp\n"
#else
		/* SumatraPDF: support TGA as output format */
		"\t\tsupported formats: pgm, ppm, pam, png, pbm, tga\n"
#endif
		"\t-p -\tpassword\n"
		"\t-r -\tresolution in dpi (default: 72)\n"
		"\t-w -\twidth (in pixels) (maximum width if -r is specified)\n"
		"\t-h -\theight (in pixels) (maximum height if -r is specified)\n"
		"\t-f -\tfit width and/or height exactly (ignore aspect)\n"
		"\t-a\tsave alpha channel (only pam, png and tga)\n"
		"\t-b -\tnumber of bits of antialiasing (0 to 8)\n"
		"\t-g\trender in grayscale\n"
		"\t-m\tshow timing information\n"
		"\t-t\tshow text (-tt for xml, -ttt for more verbose xml)\n"
		"\t-x\tshow display list\n"
		"\t-d\tdisable use of display list\n"
		"\t-5\tshow md5 checksums\n"
		"\t-R -\trotate clockwise by given number of degrees\n"
		"\t-G gamma\tgamma correct output\n"
		"\t-I\tinvert output\n"
		"\t-l\tprint outline\n"
		"\tpages\tcomma separated list of ranges\n");
	exit(1);
}

static int gettime(void)
{
	static struct timeval first;
	static int once = 1;
	struct timeval now;
	if (once)
	{
		gettimeofday(&first, NULL);
		once = 0;
	}
	gettimeofday(&now, NULL);
	return (now.tv_sec - first.tv_sec) * 1000 + (now.tv_usec - first.tv_usec) / 1000;
}

static int isrange(char *s)
{
	while (*s)
	{
		if ((*s < '0' || *s > '9') && *s != '-' && *s != ',')
			return 0;
		s++;
	}
	return 1;
}

#ifdef GDI_PLUS_BMP_RENDERER
static void drawbmp(fz_context *ctx, fz_document *doc, fz_page *page, fz_display_list *list, int pagenum)
{
	float zoom;
	fz_matrix ctm;
	fz_bbox bbox;
	fz_rect bounds, bounds2;

	int w, h;
	fz_device *dev;
	HDC dc, dc_main;
	RECT rc;
	HBRUSH bg_brush;
	HBITMAP hbmp;
	BITMAPINFO bmi = { 0 };
	int bmp_data_len;
	char *bmp_data;

	bounds = fz_bound_page(doc, page);
	zoom = resolution / 72;
	ctm = fz_scale(zoom, zoom);
	ctm = fz_concat(ctm, fz_rotate(rotation));
	bounds2 = fz_transform_rect(ctm, bounds);

	w = width;
	h = height;
	if (res_specified)
	{
		bbox = fz_round_rect(bounds2);
		if (w && bbox.x1 - bbox.x0 <= w)
			w = 0;
		if (h && bbox.y1 - bbox.y0 <= h)
			h = 0;
	}
	if (w || h)
	{
		float scalex = w / (bounds2.x1 - bounds2.x0);
		float scaley = h / (bounds2.y1 - bounds2.y0);
		if (w == 0)
			scalex = fit ? 1.0f : scaley;
		if (h == 0)
			scaley = fit ? 1.0f : scalex;
		if (!fit)
			scalex = scaley = min(scalex, scaley);
		ctm = fz_concat(ctm, fz_scale(scalex, scaley));
		bounds2 = fz_transform_rect(ctm, bounds);
	}
	bbox = fz_round_rect(bounds2);

	w = bbox.x1 - bbox.x0;
	h = bbox.y1 - bbox.y0;

	dc_main = GetDC(NULL);
	dc = CreateCompatibleDC(dc_main);
	hbmp = CreateCompatibleBitmap(dc_main, w, h);
	DeleteObject(SelectObject(dc, hbmp));

	SetRect(&rc, 0, 0, w, h);
	bg_brush = CreateSolidBrush(RGB(0xFF,0xFF,0xFF));
	FillRect(dc, &rc, bg_brush);
	DeleteObject(bg_brush);

	dev = fz_new_gdiplus_device(ctx, dc, bbox);
	if (list)
		fz_run_display_list(list, dev, ctm, bbox, NULL);
	else
		fz_run_page(doc, page, dev, ctm, NULL);
	fz_free_device(dev);

	bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
	bmi.bmiHeader.biWidth = w;
	bmi.bmiHeader.biHeight = h;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 24;
	bmi.bmiHeader.biCompression = BI_RGB;

	bmp_data_len = ((w * 3 + 3) / 4) * 4 * h;
	bmp_data = fz_malloc(ctx, bmp_data_len + 1);
	if (!GetDIBits(dc, hbmp, 0, h, bmp_data, &bmi, DIB_RGB_COLORS))
		fz_throw(ctx, "cannot draw page %d in PDF file '%s'", pagenum, filename);

	DeleteDC(dc);
	ReleaseDC(NULL, dc_main);
	DeleteObject(hbmp);

	if (output)
	{
		char buf[512];
		FILE *f;

		sprintf(buf, output, pagenum);
		f = fopen(buf, "wb");
		if (!f)
			fz_throw(ctx, "could not create raster file '%s'", buf);

		if (strstr(output, ".bmp"))
		{
			BITMAPFILEHEADER bmpfh = { 0 };
			static const int one = 1;
			if (!*(char *)&one)
				fz_throw(ctx, "rendering to BMP is not supported on big-endian architectures");

			bmpfh.bfType = MAKEWORD('B', 'M');
			bmpfh.bfOffBits = sizeof(bmpfh) + sizeof(bmi);
			bmpfh.bfSize = bmpfh.bfOffBits + bmp_data_len;

			fwrite(&bmpfh, sizeof(bmpfh), 1, f);
			fwrite(&bmi, sizeof(bmi), 1, f);
			fwrite(bmp_data, 1, bmp_data_len, f);
		}
		else
		{
			unsigned short width = w, height = h, k;

			fwrite("\0\0\x0A\0\0\0\0\0\0\0\0\0", 1, 12, f);
			putc(width & 0xFF, f); putc((width >> 8) & 0xFF, f);
			putc(height & 0xFF, f); putc((height >> 8) & 0xFF, f);
			fwrite("\x18\0", 1, 2, f);

			for (k = 0; k < height; k++)
			{
				int i, j;
				char *line = bmp_data + bmp_data_len / h * k;
				for (i = 0, j = 1; i < width; i += j, j = 1)
				{
#define memeq3(a, b) ((*(DWORD *)(a) & 0xFFFFFF) == (*(DWORD *)(b) & 0xFFFFFF))
					for (; i + j < width && j < 128 && memeq3(line + i * 3, line + (i + j) * 3); j++);
					if (j > 1)
					{
						putc(j - 1 + 128, f);
						fwrite(line + i * 3, 1, 3, f);
					}
					else
					{
						for (; i + j < width && j < 128 && !memeq3(line + (i + j - 1) * 3, line + (i + j) * 3) != 0; j++);
						putc(j - 1, f);
						fwrite(line + i * 3, 1, j * 3, f);
					}
#undef memeq3
				}
			}
			fwrite("\0\0\0\0\0\0\0\0TRUEVISION-XFILE.\0", 1, 26, f);
		}

		fclose(f);
	}

	if (showmd5)
	{
		fz_pixmap *pix = fz_new_pixmap_with_data(ctx, fz_device_rgb, bmp_data_len / 4 / h, h, bmp_data);
		unsigned char digest[16];
		int i;

		fz_md5_pixmap(pix, digest);
		printf(" ");
		for (i = 0; i < 16; i++)
			printf("%02x", digest[i]);

		fz_drop_pixmap(ctx, pix);
	}

	fz_free(ctx, bmp_data);
}
#endif

static void drawpage(fz_context *ctx, fz_document *doc, int pagenum)
{
	fz_page *page;
	fz_display_list *list = NULL;
	fz_device *dev = NULL;
	int start;

	fz_var(list);
	fz_var(dev);

	if (showtime)
	{
		start = gettime();
	}

	fz_try(ctx)
	{
		page = fz_load_page(doc, pagenum - 1);
	}
	fz_catch(ctx)
	{
		fz_throw(ctx, "cannot load page %d in file '%s'", pagenum, filename);
	}

	if (uselist)
	{
		fz_try(ctx)
		{
			list = fz_new_display_list(ctx);
			dev = fz_new_list_device(ctx, list);
			fz_run_page(doc, page, dev, fz_identity, NULL);
		}
		fz_catch(ctx)
		{
			fz_free_device(dev);
			fz_free_display_list(ctx, list);
			fz_free_page(doc, page);
			fz_throw(ctx, "cannot draw page %d in file '%s'", pagenum, filename);
		}
		fz_free_device(dev);
		dev = NULL;
	}

	if (showxml)
	{
		fz_try(ctx)
		{
			dev = fz_new_trace_device(ctx);
			printf("<page number=\"%d\">\n", pagenum);
			if (list)
				fz_run_display_list(list, dev, fz_identity, fz_infinite_bbox, NULL);
			else
				fz_run_page(doc, page, dev, fz_identity, NULL);
			printf("</page>\n");
		}
		fz_catch(ctx)
		{
			fz_free_device(dev);
			fz_free_display_list(ctx, list);
			fz_free_page(doc, page);
			fz_rethrow(ctx);
		}
		fz_free_device(dev);
		dev = NULL;
	}

	if (showtext)
	{
		fz_text_page *text = NULL;

		fz_var(text);

		fz_try(ctx)
		{
			text = fz_new_text_page(ctx, fz_bound_page(doc, page));
			dev = fz_new_text_device(ctx, sheet, text);
			if (list)
				fz_run_display_list(list, dev, fz_identity, fz_infinite_bbox, NULL);
			else
				fz_run_page(doc, page, dev, fz_identity, NULL);
			fz_free_device(dev);
			dev = NULL;
			if (showtext == TEXT_XML)
			{
				fz_print_text_page_xml(ctx, stdout, text);
			}
			else if (showtext == TEXT_HTML)
			{
				fz_print_text_page_html(ctx, stdout, text);
			}
			else if (showtext == TEXT_PLAIN)
			{
				fz_print_text_page(ctx, stdout, text);
				printf("\f\n");
			}
		}
		fz_catch(ctx)
		{
			fz_free_device(dev);
			fz_free_text_page(ctx, text);
			fz_free_display_list(ctx, list);
			fz_free_page(doc, page);
			fz_rethrow(ctx);
		}
		fz_free_text_page(ctx, text);
	}

	if (showmd5 || showtime)
		printf("page %s %d", filename, pagenum);

#ifdef GDI_PLUS_BMP_RENDERER
	// hack: use -d to "disable GDI+" when saving as TGA
	if (output && (strstr(output, ".bmp") || strstr(output, ".tga") && uselist))
		drawbmp(ctx, doc, page, list, pagenum);
	else
#endif
	if (output || showmd5 || showtime)
	{
		float zoom;
		fz_matrix ctm;
		fz_rect bounds, bounds2;
		fz_bbox bbox;
		fz_pixmap *pix = NULL;
		int w, h;

		fz_var(pix);

		bounds = fz_bound_page(doc, page);
		zoom = resolution / 72;
		ctm = fz_scale(zoom, zoom);
		ctm = fz_concat(ctm, fz_rotate(rotation));
		bounds2 = fz_transform_rect(ctm, bounds);
		bbox = fz_round_rect(bounds2);
		/* Make local copies of our width/height */
		w = width;
		h = height;
		/* If a resolution is specified, check to see whether w/h are
		 * exceeded; if not, unset them. */
		if (res_specified)
		{
			int t;
			t = bbox.x1 - bbox.x0;
			if (w && t <= w)
				w = 0;
			t = bbox.y1 - bbox.y0;
			if (h && t <= h)
				h = 0;
		}
		/* Now w or h will be 0 unless then need to be enforced. */
		if (w || h)
		{
			float scalex = w/(bounds2.x1-bounds2.x0);
			float scaley = h/(bounds2.y1-bounds2.y0);

			if (fit)
			{
				if (w == 0)
					scalex = 1.0f;
				if (h == 0)
					scaley = 1.0f;
			}
			else
			{
				if (w == 0)
					scalex = scaley;
				if (h == 0)
					scaley = scalex;
			}
			if (!fit)
			{
				if (scalex > scaley)
					scalex = scaley;
				else
					scaley = scalex;
			}
			ctm = fz_concat(ctm, fz_scale(scalex, scaley));
			bounds2 = fz_transform_rect(ctm, bounds);
		}
		bbox = fz_round_rect(bounds2);

		/* TODO: banded rendering and multi-page ppm */

		fz_try(ctx)
		{
			pix = fz_new_pixmap_with_bbox(ctx, colorspace, bbox);

			if (savealpha)
				fz_clear_pixmap(ctx, pix);
			else
				fz_clear_pixmap_with_value(ctx, pix, 255);

			dev = fz_new_draw_device(ctx, pix);
			if (list)
				fz_run_display_list(list, dev, ctm, bbox, NULL);
			else
				fz_run_page(doc, page, dev, ctm, NULL);
			fz_free_device(dev);
			dev = NULL;

			if (invert)
				fz_invert_pixmap(ctx, pix);
			if (gamma_value != 1)
				fz_gamma_pixmap(ctx, pix, gamma_value);

			if (savealpha)
				fz_unmultiply_pixmap(ctx, pix);

			if (output)
			{
				char buf[512];
				sprintf(buf, output, pagenum);
				if (strstr(output, ".pgm") || strstr(output, ".ppm") || strstr(output, ".pnm"))
					fz_write_pnm(ctx, pix, buf);
				else if (strstr(output, ".pam"))
					fz_write_pam(ctx, pix, buf, savealpha);
				else if (strstr(output, ".png"))
					fz_write_png(ctx, pix, buf, savealpha);
				else if (strstr(output, ".pbm")) {
					fz_bitmap *bit = fz_halftone_pixmap(ctx, pix, NULL);
					fz_write_pbm(ctx, bit, buf);
					fz_drop_bitmap(ctx, bit);
				}
				/* SumatraPDF: support TGA as output format */
				else if (strstr(output, ".tga"))
					fz_write_tga(ctx, pix, buf, savealpha);
			}

			if (showmd5)
			{
				unsigned char digest[16];
				int i;

				fz_md5_pixmap(pix, digest);
				printf(" ");
				for (i = 0; i < 16; i++)
					printf("%02x", digest[i]);
			}

			fz_drop_pixmap(ctx, pix);
		}
		fz_catch(ctx)
		{
			fz_free_device(dev);
			fz_drop_pixmap(ctx, pix);
			fz_free_display_list(ctx, list);
			fz_free_page(doc, page);
			fz_rethrow(ctx);
		}
	}

	if (list)
		fz_free_display_list(ctx, list);

	fz_free_page(doc, page);

	if (showtime)
	{
		int end = gettime();
		int diff = end - start;

		if (diff < timing.min)
		{
			timing.min = diff;
			timing.minpage = pagenum;
		}
		if (diff > timing.max)
		{
			timing.max = diff;
			timing.maxpage = pagenum;
		}
		timing.total += diff;
		timing.count ++;

		printf(" %dms", diff);
	}

	if (showmd5 || showtime)
		printf("\n");

	fz_flush_warnings(ctx);
}

static void drawrange(fz_context *ctx, fz_document *doc, char *range)
{
	int page, spage, epage, final;
	char *spec, *dash;

	final = fz_count_pages(doc);
	spec = fz_strsep(&range, ",");
	while (spec)
	{
		dash = strchr(spec, '-');

		if (dash == spec)
			spage = epage = final;
		else
			spage = epage = atoi(spec);

		if (dash)
		{
			if (strlen(dash) > 1)
				epage = atoi(dash + 1);
			else
				epage = final;
		}

		spage = CLAMP(spage, 1, final);
		epage = CLAMP(epage, 1, final);

		if (spage < epage)
			for (page = spage; page <= epage; page++)
				drawpage(ctx, doc, page);
		else
			for (page = spage; page >= epage; page--)
				drawpage(ctx, doc, page);

		spec = fz_strsep(&range, ",");
	}
}

static void drawoutline(fz_context *ctx, fz_document *doc)
{
	fz_outline *outline = fz_load_outline(doc);
	if (showoutline > 1)
		fz_print_outline_xml(ctx, stdout, outline);
	else
		fz_print_outline(ctx, stdout, outline);
	fz_free_outline(ctx, outline);
}

#ifdef MUPDF_COMBINED_EXE
int draw_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
	char *password = "";
	int grayscale = 0;
	fz_document *doc = NULL;
	int c;
	fz_context *ctx;

	fz_var(doc);

	while ((c = fz_getopt(argc, argv, "lo:p:r:R:ab:dgmtx5G:Iw:h:f")) != -1)
	{
		switch (c)
		{
		case 'o': output = fz_optarg; break;
		case 'p': password = fz_optarg; break;
		case 'r': resolution = atof(fz_optarg); res_specified = 1; break;
		case 'R': rotation = atof(fz_optarg); break;
		case 'a': savealpha = 1; break;
		case 'b': alphabits = atoi(fz_optarg); break;
		case 'l': showoutline++; break;
		case 'm': showtime++; break;
		case 't': showtext++; break;
		case 'x': showxml++; break;
		case '5': showmd5++; break;
		case 'g': grayscale++; break;
		case 'd': uselist = 0; break;
		case 'G': gamma_value = atof(fz_optarg); break;
		case 'w': width = atof(fz_optarg); break;
		case 'h': height = atof(fz_optarg); break;
		case 'f': fit = 1; break;
		case 'I': invert++; break;
		default: usage(); break;
		}
	}

	if (fz_optind == argc)
		usage();

	if (!showtext && !showxml && !showtime && !showmd5 && !showoutline && !output)
	{
		printf("nothing to do\n");
		exit(0);
	}

	ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
	if (!ctx)
	{
		fprintf(stderr, "cannot initialise context\n");
		exit(1);
	}

	fz_set_aa_level(ctx, alphabits);

	colorspace = fz_device_rgb;
	if (output && strstr(output, ".pgm"))
		colorspace = fz_device_gray;
	if (output && strstr(output, ".ppm"))
		colorspace = fz_device_rgb;
	if (output && strstr(output, ".pbm"))
		colorspace = fz_device_gray;
	if (grayscale)
		colorspace = fz_device_gray;

	timing.count = 0;
	timing.total = 0;
	timing.min = 1 << 30;
	timing.max = 0;
	timing.minpage = 0;
	timing.maxpage = 0;

	if (showxml || showtext == TEXT_XML)
		printf("<?xml version=\"1.0\"?>\n");

	if (showtext)
		sheet = fz_new_text_sheet(ctx);

	if (showtext == TEXT_HTML)
	{
		printf("<style>\n");
		printf("body{background-color:gray;margin:12tp;}\n");
		printf("div.page{background-color:white;margin:6pt;padding:6pt;}\n");
		printf("div.block{border:1px solid gray;margin:6pt;padding:6pt;}\n");
		printf("p{margin:0;padding:0;}\n");
		printf("</style>\n");
		printf("<body>\n");
	}

	fz_try(ctx)
	{
		while (fz_optind < argc)
		{
			filename = argv[fz_optind++];

			fz_try(ctx)
			{
				doc = fz_open_document(ctx, filename);
			}
			fz_catch(ctx)
			{
				fz_throw(ctx, "cannot open document: %s", filename);
			}

			if (fz_needs_password(doc))
				if (!fz_authenticate_password(doc, password))
					fz_throw(ctx, "cannot authenticate password: %s", filename);

			if (showxml || showtext == TEXT_XML)
				printf("<document name=\"%s\">\n", filename);

			if (showoutline)
				drawoutline(ctx, doc);

			if (showtext || showxml || showtime || showmd5 || output)
			{
				if (fz_optind == argc || !isrange(argv[fz_optind]))
					drawrange(ctx, doc, "1-");
				if (fz_optind < argc && isrange(argv[fz_optind]))
					drawrange(ctx, doc, argv[fz_optind++]);
			}

			if (showxml || showtext == TEXT_XML)
				printf("</document>\n");

			fz_close_document(doc);
			doc = NULL;
		}
	}
	fz_catch(ctx)
	{
		fz_close_document(doc);
	}

	if (showtext == TEXT_HTML)
	{
		printf("</body>\n");
		printf("<style>\n");
		fz_print_text_sheet(ctx, stdout, sheet);
		printf("</style>\n");
	}

	if (showtext)
		fz_free_text_sheet(ctx, sheet);

	if (showtime && timing.count > 0)
	{
		printf("total %dms / %d pages for an average of %dms\n",
			timing.total, timing.count, timing.total / timing.count);
		printf("fastest page %d: %dms\n", timing.minpage, timing.min);
		printf("slowest page %d: %dms\n", timing.maxpage, timing.max);
	}

	fz_free_context(ctx);
	return 0;
}
