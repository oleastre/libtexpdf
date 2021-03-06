/* This is dvipdfmx, an eXtended version of dvipdfm by Mark A. Wicks.

    Copyright (C) 2007-2014 by Jin-Hwan Cho and Shunsaku Hirata,
    the dvipdfmx project team.
    
    Copyright (C) 1998, 1999 by Mark A. Wicks <mwicks@kettering.edu>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
*/

/*
 * Concatinating content streams are only supported for streams that only uses
 * single FlateDecode filter, i.e.,
 *
 *   /Filter /FlateDecode or /Filter [/FlateDecode]
 *
 * TrimBox, BleedBox, ArtBox, Rotate ...
 */

#include "libtexpdf.h"

static int  rect_equal       (pdf_obj *rect1, pdf_obj *rect2);
#if 0
#if HAVE_ZLIB
#include <zlib.h>
static int  add_stream_flate (pdf_obj *dst, const void *data, long len);
#endif
static int  concat_stream    (pdf_obj *dst, pdf_obj *src);
#endif
/*
 * From PDFReference15_v6.pdf (p.119 and p.834)
 *
 * MediaBox rectangle (Required; inheritable)
 *
 * The media box defines the boundaries of the physical medium on which the
 * page is to be printed. It may include any extended area surrounding the
 * finished page for bleed, printing marks, or other such purposes. It may
 * also include areas close to the edges of the medium that cannot be marked
 * because of physical limitations of the output device. Content falling
 * outside this boundary can safely be discarded without affecting the
 * meaning of the PDF file.
 *
 * CropBox rectangle (Optional; inheritable)
 *
 * The crop box defines the region to which the contents of the page are to be
 * clipped (cropped) when displayed or printed. Unlike the other boxes, the
 * crop box has no defined meaning in terms of physical page geometry or
 * intended use; it merely imposes clipping on the page contents. However,
 * in the absence of additional information (such as imposition instructions
 * specified in a JDF or PJTF job ticket), the crop box will determine how
 * the pageâ€™s contents are to be positioned on the output medium. The default
 * value is the pageâ€™s media box. 
 *
 * BleedBox rectangle (Optional; PDF 1.3)
 *
 * The bleed box (PDF 1.3) defines the region to which the contents of the
 * page should be clipped when output in a production environment. This may
 * include any extra â€œbleed areaâ€ needed to accommodate the physical
 * limitations of cutting, folding, and trimming equipment. The actual printed
 * page may include printing marks that fall outside the bleed box.
 * The default value is the pageâ€™s crop box. 
 *
 * TrimBox rectangle (Optional; PDF 1.3)
 *
 * The trim box (PDF 1.3) defines the intended dimensions of the finished page
 * after trimming. It may be smaller than the media box, to allow for
 * production-related content such as printing instructions, cut marks, or
 * color bars. The default value is the pageâ€™s crop box. 
 *
 * ArtBox rectangle (Optional; PDF 1.3)
 *
 * The art box (PDF 1.3) defines the extent of the pageâ€™s meaningful content
 * (including potential white space) as intended by the pageâ€™s creator.
 * The default value is the pageâ€™s crop box.
 *
 * Rotate integer (Optional; inheritable)
 *
 * The number of degrees by which the page should be rotated clockwise when
 * displayed or printed. The value must be a multiple of 90. Default value: 0.
 */

static int
rect_equal (pdf_obj *rect1, pdf_obj *rect2)
{
  int i;

  if (!rect1 || !rect2)
    return 0;
  for (i = 0; i < 4; i++) {
    if (texpdf_number_value(texpdf_get_array(rect1, i)) !=
	texpdf_number_value(texpdf_get_array(rect2, i)))
      return 0;
  }

  return 1;
}

