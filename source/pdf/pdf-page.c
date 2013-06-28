#include "mupdf/pdf.h"

int
pdf_count_pages(pdf_document *doc)
{
	if (doc->page_count == 0)
	{
		pdf_obj *count = pdf_dict_getp(pdf_trailer(doc), "Root/Pages/Count");
		doc->page_count = pdf_to_int(count);
	}
	return doc->page_count;
}

static pdf_obj *
pdf_lookup_page_loc_imp(pdf_document *doc, pdf_obj *node, int *skip, pdf_obj **parentp, int *indexp)
{
	fz_context *ctx = doc->ctx;
	pdf_obj *kids, *hit;
	int i, len, count;

	count = pdf_to_int(pdf_dict_gets(node, "Count"));
	if (*skip > count)
	{
		/* The princess is in another castle. */
		*skip -= count;
		return NULL;
	}

	kids = pdf_dict_gets(node, "Kids");
	len = pdf_array_len(kids);

	if (pdf_mark_obj(node))
		fz_throw(ctx, FZ_ERROR_GENERIC, "cycle in page tree");

	hit = NULL;
	fz_var(hit);

	fz_try(ctx)
	{
		for (i = 0; i < len; i++)
		{
			pdf_obj *kid = pdf_array_get(kids, i);
			char *type = pdf_to_name(pdf_dict_gets(kid, "Type"));
			if (!strcmp(type, "Page"))
			{
				if (*skip == 0)
				{
					if (parentp) *parentp = node;
					if (indexp) *indexp = i;
					hit = kid;
					break;
				}
				else
				{
					(*skip)--;
				}
			}
			else if (!strcmp(type, "Pages"))
			{
				hit = pdf_lookup_page_loc_imp(doc, kid, skip, parentp, indexp);
				if (hit)
					break;
			}
			else
			{
				fz_throw(ctx, FZ_ERROR_GENERIC, "non-page object in page tree");
			}
		}
	}
	fz_always(ctx)
	{
		pdf_unmark_obj(node);
	}
	fz_catch(ctx)
	{
		fz_rethrow(ctx);
	}

	return hit;
}

pdf_obj *
pdf_lookup_page_loc(pdf_document *doc, int needle, pdf_obj **parentp, int *indexp)
{
	pdf_obj *root = pdf_dict_gets(pdf_trailer(doc), "Root");
	pdf_obj *node = pdf_dict_gets(root, "Pages");
	int skip = needle;
	pdf_obj *hit = pdf_lookup_page_loc_imp(doc, node, &skip, parentp, indexp);
	if (!hit)
		fz_throw(doc->ctx, FZ_ERROR_GENERIC, "cannot find page %d in page tree", needle);
	return hit;
}

pdf_obj *
pdf_lookup_page_obj(pdf_document *doc, int needle)
{
	return pdf_lookup_page_loc(doc, needle, NULL, NULL);
}

static int
pdf_count_pages_before_kid(pdf_document *doc, pdf_obj *parent, int kid_num)
{
	pdf_obj *count, *kid, *kids = pdf_dict_gets(parent, "Kids");
	int i, total = 0, len = pdf_array_len(kids);
	for (i = 0; i < len; i++)
	{
		kid = pdf_array_get(kids, i);
		if (pdf_to_num(kid) == kid_num)
			return total;
		count = pdf_dict_gets(kid, "Count");
		if (count)
			total += pdf_to_int(count);
		else
			total++;
	}
	fz_throw(doc->ctx, FZ_ERROR_GENERIC, "kid not found in parent's kids array");
}

int
pdf_lookup_page_number(pdf_document *doc, pdf_obj *node)
{
	int needle = pdf_to_num(node);
	int total = 0;
	int depth = 0;
	pdf_obj *parent;

	parent = pdf_dict_gets(node, "Parent");
	while (parent)
	{
		total += pdf_count_pages_before_kid(doc, parent, needle);
		needle = pdf_to_num(parent);
		parent = pdf_dict_gets(parent, "Parent");
		if (++depth > 100)
			fz_throw(doc->ctx, FZ_ERROR_GENERIC, "page tree is too deep");
	}

	return total;
}

