/*
 * Copyright 2008 George Sapountzis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#if defined(GLX_DIRECT_RENDERING) && (!defined(GLX_USE_APPLEGL) || defined(GLX_USE_APPLE))

#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/shm.h>
#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>
#include "glxclient.h"
#include <dlfcn.h>
#include "dri_common.h"
#include "drisw_priv.h"
#ifdef HAVE_DRI3
#include "dri3_priv.h"
#endif
#include <X11/extensions/shmproto.h>
#include <assert.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_xcb.h>
#include "util/u_debug.h"
#include "kopper_interface.h"
#include "loader_dri_helper.h"
#include "dri_util.h"

static int xshm_error = 0;
static int xshm_opcode = -1;

/**
 * Catches potential Xlib errors.
 */
static int
handle_xerror(Display *dpy, XErrorEvent *event)
{
   (void) dpy;

   assert(xshm_opcode != -1);
   if (event->request_code != xshm_opcode)
      return 0;

   xshm_error = event->error_code;
   return 0;
}

static Bool
XCreateDrawable(struct drisw_drawable * pdp, int shmid, Display * dpy)
{
   if (pdp->ximage) {
      XDestroyImage(pdp->ximage);
      pdp->ximage = NULL;
      if ((pdp->shminfo.shmid > 0) && (shmid != pdp->shminfo.shmid))
         XShmDetach(dpy, &pdp->shminfo);
   }

   if (!xshm_error && shmid >= 0) {
      pdp->shminfo.shmid = shmid;
      pdp->ximage = XShmCreateImage(dpy,
                                    NULL,
                                    pdp->xDepth,
                                    ZPixmap,              /* format */
                                    NULL,                 /* data */
                                    &pdp->shminfo,        /* shminfo */
                                    0, 0);                /* width, height */
      if (pdp->ximage != NULL) {
         int (*old_handler)(Display *, XErrorEvent *);

         /* dispatch pending errors */
         XSync(dpy, False);

         old_handler = XSetErrorHandler(handle_xerror);
         /* This may trigger the X protocol error we're ready to catch: */
         XShmAttach(dpy, &pdp->shminfo);
         XSync(dpy, False);

         if (xshm_error) {
         /* we are on a remote display, this error is normal, don't print it */
            XDestroyImage(pdp->ximage);
            pdp->ximage = NULL;
         }

         (void) XSetErrorHandler(old_handler);
      }
   }

   if (pdp->ximage == NULL) {
      pdp->shminfo.shmid = -1;
      pdp->ximage = XCreateImage(dpy,
                                 NULL,
                                 pdp->xDepth,
                                 ZPixmap, 0,             /* format, offset */
                                 NULL,                   /* data */
                                 0, 0,                   /* width, height */
                                 32,                     /* bitmap_pad */
                                 0);                     /* bytes_per_line */
   }

  /**
   * swrast does not handle 24-bit depth with 24 bpp, so let X do the
   * the conversion for us.
   */
  if (pdp->ximage->bits_per_pixel == 24)
     pdp->ximage->bits_per_pixel = 32;

   return True;
}

static void
XDestroyDrawable(struct drisw_drawable * pdp, Display * dpy, XID drawable)
{
   if (pdp->ximage)
      XDestroyImage(pdp->ximage);

   if (pdp->shminfo.shmid > 0)
      XShmDetach(dpy, &pdp->shminfo);

   XFreeGC(dpy, pdp->gc);
}

/**
 * swrast loader functions
 */

static void
swrastGetDrawableInfo(__DRIdrawable * draw,
                      int *x, int *y, int *w, int *h,
                      void *loaderPrivate)
{
   struct drisw_drawable *pdp = loaderPrivate;
   __GLXDRIdrawable *pdraw = &(pdp->base);
   Display *dpy = pdraw->psc->dpy;
   Drawable drawable;

   Window root;
   unsigned uw, uh, bw, depth;

   drawable = pdraw->xDrawable;

   XGetGeometry(dpy, drawable, &root, x, y, &uw, &uh, &bw, &depth);
   *w = uw;
   *h = uh;
}