static pdf_obj*
texpdf_get_page_obj (pdf_file *pf, long page_no,
                  pdf_obj **ret_bbox, pdf_obj **ret_resources)
{
  pdf_obj *page_tree;
  pdf_obj *bbox = NULL, *resources = NULL, *rotate = NULL;
  long page_idx;

  /*
   * Get Page Tree.
   */
  page_tree = NULL;
  {
    pdf_obj *trailer, *catalog;
    pdf_obj *markinfo, *tmp;

    trailer = pdf_file_get_trailer(pf);

    if (texpdf_lookup_dict(trailer, "Encrypt")) {
      WARN("This PDF document is encrypted.");
      texpdf_release_obj(trailer);
      return NULL;
    }

    catalog = pdf_deref_obj(texpdf_lookup_dict(trailer, "Root"));
    if (!PDF_OBJ_DICTTYPE(catalog)) {
      WARN("Can't read document catalog.");
      texpdf_release_obj(trailer);
      if (catalog)
	texpdf_release_obj(catalog);
      return NULL;
    }
    texpdf_release_obj(trailer);

    markinfo = pdf_deref_obj(texpdf_lookup_dict(catalog, "MarkInfo"));
    if (markinfo) {
      tmp = texpdf_lookup_dict(markinfo, "Marked");
      if (PDF_OBJ_BOOLEANTYPE(tmp) && pdf_boolean_value(tmp))
	WARN("File contains tagged PDF. Ignoring tags.");
      texpdf_release_obj(markinfo);
    }

    page_tree = pdf_deref_obj(texpdf_lookup_dict(catalog, "Pages"));
    texpdf_release_obj(catalog);
  }
  if (!page_tree) {
    WARN("Page tree not found.");
    return NULL;
  }

  /*
   * Negative page numbers are counted from the back.
   */
  {
    long count = texpdf_number_value(texpdf_lookup_dict(page_tree, "Count"));
    page_idx = page_no + (page_no >= 0 ? -1 : count);
    if (page_idx < 0 || page_idx >= count) {
	WARN("Page %ld does not exist.", page_no);
	texpdf_release_obj(page_tree);
	return NULL;
      }
    page_no = page_idx+1;
  }

  /*
   * Seek correct page. Get Media/Crop Box.
   * Media box and resources can be inherited.
   */
  {
    pdf_obj *kids_ref, *kids;
    pdf_obj *crop_box = NULL;
    pdf_obj *tmp;

    tmp = texpdf_lookup_dict(page_tree, "Resources");
    resources = tmp ? pdf_deref_obj(tmp) : texpdf_new_dict();

    while (1) {
      long kids_length, i;
 
      if ((tmp = pdf_deref_obj(texpdf_lookup_dict(page_tree, "MediaBox")))) {
	if (bbox)
	  texpdf_release_obj(bbox);
	bbox = tmp;
      }
      if ((tmp = pdf_deref_obj(texpdf_lookup_dict(page_tree, "BleedBox")))) {
        if (!rect_equal(tmp, bbox)) {
	  if (bbox)
	    texpdf_release_obj(bbox);
	  bbox = tmp;
        } else
          texpdf_release_obj(tmp);
      }
      if ((tmp = pdf_deref_obj(texpdf_lookup_dict(page_tree, "TrimBox")))) {
        if (!rect_equal(tmp, bbox)) {
	  if (bbox)
	    texpdf_release_obj(bbox);
	  bbox = tmp;
        } else
          texpdf_release_obj(tmp);
      }
      if ((tmp = pdf_deref_obj(texpdf_lookup_dict(page_tree, "ArtBox")))) {
        if (!rect_equal(tmp, bbox)) {
	  if (bbox)
	    texpdf_release_obj(bbox);
	  bbox = tmp;
        } else
          texpdf_release_obj(tmp);
      }
      if ((tmp = pdf_deref_obj(texpdf_lookup_dict(page_tree, "CropBox")))) {
	if (crop_box)
	  texpdf_release_obj(crop_box);
	crop_box = tmp;
      }
      if ((tmp = pdf_deref_obj(texpdf_lookup_dict(page_tree, "Rotate")))) {
	if (rotate)
	  texpdf_release_obj(rotate);
	rotate = tmp;
      }
      if ((tmp = pdf_deref_obj(texpdf_lookup_dict(page_tree, "Resources")))) {
#if 0
	texpdf_merge_dict(tmp, resources);
#endif
	if (resources)
	  texpdf_release_obj(resources);
	resources = tmp;
      }

      kids_ref = texpdf_lookup_dict(page_tree, "Kids");
      if (!kids_ref)
	break;
      kids = pdf_deref_obj(kids_ref);
      kids_length = texpdf_array_length(kids);

      for (i = 0; i < kids_length; i++) {
	long count;

	texpdf_release_obj(page_tree);
	page_tree = pdf_deref_obj(texpdf_get_array(kids, i));

	tmp = pdf_deref_obj(texpdf_lookup_dict(page_tree, "Count"));
	if (tmp) {
	  /* Pages object */
	  count = texpdf_number_value(tmp);
	  texpdf_release_obj(tmp);
	} else
	  /* Page object */
	  count = 1;

	if (page_idx < count)
	  break;

	page_idx -= count;
      }
      
      texpdf_release_obj(kids);

      if (i == kids_length) {
	WARN("Page %ld not found! Broken PDF file?", page_no);
	if (bbox)
	  texpdf_release_obj(bbox);
	if (crop_box)
	  texpdf_release_obj(crop_box);
	if (rotate)
	  texpdf_release_obj(rotate);
	texpdf_release_obj(resources);
	texpdf_release_obj(page_tree);
	return NULL;
      }
    }
    if (crop_box) {
      texpdf_release_obj(bbox);
      bbox = crop_box;
    }
  }

  if (!bbox) {
    WARN("No BoundingBox information available.");
    texpdf_release_obj(page_tree);
    texpdf_release_obj(resources);
    if (rotate)
      texpdf_release_obj(rotate);
    return NULL;
  }

  if (rotate) {
    if (texpdf_number_value(rotate) != 0.0)
      WARN("<< /Rotate %d >> found. (Not supported yet)",  (int)texpdf_number_value(rotate));
    texpdf_release_obj(rotate);
    rotate = NULL;
  }
  
  if (ret_bbox != NULL)
    *ret_bbox = bbox;
  if (ret_resources != NULL)
    *ret_resources = resources;

  return page_tree;
}

