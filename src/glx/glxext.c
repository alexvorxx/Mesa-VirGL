/*
 * SGI FREE SOFTWARE LICENSE B (Version 2.0, Sept. 18, 2008)
 * Copyright (C) 1991-2000 Silicon Graphics, Inc. All Rights Reserved.
 *
 * SPDX-License-Identifier: SGI-B-2.0
 */

/**
 * \file glxext.c
 * GLX protocol interface boot-strap code.
 *
 * Direct rendering support added by Precision Insight, Inc.
 *
 * \author Kevin E. Martin <kevin@precisioninsight.com>
 */

#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>

#include "glxclient.h"
#include <X11/extensions/Xext.h>
#include <X11/extensions/extutil.h>
#ifdef GLX_USE_APPLEGL
#include "apple/apple_glx.h"
#include "apple/apple_visual.h"
#endif
#include "glxextensions.h"

#include "util/u_debug.h"
#ifndef GLX_USE_APPLEGL
#include "dri_common.h"
#endif

#include "loader_x11.h"
#ifdef HAVE_LIBDRM
#include "loader_dri3_helper.h"
#endif

#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>
#include <xcb/glx.h>
#include "dri_util.h"
#if defined(GLX_DIRECT_RENDERING) && (!defined(GLX_USE_APPLEGL) || defined(GLX_USE_APPLE))
#include <dlfcn.h>
#endif

#define __GLX_MIN_CONFIG_PROPS	18
#define __GLX_EXT_CONFIG_PROPS	32

/*
** Since we send all non-core visual properties as token, value pairs,
** we require 2 words across the wire. In order to maintain backwards
** compatibility, we need to send the total number of words that the
** VisualConfigs are sent back in so old libraries can simply "ignore"
** the new properties.
*/
#define __GLX_TOTAL_CONFIG \
   (__GLX_MIN_CONFIG_PROPS + 2 * __GLX_EXT_CONFIG_PROPS)

_X_HIDDEN void
glx_message(int level, const char *f, ...)
{
   va_list args;
   int threshold = _LOADER_WARNING;
   const char *libgl_debug;

   libgl_debug = getenv("LIBGL_DEBUG");
   if (libgl_debug) {
      if (strstr(libgl_debug, "quiet"))
         threshold = _LOADER_FATAL;
      else if (strstr(libgl_debug, "verbose"))
         threshold = _LOADER_DEBUG;
   }

   /* Note that the _LOADER_* levels are lower numbers for more severe. */
   if (level <= threshold) {
      va_start(args, f);
      vfprintf(stderr, f, args);
      va_end(args);
   }
}

/*
** You can set this cell to 1 to force the gl drawing stuff to be
** one command per packet
*/
_X_HIDDEN int __glXDebug = 0;

/* Extension required boiler plate */

static const char __glXExtensionName[] = GLX_EXTENSION_NAME;
static struct glx_display *glx_displays;

static /* const */ char *error_list[] = {
   "GLXBadContext",
   "GLXBadContextState",
   "GLXBadDrawable",
   "GLXBadPixmap",
   "GLXBadContextTag",
   "GLXBadCurrentWindow",
   "GLXBadRenderRequest",
   "GLXBadLargeRequest",
   "GLXUnsupportedPrivateRequest",
   "GLXBadFBConfig",
   "GLXBadPbuffer",
   "GLXBadCurrentDrawable",
   "GLXBadWindow",
   "GLXBadProfileARB",
};

#ifdef GLX_USE_APPLEGL
static char *__glXErrorString(Display *dpy, int code, XExtCodes *codes,
                              char *buf, int n);
#endif

static
XEXT_GENERATE_ERROR_STRING(__glXErrorString, __glXExtensionName,
                           __GLX_NUMBER_ERRORS, error_list)

/*
 * GLX events are a bit funky.  We don't stuff the X event code into
 * our user exposed (via XNextEvent) structure.  Instead we use the GLX
 * private event code namespace (and hope it doesn't conflict).  Clients
 * have to know that bit 15 in the event type field means they're getting
 * a GLX event, and then handle the various sub-event types there, rather
 * than simply checking the event code and handling it directly.
 */

static Bool
__glXWireToEvent(Display *dpy, XEvent *event, xEvent *wire)
{
     struct glx_display *glx_dpy = __glXInitialize(dpy);

   if (glx_dpy == NULL)
      return False;

   switch ((wire->u.u.type & 0x7f) - glx_dpy->codes.first_event) {
   case GLX_PbufferClobber:
   {
      GLXPbufferClobberEvent *aevent = (GLXPbufferClobberEvent *)event;
      xGLXPbufferClobberEvent *awire = (xGLXPbufferClobberEvent *)wire;
      aevent->event_type = awire->event_type;
      aevent->serial = awire->sequenceNumber;
      aevent->draw_type = awire->draw_type;
      aevent->drawable = awire->drawable;
      aevent->buffer_mask = awire->buffer_mask;
      aevent->aux_buffer = awire->aux_buffer;
      aevent->x = awire->x;
      aevent->y = awire->y;
      aevent->width = awire->width;
      aevent->height = awire->height;
      aevent->count = awire->count;
      return True;
   }
   case GLX_BufferSwapComplete:
   {
      GLXBufferSwapComplete *aevent = (GLXBufferSwapComplete *)event;
      xGLXBufferSwapComplete2 *awire = (xGLXBufferSwapComplete2 *)wire;
      struct glx_drawable *glxDraw = GetGLXDrawable(dpy, awire->drawable);

      if (!glxDraw)
	 return False;

      aevent->serial = _XSetLastRequestRead(dpy, (xGenericReply *) wire);
      aevent->send_event = (awire->type & 0x80) != 0;
      aevent->display = dpy;
      aevent->event_type = awire->event_type;
      aevent->drawable = glxDraw->xDrawable;
      aevent->ust = ((CARD64)awire->ust_hi << 32) | awire->ust_lo;
      aevent->msc = ((CARD64)awire->msc_hi << 32) | awire->msc_lo;

      /* Handle 32-Bit wire sbc wraparound in both directions to cope with out
       * of sequence 64-Bit sbc's
       */
      if ((int64_t) awire->sbc < ((int64_t) glxDraw->lastEventSbc - 0x40000000))
         glxDraw->eventSbcWrap += 0x100000000;
      if ((int64_t) awire->sbc > ((int64_t) glxDraw->lastEventSbc + 0x40000000))
         glxDraw->eventSbcWrap -= 0x100000000;
      glxDraw->lastEventSbc = awire->sbc;
      aevent->sbc = awire->sbc + glxDraw->eventSbcWrap;
      return True;
   }
   default:
      /* client doesn't support server event */
      break;
   }

   return False;
}