/**
 * Align renderbuffer pitch.
 *
 * This should be chosen by the driver and the loader (libGL, xserver/glx)
 * should use the driver provided pitch.
 *
 * It seems that the xorg loader (that is the xserver loading swrast_dri for
 * indirect rendering, not client-side libGL) requires that the pitch is
 * exactly the image width padded to 32 bits. XXX
 *
 * The above restriction can probably be overcome by using ScratchPixmap and
 * CopyArea in the xserver, similar to ShmPutImage, and setting the width of
 * the scratch pixmap to 'pitch / cpp'.
 */
static inline int
bytes_per_line(unsigned pitch_bits, unsigned mul)
{
   unsigned mask = mul - 1;

   return ((pitch_bits + mask) & ~mask) / 8;
}

static void
swrastXPutImage(__DRIdrawable * draw, int op,
                int srcx, int srcy, int x, int y,
                int w, int h, int stride,
                int shmid, char *data, void *loaderPrivate)
{
   struct drisw_drawable *pdp = loaderPrivate;
   __GLXDRIdrawable *pdraw = &(pdp->base);
   Display *dpy = pdraw->psc->dpy;
   Drawable drawable;
   XImage *ximage;
   GC gc = pdp->gc;

   if (!pdp->ximage || shmid != pdp->shminfo.shmid) {
      if (!XCreateDrawable(pdp, shmid, dpy))
         return;
   }

   drawable = pdraw->xDrawable;
   ximage = pdp->ximage;
   ximage->bytes_per_line = stride ? stride : bytes_per_line(w * ximage->bits_per_pixel, 32);
   ximage->data = data;

   ximage->width = ximage->bytes_per_line / ((ximage->bits_per_pixel + 7)/ 8);
   ximage->height = h;

   if (pdp->shminfo.shmid >= 0) {
      XShmPutImage(dpy, drawable, gc, ximage, srcx, srcy, x, y, w, h, False);
      XSync(dpy, False);
   } else {
      XPutImage(dpy, drawable, gc, ximage, srcx, srcy, x, y, w, h);
   }
   ximage->data = NULL;
}

static void
swrastPutImageShm(__DRIdrawable * draw, int op,
                  int x, int y, int w, int h, int stride,
                  int shmid, char *shmaddr, unsigned offset,
                  void *loaderPrivate)
{
   struct drisw_drawable *pdp = loaderPrivate;

   if (!pdp)
      return;

   pdp->shminfo.shmaddr = shmaddr;
   swrastXPutImage(draw, op, 0, 0, x, y, w, h, stride, shmid,
                   shmaddr + offset, loaderPrivate);
}

static void
swrastPutImageShm2(__DRIdrawable * draw, int op,
                   int x, int y,
                   int w, int h, int stride,
                   int shmid, char *shmaddr, unsigned offset,
                   void *loaderPrivate)
{
   struct drisw_drawable *pdp = loaderPrivate;

   if (!pdp)
      return;

   pdp->shminfo.shmaddr = shmaddr;
   swrastXPutImage(draw, op, x, 0, x, y, w, h, stride, shmid,
                   shmaddr + offset, loaderPrivate);
}

static void
swrastPutImage2(__DRIdrawable * draw, int op,
                int x, int y, int w, int h, int stride,
                char *data, void *loaderPrivate)
{
   if (!loaderPrivate)
      return;

   swrastXPutImage(draw, op, 0, 0, x, y, w, h, stride, -1,
                   data, loaderPrivate);
}

static void
swrastPutImage(__DRIdrawable * draw, int op,
               int x, int y, int w, int h,
               char *data, void *loaderPrivate)
{
   if (!loaderPrivate)
      return;

   swrastXPutImage(draw, op, 0, 0, x, y, w, h, 0, -1,
                   data, loaderPrivate);
}