static pdf_obj*
texpdf_get_page_content (pdf_obj* page)
{
  pdf_obj *contents, *content_new;

  contents = pdf_deref_obj(texpdf_lookup_dict(page, "Contents"));
  if (!contents)
    return NULL;

  if (texpdf_obj_typeof(contents) == PDF_NULL) {
    /* empty page */
    texpdf_release_obj(contents);
    /* TODO: better don't include anything if the page is empty */
    contents = texpdf_new_stream(0);
  } else if (PDF_OBJ_ARRAYTYPE(contents)) {
    /*
     * Concatenate all content streams.
     */
    pdf_obj *content_seg;
    int      idx = 0;
    content_new = texpdf_new_stream(STREAM_COMPRESS);
    for (;;) {
      content_seg = pdf_deref_obj(texpdf_get_array(contents, idx));
      if (!content_seg)
	break;
      else if (PDF_OBJ_NULLTYPE(content_seg)) {
	/* Silently ignore. */
      }  else if (!PDF_OBJ_STREAMTYPE(content_seg)) {
	WARN("Page content not a stream object. Broken PDF file?");
        texpdf_release_obj(content_seg);
	texpdf_release_obj(content_new);
        texpdf_release_obj(contents);
	return NULL;
      } else if (pdf_concat_stream(content_new, content_seg) < 0) {
	WARN("Could not handle content stream with multiple segments.");
        texpdf_release_obj(content_seg);
	texpdf_release_obj(content_new);
        texpdf_release_obj(contents);
	return NULL;
      }
      texpdf_release_obj(content_seg);
      idx++;
    }
    texpdf_release_obj(contents);
    contents = content_new;
  } else {
    if (!PDF_OBJ_STREAMTYPE(contents)) {
      WARN("Page content not a stream object. Broken PDF file?");
      texpdf_release_obj(contents);
      return NULL;
    }
    /* Flate the contents if necessary. */
    content_new = texpdf_new_stream(STREAM_COMPRESS);
    if (pdf_concat_stream(content_new, contents) < 0) {
      WARN("Could not handle a content stream.");
      texpdf_release_obj(contents);
      texpdf_release_obj(content_new);
      return NULL;
    }
    texpdf_release_obj(contents);
    contents = content_new;
  }

  return contents;
}