static pdf_obj *
pdf_lookup_inherited_page_item(pdf_document *doc, pdf_obj *node, const char *key)
{
	int depth = 0;

	pdf_obj *val = pdf_dict_gets(node, key);
	if (val)
		return val;

	node = pdf_dict_gets(node, "Parent");
	while (node)
	{
		val = pdf_dict_gets(node, key);
		if (val)
			return val;
		node = pdf_dict_gets(node, "Parent");
		if (++depth > 100)
			fz_throw(doc->ctx, FZ_ERROR_GENERIC, "page tree is too deep");
	}

	return NULL;
}

/* We need to know whether to install a page-level transparency group */

static int pdf_resources_use_blending(pdf_document *doc, pdf_obj *rdb);

static int
pdf_extgstate_uses_blending(pdf_document *doc, pdf_obj *dict)
{
	pdf_obj *obj = pdf_dict_gets(dict, "BM");
	if (pdf_is_name(obj) && strcmp(pdf_to_name(obj), "Normal"))
		return 1;
	return 0;
}

static int
pdf_pattern_uses_blending(pdf_document *doc, pdf_obj *dict)
{
	pdf_obj *obj;
	obj = pdf_dict_gets(dict, "Resources");
	if (pdf_resources_use_blending(doc, obj))
		return 1;
	obj = pdf_dict_gets(dict, "ExtGState");
	return pdf_extgstate_uses_blending(doc, obj);
}

static int
pdf_xobject_uses_blending(pdf_document *doc, pdf_obj *dict)
{
	pdf_obj *obj = pdf_dict_gets(dict, "Resources");
	return pdf_resources_use_blending(doc, obj);
}

static int
pdf_resources_use_blending(pdf_document *doc, pdf_obj *rdb)
{
	fz_context *ctx = doc->ctx;
	pdf_obj *obj;
	int i, n, useBM = 0;

	if (!rdb)
		return 0;

	/* Have we been here before and remembered an answer? */
	if (pdf_obj_memo(rdb, &useBM))
		return useBM;

	/* stop on cyclic resource dependencies */
	if (pdf_mark_obj(rdb))
		return 0;

	fz_try(ctx)
	{
		obj = pdf_dict_gets(rdb, "ExtGState");
		n = pdf_dict_len(obj);
		for (i = 0; i < n; i++)
			if (pdf_extgstate_uses_blending(doc, pdf_dict_get_val(obj, i)))
				goto found;

		obj = pdf_dict_gets(rdb, "Pattern");
		n = pdf_dict_len(obj);
		for (i = 0; i < n; i++)
			if (pdf_pattern_uses_blending(doc, pdf_dict_get_val(obj, i)))
				goto found;

		obj = pdf_dict_gets(rdb, "XObject");
		n = pdf_dict_len(obj);
		for (i = 0; i < n; i++)
			if (pdf_xobject_uses_blending(doc, pdf_dict_get_val(obj, i)))
				goto found;
		if (0)
		{
found:
			useBM = 1;
		}
	}
	fz_always(ctx)
	{
		pdf_unmark_obj(rdb);
	}
	fz_catch(ctx)
	{
		fz_rethrow(ctx);
	}

	pdf_set_obj_memo(rdb, useBM);
	return useBM;
}