/* We don't actually support this.  It doesn't make sense for clients to
 * send each other GLX events.
 */
static Status
__glXEventToWire(Display *dpy, XEvent *event, xEvent *wire)
{
     struct glx_display *glx_dpy = __glXInitialize(dpy);

   if (glx_dpy == NULL)
      return False;

   switch (event->type) {
   case GLX_DAMAGED:
      break;
   case GLX_SAVED:
      break;
   case GLX_EXCHANGE_COMPLETE_INTEL:
      break;
   case GLX_COPY_COMPLETE_INTEL:
      break;
   case GLX_FLIP_COMPLETE_INTEL:
      break;
   default:
      /* client doesn't support server event */
      break;
   }

   return Success;
}

/************************************************************************/
/*
** Free the per screen configs data as well as the array of
** __glXScreenConfigs.
*/
static void
FreeScreenConfigs(struct glx_display * priv)
{
   struct glx_screen *psc;
   GLint i, screens;

   /* Free screen configuration information */
   screens = ScreenCount(priv->dpy);
   for (i = 0; i < screens; i++) {
      psc = priv->screens[i];
      if (!psc)
         continue;
      glx_screen_cleanup(psc);

#if defined(GLX_DIRECT_RENDERING) && !defined(GLX_USE_APPLEGL)
      if (psc->driScreen.deinitScreen)
         psc->driScreen.deinitScreen(psc);
      /* Free the direct rendering per screen data */
      driDestroyScreen(psc->frontend_screen);
#endif
      free(psc);
   }
   free((char *) priv->screens);
   priv->screens = NULL;
}

#if defined(GLX_DIRECT_RENDERING) && !defined(GLX_USE_APPLEGL)
static void
free_zombie_glx_drawable(struct set_entry *entry)
{
   __GLXDRIdrawable *pdraw = (__GLXDRIdrawable *)entry->key;

   pdraw->destroyDrawable(pdraw);
}
#endif

static void
glx_display_free(struct glx_display *priv)
{
   struct glx_context *gc;

   gc = __glXGetCurrentContext();
   if (priv->dpy == gc->currentDpy) {
      if (gc != &dummyContext)
         gc->vtable->unbind(gc);

      gc->vtable->destroy(gc);
      __glXSetCurrentContextNull();
   }

   /* Needs to be done before free screen. */
#if defined(GLX_DIRECT_RENDERING) && !defined(GLX_USE_APPLEGL)
   _mesa_set_destroy(priv->zombieGLXDrawable, free_zombie_glx_drawable);
#endif

   FreeScreenConfigs(priv);

   __glxHashDestroy(priv->glXDrawHash);

#if defined(GLX_DIRECT_RENDERING) && !defined(GLX_USE_APPLEGL)
   __glxHashDestroy(priv->drawHash);
   if (priv->dri2Hash)
      __glxHashDestroy(priv->dri2Hash);

#endif /* GLX_DIRECT_RENDERING && !GLX_USE_APPLEGL */

   free((char *) priv);
}

static int
__glXCloseDisplay(Display * dpy, XExtCodes * codes)
{
   struct glx_display *priv, **prev;

   _XLockMutex(_Xglobal_lock);
   prev = &glx_displays;
   for (priv = glx_displays; priv; prev = &priv->next, priv = priv->next) {
      if (priv->dpy == dpy) {
         *prev = priv->next;
	 break;
      }
   }
   _XUnlockMutex(_Xglobal_lock);

   if (priv != NULL)
      glx_display_free(priv);

   return 1;
}

/*
** Query the version of the GLX extension.  This procedure works even if
** the client extension is not completely set up.
*/
static Bool
QueryVersion(Display * dpy, int opcode, int *major, int *minor)
{
   xcb_connection_t *c = XGetXCBConnection(dpy);
   xcb_glx_query_version_reply_t *reply = xcb_glx_query_version_reply(c,
                                                                      xcb_glx_query_version
                                                                      (c,
                                                                       GLX_MAJOR_VERSION,
                                                                       GLX_MINOR_VERSION),
                                                                      NULL);

   if (!reply)
     return GL_FALSE;

   if (reply->major_version != GLX_MAJOR_VERSION) {
      free(reply);
      return GL_FALSE;
   }
   *major = reply->major_version;
   *minor = min(reply->minor_version, GLX_MINOR_VERSION);
   free(reply);
   return GL_TRUE;
}