int
pdf_include_page (pdf_ximage *ximage, FILE *image_file, const char *filename)
{
  pdf_file *pf;
  xform_info info;
  pdf_obj *contents = NULL, *catalog;
  pdf_obj *page = NULL, *resources = NULL, *markinfo = NULL;
  long page_no;

  pf = texpdf_open(filename, image_file);
  if (!pf)
    return -1;

  if (texpdf_file_get_version(pf) > texpdf_get_version())
    goto too_recent;

  texpdf_ximage_init_form_info(&info);

  /*
   * Get Page.
   */
  page_no = texpdf_ximage_get_page(ximage);
  if (page_no == 0)
    page_no = 1;

  page = texpdf_doc_get_page(pf, page_no, NULL, &info.bbox, &resources);
  if(!page)
    goto error_silent;

  catalog = pdf_file_get_catalog(pf);
  markinfo = pdf_deref_obj(texpdf_lookup_dict(catalog, "MarkInfo"));
  if (markinfo) {
    pdf_obj *tmp = pdf_deref_obj(texpdf_lookup_dict(markinfo, "Marked"));
    texpdf_release_obj(markinfo);
    if (!PDF_OBJ_BOOLEANTYPE(tmp)) {
      if (tmp)
	texpdf_release_obj(tmp);
      goto error;
    } else if (pdf_boolean_value(tmp))
      WARN("File contains tagged PDF. Ignoring tags.");
    texpdf_release_obj(tmp);
  }

  contents = pdf_deref_obj(texpdf_lookup_dict(page, "Contents"));
  texpdf_release_obj(page);
  page = NULL;

  /*
   * Handle page content stream.
   */
  {
    pdf_obj *content_new;

    if (!contents) {
      /*
       * Empty page
       */
      content_new = texpdf_new_stream(0);
      /* TODO: better don't include anything if the page is empty */
    } else if (PDF_OBJ_STREAMTYPE(contents)) {
      /* 
       * We must import the stream because its dictionary
       * may contain indirect references.
       */
      content_new = pdf_import_object(contents);
    } else if (PDF_OBJ_ARRAYTYPE(contents)) {
      /*
       * Concatenate all content streams.
       */
      int idx, len = texpdf_array_length(contents);
      content_new = texpdf_new_stream(STREAM_COMPRESS);
      for (idx = 0; idx < len; idx++) {
	pdf_obj *content_seg = pdf_deref_obj(texpdf_get_array(contents, idx));
	if (!PDF_OBJ_STREAMTYPE(content_seg) ||
	    pdf_concat_stream(content_new, content_seg) < 0) {
	  texpdf_release_obj(content_seg);
	  texpdf_release_obj(content_new);
	  goto error;
	}
	texpdf_release_obj(content_seg);
      }
    } else
      goto error;

    if (contents)
      texpdf_release_obj(contents);
    contents = content_new;
  }

  /*
   * Add entries to contents stream dictionary.
   */
  {
    pdf_obj *contents_dict, *bbox, *matrix;

    contents_dict = texpdf_stream_dict(contents);
    texpdf_add_dict(contents_dict,
		 texpdf_new_name("Type"), 
		 texpdf_new_name("XObject"));
    texpdf_add_dict(contents_dict,
		 texpdf_new_name("Subtype"),
		 texpdf_new_name("Form"));
    texpdf_add_dict(contents_dict,
		 texpdf_new_name("FormType"),
		 texpdf_new_number(1.0));

    bbox = texpdf_new_array();
    texpdf_add_array(bbox, texpdf_new_number(info.bbox.llx));
    texpdf_add_array(bbox, texpdf_new_number(info.bbox.lly));
    texpdf_add_array(bbox, texpdf_new_number(info.bbox.urx));
    texpdf_add_array(bbox, texpdf_new_number(info.bbox.ury));

    texpdf_add_dict(contents_dict, texpdf_new_name("BBox"), bbox);

    matrix = texpdf_new_array();
    texpdf_add_array(matrix, texpdf_new_number(1.0));
    texpdf_add_array(matrix, texpdf_new_number(0.0));
    texpdf_add_array(matrix, texpdf_new_number(0.0));
    texpdf_add_array(matrix, texpdf_new_number(1.0));
    texpdf_add_array(matrix, texpdf_new_number(0.0));
    texpdf_add_array(matrix, texpdf_new_number(0.0));

    texpdf_add_dict(contents_dict, texpdf_new_name("Matrix"), matrix);

    texpdf_add_dict(contents_dict, texpdf_new_name("Resources"),
                 pdf_import_object(resources));
    texpdf_release_obj(resources);
  }

  texpdf_close(pf);

  texpdf_ximage_set_form(ximage, &info, contents);

  return 0;

 error:
  WARN("Cannot parse document. Broken PDF file?");
 error_silent:
  if (resources)
    texpdf_release_obj(resources);
  if (markinfo)
    texpdf_release_obj(markinfo);
  if (page)
    texpdf_release_obj(page);
  if (contents)
    texpdf_release_obj(contents);

  texpdf_close(pf);

  return -1;

 too_recent:
  texpdf_close(pf);
  WARN("PDF version of input file more recent than in output file.");
  if (compat_mode) {
    WARN("Converting. Use \"-V\" switch to change output PDF version.");
    return 1;
  } else {
    WARN("Use \"-V\" switch to change output PDF version.");
    return -1;
  }
}

