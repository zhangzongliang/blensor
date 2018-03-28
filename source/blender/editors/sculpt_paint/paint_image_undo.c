/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/editors/sculpt_paint/paint_image_undo.c
 *  \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "BLI_math.h"
#include "BLI_blenlib.h"
#include "BLI_utildefines.h"
#include "BLI_threads.h"

#include "DNA_image_types.h"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "BKE_context.h"
#include "BKE_depsgraph.h"
#include "BKE_image.h"
#include "BKE_main.h"

#include "ED_paint.h"

#include "GPU_draw.h"

#include "paint_intern.h"

typedef struct UndoImageTile {
	struct UndoImageTile *next, *prev;

	char idname[MAX_ID_NAME];  /* name instead of pointer*/
	char ibufname[IMB_FILENAME_SIZE];

	union {
		float        *fp;
		unsigned int *uint;
		void         *pt;
	} rect;

	unsigned short *mask;

	int x, y;

	Image *ima;
	short source, use_float;
	char gen_type;
	bool valid;
} UndoImageTile;

/* this is a static resource for non-globality,
 * Maybe it should be exposed as part of the
 * paint operation, but for now just give a public interface */
static SpinLock undolock;

void image_undo_init_locks(void)
{
	BLI_spin_init(&undolock);
}

void image_undo_end_locks(void)
{
	BLI_spin_end(&undolock);
}

/* UNDO */
typedef enum {
	COPY = 0,
	RESTORE = 1,
	RESTORE_COPY = 2
} CopyMode;

static void undo_copy_tile(UndoImageTile *tile, ImBuf *tmpibuf, ImBuf *ibuf, CopyMode mode)
{
	if (mode == COPY) {
		/* copy or swap contents of tile->rect and region in ibuf->rect */
		IMB_rectcpy(tmpibuf, ibuf, 0, 0, tile->x * IMAPAINT_TILE_SIZE,
		            tile->y * IMAPAINT_TILE_SIZE, IMAPAINT_TILE_SIZE, IMAPAINT_TILE_SIZE);

		if (ibuf->rect_float) {
			SWAP(float *, tmpibuf->rect_float, tile->rect.fp);
		}
		else {
			SWAP(unsigned int *, tmpibuf->rect, tile->rect.uint);
		}
	}
	else {
		if (mode == RESTORE_COPY) {
			IMB_rectcpy(tmpibuf, ibuf, 0, 0, tile->x * IMAPAINT_TILE_SIZE,
			            tile->y * IMAPAINT_TILE_SIZE, IMAPAINT_TILE_SIZE, IMAPAINT_TILE_SIZE);
		}
		/* swap to the tmpbuf for easy copying */
		if (ibuf->rect_float) {
			SWAP(float *, tmpibuf->rect_float, tile->rect.fp);
		}
		else {
			SWAP(unsigned int *, tmpibuf->rect, tile->rect.uint);
		}

		IMB_rectcpy(ibuf, tmpibuf, tile->x * IMAPAINT_TILE_SIZE,
		            tile->y * IMAPAINT_TILE_SIZE, 0, 0, IMAPAINT_TILE_SIZE, IMAPAINT_TILE_SIZE);

		if (mode == RESTORE) {
			if (ibuf->rect_float) {
				SWAP(float *, tmpibuf->rect_float, tile->rect.fp);
			}
			else {
				SWAP(unsigned int *, tmpibuf->rect, tile->rect.uint);
			}
		}
	}
}

void *image_undo_find_tile(Image *ima, ImBuf *ibuf, int x_tile, int y_tile, unsigned short **mask, bool validate)
{
	ListBase *lb = undo_paint_push_get_list(UNDO_PAINT_IMAGE);
	UndoImageTile *tile;
	short use_float = ibuf->rect_float ? 1 : 0;

	for (tile = lb->first; tile; tile = tile->next) {
		if (tile->x == x_tile && tile->y == y_tile && ima->gen_type == tile->gen_type && ima->source == tile->source) {
			if (tile->use_float == use_float) {
				if (STREQ(tile->idname, ima->id.name) && STREQ(tile->ibufname, ibuf->name)) {
					if (mask) {
						/* allocate mask if requested */
						if (!tile->mask) {
							tile->mask = MEM_callocN(sizeof(unsigned short) * IMAPAINT_TILE_SIZE * IMAPAINT_TILE_SIZE,
							                         "UndoImageTile.mask");
						}

						*mask = tile->mask;
					}
					if (validate) {
						tile->valid = true;
					}
					return tile->rect.pt;
				}
			}
		}
	}

	return NULL;
}