static void
swrastGetImage2(__DRIdrawable * read,
                int x, int y, int w, int h, int stride,
                char *data, void *loaderPrivate)
{
   struct drisw_drawable *prp = loaderPrivate;
   __GLXDRIdrawable *pread = &(prp->base);
   Display *dpy = pread->psc->dpy;
   Drawable readable;
   XImage *ximage;

   if (!prp->ximage || prp->shminfo.shmid >= 0) {
      if (!XCreateDrawable(prp, -1, dpy))
         return;
   }

   readable = pread->xDrawable;

   ximage = prp->ximage;
   ximage->data = data;
   ximage->width = w;
   ximage->height = h;
   ximage->bytes_per_line = stride ? stride : bytes_per_line(w * ximage->bits_per_pixel, 32);

   XGetSubImage(dpy, readable, x, y, w, h, ~0L, ZPixmap, ximage, 0, 0);

   ximage->data = NULL;
}

static void
swrastGetImage(__DRIdrawable * read,
               int x, int y, int w, int h,
               char *data, void *loaderPrivate)
{
   swrastGetImage2(read, x, y, w, h, 0, data, loaderPrivate);
}

static GLboolean
swrastGetImageShm2(__DRIdrawable * read,
                   int x, int y, int w, int h,
                   int shmid, void *loaderPrivate)
{
   struct drisw_drawable *prp = loaderPrivate;
   __GLXDRIdrawable *pread = &(prp->base);
   Display *dpy = pread->psc->dpy;
   Drawable readable;
   XImage *ximage;

   if (!prp->ximage || shmid != prp->shminfo.shmid) {
      if (!XCreateDrawable(prp, shmid, dpy))
         return GL_FALSE;
   }

   if (prp->shminfo.shmid == -1)
      return GL_FALSE;
   readable = pread->xDrawable;

   ximage = prp->ximage;
   ximage->data = prp->shminfo.shmaddr; /* no offset */
   ximage->width = w;
   ximage->height = h;
   ximage->bytes_per_line = bytes_per_line(w * ximage->bits_per_pixel, 32);

   XShmGetImage(dpy, readable, ximage, x, y, ~0L);
   return GL_TRUE;
}

static void
swrastGetImageShm(__DRIdrawable * read,
                  int x, int y, int w, int h,
                  int shmid, void *loaderPrivate)
{
   swrastGetImageShm2(read, x, y, w, h, shmid, loaderPrivate);
}

static const __DRIswrastLoaderExtension swrastLoaderExtension_shm = {
   .base = {__DRI_SWRAST_LOADER, 6 },

   .getDrawableInfo     = swrastGetDrawableInfo,
   .putImage            = swrastPutImage,
   .getImage            = swrastGetImage,
   .putImage2           = swrastPutImage2,
   .getImage2           = swrastGetImage2,
   .putImageShm         = swrastPutImageShm,
   .getImageShm         = swrastGetImageShm,
   .putImageShm2        = swrastPutImageShm2,
   .getImageShm2        = swrastGetImageShm2,
};

static const __DRIswrastLoaderExtension swrastLoaderExtension = {
   .base = {__DRI_SWRAST_LOADER, 3 },

   .getDrawableInfo     = swrastGetDrawableInfo,
   .putImage            = swrastPutImage,
   .getImage            = swrastGetImage,
   .putImage2           = swrastPutImage2,
   .getImage2           = swrastGetImage2,
};

static_assert(sizeof(struct kopper_vk_surface_create_storage) >= sizeof(VkXcbSurfaceCreateInfoKHR), "");

static void
kopperSetSurfaceCreateInfo(void *_draw, struct kopper_loader_info *out)
{
    __GLXDRIdrawable *draw = _draw;
    VkXcbSurfaceCreateInfoKHR *xcb = (VkXcbSurfaceCreateInfoKHR *)&out->bos;

    xcb->sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    xcb->pNext = NULL;
    xcb->flags = 0;
    xcb->connection = XGetXCBConnection(draw->psc->dpy);
    xcb->window = draw->xDrawable;
}

static const __DRIkopperLoaderExtension kopperLoaderExtension = {
    .base = { __DRI_KOPPER_LOADER, 1 },

    .SetSurfaceCreateInfo   = kopperSetSurfaceCreateInfo,
};