typedef enum {
  OP_SETCOLOR		= 1,
  OP_CLOSEandCLIP	= 2,
  OP_CLIP		= 3,
  OP_CONCATMATRIX	= 4,
  OP_SETCOLORSPACE	= 5,
  OP_RECTANGLE		= 6,
  OP_CURVETO		= 7,
  OP_CLOSEPATH		= 8,
  OP_LINETO		= 9,
  OP_MOVETO		= 10,
  OP_NOOP		= 11,
  OP_GSAVE		= 12,
  OP_GRESTORE		= 13,
  OP_CURVETO1		= 14,
  OP_CURVETO2		= 15,
  OP_UNKNOWN		= 16
} pdf_opcode;

static struct operator
{
  const char *token;
  int         opcode;
} pdf_operators[] = {
  {"SCN",	OP_SETCOLOR},
  {"b*",	OP_CLOSEandCLIP},
  {"B*",	OP_CLIP},
  {"cm",	OP_CONCATMATRIX},
  {"CS",	OP_SETCOLORSPACE},
  {"f*",	0},
  {"gs",	-1},
  {"re",	OP_RECTANGLE},
  {"rg",	-3},
  {"RG",	-3},
  {"sc",	OP_SETCOLOR},
  {"SC",	OP_SETCOLOR},
  {"W*",	OP_CLIP},
  {"b",		OP_CLOSEandCLIP},
  {"B",		OP_CLIP},
  {"c",		OP_CURVETO},
  {"d",		-2},
  {"f",		0},
  {"F",		0},
  {"g",		-1},
  {"G",		-1},
  {"h",		OP_CLOSEPATH},
  {"i",		-1},
  {"j",		-1},
  {"J",		-1},
  {"k",		-4},
  {"K",		-4},
  {"l",		OP_LINETO},
  {"m",		OP_MOVETO},
  {"M",		-1},
  {"n",		OP_NOOP},
  {"q",		OP_GSAVE},
  {"Q",		OP_GRESTORE},
  {"s",		OP_CLOSEandCLIP},
  {"S",		OP_CLIP},
  {"v",		OP_CURVETO1},
  {"w",		-1},
  {"W",		OP_CLIP},
  {"y",		OP_CURVETO2}
};


