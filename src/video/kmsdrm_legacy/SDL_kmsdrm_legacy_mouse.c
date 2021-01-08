/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2021 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_KMSDRM

#include "SDL_kmsdrm_legacy_video.h"
#include "SDL_kmsdrm_legacy_mouse.h"
#include "SDL_kmsdrm_legacy_dyn.h"

#include "../../events/SDL_mouse_c.h"
#include "../../events/default_cursor.h"

static SDL_Cursor *KMSDRM_LEGACY_CreateDefaultCursor(void);
static SDL_Cursor *KMSDRM_LEGACY_CreateCursor(SDL_Surface * surface, int hot_x, int hot_y);
static int KMSDRM_LEGACY_ShowCursor(SDL_Cursor * cursor);
static void KMSDRM_LEGACY_MoveCursor(SDL_Cursor * cursor);
static void KMSDRM_LEGACY_FreeCursor(SDL_Cursor * cursor);
static void KMSDRM_LEGACY_WarpMouse(SDL_Window * window, int x, int y);
static int KMSDRM_LEGACY_WarpMouseGlobal(int x, int y);

/**************************************************************************************/
/* BEFORE CODING ANYTHING MOUSE/CURSOR RELATED, REMEMBER THIS.                        */
/* How does SDL manage cursors internally? First, mouse =! cursor. The mouse can have */
/* many cursors in mouse->cursors.                                                    */
/* -SDL tells us to create a cursor with KMSDRM_CreateCursor(). It can create many    */
/*  cursosr with this, not only one.                                                  */
/* -SDL stores those cursors in a cursors array, in mouse->cursors.                   */
/* -Whenever it wants (or the programmer wants) takes a cursor from that array        */
/*  and shows it on screen with KMSDRM_ShowCursor().                                  */
/*  KMSDRM_ShowCursor() simply shows or hides the cursor it receives: it does NOT     */
/*  mind if it's mouse->cur_cursor, etc.                                              */
/* -If KMSDRM_ShowCursor() returns succesfully, that cursor becomes mouse->cur_cursor */
/*  and mouse->cursor_shown is 1.                                                     */
/**************************************************************************************/

static SDL_Cursor *
KMSDRM_LEGACY_CreateDefaultCursor(void)
{
    return SDL_CreateCursor(default_cdata, default_cmask, DEFAULT_CWIDTH, DEFAULT_CHEIGHT, DEFAULT_CHOTX, DEFAULT_CHOTY);
}

/* Converts a pixel from straight-alpha [AA, RR, GG, BB], which the SDL cursor surface has,
   to premultiplied-alpha [AA. AA*RR, AA*GG, AA*BB].
   These multiplications have to be done with floats instead of uint32_t's,
   and the resulting values have to be converted to be relative to the 0-255 interval,
   where 255 is 1.00 and anything between 0 and 255 is 0.xx. */
void legacy_alpha_premultiply_ARGB8888 (uint32_t *pixel) {

    uint32_t A, R, G, B;

    /* Component bytes extraction. */
    A = (*pixel >> (3 << 3)) & 0xFF;
    R = (*pixel >> (2 << 3)) & 0xFF;
    G = (*pixel >> (1 << 3)) & 0xFF;
    B = (*pixel >> (0 << 3)) & 0xFF;

    /* Alpha pre-multiplication of each component. */
    R = (float)A * ((float)R /255);
    G = (float)A * ((float)G /255);
    B = (float)A * ((float)B /255);

    /* ARGB8888 pixel recomposition. */
    (*pixel) = (((uint32_t)A << 24) | ((uint32_t)R << 16) | ((uint32_t)G << 8)) | ((uint32_t)B << 0);
}

/* This simply gets the cursor soft-buffer ready.
   We don't copy it to a GBO BO until ShowCursor() because the cusor GBM BO (living
   in dispata) is destroyed and recreated when we recreate windows, etc. */