/*
 * We don't want to enable this GLX_OML_swap_method in glxext.h,
 * because we can't support it.  The X server writes it out though,
 * so we should handle it somehow, to avoid false warnings.
 */
enum {
    IGNORE_GLX_SWAP_METHOD_OML = 0x8060
};


static GLint
convert_from_x_visual_type(int visualType)
{
   static const int glx_visual_types[] = {
      [StaticGray]  = GLX_STATIC_GRAY,
      [GrayScale]   = GLX_GRAY_SCALE,
      [StaticColor] = GLX_STATIC_COLOR,
      [PseudoColor] = GLX_PSEUDO_COLOR,
      [TrueColor]   = GLX_TRUE_COLOR,
      [DirectColor] = GLX_DIRECT_COLOR,
   };

   if (visualType < ARRAY_SIZE(glx_visual_types))
      return glx_visual_types[visualType];

   return GLX_NONE;
}

/*
 * getVisualConfigs uses the !tagged_only path.
 * getFBConfigs uses the tagged_only path.
 */
_X_HIDDEN void
__glXInitializeVisualConfigFromTags(struct glx_config * config, int count,
                                    const INT32 * bp, Bool tagged_only,
                                    Bool fbconfig_style_tags)
{
   int i;

   if (!tagged_only) {
      /* Copy in the first set of properties */
      config->visualID = *bp++;

      config->visualType = convert_from_x_visual_type(*bp++);

      config->renderType = *bp++ ? GLX_RGBA_BIT : GLX_COLOR_INDEX_BIT;

      config->redBits = *bp++;
      config->greenBits = *bp++;
      config->blueBits = *bp++;
      config->alphaBits = *bp++;
      config->accumRedBits = *bp++;
      config->accumGreenBits = *bp++;
      config->accumBlueBits = *bp++;
      config->accumAlphaBits = *bp++;

      config->doubleBufferMode = *bp++;
      config->stereoMode = *bp++;

      config->rgbBits = *bp++;
      config->depthBits = *bp++;
      config->stencilBits = *bp++;
      config->numAuxBuffers = *bp++;
      config->level = *bp++;

#ifdef GLX_USE_APPLEGL
       /* AppleSGLX supports pixmap and pbuffers with all config. */
       config->drawableType = GLX_WINDOW_BIT | GLX_PIXMAP_BIT | GLX_PBUFFER_BIT;
       /* Unfortunately this can create an ABI compatibility problem. */
       count -= 18;
#else
      count -= __GLX_MIN_CONFIG_PROPS;
#endif
   }

   /*
    ** Additional properties may be in a list at the end
    ** of the reply.  They are in pairs of property type
    ** and property value.
    */

#define FETCH_OR_SET(tag) \
    config-> tag = ( fbconfig_style_tags ) ? *bp++ : 1

   for (i = 0; i < count; i += 2) {
      long int tag = *bp++;

      switch (tag) {
      case GLX_RGBA:
         if (fbconfig_style_tags)
            config->renderType = *bp++ ? GLX_RGBA_BIT : GLX_COLOR_INDEX_BIT;
         else
            config->renderType = GLX_RGBA_BIT;
         break;
      case GLX_BUFFER_SIZE:
         config->rgbBits = *bp++;
         break;
      case GLX_LEVEL:
         config->level = *bp++;
         break;
      case GLX_DOUBLEBUFFER:
         FETCH_OR_SET(doubleBufferMode);
         break;
      case GLX_STEREO:
         FETCH_OR_SET(stereoMode);
         break;
      case GLX_AUX_BUFFERS:
         config->numAuxBuffers = *bp++;
         break;
      case GLX_RED_SIZE:
         config->redBits = *bp++;
         break;
      case GLX_GREEN_SIZE:
         config->greenBits = *bp++;
         break;
      case GLX_BLUE_SIZE:
         config->blueBits = *bp++;
         break;
      case GLX_ALPHA_SIZE:
         config->alphaBits = *bp++;
         break;
      case GLX_DEPTH_SIZE:
         config->depthBits = *bp++;
         break;
      case GLX_STENCIL_SIZE:
         config->stencilBits = *bp++;
         break;
      case GLX_ACCUM_RED_SIZE:
         config->accumRedBits = *bp++;
         break;
      case GLX_ACCUM_GREEN_SIZE:
         config->accumGreenBits = *bp++;
         break;
      case GLX_ACCUM_BLUE_SIZE:
         config->accumBlueBits = *bp++;
         break;
      case GLX_ACCUM_ALPHA_SIZE:
         config->accumAlphaBits = *bp++;
         break;
      case GLX_VISUAL_CAVEAT_EXT:
         config->visualRating = *bp++;
         break;
      case GLX_X_VISUAL_TYPE:
         config->visualType = *bp++;
         break;
      case GLX_TRANSPARENT_TYPE:
         config->transparentPixel = *bp++;
         break;
      case GLX_TRANSPARENT_INDEX_VALUE:
         config->transparentIndex = *bp++;
         break;
      case GLX_TRANSPARENT_RED_VALUE:
         config->transparentRed = *bp++;
         break;
      case GLX_TRANSPARENT_GREEN_VALUE:
         config->transparentGreen = *bp++;
         break;
      case GLX_TRANSPARENT_BLUE_VALUE:
         config->transparentBlue = *bp++;
         break;
      case GLX_TRANSPARENT_ALPHA_VALUE:
         config->transparentAlpha = *bp++;
         break;
      case GLX_VISUAL_ID:
         config->visualID = *bp++;
         break;
      case GLX_DRAWABLE_TYPE:
         config->drawableType = *bp++;
#ifdef GLX_USE_APPLEGL
         /* AppleSGLX supports pixmap and pbuffers with all config. */
         config->drawableType |= GLX_WINDOW_BIT | GLX_PIXMAP_BIT | GLX_PBUFFER_BIT;
#endif
         break;
      case GLX_RENDER_TYPE: /* fbconfig render type bits */
         config->renderType = *bp++;
         break;
      case GLX_X_RENDERABLE:
         config->xRenderable = *bp++;
         break;
      case GLX_FBCONFIG_ID:
         config->fbconfigID = *bp++;
         break;
      case GLX_MAX_PBUFFER_WIDTH:
         config->maxPbufferWidth = *bp++;
         break;
      case GLX_MAX_PBUFFER_HEIGHT:
         config->maxPbufferHeight = *bp++;
         break;
      case GLX_MAX_PBUFFER_PIXELS:
         config->maxPbufferPixels = *bp++;
         break;
#ifndef GLX_USE_APPLEGL
      case GLX_OPTIMAL_PBUFFER_WIDTH_SGIX:
         config->optimalPbufferWidth = *bp++;
         break;
      case GLX_OPTIMAL_PBUFFER_HEIGHT_SGIX:
         config->optimalPbufferHeight = *bp++;
         break;
      case GLX_VISUAL_SELECT_GROUP_SGIX:
         config->visualSelectGroup = *bp++;
         break;
#endif
      case GLX_SAMPLE_BUFFERS_SGIS:
         config->sampleBuffers = *bp++;
         break;
      case GLX_SAMPLES_SGIS:
         config->samples = *bp++;
         break;
      case IGNORE_GLX_SWAP_METHOD_OML:
         /* We ignore this tag.  See the comment above this function. */
         ++bp;
         break;
#ifndef GLX_USE_APPLEGL
      case GLX_BIND_TO_TEXTURE_RGB_EXT:
         config->bindToTextureRgb = *bp++;
         break;
      case GLX_BIND_TO_TEXTURE_RGBA_EXT:
         config->bindToTextureRgba = *bp++;
         break;
      case GLX_BIND_TO_MIPMAP_TEXTURE_EXT:
         config->bindToMipmapTexture = *bp++;
         break;
      case GLX_BIND_TO_TEXTURE_TARGETS_EXT:
         config->bindToTextureTargets = *bp++;
         break;
      case GLX_Y_INVERTED_EXT:
         config->yInverted = *bp++;
         break;
#endif
      case GLX_FRAMEBUFFER_SRGB_CAPABLE_EXT:
         config->sRGBCapable = *bp++;
         break;

      case GLX_USE_GL:
         if (fbconfig_style_tags)
            bp++;
         break;
      case GLX_FLOAT_COMPONENTS_NV:
         config->floatComponentsNV = *bp++;
         break;
      case None:
         i = count;
         break;
      default: {
            long int tagvalue = *bp++;
            DebugMessageF("WARNING: unknown fbconfig attribute from server: "
                          "tag 0x%lx value 0x%lx\n", tag, tagvalue);
            break;
         }
      }
   }
}

