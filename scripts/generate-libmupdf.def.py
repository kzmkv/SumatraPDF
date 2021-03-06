"""
Generates a list of all exports from libmupdf.dll from the function lists
contained in the mupdf/*/*.h headers.
"""

import os, re
from util import verify_started_in_right_directory

def generateExports(header, exclude=[]):
	data = open(header, "r").read()
	functions = re.findall(r"^\w+ (?:\w+ )?\*?(\w+)\(.*?\);", data, re.MULTILINE | re.DOTALL)
	exports = "\n".join(["\t" + name for name in functions if name not in exclude])
	return exports

LIBMUPDF_DEF = """\
; This file is auto-generated by generate-libmupdf.def.py

LIBRARY libmupdf
EXPORTS

; Fitz exports

%(fitz_exports)s

; MuPDF exports

%(mupdf_exports)s

; MuXPS exports

%(muxps_exports)s

; MuCBZ exports

%(mucbz_exports)s

; jpeg exports

	jpeg_resync_to_restart
	jpeg_finish_decompress
	jpeg_read_scanlines
	jpeg_start_decompress
	jpeg_read_header
	jpeg_CreateDecompress
	jpeg_destroy_decompress
	jpeg_std_error

; zlib exports

	gzerror
	gzprintf
	gzopen
	gzwopen
	gzseek
	gztell
	gzread
	gzclose
	inflateInit_
	inflateInit2_
	inflate
	inflateEnd
	deflateInit_
	deflate
	deflateEnd
	compress
	compressBound
	crc32
"""

def main():
	fitz_exports = generateExports("fitz/fitz.h") + "\n\n" + generateExports("fitz/fitz-internal.h", ["fz_assert_lock_held", "fz_assert_lock_not_held", "fz_lock_debug_lock", "fz_lock_debug_unlock", "fz_purge_glyph_cache"])
	mupdf_exports = generateExports("pdf/mupdf.h") + "\n\n" + generateExports("pdf/mupdf-internal.h")
	muxps_exports = generateExports("xps/muxps.h") + "\n\n" + generateExports("xps/muxps-internal.h", ["xps_parse_solid_color_brush", "xps_print_path"])
	mucbz_exports = generateExports("cbz/mucbz.h")
	
	list = LIBMUPDF_DEF % locals()
	open("../src/libmupdf.def", "wb").write(list.replace("\n", "\r\n"))

if __name__ == "__main__":
	if os.path.exists("generate-libmupdf.def.py"):
		os.chdir("..")
	verify_started_in_right_directory()
	
	os.chdir("mupdf")
	main()