static const __DRIextension *loader_extensions_shm[] = {
   &swrastLoaderExtension_shm.base,
   &kopperLoaderExtension.base,
   NULL
};

static const __DRIextension *loader_extensions_noshm[] = {
   &swrastLoaderExtension.base,
   &kopperLoaderExtension.base,
   NULL
};

static const __DRIextension *kopper_extensions_noshm[] = {
   &swrastLoaderExtension.base,
   &kopperLoaderExtension.base,
   &dri2UseInvalidate.base,
   &driBackgroundCallable.base,
   NULL
};

/**
 * GLXDRI functions
 */


static void
drisw_wait_gl(struct glx_context *context)
{
   glFinish();
}

static void
drisw_wait_x(struct glx_context *context)
{
   XSync(context->currentDpy, False);
}

static void
drisw_bind_tex_image(__GLXDRIdrawable *base,
                     int buffer, const int *attrib_list)
{
   struct glx_context *gc = __glXGetCurrentContext();

   if (base != NULL) {
      dri_set_tex_buffer2(gc->driContext,
                          base->textureTarget,
                          base->textureFormat,
                          base->dri_drawable);
   }
}

int
kopper_get_buffer_age(__GLXDRIdrawable *pdraw);
int
kopper_get_buffer_age(__GLXDRIdrawable *pdraw)
{
   return kopperQueryBufferAge(pdraw->dri_drawable);
}

static const struct glx_context_vtable drisw_context_vtable = {
   .destroy             = dri_destroy_context,
   .bind                = dri_bind_context,
   .unbind              = dri_unbind_context,
   .wait_gl             = drisw_wait_gl,
   .wait_x              = drisw_wait_x,
};

static void
driswDestroyDrawable(__GLXDRIdrawable * pdraw)
{
   struct drisw_drawable *pdp = (struct drisw_drawable *) pdraw;

   driDestroyDrawable(pdp->base.dri_drawable);

   XDestroyDrawable(pdp, pdraw->psc->dpy, pdraw->drawable);
   free(pdp);
}

static __GLXDRIdrawable *
driswCreateDrawable(struct glx_screen *base, XID xDrawable,
                    GLXDrawable drawable, int type,
                    struct glx_config *modes)
{
   struct drisw_drawable *pdp;
   __GLXDRIconfigPrivate *config = (__GLXDRIconfigPrivate *) modes;
   unsigned depth;
   struct drisw_screen *psc = (struct drisw_screen *) base;
   Display *dpy = psc->base.dpy;

   xcb_connection_t *conn = XGetXCBConnection(dpy);
   xcb_generic_error_t *error;
   xcb_get_geometry_cookie_t cookie = xcb_get_geometry(conn, xDrawable);
   xcb_get_geometry_reply_t *reply = xcb_get_geometry_reply(conn, cookie, &error);
   if (reply)
      depth = reply->depth;
   free(reply);
   if (!reply || error)
      return NULL;

   pdp = calloc(1, sizeof(*pdp));
   if (!pdp)
      return NULL;

   pdp->base.xDrawable = xDrawable;
   pdp->base.drawable = drawable;
   pdp->base.psc = &psc->base;
   pdp->config = modes;
   pdp->gc = XCreateGC(dpy, xDrawable, 0, NULL);
   pdp->xDepth = 0;

   /* Use the visual depth, if this fbconfig corresponds to a visual */
   if (pdp->config->visualID != 0) {
      int matches = 0;
      XVisualInfo *visinfo, template;

      template.visualid = pdp->config->visualID;
      template.screen = pdp->config->screen;
      visinfo = XGetVisualInfo(dpy, VisualIDMask | VisualScreenMask,
                               &template, &matches);

      if (visinfo && matches) {
         pdp->xDepth = visinfo->depth;
         XFree(visinfo);
      }
   }

   /* Otherwise, or if XGetVisualInfo failed, ask the server */
   if (pdp->xDepth == 0) {
      pdp->xDepth = depth;
   }