static struct glx_config *
createConfigsFromProperties(Display * dpy, int nvisuals, int nprops,
                            int screen, GLboolean tagged_only)
{
   INT32 buf[__GLX_TOTAL_CONFIG], *props;
   unsigned prop_size;
   struct glx_config *modes, *m;
   int i;

   if (nprops == 0)
      return NULL;

   /* Check number of properties */
   if (nprops < __GLX_MIN_CONFIG_PROPS)
      return NULL;

   /* Allocate memory for our config structure */
   modes = glx_config_create_list(nvisuals);
   if (!modes)
      return NULL;

   prop_size = nprops * __GLX_SIZE_INT32;
   if (prop_size <= sizeof(buf))
      props = buf;
   else
      props = malloc(prop_size);

   /* Read each config structure and convert it into our format */
   m = modes;
   for (i = 0; i < nvisuals; i++) {
      _XRead(dpy, (char *) props, prop_size);
      /* If this is GLXGetVisualConfigs then the reply will not include
       * any drawable type info, but window support is implied because
       * that's what a Visual describes, and pixmap support is implied
       * because you almost certainly have a pixmap format corresponding
       * to your visual format. 
       */
      if (!tagged_only)
         m->drawableType = GLX_WINDOW_BIT | GLX_PIXMAP_BIT;
      __glXInitializeVisualConfigFromTags(m, nprops, props,
                                          tagged_only, GL_TRUE);
      m->screen = screen;
      m = m->next;
   }

   if (props != buf)
      free(props);

   return modes;
}

static GLboolean
getVisualConfigs(struct glx_screen *psc,
		  struct glx_display *priv, int screen)
{
   xGLXGetVisualConfigsReq *req;
   xGLXGetVisualConfigsReply reply;
   Display *dpy = priv->dpy;

   LockDisplay(dpy);

   psc->visuals = NULL;
   GetReq(GLXGetVisualConfigs, req);
   req->reqType = priv->codes.major_opcode;
   req->glxCode = X_GLXGetVisualConfigs;
   req->screen = screen;

   if (!_XReply(dpy, (xReply *) & reply, 0, False))
      goto out;

   psc->visuals = createConfigsFromProperties(dpy,
                                              reply.numVisuals,
                                              reply.numProps,
                                              screen, GL_FALSE);

 out:
   UnlockDisplay(dpy);
   return psc->visuals != NULL;
}