static void
pdf_load_transition(pdf_document *doc, pdf_page *page, pdf_obj *transdict)
{
	char *name;
	pdf_obj *obj;
	int type;

	obj = pdf_dict_gets(transdict, "D");
	page->transition.duration = (obj ? pdf_to_real(obj) : 1);

	page->transition.vertical = (pdf_to_name(pdf_dict_gets(transdict, "Dm"))[0] != 'H');
	page->transition.outwards = (pdf_to_name(pdf_dict_gets(transdict, "M"))[0] != 'I');
	/* FIXME: If 'Di' is None, it should be handled differently, but
	 * this only affects Fly, and we don't implement that currently. */
	page->transition.direction = (pdf_to_int(pdf_dict_gets(transdict, "Di")));
	/* FIXME: Read SS for Fly when we implement it */
	/* FIXME: Read B for Fly when we implement it */

	name = pdf_to_name(pdf_dict_gets(transdict, "S"));
	if (!strcmp(name, "Split"))
		type = FZ_TRANSITION_SPLIT;
	else if (!strcmp(name, "Blinds"))
		type = FZ_TRANSITION_BLINDS;
	else if (!strcmp(name, "Box"))
		type = FZ_TRANSITION_BOX;
	else if (!strcmp(name, "Wipe"))
		type = FZ_TRANSITION_WIPE;
	else if (!strcmp(name, "Dissolve"))
		type = FZ_TRANSITION_DISSOLVE;
	else if (!strcmp(name, "Glitter"))
		type = FZ_TRANSITION_GLITTER;
	else if (!strcmp(name, "Fly"))
		type = FZ_TRANSITION_FLY;
	else if (!strcmp(name, "Push"))
		type = FZ_TRANSITION_PUSH;
	else if (!strcmp(name, "Cover"))
		type = FZ_TRANSITION_COVER;
	else if (!strcmp(name, "Uncover"))
		type = FZ_TRANSITION_UNCOVER;
	else if (!strcmp(name, "Fade"))
		type = FZ_TRANSITION_FADE;
	else
		type = FZ_TRANSITION_NONE;
	page->transition.type = type;
}

pdf_page *
pdf_load_page(pdf_document *doc, int number)
{
	fz_context *ctx = doc->ctx;
	pdf_page *page;
	pdf_annot *annot;
	pdf_obj *pageobj, *pageref, *obj;
	fz_rect mediabox, cropbox, realbox;
	float userunit;
	fz_matrix mat;

	pageref = pdf_lookup_page_obj(doc, number);
	pageobj = pdf_resolve_indirect(pageref);

	page = fz_malloc_struct(ctx, pdf_page);
	page->resources = NULL;
	page->contents = NULL;
	page->transparency = 0;
	page->links = NULL;
	page->annots = NULL;
	page->deleted_annots = NULL;
	page->tmp_annots = NULL;
	page->me = pdf_keep_obj(pageobj);

	obj = pdf_dict_gets(pageobj, "UserUnit");
	if (pdf_is_real(obj))
		userunit = pdf_to_real(obj);
	else
		userunit = 1;

	pdf_to_rect(ctx, pdf_lookup_inherited_page_item(doc, pageobj, "MediaBox"), &mediabox);
	if (fz_is_empty_rect(&mediabox))
	{
		fz_warn(ctx, "cannot find page size for page %d", number + 1);
		mediabox.x0 = 0;
		mediabox.y0 = 0;
		mediabox.x1 = 612;
		mediabox.y1 = 792;
	}

	pdf_to_rect(ctx, pdf_lookup_inherited_page_item(doc, pageobj, "CropBox"), &cropbox);
	if (!fz_is_empty_rect(&cropbox))
		fz_intersect_rect(&mediabox, &cropbox);

	page->mediabox.x0 = fz_min(mediabox.x0, mediabox.x1) * userunit;
	page->mediabox.y0 = fz_min(mediabox.y0, mediabox.y1) * userunit;
	page->mediabox.x1 = fz_max(mediabox.x0, mediabox.x1) * userunit;
	page->mediabox.y1 = fz_max(mediabox.y0, mediabox.y1) * userunit;

	if (page->mediabox.x1 - page->mediabox.x0 < 1 || page->mediabox.y1 - page->mediabox.y0 < 1)
	{
		fz_warn(ctx, "invalid page size in page %d", number + 1);
		page->mediabox = fz_unit_rect;
	}

	page->rotate = pdf_to_int(pdf_lookup_inherited_page_item(doc, pageobj, "Rotate"));
	/* Snap page->rotate to 0, 90, 180 or 270 */
	if (page->rotate < 0)
		page->rotate = 360 - ((-page->rotate) % 360);
	if (page->rotate >= 360)
		page->rotate = page->rotate % 360;
	page->rotate = 90*((page->rotate + 45)/90);
	if (page->rotate > 360)
		page->rotate = 0;

	fz_pre_rotate(fz_scale(&page->ctm, 1, -1), -page->rotate);
	realbox = page->mediabox;
	fz_transform_rect(&realbox, &page->ctm);
	fz_pre_scale(fz_translate(&mat, -realbox.x0, -realbox.y0), userunit, userunit);
	fz_concat(&page->ctm, &page->ctm, &mat);

	obj = pdf_dict_gets(pageobj, "Annots");
	if (obj)
	{
		page->links = pdf_load_link_annots(doc, obj, &page->ctm);
		page->annots = pdf_load_annots(doc, obj, page);
	}

	page->duration = pdf_to_real(pdf_dict_gets(pageobj, "Dur"));

	obj = pdf_dict_gets(pageobj, "Trans");
	page->transition_present = (obj != NULL);
	if (obj)
	{
		pdf_load_transition(doc, page, obj);
	}

	// TODO: inherit
	page->resources = pdf_lookup_inherited_page_item(doc, pageobj, "Resources");
	if (page->resources)
		pdf_keep_obj(page->resources);

	obj = pdf_dict_gets(pageobj, "Contents");
	fz_try(ctx)
	{
		page->contents = pdf_keep_obj(obj);

		if (pdf_resources_use_blending(doc, page->resources))
			page->transparency = 1;

		for (annot = page->annots; annot && !page->transparency; annot = annot->next)
			if (annot->ap && pdf_resources_use_blending(doc, annot->ap->resources))
				page->transparency = 1;
	}
	fz_catch(ctx)
	{
		pdf_free_page(doc, page);
		fz_rethrow_message(ctx, "cannot load page %d contents (%d 0 R)", number + 1, pdf_to_num(pageref));
	}

	return page;
}