int
texpdf_copy_clip (pdf_doc *p, FILE *image_file, int pageNo, double x_user, double y_user)
{
  pdf_obj *page_tree, *contents;
  int depth = 0, top = -1;
  const char *clip_path, *end_path;
  char *save_path, *temp;
  pdf_tmatrix M;
  double stack[6];
  pdf_file *pf;
  
  pf = texpdf_open(NULL, image_file);
  if (!pf)
    return -1;

  texpdf_dev_currentmatrix(&M);
  texpdf_invertmatrix(&M);
  M.e += x_user; M.f += y_user;
  page_tree = texpdf_get_page_obj (pf, pageNo, NULL, NULL);
  if (!page_tree) {
    texpdf_close(pf);
    return -1;
  }

  contents = texpdf_get_page_content(page_tree);
  texpdf_release_obj(page_tree);
  if (!contents) {
    texpdf_close(pf);
    return -1;
  }

  texpdf_doc_add_page_content(p, " ", 1);

  save_path = malloc(pdf_stream_length(contents) + 1);
  strncpy(save_path, (const char *) pdf_stream_dataptr(contents),  pdf_stream_length(contents));
  clip_path = save_path;
  end_path = clip_path + pdf_stream_length(contents);
  depth = 0;

  for (; clip_path < end_path; clip_path++) {
    int color_dimen = 0;	/* silence uninitialized warning */
    char *token;
    texpdf_skip_white(&clip_path, end_path);
    if (clip_path == end_path)
      break;
    if (depth > 1) {
      if (*clip_path == 'q')
        depth++;
      if (*clip_path == 'Q')
	depth--;
      texpdf_parse_ident(&clip_path, end_path);
      continue;
    } else if (*clip_path == '-'
	    || *clip_path == '+'
	    || *clip_path == '.'
	    || isdigit((unsigned char)*clip_path)) {
      stack[++top] = strtod(clip_path, &temp);
      clip_path = temp;
    } else if (*clip_path == '[') {
      /* Ignore, but put a dummy value on the stack (in case of d operator) */
      texpdf_parse_pdf_array(&clip_path, end_path, pf);
      stack[++top] = 0;
    } else if (*clip_path == '/') {
      if  (strncmp("/DeviceGray",	clip_path, 11) == 0
	|| strncmp("/Indexed",		clip_path, 8)  == 0
	|| strncmp("/CalGray",		clip_path, 8)  == 0) {
	color_dimen = 1;
	continue;
      }
      else if  (strncmp("/DeviceRGB",	clip_path, 10) == 0
	|| strncmp("/CalRGB",		clip_path, 7)  == 0
	|| strncmp("/Lab",		clip_path, 4)  == 0) {
	color_dimen = 3;
	continue;
      }
      else if  (strncmp("/DeviceCMYK",	clip_path, 11) == 0) {
	color_dimen = 4;
	continue;
      }
      else {
        clip_path++;
        texpdf_parse_ident(&clip_path, end_path);
	texpdf_skip_white(&clip_path, end_path);
	token = texpdf_parse_ident(&clip_path, end_path);
        if (strcmp(token, "gs") == 0) {
	  continue;
	}
        return -1;
      }
    } else {
      int j;
      pdf_tmatrix T;
      pdf_coord  p0, p1, p2, p3;

      token = texpdf_parse_ident(&clip_path, end_path);
      for (j = 0; j < sizeof(pdf_operators) / sizeof(pdf_operators[0]); j++)
        if (strcmp(token, pdf_operators[j].token) == 0)
	  break;
      if (j == sizeof(pdf_operators) / sizeof(pdf_operators[0])) {
        return -1;
      }
      switch (pdf_operators[j].opcode) {
	case  0:
	case -1:
	case -2:
	case -3:
	case -4:
	  /* Just pop the stack and do nothing. */
	  top += pdf_operators[j].opcode;
	  if (top < -1)
	    return -1;
	  break;
	case OP_SETCOLOR:
	  top -= color_dimen;
	  if (top < -1)
	    return -1;
	  break;
	case OP_CLOSEandCLIP:
	  texpdf_dev_closepath();
	case OP_CLIP:
#if 0
	  texpdf_dev_clip();
#else
	  texpdf_dev_flushpath(p, 'W', PDF_FILL_RULE_NONZERO);
#endif
	  break;
	case OP_CONCATMATRIX:
	  if (top < 5)
	    return -1;
	  T.f = stack[top--];
	  T.e = stack[top--];
	  T.d = stack[top--];
	  T.c = stack[top--];
	  T.b = stack[top--];
	  T.a = stack[top--];
	  pdf_concatmatrix(&M, &T);
	  break;
	case OP_SETCOLORSPACE:
	  /* Do nothing. */
	  break;
	case OP_RECTANGLE:
	  if (top < 3)
	    return -1;
	  p1.y = stack[top--];
	  p1.x = stack[top--];
	  p0.y = stack[top--];
	  p0.x = stack[top--];
	  if (M.b == 0 && M.c == 0) {
	    pdf_tmatrix M0;
	    M0.a = M.a; M0.b = M.b; M0.c = M.c; M0.d = M.d;
	    M0.e = 0; M0.f = 0;
	    texpdf_dev_transform(&p0, &M);
	    texpdf_dev_transform(&p1, &M0);
	    texpdf_dev_rectadd(p, p0.x, p0.y, p1.x, p1.y);
	  } else {
	    p2.x = p0.x + p1.x; p2.y = p0.y + p1.y;
	    p3.x = p0.x; p3.y = p0.y + p1.y;
	    p1.x += p0.x; p1.y = p0.y;
	    texpdf_dev_transform(&p0, &M);
	    texpdf_dev_transform(&p1, &M);
	    texpdf_dev_transform(&p2, &M);
	    texpdf_dev_transform(&p3, &M);
	    texpdf_dev_moveto(p0.x, p0.y);
	    texpdf_dev_lineto(p1.x, p1.y);
	    texpdf_dev_lineto(p2.x, p2.y);
	    texpdf_dev_lineto(p3.x, p3.y);
	    texpdf_dev_closepath();
	  }
	  break;
	case OP_CURVETO:
	  if (top < 5)
	    return -1;
	  p0.y = stack[top--];
	  p0.x = stack[top--];
	  texpdf_dev_transform(&p0, &M);
	  p1.y = stack[top--];
	  p1.x = stack[top--];
	  texpdf_dev_transform(&p1, &M);
	  p2.y = stack[top--];
	  p2.x = stack[top--];
	  texpdf_dev_transform(&p2, &M);
	  texpdf_dev_curveto(p2.x, p2.y, p1.x, p1.y, p0.x, p0.y);
	  break;
	case OP_CLOSEPATH:
	  texpdf_dev_closepath();
	  break;
	case OP_LINETO:
	  if (top < 1)
	    return -1;
	  p0.y = stack[top--];
	  p0.x = stack[top--];
	  texpdf_dev_transform(&p0, &M);
	  texpdf_dev_lineto(p0.x, p0.y);
	  break;
	case OP_MOVETO:
	  if (top < 1)
	    return -1;
	  p0.y = stack[top--];
	  p0.x = stack[top--];
	  texpdf_dev_transform(&p0, &M);
	  texpdf_dev_moveto(p0.x, p0.y);
	  break;
	case OP_NOOP:
	  texpdf_doc_add_page_content(p, " n", 2);
	  break;
	case OP_GSAVE:
	  depth++;
	  break;
	case OP_GRESTORE:
	  depth--;
	  break;
	case OP_CURVETO1:
	  if (top < 3)
	    return -1;
	  p0.y = stack[top--];
	  p0.x = stack[top--];
	  texpdf_dev_transform(&p0, &M);
	  p1.y = stack[top--];
	  p1.x = stack[top--];
	  texpdf_dev_transform(&p1, &M);
	  texpdf_dev_vcurveto(p1.x, p1.y, p0.x, p0.y);
	  break;
	case OP_CURVETO2:
	  if (top < 3)
	    return -1;
	  p0.y = stack[top--];
	  p0.x = stack[top--];
	  texpdf_dev_transform(&p0, &M);
	  p1.y = stack[top--];
	  p1.x = stack[top--];
	  texpdf_dev_transform(&p1, &M);
	  texpdf_dev_ycurveto(p1.x, p1.y, p0.x, p0.y);
	  break;
	default:
	  return -1;
      }
    }
  }
  free(save_path);

  texpdf_release_obj(contents);
  texpdf_close(pf);

  return 0;
}