static GLboolean
getFBConfigs(struct glx_screen *psc, struct glx_display *priv, int screen)
{
   xGLXGetFBConfigsReq *fb_req;
   xGLXGetFBConfigsReply reply;
   Display *dpy = priv->dpy;

   psc->serverGLXexts = __glXQueryServerString(dpy, screen, GLX_EXTENSIONS);

   if (psc->serverGLXexts == NULL) {
      return GL_FALSE;
   }

   LockDisplay(dpy);

   psc->configs = NULL;
   GetReq(GLXGetFBConfigs, fb_req);
   fb_req->reqType = priv->codes.major_opcode;
   fb_req->glxCode = X_GLXGetFBConfigs;
   fb_req->screen = screen;

   if (!_XReply(dpy, (xReply *) & reply, 0, False))
      goto out;

   psc->configs = createConfigsFromProperties(dpy,
                                              reply.numFBConfigs,
                                              reply.numAttribs * 2,
                                              screen, GL_TRUE);

 out:
   UnlockDisplay(dpy);
   return psc->configs != NULL;
}

_X_HIDDEN Bool
glx_screen_init(struct glx_screen *psc,
		 int screen, struct glx_display * priv)
{
   /* Initialize per screen dynamic client GLX extensions */
   psc->ext_list_first_time = GL_TRUE;
   psc->scr = screen;
   psc->dpy = priv->dpy;
   psc->display = priv;

   if (!getVisualConfigs(psc, priv, screen))
      return GL_FALSE;

   if (!getFBConfigs(psc, priv, screen))
      return GL_FALSE;

   return GL_TRUE;
}

_X_HIDDEN void
glx_screen_cleanup(struct glx_screen *psc)
{
   if (psc->configs) {
      glx_config_destroy_list(psc->configs);
      free(psc->effectiveGLXexts);
      psc->configs = NULL;   /* NOTE: just for paranoia */
   }
   if (psc->visuals) {
      glx_config_destroy_list(psc->visuals);
      psc->visuals = NULL;   /* NOTE: just for paranoia */
   }
#if defined(GLX_DIRECT_RENDERING) && (!defined(GLX_USE_APPLEGL) || defined(GLX_USE_APPLE))
   if (psc->driver_configs) {
      driDestroyConfigs(psc->driver_configs);
      psc->driver_configs = NULL;
   }
#endif
   free((char *) psc->serverGLXexts);
   free((char *) psc->serverGLXvendor);
   free((char *) psc->serverGLXversion);
   free(psc->driverName);
}

static void
bind_extensions(struct glx_screen *psc, const char *driverName)
{
   unsigned mask;

   if (psc->display->driver != GLX_DRIVER_SW) {
      __glXEnableDirectExtension(psc, "GLX_EXT_buffer_age");
      __glXEnableDirectExtension(psc, "GLX_EXT_swap_control");
      __glXEnableDirectExtension(psc, "GLX_SGI_swap_control");
      __glXEnableDirectExtension(psc, "GLX_MESA_swap_control");
      __glXEnableDirectExtension(psc, "GLX_OML_sync_control");
      __glXEnableDirectExtension(psc, "GLX_SGI_video_sync");
      // for zink this needs to check whether RELAXED is available
      if (psc->display->driver == GLX_DRIVER_DRI3)
         __glXEnableDirectExtension(psc, "GLX_EXT_swap_control_tear");
   }
   if (psc->display->driver != GLX_DRIVER_ZINK_YES)
      __glXEnableDirectExtension(psc, "GLX_MESA_copy_sub_buffer");
   __glXEnableDirectExtension(psc, "GLX_SGI_make_current_read");

   if (psc->can_EXT_texture_from_pixmap)
      __glXEnableDirectExtension(psc, "GLX_EXT_texture_from_pixmap");

   /*
    * GLX_INTEL_swap_event is broken on the server side, where it's
    * currently unconditionally enabled. This completely breaks
    * systems running on drivers which don't support that extension.
    * There's no way to test for its presence on this side, so instead
    * of disabling it unconditionally, just disable it for drivers
    * which are known to not support it.
    *
    * This was fixed in xserver 1.15.0 (190b03215), so now we only
    * disable the broken driver.
    */
   if (!driverName || strcmp(driverName, "vmwgfx") != 0) {
      __glXEnableDirectExtension(psc, "GLX_INTEL_swap_event");
   }

   mask = driGetAPIMask(psc->frontend_screen);

   __glXEnableDirectExtension(psc, "GLX_ARB_create_context");
   __glXEnableDirectExtension(psc, "GLX_ARB_create_context_profile");
   __glXEnableDirectExtension(psc, "GLX_ARB_create_context_no_error");
   __glXEnableDirectExtension(psc, "GLX_EXT_no_config_context");

   if ((mask & ((1 << __DRI_API_GLES) |
                (1 << __DRI_API_GLES2) |
                (1 << __DRI_API_GLES3))) != 0) {
      __glXEnableDirectExtension(psc,
                                 "GLX_EXT_create_context_es_profile");
      __glXEnableDirectExtension(psc,
                                 "GLX_EXT_create_context_es2_profile");
   }

   if (dri_get_screen_param(psc->frontend_screen, PIPE_CAP_DEVICE_RESET_STATUS_QUERY))
      __glXEnableDirectExtension(psc,
                                 "GLX_ARB_create_context_robustness");

   __glXEnableDirectExtension(psc, "GLX_ARB_context_flush_control");
   __glXEnableDirectExtension(psc, "GLX_MESA_query_renderer");

   __glXEnableDirectExtension(psc, "GLX_MESA_gl_interop");

   char *tmp;
   if (dri2GalliumConfigQuerys(psc->frontend_screen, "glx_extension_override",
                                    &tmp) == 0)
      __glXParseExtensionOverride(psc, tmp);

   if (dri2GalliumConfigQuerys(psc->frontend_screen,
                                    "indirect_gl_extension_override",
                                    &tmp) == 0)
      __IndirectGlParseExtensionOverride(psc, tmp);

   {
      uint8_t force = false;
      if (dri2GalliumConfigQueryb(psc->frontend_screen, "force_direct_glx_context",
                                    &force) == 0) {
         psc->force_direct_context = force;
      }

      uint8_t invalid_glx_destroy_window = false;
      if (dri2GalliumConfigQueryb(psc->frontend_screen,
                                    "allow_invalid_glx_destroy_window",
                                    &invalid_glx_destroy_window) == 0) {
         psc->allow_invalid_glx_destroy_window = invalid_glx_destroy_window;
      }

      uint8_t keep_native_window_glx_drawable = false;
      if (dri2GalliumConfigQueryb(psc->frontend_screen,
                                    "keep_native_window_glx_drawable",
                                    &keep_native_window_glx_drawable) == 0) {
         psc->keep_native_window_glx_drawable = keep_native_window_glx_drawable;
      }
   }
}