   pdp->swapInterval = dri_get_initial_swap_interval(psc->driScreen);
   /* Create a new drawable */
   pdp->base.dri_drawable = dri_create_drawable(psc->driScreen, config->driConfig, !(type & GLX_WINDOW_BIT), pdp);
   if (psc->kopper)
      kopperSetSwapInterval(pdp->base.dri_drawable, pdp->swapInterval);

   if (!pdp->base.dri_drawable) {
      XDestroyDrawable(pdp, psc->base.dpy, xDrawable);
      free(pdp);
      return NULL;
   }

   pdp->base.destroyDrawable = driswDestroyDrawable;

   return &pdp->base;
}

static int64_t
driswSwapBuffers(__GLXDRIdrawable * pdraw,
                 int64_t target_msc, int64_t divisor, int64_t remainder,
                 Bool flush)
{
   struct drisw_screen *psc = (struct drisw_screen *) pdraw->psc;

   (void) target_msc;
   (void) divisor;
   (void) remainder;

   if (flush) {
      glFlush();
   }

   if (psc->kopper)
       return kopperSwapBuffers(pdraw->dri_drawable, 0);

   driSwapBuffers(pdraw->dri_drawable);

   return 0;
}

static void
drisw_copy_sub_buffer(__GLXDRIdrawable * pdraw,
                      int x, int y, int width, int height, Bool flush)
{
   if (flush) {
      glFlush();
   }

   driswCopySubBuffer(pdraw->dri_drawable, x, y, width, height);
}

static void
driswDestroyScreen(struct glx_screen *base)
{
   struct drisw_screen *psc = (struct drisw_screen *) base;

   /* Free the direct rendering per screen data */
   driDestroyScreen(psc->driScreen);
   driDestroyConfigs(psc->driver_configs);
   psc->driScreen = NULL;
   free(psc);
}

static char *
drisw_get_driver_name(struct glx_screen *glx_screen)
{
   struct drisw_screen *psc = (struct drisw_screen *) glx_screen;
   return strdup(psc->base.driverName);
}

static const struct glx_screen_vtable drisw_screen_vtable = {
   .create_context         = dri_common_create_context,
   .create_context_attribs = dri_create_context_attribs,
   .query_renderer_integer = glx_dri_query_renderer_integer,
   .query_renderer_string  = glx_dri_query_renderer_string,
   .get_driver_name        = drisw_get_driver_name,
};

static int
check_xshm(Display *dpy)
{
   xcb_connection_t *c = XGetXCBConnection(dpy);
   xcb_void_cookie_t cookie;
   xcb_generic_error_t *error;
   int ret = True;
   xcb_query_extension_cookie_t shm_cookie;
   xcb_query_extension_reply_t *shm_reply;
   bool has_mit_shm;

   shm_cookie = xcb_query_extension(c, 7, "MIT-SHM");
   shm_reply = xcb_query_extension_reply(c, shm_cookie, NULL);
   xshm_opcode = shm_reply->major_opcode;

   has_mit_shm = shm_reply->present;
   free(shm_reply);
   if (!has_mit_shm)
      return False;

   cookie = xcb_shm_detach_checked(c, 0);
   if ((error = xcb_request_check(c, cookie))) {
      /* BadRequest means we're a remote client. If we were local we'd
       * expect BadValue since 'info' has an invalid segment name.
       */
      if (error->error_code == BadRequest)
         ret = False;
      free(error);
   }

   return ret;
}

static int
driswKopperSetSwapInterval(__GLXDRIdrawable *pdraw, int interval)
{
   struct drisw_drawable *pdp = (struct drisw_drawable *) pdraw;
   struct drisw_screen *psc = (struct drisw_screen *) pdp->base.psc;

   if (!dri_valid_swap_interval(psc->driScreen, interval))
      return GLX_BAD_VALUE;

   kopperSetSwapInterval(pdp->base.dri_drawable, interval);
   pdp->swapInterval = interval;

   return 0;
}