void *image_undo_push_tile(
        Image *ima, ImBuf *ibuf, ImBuf **tmpibuf, int x_tile, int y_tile,
        unsigned short **mask, bool **valid, bool proj, bool find_prev)
{
	ListBase *lb = undo_paint_push_get_list(UNDO_PAINT_IMAGE);
	UndoImageTile *tile;
	int allocsize;
	short use_float = ibuf->rect_float ? 1 : 0;
	void *data;

	/* check if tile is already pushed */

	/* in projective painting we keep accounting of tiles, so if we need one pushed, just push! */
	if (find_prev) {
		data = image_undo_find_tile(ima, ibuf, x_tile, y_tile, mask, true);
		if (data) {
			return data;
		}
	}

	if (*tmpibuf == NULL) {
		*tmpibuf = IMB_allocImBuf(IMAPAINT_TILE_SIZE, IMAPAINT_TILE_SIZE, 32, IB_rectfloat | IB_rect);
	}

	tile = MEM_callocN(sizeof(UndoImageTile), "UndoImageTile");
	BLI_strncpy(tile->idname, ima->id.name, sizeof(tile->idname));
	tile->x = x_tile;
	tile->y = y_tile;

	/* add mask explicitly here */
	if (mask) {
		*mask = tile->mask = MEM_callocN(sizeof(unsigned short) * IMAPAINT_TILE_SIZE * IMAPAINT_TILE_SIZE,
		                         "UndoImageTile.mask");
	}
	allocsize = IMAPAINT_TILE_SIZE * IMAPAINT_TILE_SIZE * 4;
	allocsize *= (ibuf->rect_float) ? sizeof(float) : sizeof(char);
	tile->rect.pt = MEM_mapallocN(allocsize, "UndeImageTile.rect");

	BLI_strncpy(tile->ibufname, ibuf->name, sizeof(tile->ibufname));

	tile->gen_type = ima->gen_type;
	tile->source = ima->source;
	tile->use_float = use_float;
	tile->valid = true;
	tile->ima = ima;

	if (valid) {
		*valid = &tile->valid;
	}
	undo_copy_tile(tile, *tmpibuf, ibuf, COPY);

	if (proj) {
		BLI_spin_lock(&undolock);
	}
	undo_paint_push_count_alloc(UNDO_PAINT_IMAGE, allocsize);
	BLI_addtail(lb, tile);

	if (proj) {
		BLI_spin_unlock(&undolock);
	}
	return tile->rect.pt;
}

void image_undo_remove_masks(void)
{
	ListBase *lb = undo_paint_push_get_list(UNDO_PAINT_IMAGE);
	UndoImageTile *tile;

	for (tile = lb->first; tile; tile = tile->next) {
		if (tile->mask) {
			MEM_freeN(tile->mask);
			tile->mask = NULL;
		}
	}
}

static void image_undo_restore_runtime(ListBase *lb)
{
	ImBuf *ibuf, *tmpibuf;
	UndoImageTile *tile;

	tmpibuf = IMB_allocImBuf(IMAPAINT_TILE_SIZE, IMAPAINT_TILE_SIZE, 32,
	                         IB_rectfloat | IB_rect);

	for (tile = lb->first; tile; tile = tile->next) {
		Image *ima = tile->ima;
		ibuf = BKE_image_acquire_ibuf(ima, NULL, NULL);

		undo_copy_tile(tile, tmpibuf, ibuf, RESTORE);

		GPU_free_image(ima); /* force OpenGL reload (maybe partial update will operate better?) */
		if (ibuf->rect_float) {
			ibuf->userflags |= IB_RECT_INVALID; /* force recreate of char rect */
		}
		if (ibuf->mipmap[0]) {
			ibuf->userflags |= IB_MIPMAP_INVALID;  /* force mipmap recreatiom */
		}
		ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;

		BKE_image_release_ibuf(ima, ibuf, NULL);
	}

	IMB_freeImBuf(tmpibuf);
}