/*
** Allocate the memory for the per screen configs for each screen.
** If that works then fetch the per screen configs data.
*/
static Bool
AllocAndFetchScreenConfigs(Display * dpy, struct glx_display * priv, enum glx_driver glx_driver, Bool driver_name_is_inferred)
{
   struct glx_screen *psc;
   GLint i, screens;
   unsigned screen_count = 0;
   bool zink = (glx_driver & (GLX_DRIVER_ZINK_INFER | GLX_DRIVER_ZINK_YES)) > 0;

   /*
    ** First allocate memory for the array of per screen configs.
    */
   screens = ScreenCount(dpy);
   priv->screens = calloc(screens, sizeof *priv->screens);
   if (!priv->screens)
      return GL_FALSE;

   for (i = 0; i < screens; i++) {
      psc = NULL;
#if defined(GLX_DIRECT_RENDERING) && !defined(GLX_USE_APPLEGL)
#if defined(GLX_USE_DRM)
      if (glx_driver & GLX_DRIVER_DRI3) {
         bool use_zink;
         psc = dri3_create_screen(i, priv, driver_name_is_inferred, &use_zink);
         if (use_zink) {
            glx_driver |= GLX_DRIVER_ZINK_YES;
            zink = true;
            driver_name_is_inferred = false;
         }
      }
#if defined(HAVE_X11_DRI2)
      if (psc == NULL && glx_driver & GLX_DRIVER_DRI2 && dri2CheckSupport(dpy)) {
	      psc = dri2CreateScreen(i, priv, driver_name_is_inferred);
         if (psc)
            priv->dri2Hash = __glxHashCreate();
      }
#endif
#endif /* GLX_USE_DRM */

#ifdef GLX_USE_WINDOWSGL
      if (psc == NULL && glx_driver & GLX_DRIVER_WINDOWS) {
	      psc = driwindowsCreateScreen(i, priv, driver_name_is_inferred);
      }
#endif

#endif /* GLX_DIRECT_RENDERING && !GLX_USE_APPLEGL */
#if defined(GLX_DIRECT_RENDERING) && (!defined(GLX_USE_APPLEGL) || defined(GLX_USE_APPLE))
      if (psc == NULL &&
          (glx_driver & GLX_DRIVER_SW || zink)) {
	      psc = driswCreateScreen(i, priv, glx_driver, driver_name_is_inferred);
      }
#endif

      bool indirect = false;

#if defined(GLX_USE_APPLEGL) && !defined(GLX_USE_APPLE)
      if (psc == NULL)
         psc = applegl_create_screen(i, priv);
#else
      if (psc == NULL && !zink)
      {
         psc = indirect_create_screen(i, priv);
         indirect = true;
      }
#endif
      priv->screens[i] = psc;
      if (psc)
         screen_count++;

      if(indirect) /* Load extensions required only for indirect glx */
         glxSendClientInfo(priv, i);
      else if (priv->driver != GLX_DRIVER_WINDOWS)
         bind_extensions(psc, psc->driverName);
   }
   if (zink && !screen_count)
      return GL_FALSE;
   SyncHandle();
   return GL_TRUE;
}

/*
** Initialize the client side extension code.
*/
 _X_HIDDEN struct glx_display *