#if 0
#define WBUF_SIZE 4096
#if HAVE_ZLIB
static int
add_stream_flate (pdf_obj *dst, const void *data, long len)
{
  z_stream z;
  Bytef wbuf[WBUF_SIZE];

  z.zalloc = Z_NULL; z.zfree = Z_NULL; z.opaque = Z_NULL;

  z.next_in  = (z_const Bytef *) data; z.avail_in  = len;
  z.next_out = (Bytef *) wbuf; z.avail_out = WBUF_SIZE;

  if (inflateInit(&z) != Z_OK) {
    WARN("inflateInit() failed.");
    return -1;
  }

  for (;;) {
    int status;
    status = inflate(&z, Z_NO_FLUSH);
    if (status == Z_STREAM_END)
      break;
    else if (status != Z_OK) {
      WARN("inflate() failed. Broken PDF file?");
      inflateEnd(&z);
      return -1;
    }

    if (z.avail_out == 0) {
      texpdf_add_stream(dst, wbuf, WBUF_SIZE);
      z.next_out  = wbuf;
      z.avail_out = WBUF_SIZE;
    }
  }

  if (WBUF_SIZE - z.avail_out > 0)
    texpdf_add_stream(dst, wbuf, WBUF_SIZE - z.avail_out);

  return (inflateEnd(&z) == Z_OK ? 0 : -1);
}
#endif