fz_rect *
pdf_bound_page(pdf_document *doc, pdf_page *page, fz_rect *bounds)
{
	fz_matrix mtx;
	fz_rect mediabox = page->mediabox;
	fz_transform_rect(&mediabox, fz_rotate(&mtx, page->rotate));
	bounds->x0 = bounds->y0 = 0;
	bounds->x1 = mediabox.x1 - mediabox.x0;
	bounds->y1 = mediabox.y1 - mediabox.y0;
	return bounds;
}

fz_link *
pdf_load_links(pdf_document *doc, pdf_page *page)
{
	return fz_keep_link(doc->ctx, page->links);
}

void
pdf_free_page(pdf_document *doc, pdf_page *page)
{
	if (page == NULL)
		return;
	pdf_drop_obj(page->resources);
	pdf_drop_obj(page->contents);
	if (page->links)
		fz_drop_link(doc->ctx, page->links);
	if (page->annots)
		pdf_free_annot(doc->ctx, page->annots);
	if (page->deleted_annots)
		pdf_free_annot(doc->ctx, page->deleted_annots);
	if (page->tmp_annots)
		pdf_free_annot(doc->ctx, page->tmp_annots);
	/* doc->focus, when not NULL, refers to one of
	 * the annotations and must be NULLed when the
	 * annotations are destroyed. doc->focus_obj
	 * keeps track of the actual annotation object. */
	doc->focus = NULL;
	pdf_drop_obj(page->me);
	fz_free(doc->ctx, page);
}

void
pdf_delete_page(pdf_document *doc, int at)
{
	pdf_obj *parent, *kids;
	int i;

	pdf_lookup_page_loc(doc, at, &parent, &i);
	kids = pdf_dict_gets(parent, "Kids");
	pdf_array_delete(kids, i);

	while (parent)
	{
		int count = pdf_to_int(pdf_dict_gets(parent, "Count"));
		pdf_dict_puts_drop(parent, "Count", pdf_new_int(doc, count - 1));
		parent = pdf_dict_gets(parent, "Parent");
	}
}

