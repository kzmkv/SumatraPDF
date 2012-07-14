/*
 * PDF cleaning tool: general purpose pdf syntax washer.
 *
 * Rewrite PDF with pretty printed objects.
 * Garbage collect unreachable objects.
 * Inflate compressed streams.
 * Create subset documents.
 *
 * TODO: linearize document for fast web view
 */

#include "fitz.h"
#include "mupdf-internal.h"

static FILE *out = NULL;

static pdf_document *xref = NULL;
static fz_context *ctx = NULL;

static void usage(void)
{
	fprintf(stderr,
		"usage: mupdfclean [options] input.pdf [output.pdf] [pages]\n"
		"\t-p -\tpassword\n"
		"\t-g\tgarbage collect unused objects\n"
		"\t-gg\tin addition to -g compact xref table\n"
		"\t-ggg\tin addition to -gg merge duplicate objects\n"
		"\t-d\tdecompress all streams\n"
		"\t-i\ttoggle decompression of image streams\n"
		"\t-f\ttoggle decompression of font streams\n"
		"\t-a\tascii hex encode binary streams\n"
		"\tpages\tcomma separated list of ranges\n");
	exit(1);
}

/*
 * Recreate page tree to only retain specified pages.
 */

static void retainpages(int argc, char **argv)
{
	pdf_obj *oldroot, *root, *pages, *kids, *countobj, *parent, *olddests;

	/* Keep only pages/type and (reduced) dest entries to avoid
	 * references to unretained pages */
	oldroot = pdf_dict_gets(xref->trailer, "Root");
	pages = pdf_dict_gets(oldroot, "Pages");
	olddests = pdf_load_name_tree(xref, "Dests");

	root = pdf_new_dict(ctx, 2);
	pdf_dict_puts(root, "Type", pdf_dict_gets(oldroot, "Type"));
	pdf_dict_puts(root, "Pages", pdf_dict_gets(oldroot, "Pages"));

	pdf_update_object(xref, pdf_to_num(oldroot), pdf_to_gen(oldroot), root);

	pdf_drop_obj(root);

	/* Create a new kids array with only the pages we want to keep */
	parent = pdf_new_indirect(ctx, pdf_to_num(pages), pdf_to_gen(pages), xref);
	kids = pdf_new_array(ctx, 1);

	/* Retain pages specified */
	while (argc - fz_optind)
	{
		int page, spage, epage;
		char *spec, *dash;
		char *pagelist = argv[fz_optind];

		spec = fz_strsep(&pagelist, ",");
		while (spec)
		{
			dash = strchr(spec, '-');

			if (dash == spec)
				spage = epage = pdf_count_pages(xref);
			else
				spage = epage = atoi(spec);

			if (dash)
			{
				if (strlen(dash) > 1)
					epage = atoi(dash + 1);
				else
					epage = pdf_count_pages(xref);
			}

			if (spage > epage)
				page = spage, spage = epage, epage = page;

			if (spage < 1)
				spage = 1;
			if (epage > pdf_count_pages(xref))
				epage = pdf_count_pages(xref);

			for (page = spage; page <= epage; page++)
			{
				pdf_obj *pageobj = xref->page_objs[page-1];
				pdf_obj *pageref = xref->page_refs[page-1];

				pdf_dict_puts(pageobj, "Parent", parent);

				/* Store page object in new kids array */
				pdf_array_push(kids, pageref);
			}

			spec = fz_strsep(&pagelist, ",");
		}

		fz_optind++;
	}

	pdf_drop_obj(parent);

	/* Update page count and kids array */
	countobj = pdf_new_int(ctx, pdf_array_len(kids));
	pdf_dict_puts(pages, "Count", countobj);
	pdf_drop_obj(countobj);
	pdf_dict_puts(pages, "Kids", kids);
	pdf_drop_obj(kids);

	/* Also preserve the (partial) Dests name tree */
	if (olddests)
	{
		int i;
		pdf_obj *names = pdf_new_dict(ctx, 1);
		pdf_obj *dests = pdf_new_dict(ctx, 1);
		pdf_obj *names_list = pdf_new_array(ctx, 32);

		for (i = 0; i < pdf_dict_len(olddests); i++)
		{
			pdf_obj *key = pdf_dict_get_key(olddests, i);
			pdf_obj *val = pdf_dict_get_val(olddests, i);
			pdf_obj *key_str = pdf_new_string(ctx, pdf_to_name(key), strlen(pdf_to_name(key)));
			pdf_obj *dest = pdf_dict_gets(val, "D");

			dest = pdf_array_get(dest ? dest : val, 0);
			if (pdf_array_contains(pdf_dict_gets(pages, "Kids"), dest))
			{
				pdf_array_push(names_list, key_str);
				pdf_array_push(names_list, val);
			}
			pdf_drop_obj(key_str);
		}

		root = pdf_dict_gets(xref->trailer, "Root");
		pdf_dict_puts(dests, "Names", names_list);
		pdf_dict_puts(names, "Dests", dests);
		pdf_dict_puts(root, "Names", names);

		pdf_drop_obj(names);
		pdf_drop_obj(dests);
		pdf_drop_obj(names_list);
		pdf_drop_obj(olddests);
	}
}

#ifdef MUPDF_COMBINED_EXE
int pdfclean_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
	char *infile;
	char *outfile = "out.pdf";
	char *password = "";
	int c;
	int subset;
	fz_write_options opts;

	opts.dogarbage = 0;
	opts.doexpand = 0;
	opts.doascii = 0;

	while ((c = fz_getopt(argc, argv, "adfgip:")) != -1)
	{
		switch (c)
		{
		case 'p': password = fz_optarg; break;
		case 'g': opts.dogarbage ++; break;
		case 'd': opts.doexpand ^= fz_expand_all; break;
		case 'f': opts.doexpand ^= fz_expand_fonts; break;
		case 'i': opts.doexpand ^= fz_expand_images; break;
		case 'a': opts.doascii ++; break;
		default: usage(); break;
		}
	}

	if (argc - fz_optind < 1)
		usage();

	infile = argv[fz_optind++];

	if (argc - fz_optind > 0 &&
		(strstr(argv[fz_optind], ".pdf") || strstr(argv[fz_optind], ".PDF")))
	{
		outfile = argv[fz_optind++];
	}

	subset = 0;
	if (argc - fz_optind > 0)
		subset = 1;

	ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
	if (!ctx)
	{
		fprintf(stderr, "cannot initialise context\n");
		exit(1);
	}

	xref = pdf_open_document(ctx, infile);
	if (pdf_needs_password(xref))
		if (!pdf_authenticate_password(xref, password))
			fz_throw(ctx, "cannot authenticate password: %s", infile);

	/* Only retain the specified subset of the pages */
	if (subset)
		retainpages(argc, argv);

	pdf_write(xref, outfile, &opts);

	pdf_close_document(xref);
	fz_free_context(ctx);
	return 0;
}