static int
kopperGetSwapInterval(__GLXDRIdrawable *pdraw)
{
   struct drisw_drawable *pdp = (struct drisw_drawable *) pdraw;

   return pdp->swapInterval;
}

struct glx_screen *
driswCreateScreen(int screen, struct glx_display *priv, enum glx_driver glx_driver, bool driver_name_is_inferred)
{
   __GLXDRIscreen *psp;
   const __DRIconfig **driver_configs;
   struct drisw_screen *psc;
   struct glx_config *configs = NULL, *visuals = NULL;
   const __DRIextension **loader_extensions_local;
   bool kopper_disable = debug_get_bool_option("LIBGL_KOPPER_DISABLE", false);

   /* this is only relevant if zink bits are set */
   glx_driver &= (GLX_DRIVER_ZINK_INFER | GLX_DRIVER_ZINK_YES);
   const char *driver = glx_driver && !kopper_disable ? "zink" : "swrast";

   psc = calloc(1, sizeof *psc);
   if (psc == NULL)
      return NULL;
   psc->kopper = !strcmp(driver, "zink");

   if (!glx_screen_init(&psc->base, screen, priv)) {
      free(psc);
      return NULL;
   }

   psc->base.driverName = strdup(driver);

   if (glx_driver)
      loader_extensions_local = kopper_extensions_noshm;
   else if (!check_xshm(psc->base.dpy))
      loader_extensions_local = loader_extensions_noshm;
   else
      loader_extensions_local = loader_extensions_shm;

   psc->driScreen = driCreateNewScreen3(screen, -1, loader_extensions_local,
                                        glx_driver ? DRI_SCREEN_KOPPER : DRI_SCREEN_SWRAST,
                                        &driver_configs, driver_name_is_inferred,
                                        priv->has_multibuffer, psc);
   if (psc->driScreen == NULL) {
      if (!glx_driver || !driver_name_is_inferred)
         ErrorMessageF("glx: failed to create drisw screen\n");
      goto handle_error;
   }

   configs = driConvertConfigs(psc->base.configs, driver_configs);
   visuals = driConvertConfigs(psc->base.visuals, driver_configs);

   if (!configs || !visuals) {
       ErrorMessageF("No matching fbConfigs or visuals found\n");
       goto handle_error;
   }

   glx_config_destroy_list(psc->base.configs);
   psc->base.configs = configs;
   glx_config_destroy_list(psc->base.visuals);
   psc->base.visuals = visuals;

   psc->driver_configs = driver_configs;

   psc->base.vtable = &drisw_screen_vtable;
   psc->base.context_vtable = &drisw_context_vtable;
   psp = &psc->vtable;
   psc->base.driScreen = psp;
   psc->base.frontend_screen = psc->driScreen;
   psc->base.can_EXT_texture_from_pixmap = true;
   psp->destroyScreen = driswDestroyScreen;
   psp->createDrawable = driswCreateDrawable;
   psp->swapBuffers = driswSwapBuffers;
   psp->bindTexImage = drisw_bind_tex_image;

   if (!glx_driver)
      psp->copySubBuffer = drisw_copy_sub_buffer;

   if (psc->kopper) {
      psp->setSwapInterval = driswKopperSetSwapInterval;
      psp->getSwapInterval = kopperGetSwapInterval;
      psp->maxSwapInterval = 1;
   }

   priv->driver = glx_driver ? GLX_DRIVER_ZINK_YES : GLX_DRIVER_SW;

   return &psc->base;

 handle_error:
   if (configs)
       glx_config_destroy_list(configs);
   if (visuals)
       glx_config_destroy_list(visuals);
   if (psc->driScreen)
       driDestroyScreen(psc->driScreen);
   psc->driScreen = NULL;

   glx_screen_cleanup(&psc->base);
   free(psc);

   if (glx_driver & GLX_DRIVER_ZINK_YES && !driver_name_is_inferred)
      CriticalErrorMessageF("failed to load driver: %s\n", driver);

   return NULL;
}

#endif /* GLX_DIRECT_RENDERING */