__glXInitialize(Display * dpy)
{
   XExtCodes *codes;
   struct glx_display *dpyPriv, *d;
   int i, majorVersion = 0;

   _XLockMutex(_Xglobal_lock);

   for (dpyPriv = glx_displays; dpyPriv; dpyPriv = dpyPriv->next) {
      if (dpyPriv->dpy == dpy) {
	 _XUnlockMutex(_Xglobal_lock);
	 return dpyPriv;
      }
   }

   /* Drop the lock while we create the display private. */
   _XUnlockMutex(_Xglobal_lock);

   dpyPriv = calloc(1, sizeof *dpyPriv);
   if (!dpyPriv)
      return NULL;

   codes = XInitExtension(dpy, __glXExtensionName);
   if (!codes) {
      free(dpyPriv);
      return NULL;
   }

   dpyPriv->codes = *codes;
   dpyPriv->dpy = dpy;

   /* This GLX implementation requires GLX 1.3 */
   if (!QueryVersion(dpy, dpyPriv->codes.major_opcode,
		     &majorVersion, &dpyPriv->minorVersion)
       || (majorVersion != 1)
       || (majorVersion == 1 && dpyPriv->minorVersion < 3)) {
      free(dpyPriv);
      return NULL;
   }

   for (i = 0; i < __GLX_NUMBER_EVENTS; i++) {
      XESetWireToEvent(dpy, dpyPriv->codes.first_event + i, __glXWireToEvent);
      XESetEventToWire(dpy, dpyPriv->codes.first_event + i, __glXEventToWire);
   }

   XESetCloseDisplay(dpy, dpyPriv->codes.extension, __glXCloseDisplay);
   XESetErrorString (dpy, dpyPriv->codes.extension, __glXErrorString);

   dpyPriv->glXDrawHash = __glxHashCreate();

   enum glx_driver glx_driver = 0;
   const char *env = getenv("MESA_LOADER_DRIVER_OVERRIDE");

#if defined(GLX_DIRECT_RENDERING) && (!defined(GLX_USE_APPLEGL) || defined(GLX_USE_APPLE))
   Bool glx_direct = !debug_get_bool_option("LIBGL_ALWAYS_INDIRECT", false);
   Bool glx_accel = !debug_get_bool_option("LIBGL_ALWAYS_SOFTWARE", false);
   Bool dri3 = !debug_get_bool_option("LIBGL_DRI3_DISABLE", false);
   Bool kopper = !debug_get_bool_option("LIBGL_KOPPER_DISABLE", false);

   if (env && !strcmp(env, "zink"))
      glx_driver |= GLX_DRIVER_ZINK_YES;

   dpyPriv->drawHash = __glxHashCreate();

   dpyPriv->zombieGLXDrawable = _mesa_pointer_set_create(NULL);

   /* Set the logger before the *CreateDisplay functions. */
   loader_set_logger(glx_message);

   /*
    ** Initialize the direct rendering per display data and functions.
    ** Note: This _must_ be done before calling any other DRI routines
    ** (e.g., those called in AllocAndFetchScreenConfigs).
    */
#if defined(GLX_USE_DRM)
   bool dri3_err = false;
   if (glx_direct && glx_accel && dri3)
      dpyPriv->has_multibuffer = x11_dri3_check_multibuffer(XGetXCBConnection(dpy), &dri3_err, &dpyPriv->has_explicit_modifiers);
   if (glx_direct && glx_accel &&
       (!(glx_driver & GLX_DRIVER_ZINK_YES) || !kopper)) {
      if (dri3) {
         /* dri3 is tried as long as this doesn't error; whether modifiers work is not relevant */
         if (!dri3_err) {
            glx_driver |= GLX_DRIVER_DRI3;
            /* nouveau wants to fallback to zink so if we get a screen enable try_zink */
            if (!debug_get_bool_option("LIBGL_KOPPER_DISABLE", false))
               glx_driver |= GLX_DRIVER_ZINK_INFER;
         }
      }
#if defined(HAVE_X11_DRI2)
      if (!debug_get_bool_option("LIBGL_DRI2_DISABLE", false))
         glx_driver |= GLX_DRIVER_DRI2;
#endif
#if defined(HAVE_ZINK)
      if (!(glx_driver & (GLX_DRIVER_DRI2 | GLX_DRIVER_DRI3)))
         if (kopper && !getenv("GALLIUM_DRIVER"))
            glx_driver |= GLX_DRIVER_ZINK_INFER;
#endif /* HAVE_ZINK */
   }
#endif /* GLX_USE_DRM */
   if (glx_direct)
      glx_driver |= GLX_DRIVER_SW;

   if (!dpyPriv->has_explicit_modifiers && glx_accel && !debug_get_bool_option("LIBGL_KOPPER_DRI2", false)) {
      if (glx_driver & GLX_DRIVER_ZINK_YES) {
         /* only print error if zink was explicitly requested */
         CriticalErrorMessageF("DRI3 not available\n");
         free(dpyPriv);
         return NULL;
      }
      /* if no dri3 and not using dri2, disable zink */
      glx_driver &= ~GLX_DRIVER_ZINK_INFER;
   }

#ifdef GLX_USE_WINDOWSGL
   if (glx_direct && glx_accel)
      glx_driver |= GLX_DRIVER_WINDOWS;
#else
#ifndef RTLD_NOW
#define RTLD_NOW 0
#endif
#ifndef RTLD_GLOBAL
#define RTLD_GLOBAL 0
#endif

#ifndef GL_LIB_NAME
#define GL_LIB_NAME "libGL.so.1"
#endif

   void *glhandle = dlopen(GL_LIB_NAME, RTLD_NOW | RTLD_GLOBAL);
   if (glhandle)
      dlclose(glhandle);

#endif
#endif /* GLX_DIRECT_RENDERING && !GLX_USE_APPLEGL */

#if defined(GLX_USE_APPLEGL) && !defined(GLX_USE_APPLE)
   glx_driver |= GLX_DRIVER_SW;
#endif

   if (!AllocAndFetchScreenConfigs(dpy, dpyPriv, glx_driver, !env)) {
      Bool fail = True;
#if defined(GLX_DIRECT_RENDERING) && (!defined(GLX_USE_APPLEGL) || defined(GLX_USE_APPLE))
      if (glx_driver & GLX_DRIVER_ZINK_INFER) {
         fail = !AllocAndFetchScreenConfigs(dpy, dpyPriv, GLX_DRIVER_SW, true);
      }
#endif
      if (fail) {
         free(dpyPriv);
         return NULL;
      }
   }

   glxSendClientInfo(dpyPriv, -1);

   /* Grab the lock again and add the display private, unless somebody
    * beat us to initializing on this display in the meantime. */
   _XLockMutex(_Xglobal_lock);

   for (d = glx_displays; d; d = d->next) {
      if (d->dpy == dpy) {
	 _XUnlockMutex(_Xglobal_lock);
	 glx_display_free(dpyPriv);
	 return d;
      }
   }

   dpyPriv->next = glx_displays;
   glx_displays = dpyPriv;

   _XUnlockMutex(_Xglobal_lock);

   return dpyPriv;
}