static void image_undo_restore_list(bContext *C, ListBase *lb)
{
	Main *bmain = CTX_data_main(C);
	Image *ima = NULL;
	ImBuf *ibuf, *tmpibuf;
	UndoImageTile *tile;

	tmpibuf = IMB_allocImBuf(IMAPAINT_TILE_SIZE, IMAPAINT_TILE_SIZE, 32,
	                         IB_rectfloat | IB_rect);

	for (tile = lb->first; tile; tile = tile->next) {
		short use_float;

		/* find image based on name, pointer becomes invalid with global undo */
		if (ima && STREQ(tile->idname, ima->id.name)) {
			/* ima is valid */
		}
		else {
			ima = BLI_findstring(&bmain->image, tile->idname, offsetof(ID, name));
		}

		ibuf = BKE_image_acquire_ibuf(ima, NULL, NULL);

		if (ima && ibuf && !STREQ(tile->ibufname, ibuf->name)) {
			/* current ImBuf filename was changed, probably current frame
			 * was changed when painting on image sequence, rather than storing
			 * full image user (which isn't so obvious, btw) try to find ImBuf with
			 * matched file name in list of already loaded images */

			BKE_image_release_ibuf(ima, ibuf, NULL);

			ibuf = BKE_image_get_ibuf_with_name(ima, tile->ibufname);
		}

		if (!ima || !ibuf || !(ibuf->rect || ibuf->rect_float)) {
			BKE_image_release_ibuf(ima, ibuf, NULL);
			continue;
		}

		if (ima->gen_type != tile->gen_type || ima->source != tile->source) {
			BKE_image_release_ibuf(ima, ibuf, NULL);
			continue;
		}

		use_float = ibuf->rect_float ? 1 : 0;

		if (use_float != tile->use_float) {
			BKE_image_release_ibuf(ima, ibuf, NULL);
			continue;
		}

		undo_copy_tile(tile, tmpibuf, ibuf, RESTORE_COPY);

		GPU_free_image(ima); /* force OpenGL reload */
		if (ibuf->rect_float) {
			ibuf->userflags |= IB_RECT_INVALID; /* force recreate of char rect */
		}
		if (ibuf->mipmap[0]) {
			ibuf->userflags |= IB_MIPMAP_INVALID;  /* force mipmap recreatiom */
		}
		ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;

		DAG_id_tag_update(&ima->id, 0);

		BKE_image_release_ibuf(ima, ibuf, NULL);
	}

	IMB_freeImBuf(tmpibuf);
}

static void image_undo_free_list(ListBase *lb)
{
	UndoImageTile *tile;

	for (tile = lb->first; tile; tile = tile->next) {
		MEM_freeN(tile->rect.pt);
	}
}

void ED_image_undo_push_begin(const char *name)
{
	ED_undo_paint_push_begin(UNDO_PAINT_IMAGE, name, image_undo_restore_list, image_undo_free_list, NULL);
}

void ED_image_undo_push_end(void)
{
	ListBase *lb = undo_paint_push_get_list(UNDO_PAINT_IMAGE);
	UndoImageTile *tile;
	int deallocsize = 0;
	int allocsize = IMAPAINT_TILE_SIZE * IMAPAINT_TILE_SIZE * 4;

	/* first dispose of invalid tiles (may happen due to drag dot for instance) */
	for (tile = lb->first; tile;) {
		if (!tile->valid) {
			UndoImageTile *tmp_tile = tile->next;
			deallocsize += allocsize * ((tile->use_float) ? sizeof(float) : sizeof(char));
			MEM_freeN(tile->rect.pt);
			BLI_freelinkN(lb, tile);
			tile = tmp_tile;
		}
		else {
			tile = tile->next;
		}
	}

	/* don't forget to remove the size of deallocated tiles */
	undo_paint_push_count_alloc(UNDO_PAINT_IMAGE, -deallocsize);

	ED_undo_paint_push_end(UNDO_PAINT_IMAGE);
}

static void image_undo_invalidate(void)
{
	UndoImageTile *tile;
	ListBase *lb = undo_paint_push_get_list(UNDO_PAINT_IMAGE);

	for (tile = lb->first; tile; tile = tile->next) {
		tile->valid = false;
	}
}

/* restore painting image to previous state. Used for anchored and drag-dot style brushes*/
void ED_image_undo_restore(void)
{
	ListBase *lb = undo_paint_push_get_list(UNDO_PAINT_IMAGE);
	image_undo_restore_runtime(lb);
	image_undo_invalidate();
}