static SDL_Cursor *
KMSDRM_LEGACY_CreateCursor(SDL_Surface * surface, int hot_x, int hot_y)
{
    KMSDRM_LEGACY_CursorData *curdata;
    SDL_Cursor *cursor, *ret;

    curdata = NULL;
    ret = NULL;

    /* All code below assumes ARGB8888 format for the cursor surface,
       like other backends do. Also, the GBM BO pixels have to be
       alpha-premultiplied, but the SDL surface we receive has
       straight-alpha pixels, so we always have to convert. */ 
    SDL_assert(surface->format->format == SDL_PIXELFORMAT_ARGB8888);
    SDL_assert(surface->pitch == surface->w * 4);

    cursor = (SDL_Cursor *) SDL_calloc(1, sizeof(*cursor));
    if (!cursor) {
        SDL_OutOfMemory();
        goto cleanup;
    }
    curdata = (KMSDRM_LEGACY_CursorData *) SDL_calloc(1, sizeof(*curdata));
    if (!curdata) {
        SDL_OutOfMemory();
        goto cleanup;
    }

    /* hox_x and hot_y are the coordinates of the "tip of the cursor" from it's base. */
    curdata->hot_x = hot_x;
    curdata->hot_y = hot_y;
    curdata->w = surface->w;
    curdata->h = surface->h;
    curdata->buffer = NULL;

    /* Configure the cursor buffer info.
       This buffer has the original size of the cursor surface we are given. */
    curdata->buffer_pitch = surface->pitch;
    curdata->buffer_size = surface->pitch * surface->h;
    curdata->buffer = (uint32_t*)SDL_malloc(curdata->buffer_size);

    if (!curdata->buffer) {
        SDL_OutOfMemory();
        goto cleanup;
    }

    if (SDL_MUSTLOCK(surface)) {
        if (SDL_LockSurface(surface) < 0) {
            /* Could not lock surface */
            goto cleanup;
        }
    }

    /* Copy the surface pixels to the cursor buffer, for future use in ShowCursor() */
    SDL_memcpy(curdata->buffer, surface->pixels, curdata->buffer_size);

    if (SDL_MUSTLOCK(surface)) {
        SDL_UnlockSurface(surface);
    }

    cursor->driverdata = curdata;

    ret = cursor;

cleanup:
    if (ret == NULL) {
	if (curdata) {
	    if (curdata->buffer) {
		SDL_free(curdata->buffer);
	    }
	    SDL_free(curdata);
	}
	if (cursor) {
	    SDL_free(cursor);
	}
    }

    return ret;
}

/* When we create a window, we have to test if we have to show the cursor,
   and explicily do so if necessary.
   This is because when we destroy a window, we take the cursor away from the
   cursor plane, and destroy the cusror GBM BO. So we have to re-show it,
   so to say. */
void
KMSDRM_LEGACY_InitCursor()
{
    SDL_Mouse *mouse = NULL;
    mouse = SDL_GetMouse();

    if (!mouse) {
        return;
    }
    if  (!(mouse->cur_cursor)) {
        return;
    }

    if  (!(mouse->cursor_shown)) {
        return;
    }

    KMSDRM_LEGACY_ShowCursor(mouse->cur_cursor);
}

/* Show the specified cursor, or hide if cursor is NULL or has no focus. */
static int
KMSDRM_LEGACY_ShowCursor(SDL_Cursor * cursor)
{
    SDL_VideoDevice *video_device = SDL_GetVideoDevice();
    SDL_VideoData *viddata = ((SDL_VideoData *)video_device->driverdata);
    SDL_DisplayData *dispdata = (SDL_DisplayData *)SDL_GetDisplayDriverData(0);
    SDL_Mouse *mouse;
    KMSDRM_LEGACY_CursorData *curdata;

    uint32_t bo_handle;

    size_t bo_stride;
    size_t bufsize;
    uint32_t *ready_buffer = NULL;
    uint32_t pixel;

    int i,j;
    int ret;

    mouse = SDL_GetMouse();
    if (!mouse) {
        return SDL_SetError("No mouse.");
    }

    /*********************************************************/
    /* Hide cursor if it's NULL or it has no focus(=winwow). */
    /*********************************************************/
    if (!cursor || !mouse->focus) {
        /* Hide the drm cursor with no more considerations because
           SDL_VideoQuit() takes us here after disabling the mouse
           so there is no mouse->cur_cursor by now. */
	ret = KMSDRM_LEGACY_drmModeSetCursor(viddata->drm_fd,
	    dispdata->crtc->crtc_id, 0, 0, 0);
	if (ret) {
	    ret = SDL_SetError("Could not hide current cursor with drmModeSetCursor().");
	}
        return ret;
    }

    /************************************************/
    /* If cursor != NULL, DO show cursor on display */
    /************************************************/
    curdata = (KMSDRM_LEGACY_CursorData *) cursor->driverdata;

    if (!curdata || !dispdata->cursor_bo) {
        return SDL_SetError("Cursor not initialized properly.");
    }

    /* Prepare a buffer we can dump to our GBM BO (different
       size, alpha premultiplication...) */
    bo_stride = KMSDRM_LEGACY_gbm_bo_get_stride(dispdata->cursor_bo);
    bufsize = bo_stride * curdata->h;

    ready_buffer = (uint32_t*)SDL_malloc(bufsize);
    if (!ready_buffer) {
        ret = SDL_OutOfMemory();
        goto cleanup;
    }

    /* Clean the whole buffer we are preparing. */
    SDL_memset(ready_buffer, 0x00, bo_stride * curdata->h);

    /* Copy from the cursor buffer to a buffer that we can dump to the GBM BO,
       pre-multiplying by alpha each pixel as we go. */
    for (i = 0; i < curdata->h; i++) {
        for (j = 0; j < curdata->w; j++) {
            pixel = ((uint32_t*)curdata->buffer)[i * curdata->w + j];
            legacy_alpha_premultiply_ARGB8888 (&pixel);
            SDL_memcpy(ready_buffer + (i * dispdata->cursor_w) + j, &pixel, 4);
        }
    }

    /* Dump the cursor buffer to our GBM BO. */
    if (KMSDRM_LEGACY_gbm_bo_write(dispdata->cursor_bo, ready_buffer, bufsize)) {
        ret = SDL_SetError("Could not write to GBM cursor BO");
        goto cleanup;
    }

    /* Put the GBM BO buffer on screen using the DRM interface. */
    bo_handle = KMSDRM_LEGACY_gbm_bo_get_handle(dispdata->cursor_bo).u32;
    if (curdata->hot_x == 0 && curdata->hot_y == 0) {
        ret = KMSDRM_LEGACY_drmModeSetCursor(viddata->drm_fd, dispdata->crtc->crtc_id,
            bo_handle, dispdata->cursor_w, dispdata->cursor_h);
    } else {
        ret = KMSDRM_LEGACY_drmModeSetCursor2(viddata->drm_fd, dispdata->crtc->crtc_id,
            bo_handle, dispdata->cursor_w, dispdata->cursor_h, curdata->hot_x, curdata->hot_y);
    }

    if (ret) {
        ret = SDL_SetError("Failed to set DRM cursor.");
        goto cleanup;
    }

cleanup:

    if (ready_buffer) {
        SDL_free(ready_buffer);
    }
    return ret;
}