static int
concat_stream (pdf_obj *dst, pdf_obj *src)
{
  const char *stream_data;
  long        stream_length;
  pdf_obj    *stream_dict;
  pdf_obj    *filter;

  if (!PDF_OBJ_STREAMTYPE(dst) || !PDF_OBJ_STREAMTYPE(src))
    ERROR("Invalid type.");

  stream_data   = pdf_stream_dataptr(src);
  stream_length = pdf_stream_length (src);
  stream_dict   = texpdf_stream_dict   (src);

  if (texpdf_lookup_dict(stream_dict, "DecodeParms")) {
    WARN("DecodeParams not supported.");
    return -1;
  }

  filter = texpdf_lookup_dict(stream_dict, "Filter");
  if (!filter) {
    texpdf_add_stream(dst, stream_data, stream_length);
    return 0;
#if HAVE_ZLIB
  } else {
    char *filter_name;
    if (PDF_OBJ_NAMETYPE(filter)) {
      filter_name = texpdf_name_value(filter);
      if (filter_name && !strcmp(filter_name, "FlateDecode"))
	return add_stream_flate(dst, stream_data, stream_length);
      else {
	WARN("DecodeFilter \"%s\" not supported.", filter_name);
	return -1;
      }
    } else if (PDF_OBJ_ARRAYTYPE(filter)) {
      if (texpdf_array_length(filter) > 1) {
	WARN("Multiple DecodeFilter not supported.");
	return -1;
      } else {
	filter_name = texpdf_name_value(texpdf_get_array(filter, 0));
	if (filter_name && !strcmp(filter_name, "FlateDecode"))
	  return add_stream_flate(dst, stream_data, stream_length);
	else {
	  WARN("DecodeFilter \"%s\" not supported.", filter_name);
	  return -1;
	}
      }
    } else
      ERROR("Broken PDF file?");
#endif /* HAVE_ZLIB */
  }

  return -1;
}
#endif