/*
** Setup for sending a GLX command on dpy.  Make sure the extension is
** initialized.  Try to avoid calling __glXInitialize as its kinda slow.
*/
_X_HIDDEN CARD8
__glXSetupForCommand(Display * dpy)
{
    struct glx_context *gc;
    struct glx_display *priv;

   /* If this thread has a current context, flush its rendering commands */
   gc = __glXGetCurrentContext();
   if (gc->currentDpy) {
      /* Flush rendering buffer of the current context, if any */
      (void) __glXFlushRenderBuffer(gc, gc->pc);

      if (gc->currentDpy == dpy) {
         /* Use opcode from gc because its right */
         return gc->majorOpcode;
      }
      else {
         /*
          ** Have to get info about argument dpy because it might be to
          ** a different server
          */
      }
   }

   /* Forced to lookup extension via the slow initialize route */
   priv = __glXInitialize(dpy);
   if (!priv) {
      return 0;
   }
   return priv->codes.major_opcode;
}

/**
 * Flush the drawing command transport buffer.
 *
 * \param ctx  Context whose transport buffer is to be flushed.
 * \param pc   Pointer to first unused buffer location.
 *
 * \todo
 * Modify this function to use \c ctx->pc instead of the explicit
 * \c pc parameter.
 */
_X_HIDDEN GLubyte *
__glXFlushRenderBuffer(struct glx_context * ctx, GLubyte * pc)
{
   Display *const dpy = ctx->currentDpy;
   xcb_connection_t *c = XGetXCBConnection(dpy);
   const GLint size = pc - ctx->buf;

   if ((dpy != NULL) && (size > 0)) {
      xcb_glx_render(c, ctx->currentContextTag, size,
                     (const uint8_t *) ctx->buf);
   }

   /* Reset pointer and return it */
   ctx->pc = ctx->buf;
   return ctx->pc;
}


/**
 * Send a portion of a GLXRenderLarge command to the server.  The advantage of
 * this function over \c __glXSendLargeCommand is that callers can use the
 * data buffer in the GLX context and may be able to avoid allocating an
 * extra buffer.  The disadvantage is the clients will have to do more
 * GLX protocol work (i.e., calculating \c totalRequests, etc.).
 *
 * \sa __glXSendLargeCommand
 *
 * \param gc             GLX context
 * \param requestNumber  Which part of the whole command is this?  The first
 *                       request is 1.
 * \param totalRequests  How many requests will there be?
 * \param data           Command data.
 * \param dataLen        Size, in bytes, of the command data.
 */
_X_HIDDEN void
__glXSendLargeChunk(struct glx_context * gc, GLint requestNumber,
                    GLint totalRequests, const GLvoid * data, GLint dataLen)
{
   Display *dpy = gc->currentDpy;
   xcb_connection_t *c = XGetXCBConnection(dpy);
   xcb_glx_render_large(c, gc->currentContextTag, requestNumber,
                        totalRequests, dataLen, data);
}


/**
 * Send a command that is too large for the GLXRender protocol request.
 *
 * Send a large command, one that is too large for some reason to
 * send using the GLXRender protocol request.  One reason to send
 * a large command is to avoid copying the data.
 *
 * \param ctx        GLX context
 * \param header     Header data.
 * \param headerLen  Size, in bytes, of the header data.  It is assumed that
 *                   the header data will always be small enough to fit in
 *                   a single X protocol packet.
 * \param data       Command data.
 * \param dataLen    Size, in bytes, of the command data.
 */
_X_HIDDEN void
__glXSendLargeCommand(struct glx_context * ctx,
                      const GLvoid * header, GLint headerLen,
                      const GLvoid * data, GLint dataLen)
{
   GLint maxSize;
   GLint totalRequests, requestNumber;

   /*
    ** Calculate the maximum amount of data can be stuffed into a single
    ** packet.  sz_xGLXRenderReq is added because bufSize is the maximum
    ** packet size minus sz_xGLXRenderReq.
    */
   maxSize = (ctx->bufSize + sz_xGLXRenderReq) - sz_xGLXRenderLargeReq;
   totalRequests = 1 + (dataLen / maxSize);
   if (dataLen % maxSize)
      totalRequests++;

   /*
    ** Send all of the command, except the large array, as one request.
    */
   assert(headerLen <= maxSize);
   __glXSendLargeChunk(ctx, 1, totalRequests, header, headerLen);

   /*
    ** Send enough requests until the whole array is sent.
    */
   for (requestNumber = 2; requestNumber <= (totalRequests - 1);
        requestNumber++) {
      __glXSendLargeChunk(ctx, requestNumber, totalRequests, data, maxSize);
      data = (const GLvoid *) (((const GLubyte *) data) + maxSize);
      dataLen -= maxSize;
      assert(dataLen > 0);
   }

   assert(dataLen <= maxSize);
   __glXSendLargeChunk(ctx, requestNumber, totalRequests, data, dataLen);
}