void
pdf_insert_page(pdf_document *doc, pdf_page *page, int at)
{
	int count = pdf_count_pages(doc);
	pdf_obj *parent, *kids;
	int i;

	if (count == 0)
	{
		/* TODO: create new page tree? */
		fz_throw(doc->ctx, FZ_ERROR_GENERIC, "empty page tree, cannot insert page");
	}
	else if (at >= count)
	{
		if (at > count)
			fz_throw(doc->ctx, FZ_ERROR_GENERIC, "cannot insert page beyond end of page tree");

		/* append after last page */
		pdf_lookup_page_loc(doc, count - 1, &parent, &i);
		kids = pdf_dict_gets(parent, "Kids");
		pdf_array_insert_drop(kids, pdf_new_ref(doc, page->me), i + 1);
	}
	else
	{
		/* insert before found page */
		pdf_lookup_page_loc(doc, at, &parent, &i);
		kids = pdf_dict_gets(parent, "Kids");
		pdf_array_insert_drop(kids, pdf_new_ref(doc, page->me), i);
	}

	/* Adjust page counts */
	while (parent)
	{
		int count = pdf_to_int(pdf_dict_gets(parent, "Count"));
		pdf_dict_puts_drop(parent, "Count", pdf_new_int(doc, count + 1));
		parent = pdf_dict_gets(parent, "Parent");
	}
}

void
pdf_delete_page_range(pdf_document *doc, int start, int end)
{
	while (start < end)
		pdf_delete_page(doc, start++);
}

pdf_page *
pdf_create_page(pdf_document *doc, fz_rect mediabox, int res, int rotate)
{
	pdf_page *page = NULL;
	pdf_obj *pageobj, *obj;
	float userunit = 1;
	fz_context *ctx = doc->ctx;
	fz_matrix ctm, tmp;
	fz_rect realbox;

	page = fz_malloc_struct(ctx, pdf_page);
	obj = NULL;
	fz_var(obj);

	fz_try(ctx)
	{
		page->resources = NULL;
		page->contents = NULL;
		page->transparency = 0;
		page->links = NULL;
		page->annots = NULL;
		page->me = pageobj = pdf_new_dict(doc, 4);

		pdf_dict_puts_drop(pageobj, "Type", pdf_new_name(doc, "Page"));

		page->mediabox.x0 = fz_min(mediabox.x0, mediabox.x1) * userunit;
		page->mediabox.y0 = fz_min(mediabox.y0, mediabox.y1) * userunit;
		page->mediabox.x1 = fz_max(mediabox.x0, mediabox.x1) * userunit;
		page->mediabox.y1 = fz_max(mediabox.y0, mediabox.y1) * userunit;
		pdf_dict_puts_drop(pageobj, "MediaBox", pdf_new_rect(doc, &page->mediabox));

		/* Snap page->rotate to 0, 90, 180 or 270 */
		if (page->rotate < 0)
			page->rotate = 360 - ((-page->rotate) % 360);
		if (page->rotate >= 360)
			page->rotate = page->rotate % 360;
		page->rotate = 90*((page->rotate + 45)/90);
		if (page->rotate > 360)
			page->rotate = 0;
		pdf_dict_puts_drop(pageobj, "Rotate", pdf_new_int(doc, page->rotate));

		fz_pre_rotate(fz_scale(&ctm, 1, -1), -page->rotate);
		realbox = page->mediabox;
		fz_transform_rect(&realbox, &ctm);
		fz_pre_scale(fz_translate(&tmp, -realbox.x0, -realbox.y0), userunit, userunit);
		fz_concat(&ctm, &ctm, &tmp);
		page->ctm = ctm;
		obj = pdf_new_dict(doc, 4);
		page->contents = pdf_new_ref(doc, obj);
		pdf_drop_obj(obj);
		obj = NULL;
		pdf_dict_puts(pageobj, "Contents", page->contents);
	}
	fz_catch(ctx)
	{
		pdf_drop_obj(page->me);
		pdf_drop_obj(obj);
		fz_free(ctx, page);
		fz_rethrow_message(ctx, "Failed to create page");
	}

	return page;
}