/* This is only for freeing the SDL_cursor.*/
static void
KMSDRM_LEGACY_FreeCursor(SDL_Cursor * cursor)
{
    KMSDRM_LEGACY_CursorData *curdata;

    /* Even if the cursor is not ours, free it. */
    if (cursor) {
        curdata = (KMSDRM_LEGACY_CursorData *) cursor->driverdata;
        /* Free cursor buffer */
        if (curdata->buffer) {
            SDL_free(curdata->buffer);
            curdata->buffer = NULL;
        }
        /* Free cursor itself */
        if (cursor->driverdata) {
            SDL_free(cursor->driverdata);
        }
        SDL_free(cursor);
    }
}

/* Warp the mouse to (x,y) */
static void
KMSDRM_LEGACY_WarpMouse(SDL_Window * window, int x, int y)
{
    /* Only one global/fullscreen window is supported */
    KMSDRM_LEGACY_WarpMouseGlobal(x, y);
}

/* Warp the mouse to (x,y) */
static int
KMSDRM_LEGACY_WarpMouseGlobal(int x, int y)
{
    SDL_Mouse *mouse = SDL_GetMouse();
    SDL_DisplayData *dispdata = (SDL_DisplayData *)SDL_GetDisplayDriverData(0);

    if (mouse && mouse->cur_cursor && mouse->cur_cursor->driverdata) {
        /* Update internal mouse position. */
        SDL_SendMouseMotion(mouse->focus, mouse->mouseID, 0, x, y);

        /* And now update the cursor graphic position on screen. */
        if (dispdata->cursor_bo) {
	    int ret, drm_fd;
	    drm_fd = KMSDRM_LEGACY_gbm_device_get_fd(
		KMSDRM_LEGACY_gbm_bo_get_device(dispdata->cursor_bo));
	    ret = KMSDRM_LEGACY_drmModeMoveCursor(drm_fd, dispdata->crtc->crtc_id, x, y);

	    if (ret) {
		SDL_SetError("drmModeMoveCursor() failed.");
	    }

	    return ret;

        } else {
            return SDL_SetError("Cursor not initialized properly.");
        }
    } else {
        return SDL_SetError("No mouse or current cursor.");
    }

    return 0;
}

/* UNDO WHAT WE DID IN KMSDRM_InitMouse(). */
void
KMSDRM_LEGACY_DeinitMouse(_THIS)
{
    SDL_VideoDevice *video_device = SDL_GetVideoDevice();
    SDL_DisplayData *dispdata = (SDL_DisplayData *)SDL_GetDisplayDriverData(0);
   
    /* Destroy the curso GBM BO. */
    if (video_device && dispdata->cursor_bo) {
	KMSDRM_LEGACY_gbm_bo_destroy(dispdata->cursor_bo);
	dispdata->cursor_bo = NULL;
    }
}

/* Create cursor BO. */
void
KMSDRM_LEGACY_InitMouse(_THIS)
{
    SDL_VideoDevice *dev = SDL_GetVideoDevice();
    SDL_VideoData *viddata = ((SDL_VideoData *)dev->driverdata);
    SDL_DisplayData *dispdata = (SDL_DisplayData *)SDL_GetDisplayDriverData(0);
    SDL_Mouse *mouse = SDL_GetMouse();

    mouse->CreateCursor = KMSDRM_LEGACY_CreateCursor;
    mouse->ShowCursor = KMSDRM_LEGACY_ShowCursor;
    mouse->MoveCursor = KMSDRM_LEGACY_MoveCursor;
    mouse->FreeCursor = KMSDRM_LEGACY_FreeCursor;
    mouse->WarpMouse = KMSDRM_LEGACY_WarpMouse;
    mouse->WarpMouseGlobal = KMSDRM_LEGACY_WarpMouseGlobal;

    /************************************************/
    /* Create the cursor GBM BO, if we haven't yet. */
    /************************************************/
    if (!dispdata->cursor_bo) {

        if (!KMSDRM_LEGACY_gbm_device_is_format_supported(viddata->gbm_dev,
              GBM_FORMAT_ARGB8888,
              GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE))
        {
            SDL_SetError("Unsupported pixel format for cursor");
            return;
        }

	if (KMSDRM_LEGACY_drmGetCap(viddata->drm_fd,
              DRM_CAP_CURSOR_WIDTH,  &dispdata->cursor_w) ||
	      KMSDRM_LEGACY_drmGetCap(viddata->drm_fd, DRM_CAP_CURSOR_HEIGHT,
              &dispdata->cursor_h))
	{
	    SDL_SetError("Could not get the recommended GBM cursor size");
	    goto cleanup;
	}

	if (dispdata->cursor_w == 0 || dispdata->cursor_h == 0) {
	    SDL_SetError("Could not get an usable GBM cursor size");
	    goto cleanup;
	}

	dispdata->cursor_bo = KMSDRM_LEGACY_gbm_bo_create(viddata->gbm_dev,
	    dispdata->cursor_w, dispdata->cursor_h,
	    GBM_FORMAT_ARGB8888, GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE | GBM_BO_USE_LINEAR);

	if (!dispdata->cursor_bo) {
	    SDL_SetError("Could not create GBM cursor BO");
	    goto cleanup;
	}
    }

    /* SDL expects to set the default cursor on screen when we init the mouse,
       but since we have moved the KMSDRM_InitMouse() call to KMSDRM_CreateWindow(),
       we end up calling KMSDRM_InitMouse() every time we create a window, so we
       have to prevent this from being done every time a new window is created.
       If we don't, new default cursors would stack up on mouse->cursors and SDL
       would have to hide and delete them at quit, not to mention the memory leak... */
    if(dispdata->set_default_cursor_pending) { 
        SDL_SetDefaultCursor(KMSDRM_LEGACY_CreateDefaultCursor());
        dispdata->set_default_cursor_pending = SDL_FALSE;
    }

    return;

cleanup:
    if (dispdata->cursor_bo) {
	KMSDRM_LEGACY_gbm_bo_destroy(dispdata->cursor_bo);
	dispdata->cursor_bo = NULL;
    }
}

void
KMSDRM_LEGACY_QuitMouse(_THIS)
{
    /* TODO: ? */
}

/* This is called when a mouse motion event occurs */
static void
KMSDRM_LEGACY_MoveCursor(SDL_Cursor * cursor)
{
    SDL_Mouse *mouse = SDL_GetMouse();
    SDL_DisplayData *dispdata = (SDL_DisplayData *)SDL_GetDisplayDriverData(0);
    int drm_fd, ret;

    /* We must NOT call SDL_SendMouseMotion() here or we will enter recursivity!
       That's why we move the cursor graphic ONLY. */
    if (mouse && mouse->cur_cursor && mouse->cur_cursor->driverdata) {
        drm_fd = KMSDRM_LEGACY_gbm_device_get_fd(KMSDRM_LEGACY_gbm_bo_get_device(dispdata->cursor_bo));
        ret = KMSDRM_LEGACY_drmModeMoveCursor(drm_fd, dispdata->crtc->crtc_id, mouse->x, mouse->y);

        if (ret) {
            SDL_SetError("drmModeMoveCursor() failed.");
        }
    }
}

#endif /* SDL_VIDEO_DRIVER_KMSDRM */

/* vi: set ts=4 sw=4 expandtab: */
