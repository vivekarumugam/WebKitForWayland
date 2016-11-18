/*
 * Copyright (C) 2007, 2009 Apple Inc.  All rights reserved.
 * Copyright (C) 2007 Collabora Ltd.  All rights reserved.
 * Copyright (C) 2007 Alp Toker <alp@atoker.com>
 * Copyright (C) 2009 Gustavo Noronha Silva <gns@gnome.org>
 * Copyright (C) 2009, 2010 Igalia S.L
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * aint with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "MediaPlayerPrivateGStreamerBase.h"

#if ENABLE(VIDEO) && USE(GSTREAMER)

#include "ColorSpace.h"
#include "GStreamerUtilities.h"
#include "GraphicsContext.h"
#include "GraphicsTypes.h"
#include "ImageGStreamer.h"
#include "ImageOrientation.h"
#include "IntRect.h"
#include "MediaPlayer.h"
#include "NotImplemented.h"
#include "UUID.h"
#include "VideoSinkGStreamer.h"
#include "WebKitWebSourceGStreamer.h"
#include <wtf/glib/GMutexLocker.h>
#include <wtf/glib/GUniquePtr.h>
#include <wtf/text/CString.h>
#include <wtf/MathExtras.h>

#include <gst/audio/streamvolume.h>
#include <gst/video/gstvideometa.h>

#if USE(GSTREAMER_GL)
#include <gst/app/gstappsink.h>
#define GST_USE_UNSTABLE_API
#include <gst/gl/gl.h>
#undef GST_USE_UNSTABLE_API

#include "GLContext.h"
#if USE(GLX)
#include "GLContextGLX.h"
#include <gst/gl/x11/gstgldisplay_x11.h>
#elif USE(EGL)
#include "GLContextEGL.h"
#include <gst/gl/egl/gstgldisplay_egl.h>
#endif

#if PLATFORM(X11)
#include "PlatformDisplayX11.h"
#elif PLATFORM(WAYLAND)
#include "PlatformDisplayWayland.h"
#elif PLATFORM(WPE)
#include "PlatformDisplayWPE.h"
#endif

// gstglapi.h may include eglplatform.h and it includes X.h, which
// defines None, breaking MediaPlayer::None enum
#if PLATFORM(X11) && GST_GL_HAVE_PLATFORM_EGL
#undef None
#endif // PLATFORM(X11) && GST_GL_HAVE_PLATFORM_EGL
#endif // USE(GSTREAMER_GL)

#if GST_CHECK_VERSION(1, 1, 0) && USE(TEXTURE_MAPPER_GL)
#include "BitmapTextureGL.h"
#include "BitmapTexturePool.h"
#include "TextureMapperGL.h"
#endif
#if USE(COORDINATED_GRAPHICS_THREADED)
#include "TextureMapperPlatformLayerBuffer.h"
#endif

#define WL_EGL_PLATFORM

#if USE(OPENGL_ES_2)
#if GST_CHECK_VERSION(1, 8, 1)
#if !USE(HOLE_PUNCH_GSTREAMER)
#define GST_USE_UNSTABLE_API
#include <gst/gl/egl/gstglmemoryegl.h>
#undef GST_USE_UNSTABLE_API
#endif
#endif
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#endif

#include <EGL/egl.h>

#if ENABLE(ENCRYPTED_MEDIA)
#include "WebKitClearKeyDecryptorGStreamer.h"
#endif

#if ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2)
#include <runtime/JSCInlines.h>
#include <runtime/TypedArrayInlines.h>
#include <runtime/Uint8Array.h>
#endif

#if ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2)
#if ENABLE(ENCRYPTED_MEDIA_V2)
#include "CDMPRSessionGStreamer.h"
#if PLATFORM(WPE)
#include <wpe/CDMPrivateEncKeyWPE.h>
#include <wpe/CDMSessionEncKeyWPE.h>
#include <Modules/encryptedmedia/CDM.h>
#include <Modules/encryptedmedia/CDMPrivateClearKey.h>
#include "WebKitOpenCDMiWidevineDecryptorGStreamer.h"
#endif
#endif
#if USE(PLAYREADY)
#include "PlayreadySession.h"
#endif
#include "WebKitPlayReadyDecryptorGStreamer.h"
#endif

#if USE(CAIRO) && ENABLE(ACCELERATED_2D_CANVAS)
#include <cairo-gl.h>
#endif

GST_DEBUG_CATEGORY(webkit_media_player_debug);
#define GST_CAT_DEFAULT webkit_media_player_debug

using namespace std;

namespace WebCore {

void registerWebKitGStreamerElements()
{
    if (!webkitGstCheckVersion(1, 6, 1))
        return;

#if ENABLE(ENCRYPTED_MEDIA)
    GRefPtr<GstElementFactory> clearKeyDecryptorFactory = gst_element_factory_find("webkitclearkey");
    if (!clearKeyDecryptorFactory)
        gst_element_register(0, "webkitclearkey", GST_RANK_PRIMARY + 100, WEBKIT_TYPE_MEDIA_CK_DECRYPT);
#endif

#if (ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2))
#if  USE(PLAYREADY)
    GRefPtr<GstElementFactory> playReadyDecryptorFactory = gst_element_factory_find("webkitplayreadydec");
    if (!playReadyDecryptorFactory)
        gst_element_register(0, "webkitplayreadydec", GST_RANK_PRIMARY + 100, WEBKIT_TYPE_MEDIA_PLAYREADY_DECRYPT);
#endif

    GRefPtr<GstElementFactory> widevineDecryptorFactory = gst_element_factory_find("webkitwidevine");
    if (!widevineDecryptorFactory)
        gst_element_register(0, "webkitwidevine", GST_RANK_PRIMARY + 100, OPENCDMI_TYPE_WIDEVINE_DECRYPT);
#endif
}

static int greatestCommonDivisor(int a, int b)
{
    while (b) {
        int temp = a;
        a = b;
        b = temp % b;
    }

    return ABS(a);
}

#if USE(COORDINATED_GRAPHICS_THREADED) && USE(GSTREAMER_GL)
class GstVideoFrameHolder : public TextureMapperPlatformLayerBuffer::UnmanagedBufferDataHolder {
public:
    explicit GstVideoFrameHolder(GstSample* sample, TextureMapperGL::Flags flags)
    {
        GstVideoInfo videoInfo;
        if (UNLIKELY(!getSampleVideoInfo(sample, videoInfo)))
            return;

        m_size = IntSize(GST_VIDEO_INFO_WIDTH(&videoInfo), GST_VIDEO_INFO_HEIGHT(&videoInfo));
        m_flags = flags | (GST_VIDEO_INFO_HAS_ALPHA(&videoInfo) ? TextureMapperGL::ShouldBlend : 0);

        GstBuffer* buffer = gst_sample_get_buffer(sample);
        if (UNLIKELY(!gst_video_frame_map(&m_videoFrame, &videoInfo, buffer, static_cast<GstMapFlags>(GST_MAP_READ | GST_MAP_GL))))
            return;

        m_textureID = *reinterpret_cast<GLuint*>(m_videoFrame.data[0]);
        m_isValid = true;
    }

    virtual ~GstVideoFrameHolder()
    {
        if (UNLIKELY(!m_isValid))
            return;

        gst_video_frame_unmap(&m_videoFrame);
    }

    const IntSize& size() const { return m_size; }
    TextureMapperGL::Flags flags() const { return m_flags; }
    GLuint textureID() const { return m_textureID; }
    GC3Dint internalFormat() const { return m_internalFormat; }
    bool isValid() const { return m_isValid; }

private:
    GstVideoFrame m_videoFrame;
    IntSize m_size;
    TextureMapperGL::Flags m_flags;
    GLuint m_textureID;
    GC3Dint m_internalFormat;
    bool m_isValid { false };
};
#endif // USE(COORDINATED_GRAPHICS_THREADED) && USE(GSTREAMER_GL)

MediaPlayerPrivateGStreamerBase::MediaPlayerPrivateGStreamerBase(MediaPlayer* player)
    : m_player(player)
    , m_fpsSink(0)
    , m_readyState(MediaPlayer::HaveNothing)
    , m_networkState(MediaPlayer::Empty)
    , m_isEndReached(false)
    , m_sample(0)
#if USE(GSTREAMER_GL)
    , m_drawTimer(RunLoop::main(), this, &MediaPlayerPrivateGStreamerBase::repaint)
#endif
    , m_repaintHandler(0)
    , m_usingFallbackVideoSink(false)
#if ENABLE(ENCRYPTED_MEDIA)
#if USE(PLAYREADY)
    , m_prSession(0)
#endif
#endif
#if ENABLE(ENCRYPTED_MEDIA_V2)
    , m_cdmSession(0)
#endif
#if USE(TEXTURE_MAPPER_GL)
    , m_textureMapperRotationFlag(0)
#endif
{
    g_mutex_init(&m_sampleMutex);
#if USE(COORDINATED_GRAPHICS_THREADED)
    m_platformLayerProxy = adoptRef(new TextureMapperPlatformLayerProxy());
#endif

#if USE(HOLE_PUNCH_GSTREAMER)
#if USE(COORDINATED_GRAPHICS_THREADED)
    LockHolder locker(m_platformLayerProxy->lock());
    m_platformLayerProxy->pushNextBuffer(std::make_unique<TextureMapperPlatformLayerBuffer>(0, m_size, TextureMapperGL::ShouldOverwriteRect, GraphicsContext3D::DONT_CARE));
#endif
#endif

}

MediaPlayerPrivateGStreamerBase::~MediaPlayerPrivateGStreamerBase()
{
    m_notifier.cancelPendingNotifications();

    if (m_videoSink) {
        g_signal_handlers_disconnect_matched(m_videoSink.get(), G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);
#if USE(GSTREAMER_GL)
        if (GST_IS_BIN(m_videoSink.get())) {
            GRefPtr<GstElement> appsink = adoptGRef(gst_bin_get_by_name(GST_BIN_CAST(m_videoSink.get()), "webkit-gl-video-sink"));
            g_signal_handlers_disconnect_by_data(appsink.get(), this);
        }
#endif
    }

    g_mutex_clear(&m_sampleMutex);

    m_player = nullptr;

    if (m_volumeElement)
        g_signal_handlers_disconnect_matched(m_volumeElement.get(), G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, this);


#if ENABLE(ENCRYPTED_MEDIA)
#if USE(PLAYREADY)
    if (m_prSession)
        delete m_prSession;
    m_prSession = nullptr;
#endif
#elif ENABLE(ENCRYPTED_MEDIA_V2)
    m_cdmSession = nullptr;
#endif


#if USE(TEXTURE_MAPPER_GL) && !USE(COORDINATED_GRAPHICS)
    if (client())
        client()->platformLayerWillBeDestroyed();
#endif
}

void MediaPlayerPrivateGStreamerBase::setPipeline(GstElement* pipeline)
{
    m_pipeline = pipeline;
#if USE(HOLE_PUNCH_GSTREAMER) && (USE(WESTEROS_SINK) || USE(FUSION_SINK))
    updateVideoRectangle();
#endif
}

void MediaPlayerPrivateGStreamerBase::clearSamples()
{
#if USE(COORDINATED_GRAPHICS_THREADED)
    // Disconnect handlers to ensure that new samples aren't going to arrive
    // before the pipeline destruction
    if (m_repaintHandler) {
        g_signal_handler_disconnect(m_videoSink.get(), m_repaintHandler);
        m_repaintHandler = 0;
    }
#endif

    WTF::GMutexLocker<GMutex> lock(m_sampleMutex);
    m_sample = nullptr;
}

bool MediaPlayerPrivateGStreamerBase::handleSyncMessage(GstMessage* message)
{
    UNUSED_PARAM(message);
#if USE(GSTREAMER_GL)
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_NEED_CONTEXT) {
        const gchar* contextType;
        gst_message_parse_context_type(message, &contextType);

        if (!ensureGstGLContext())
            return false;

        if (!g_strcmp0(contextType, GST_GL_DISPLAY_CONTEXT_TYPE)) {
            GRefPtr<GstContext> displayContext = adoptGRef(gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE));
            gst_context_set_gl_display(displayContext.get(), m_glDisplay.get());
            gst_element_set_context(GST_ELEMENT(message->src), displayContext.get());
            return true;
        }

        if (!g_strcmp0(contextType, "gst.gl.app_context")) {
            GRefPtr<GstContext> appContext = adoptGRef(gst_context_new("gst.gl.app_context", TRUE));
            GstStructure* structure = gst_context_writable_structure(appContext.get());
            gst_structure_set(structure, "context", GST_GL_TYPE_CONTEXT, m_glContext.get(), nullptr);
            gst_element_set_context(GST_ELEMENT(message->src), appContext.get());
            return true;
        }
    }
#endif // USE(GSTREAMER_GL)

#if ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2)
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ELEMENT) {
        const GstStructure* structure = gst_message_get_structure(message);
        if (gst_structure_has_name(structure, "drm-key-needed")) {
            GST_DEBUG("handling drm-key-needed message");
#if USE(PLAYREADY)
            PlayreadySession* session = prSession();
            if (session && session->keyRequested()) {
                GST_DEBUG("key requested already");
                if (session->ready()) {
                    GST_DEBUG("key already negotiated");
                    emitSession();
                }
                return false;
            }
#endif
            // Here we receive the DRM init data from the pipeline: we will emit
            // the needkey event with that data and the browser might create a
            // CDMSession from this event handler. If such a session was created
            // We will emit the message event from the session to provide the
            // DRM challenge to the browser and wait for an update. If on the
            // contrary no session was created we won't wait and let the pipeline
            // error out by itself.
            GstBuffer* data;
            const char* keySystemId;
            gboolean valid = gst_structure_get(structure, "data", GST_TYPE_BUFFER, &data,
                                               "key-system-id", G_TYPE_STRING, &keySystemId, nullptr);
            GstMapInfo mapInfo;
            if (!valid || !gst_buffer_map(data, &mapInfo, GST_MAP_READ))
                return false;

            GST_DEBUG("scheduling keyNeeded event");
            // FIXME: Provide a somehow valid sessionId.
#if ENABLE(ENCRYPTED_MEDIA)
            needKey(keySystemId, "sessionId", reinterpret_cast<const unsigned char *>(mapInfo.data), mapInfo.size);
#elif ENABLE(ENCRYPTED_MEDIA_V2)
            RefPtr<Uint8Array> initData = Uint8Array::create(reinterpret_cast<const unsigned char *>(mapInfo.data), mapInfo.size);
            needKey(initData);
#else
            ASSERT_NOT_REACHED();
#endif
            gst_buffer_unmap(data, &mapInfo);
            return true;
        }
    }
#endif // ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2)

    return false;
}

#if USE(GSTREAMER_GL)
bool MediaPlayerPrivateGStreamerBase::ensureGstGLContext()
{
    if (m_glContext)
        return true;

    if (!m_glDisplay) {
        const auto& sharedDisplay = PlatformDisplay::sharedDisplay();
#if PLATFORM(X11)
        m_glDisplay = GST_GL_DISPLAY(gst_gl_display_x11_new_with_display(downcast<PlatformDisplayX11>(sharedDisplay).native()));
#elif PLATFORM(WAYLAND)
        m_glDisplay = GST_GL_DISPLAY(gst_gl_display_egl_new_with_egl_display(downcast<PlatformDisplayWayland>(sharedDisplay).eglDisplay()));
#elif PLATFORM(WPE)
        m_glDisplay = GST_GL_DISPLAY(gst_gl_display_egl_new_with_egl_display(downcast<PlatformDisplayWPE>(sharedDisplay).eglDisplay()));
#endif
    }

    GLContext* webkitContext = GLContext::sharingContext();
    // EGL and GLX are mutually exclusive, no need for ifdefs here.
    GstGLPlatform glPlatform = webkitContext->isEGLContext() ? GST_GL_PLATFORM_EGL : GST_GL_PLATFORM_GLX;

#if USE(OPENGL_ES_2)
    GstGLAPI glAPI = GST_GL_API_GLES2;
#elif USE(OPENGL)
    GstGLAPI glAPI = GST_GL_API_OPENGL;
#else
    ASSERT_NOT_REACHED();
#endif

    PlatformGraphicsContext3D contextHandle = webkitContext->platformContext();
    if (!contextHandle)
        return false;

    m_glContext = gst_gl_context_new_wrapped(m_glDisplay.get(), reinterpret_cast<guintptr>(contextHandle), glPlatform, glAPI);

    return true;
}
#endif // USE(GSTREAMER_GL)

// Returns the size of the video
FloatSize MediaPlayerPrivateGStreamerBase::naturalSize() const
{
#if USE(HOLE_PUNCH_GSTREAMER)
    // We don't care about the natural size of the video, the external sink will deal with it.
    // This means that the video will always have the size of the <video> component or the default 300x150
    return m_size;
#endif

    if (!hasVideo())
        return FloatSize();

    if (!m_videoSize.isEmpty())
        return m_videoSize;

    WTF::GMutexLocker<GMutex> lock(m_sampleMutex);

    GRefPtr<GstCaps> caps;
    // We may not have enough data available for the video sink yet.
    if (!GST_IS_SAMPLE(m_sample.get()))
        return FloatSize();

    if (GST_IS_SAMPLE(m_sample.get()) && !caps)
        caps = gst_sample_get_caps(m_sample.get());

    if (!caps) {
        GRefPtr<GstPad> videoSinkPad = adoptGRef(gst_element_get_static_pad(m_videoSink.get(), "sink"));
        if (videoSinkPad)
            caps = gst_pad_get_current_caps(videoSinkPad.get());
    }

    if (!caps)
        return FloatSize();

    // TODO: handle possible clean aperture data. See
    // https://bugzilla.gnome.org/show_bug.cgi?id=596571
    // TODO: handle possible transformation matrix. See
    // https://bugzilla.gnome.org/show_bug.cgi?id=596326

    // Get the video PAR and original size, if this fails the
    // video-sink has likely not yet negotiated its caps.
    int pixelAspectRatioNumerator, pixelAspectRatioDenominator, stride;
    IntSize originalSize;
    GstVideoFormat format;
    if (!getVideoSizeAndFormatFromCaps(caps.get(), originalSize, format, pixelAspectRatioNumerator, pixelAspectRatioDenominator, stride))
        return FloatSize();

#if USE(TEXTURE_MAPPER_GL)
    // When using accelerated compositing, if the video is tagged as rotated 90 or 270 degrees, swap width and height.
    if (m_player->client().mediaPlayerRenderingCanBeAccelerated(m_player)) {
        if (m_videoSourceOrientation.usesWidthAsHeight())
            originalSize = originalSize.transposedSize();
    }
#endif

    GST_DEBUG("Original video size: %dx%d", originalSize.width(), originalSize.height());
    GST_DEBUG("Pixel aspect ratio: %d/%d", pixelAspectRatioNumerator, pixelAspectRatioDenominator);

    // Calculate DAR based on PAR and video size.
    int displayWidth = originalSize.width() * pixelAspectRatioNumerator;
    int displayHeight = originalSize.height() * pixelAspectRatioDenominator;

    // Divide display width and height by their GCD to avoid possible overflows.
    int displayAspectRatioGCD = greatestCommonDivisor(displayWidth, displayHeight);
    displayWidth /= displayAspectRatioGCD;
    displayHeight /= displayAspectRatioGCD;

    // Apply DAR to original video size. This is the same behavior as in xvimagesink's setcaps function.
    guint64 width = 0, height = 0;
    if (!(originalSize.height() % displayHeight)) {
        GST_DEBUG("Keeping video original height");
        width = gst_util_uint64_scale_int(originalSize.height(), displayWidth, displayHeight);
        height = static_cast<guint64>(originalSize.height());
    } else if (!(originalSize.width() % displayWidth)) {
        GST_DEBUG("Keeping video original width");
        height = gst_util_uint64_scale_int(originalSize.width(), displayHeight, displayWidth);
        width = static_cast<guint64>(originalSize.width());
    } else {
        GST_DEBUG("Approximating while keeping original video height");
        width = gst_util_uint64_scale_int(originalSize.height(), displayWidth, displayHeight);
        height = static_cast<guint64>(originalSize.height());
    }

    GST_DEBUG("Natural size: %" G_GUINT64_FORMAT "x%" G_GUINT64_FORMAT, width, height);
    m_videoSize = FloatSize(static_cast<int>(width), static_cast<int>(height));
    return m_videoSize;
}

void MediaPlayerPrivateGStreamerBase::setVolume(float volume)
{
    if (!m_volumeElement)
        return;

    GST_DEBUG("Setting volume: %f", volume);
    gst_stream_volume_set_volume(m_volumeElement.get(), GST_STREAM_VOLUME_FORMAT_CUBIC, static_cast<double>(volume));
}

#if PLATFORM(WPE)
float MediaPlayerPrivateGStreamerBase::volume() const
{
    if (!m_volumeElement)
        return 0;

    return gst_stream_volume_get_volume(m_volumeElement.get(), GST_STREAM_VOLUME_FORMAT_CUBIC);
}
#endif

void MediaPlayerPrivateGStreamerBase::notifyPlayerOfVolumeChange()
{
    if (!m_player || !m_volumeElement)
        return;
    double volume;
    volume = gst_stream_volume_get_volume(m_volumeElement.get(), GST_STREAM_VOLUME_FORMAT_CUBIC);
    // get_volume() can return values superior to 1.0 if the user
    // applies software user gain via third party application (GNOME
    // volume control for instance).
    volume = CLAMP(volume, 0.0, 1.0);
    m_player->volumeChanged(static_cast<float>(volume));
}

void MediaPlayerPrivateGStreamerBase::volumeChangedCallback(MediaPlayerPrivateGStreamerBase* player)
{
    // This is called when m_volumeElement receives the notify::volume signal.
    GST_DEBUG("Volume changed to: %f", player->volume());

    player->m_notifier.notify(MainThreadNotification::VolumeChanged, [player] { player->notifyPlayerOfVolumeChange(); });
}

MediaPlayer::NetworkState MediaPlayerPrivateGStreamerBase::networkState() const
{
    return m_networkState;
}

MediaPlayer::ReadyState MediaPlayerPrivateGStreamerBase::readyState() const
{
    return m_readyState;
}

void MediaPlayerPrivateGStreamerBase::sizeChanged()
{
    notImplemented();
}

void MediaPlayerPrivateGStreamerBase::setMuted(bool muted)
{
    if (!m_volumeElement)
        return;

    g_object_set(m_volumeElement.get(), "mute", muted, NULL);
}

bool MediaPlayerPrivateGStreamerBase::muted() const
{
    if (!m_volumeElement)
        return false;

    bool muted;
    g_object_get(m_volumeElement.get(), "mute", &muted, NULL);
    return muted;
}

void MediaPlayerPrivateGStreamerBase::notifyPlayerOfMute()
{
    if (!m_player || !m_volumeElement)
        return;

    gboolean muted;
    g_object_get(m_volumeElement.get(), "mute", &muted, NULL);
    m_player->muteChanged(static_cast<bool>(muted));
}

void MediaPlayerPrivateGStreamerBase::muteChangedCallback(MediaPlayerPrivateGStreamerBase* player)
{
    // This is called when m_volumeElement receives the notify::mute signal.
    player->m_notifier.notify(MainThreadNotification::MuteChanged, [player] { player->notifyPlayerOfMute(); });
}

#if USE(TEXTURE_MAPPER_GL) && !USE(COORDINATED_GRAPHICS_MULTIPROCESS)
void MediaPlayerPrivateGStreamerBase::updateTexture(BitmapTextureGL& texture, GstVideoInfo& videoInfo)
{
    GstBuffer* buffer = gst_sample_get_buffer(m_sample.get());

#if GST_CHECK_VERSION(1, 1, 0)
    GstVideoGLTextureUploadMeta* meta;
    if ((meta = gst_buffer_get_video_gl_texture_upload_meta(buffer))) {
        if (meta->n_textures == 1) { // BRGx & BGRA formats use only one texture.
            guint ids[4] = { texture.id(), 0, 0, 0 };

            if (gst_video_gl_texture_upload_meta_upload(meta, ids))
                return;
        }
    }
#endif

    // Right now the TextureMapper only supports chromas with one plane
    ASSERT(GST_VIDEO_INFO_N_PLANES(&videoInfo) == 1);

    GstVideoFrame videoFrame;
    if (!gst_video_frame_map(&videoFrame, &videoInfo, buffer, GST_MAP_READ))
        return;

    int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&videoFrame, 0);
    const void* srcData = GST_VIDEO_FRAME_PLANE_DATA(&videoFrame, 0);
    texture.updateContents(srcData, WebCore::IntRect(0, 0, GST_VIDEO_INFO_WIDTH(&videoInfo), GST_VIDEO_INFO_HEIGHT(&videoInfo)), WebCore::IntPoint(0, 0), stride, BitmapTexture::UpdateCannotModifyOriginalImageData);
    gst_video_frame_unmap(&videoFrame);
}
#endif

#if USE(COORDINATED_GRAPHICS_THREADED)
void MediaPlayerPrivateGStreamerBase::pushTextureToCompositor()
{
#if !USE(GSTREAMER_GL)
    class ConditionNotifier {
    public:
        ConditionNotifier(Lock& lock, Condition& condition)
            : m_locker(lock), m_condition(condition)
        {
        }
        ~ConditionNotifier()
        {
            m_condition.notifyOne();
        }
    private:
        LockHolder m_locker;
        Condition& m_condition;
    };
    ConditionNotifier notifier(m_drawMutex, m_drawCondition);
#endif

    WTF::GMutexLocker<GMutex> lock(m_sampleMutex);
    if (!GST_IS_SAMPLE(m_sample.get()))
        return;

    LockHolder holder(m_platformLayerProxy->lock());

    if (!m_platformLayerProxy->isActive())
        return;

#if USE(GSTREAMER_GL)
    std::unique_ptr<GstVideoFrameHolder> frameHolder = std::make_unique<GstVideoFrameHolder>(m_sample.get(), m_textureMapperRotationFlag);
    if (UNLIKELY(!frameHolder->isValid()))
        return;

    std::unique_ptr<TextureMapperPlatformLayerBuffer> layerBuffer = std::make_unique<TextureMapperPlatformLayerBuffer>(frameHolder->textureID(), frameHolder->size(), frameHolder->flags(), GraphicsContext3D::RGBA);
    layerBuffer->setUnmanagedBufferDataHolder(WTFMove(frameHolder));
    m_platformLayerProxy->pushNextBuffer(WTFMove(layerBuffer));
#else
    GstVideoInfo videoInfo;
    if (UNLIKELY(!getSampleVideoInfo(m_sample.get(), videoInfo)))
        return;

    IntSize size = IntSize(GST_VIDEO_INFO_WIDTH(&videoInfo), GST_VIDEO_INFO_HEIGHT(&videoInfo));
    std::unique_ptr<TextureMapperPlatformLayerBuffer> buffer = m_platformLayerProxy->getAvailableBuffer(size, GraphicsContext3D::DONT_CARE);
    if (UNLIKELY(!buffer)) {
        if (UNLIKELY(!m_context3D))
            m_context3D = GraphicsContext3D::create(GraphicsContext3D::Attributes(), nullptr, GraphicsContext3D::RenderToCurrentGLContext);

        RefPtr<BitmapTexture> texture = adoptRef(new BitmapTextureGL(m_context3D.copyRef()));
        texture->reset(size, GST_VIDEO_INFO_HAS_ALPHA(&videoInfo) ? BitmapTexture::SupportsAlpha : BitmapTexture::NoFlag);
        buffer = std::make_unique<TextureMapperPlatformLayerBuffer>(WTFMove(texture));
    }
    updateTexture(buffer->textureGL(), videoInfo);
    buffer->setExtraFlags(m_textureMapperRotationFlag | (GST_VIDEO_INFO_HAS_ALPHA(&videoInfo) ? TextureMapperGL::ShouldBlend : 0));
    m_platformLayerProxy->pushNextBuffer(WTFMove(buffer));
#endif
}
#endif

void MediaPlayerPrivateGStreamerBase::repaint()
{
    ASSERT(m_sample);
    ASSERT(isMainThread());

#if USE(TEXTURE_MAPPER_GL) && !USE(COORDINATED_GRAPHICS)
    if (supportsAcceleratedRendering() && m_player->client().mediaPlayerRenderingCanBeAccelerated(m_player) && client()) {
        client()->setPlatformLayerNeedsDisplay();
#if USE(GSTREAMER_GL)
        m_drawCondition.notifyOne();
#endif
        return;
    }
#endif

    m_player->repaint();

#if USE(GSTREAMER_GL)
    m_drawCondition.notifyOne();
#endif
}

void MediaPlayerPrivateGStreamerBase::triggerRepaint(GstSample* sample)
{
    bool triggerResize;
    {
        WTF::GMutexLocker<GMutex> lock(m_sampleMutex);
        triggerResize = !m_sample;
        m_sample = sample;
    }

    if (triggerResize) {
        GST_DEBUG("First sample reached the sink, triggering video dimensions update");
        m_notifier.notify(MainThreadNotification::SizeChanged, [this] { m_player->sizeChanged(); });
    }

#if USE(COORDINATED_GRAPHICS_THREADED)
#if USE(GSTREAMER_GL)
    pushTextureToCompositor();
#else
    {
        LockHolder lock(m_drawMutex);
        if (!m_platformLayerProxy->scheduleUpdateOnCompositorThread([this] { this->pushTextureToCompositor(); }))
            return;
        m_drawCondition.wait(m_drawMutex);
    }
#endif
    return;
#else
#if USE(GSTREAMER_GL)
    {
        ASSERT(!isMainThread());

        LockHolder locker(m_drawMutex);
        m_drawTimer.startOneShot(0);
        m_drawCondition.wait(m_drawMutex);
    }
#else
    repaint();
#endif
#endif
}

#if !USE(HOLE_PUNCH_GSTREAMER)
void MediaPlayerPrivateGStreamerBase::repaintCallback(MediaPlayerPrivateGStreamerBase* player, GstSample* sample)
{
    player->triggerRepaint(sample);
}
#endif

#if USE(GSTREAMER_GL)
GstFlowReturn MediaPlayerPrivateGStreamerBase::newSampleCallback(GstElement* sink, MediaPlayerPrivateGStreamerBase* player)
{
    GRefPtr<GstSample> sample = adoptGRef(gst_app_sink_pull_sample(GST_APP_SINK(sink)));
    player->triggerRepaint(sample.get());
    return GST_FLOW_OK;
}

GstFlowReturn MediaPlayerPrivateGStreamerBase::newPrerollCallback(GstElement* sink, MediaPlayerPrivateGStreamerBase* player)
{
    GRefPtr<GstSample> sample = adoptGRef(gst_app_sink_pull_preroll(GST_APP_SINK(sink)));
    player->triggerRepaint(sample.get());
    return GST_FLOW_OK;
}

void MediaPlayerPrivateGStreamerBase::clearCurrentBuffer()
{
    WTF::GMutexLocker<GMutex> lock(m_sampleMutex);
    m_sample.clear();

    {
        LockHolder locker(m_platformLayerProxy->lock());

        if (m_platformLayerProxy->isActive())
            m_platformLayerProxy->dropCurrentBufferWhilePreservingTexture();
    }
}
#endif

void MediaPlayerPrivateGStreamerBase::setSize(const IntSize& size)
{
    if (size == m_size)
        return;

    GST_INFO("Setting size to %dx%d", size.width(), size.height());
    m_size = size;

#if USE(WESTEROS_SINK) || USE(FUSION_SINK)
    updateVideoRectangle();
#endif
}

void MediaPlayerPrivateGStreamerBase::setPosition(const IntPoint& position)
{
    if (position == m_position)
        return;

    m_position = position;

#if USE(WESTEROS_SINK) || USE(FUSION_SINK)
    updateVideoRectangle();
#endif
}

#if USE(WESTEROS_SINK) || USE(FUSION_SINK)
void MediaPlayerPrivateGStreamerBase::updateVideoRectangle()
{
    if (!m_pipeline)
        return;

    GRefPtr<GstElement> sinkElement;
    g_object_get(m_pipeline.get(), "video-sink", &sinkElement.outPtr(), nullptr);
    if(!sinkElement)
        return;

    GST_INFO("Setting video sink size and position to x:%d y:%d, width=%d, height=%d", m_position.x(), m_position.y(), m_size.width(), m_size.height());

    GUniquePtr<gchar> rectString(g_strdup_printf("%d,%d,%d,%d", m_position.x(), m_position.y(), m_size.width(),m_size.height()));
    g_object_set(sinkElement.get(), "rectangle", rectString.get(), nullptr);
}
#endif

void MediaPlayerPrivateGStreamerBase::paint(GraphicsContext& context, const FloatRect& rect)
{
    if (context.paintingDisabled())
        return;

    if (!m_player->visible())
        return;

    WTF::GMutexLocker<GMutex> lock(m_sampleMutex);
    if (!GST_IS_SAMPLE(m_sample.get()))
        return;

    ImagePaintingOptions paintingOptions(CompositeCopy);
    if (m_player->client().mediaPlayerRenderingCanBeAccelerated(m_player))
        paintingOptions.m_orientationDescription.setImageOrientationEnum(m_videoSourceOrientation);

    RefPtr<ImageGStreamer> gstImage = ImageGStreamer::createImage(m_sample.get());
    if (!gstImage)
        return;

    if (Image* image = reinterpret_cast<Image*>(gstImage->image().get()))
        context.drawImage(*image, rect, gstImage->rect(), paintingOptions);
}

#if USE(TEXTURE_MAPPER_GL) && !USE(COORDINATED_GRAPHICS)
void MediaPlayerPrivateGStreamerBase::paintToTextureMapper(TextureMapper& textureMapper, const FloatRect& targetRect, const TransformationMatrix& matrix, float opacity)
{
    if (!m_player->visible())
        return;

    if (m_usingFallbackVideoSink) {
        RefPtr<BitmapTexture> texture;
        IntSize size;
        TextureMapperGL::Flags flags;
        {
            WTF::GMutexLocker<GMutex> lock(m_sampleMutex);

            GstVideoInfo videoInfo;
            if (UNLIKELY(!getSampleVideoInfo(m_sample.get(), videoInfo)))
                return;

            size = IntSize(GST_VIDEO_INFO_WIDTH(&videoInfo), GST_VIDEO_INFO_HEIGHT(&videoInfo));
            flags = m_textureMapperRotationFlag | (GST_VIDEO_INFO_HAS_ALPHA(&videoInfo) ? TextureMapperGL::ShouldBlend : 0);
            texture = textureMapper.acquireTextureFromPool(size, GST_VIDEO_INFO_HAS_ALPHA(&videoInfo) ? BitmapTexture::SupportsAlpha : BitmapTexture::NoFlag);
            updateTexture(static_cast<BitmapTextureGL&>(*texture), videoInfo);
        }
        TextureMapperGL& texmapGL = reinterpret_cast<TextureMapperGL&>(textureMapper);
        BitmapTextureGL* textureGL = static_cast<BitmapTextureGL*>(texture.get());
        texmapGL.drawTexture(textureGL->id(), flags, textureGL->size(), targetRect, matrix, opacity);
        return;
    }

#if USE(GSTREAMER_GL)
    WTF::GMutexLocker<GMutex> lock(m_sampleMutex);

    GstVideoInfo videoInfo;
    if (!getSampleVideoInfo(m_sample.get(), videoInfo))
        return;

    GstBuffer* buffer = gst_sample_get_buffer(m_sample.get());
    GstVideoFrame videoFrame;
    if (!gst_video_frame_map(&videoFrame, &videoInfo, buffer, static_cast<GstMapFlags>(GST_MAP_READ | GST_MAP_GL)))
        return;

    unsigned textureID = *reinterpret_cast<unsigned*>(videoFrame.data[0]);
    TextureMapperGL::Flags flags = m_textureMapperRotationFlag | (GST_VIDEO_INFO_HAS_ALPHA(&videoInfo) ? TextureMapperGL::ShouldBlend : 0);

    IntSize size = IntSize(GST_VIDEO_INFO_WIDTH(&videoInfo), GST_VIDEO_INFO_HEIGHT(&videoInfo));
    TextureMapperGL& textureMapperGL = reinterpret_cast<TextureMapperGL&>(textureMapper);
    textureMapperGL.drawTexture(textureID, flags, size, targetRect, matrix, opacity);
    gst_video_frame_unmap(&videoFrame);
#endif
}
#endif

#if USE(GSTREAMER_GL)
NativeImagePtr MediaPlayerPrivateGStreamerBase::nativeImageForCurrentTime()
{
#if USE(CAIRO) && ENABLE(ACCELERATED_2D_CANVAS)
    if (m_usingFallbackVideoSink)
        return nullptr;

    WTF::GMutexLocker<GMutex> lock(m_sampleMutex);

    GstVideoInfo videoInfo;
    if (!getSampleVideoInfo(m_sample.get(), videoInfo))
        return nullptr;

    GstBuffer* buffer = gst_sample_get_buffer(m_sample.get());
    GstVideoFrame videoFrame;
    if (!gst_video_frame_map(&videoFrame, &videoInfo, buffer, static_cast<GstMapFlags>(GST_MAP_READ | GST_MAP_GL)))
        return nullptr;

    GLContext* context = GLContext::sharingContext();
    context->makeContextCurrent();
    cairo_device_t* device = context->cairoDevice();

    // Thread-awareness is a huge performance hit on non-Intel drivers.
    cairo_gl_device_set_thread_aware(device, FALSE);

    unsigned textureID = *reinterpret_cast<unsigned*>(videoFrame.data[0]);
    IntSize size = IntSize(GST_VIDEO_INFO_WIDTH(&videoInfo), GST_VIDEO_INFO_HEIGHT(&videoInfo));
    RefPtr<cairo_surface_t> surface = adoptRef(cairo_gl_surface_create_for_texture(device, CAIRO_CONTENT_COLOR_ALPHA, textureID, size.width(), size.height()));

    IntSize rotatedSize = m_videoSourceOrientation.usesWidthAsHeight() ? size.transposedSize() : size;
    RefPtr<cairo_surface_t> rotatedSurface = adoptRef(cairo_gl_surface_create(device, CAIRO_CONTENT_COLOR_ALPHA, rotatedSize.width(), rotatedSize.height()));
    RefPtr<cairo_t> cr = adoptRef(cairo_create(rotatedSurface.get()));

    switch (m_videoSourceOrientation) {
    case DefaultImageOrientation:
        break;
    case OriginRightTop:
        cairo_translate(cr.get(), rotatedSize.width() * 0.5, rotatedSize.height() * 0.5);
        cairo_rotate(cr.get(), piOverTwoDouble);
        cairo_translate(cr.get(), -rotatedSize.height() * 0.5, -rotatedSize.width() * 0.5);
        break;
    case OriginBottomRight:
        cairo_translate(cr.get(), rotatedSize.width() * 0.5, rotatedSize.height() * 0.5);
        cairo_rotate(cr.get(), piDouble);
        cairo_translate(cr.get(), -rotatedSize.width() * 0.5, -rotatedSize.height() * 0.5);
        break;
    case OriginLeftBottom:
        cairo_translate(cr.get(), rotatedSize.width() * 0.5, rotatedSize.height() * 0.5);
        cairo_rotate(cr.get(), 3 * piOverTwoDouble);
        cairo_translate(cr.get(), -rotatedSize.height() * 0.5, -rotatedSize.width() * 0.5);
        break;
    default:
        ASSERT_NOT_REACHED();
        break;
    }
    cairo_set_source_surface(cr.get(), surface.get(), 0, 0);
    cairo_set_operator(cr.get(), CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr.get());

    gst_video_frame_unmap(&videoFrame);

    return rotatedSurface;
#else
    return nullptr;
#endif
}
#endif

void MediaPlayerPrivateGStreamerBase::setVideoSourceOrientation(const ImageOrientation& orientation)
{
    if (m_videoSourceOrientation == orientation)
        return;

    m_videoSourceOrientation = orientation;

#if USE(TEXTURE_MAPPER_GL)
    switch (m_videoSourceOrientation) {
    case DefaultImageOrientation:
        m_textureMapperRotationFlag = 0;
        break;
    case OriginRightTop:
        m_textureMapperRotationFlag = TextureMapperGL::ShouldRotateTexture90;
        break;
    case OriginBottomRight:
        m_textureMapperRotationFlag = TextureMapperGL::ShouldRotateTexture180;
        break;
    case OriginLeftBottom:
        m_textureMapperRotationFlag = TextureMapperGL::ShouldRotateTexture270;
        break;
    default:
        ASSERT_NOT_REACHED();
        break;
    }
#endif
}

bool MediaPlayerPrivateGStreamerBase::supportsFullscreen() const
{
    return true;
}

PlatformMedia MediaPlayerPrivateGStreamerBase::platformMedia() const
{
    return NoPlatformMedia;
}

MediaPlayer::MovieLoadType MediaPlayerPrivateGStreamerBase::movieLoadType() const
{
    if (m_readyState == MediaPlayer::HaveNothing)
        return MediaPlayer::Unknown;

    if (isLiveStream())
        return MediaPlayer::LiveStream;

    return MediaPlayer::Download;
}

#if USE(GSTREAMER_GL)
GstElement* MediaPlayerPrivateGStreamerBase::createGLAppSink()
{
    if (!webkitGstCheckVersion(1, 8, 0))
        return nullptr;

    GstElement* appsink = gst_element_factory_make("appsink", "webkit-gl-video-sink");
    if (!appsink)
        return nullptr;

    g_object_set(appsink, "enable-last-sample", FALSE, "emit-signals", TRUE, "max-buffers", 1, nullptr);
    g_signal_connect(appsink, "new-sample", G_CALLBACK(newSampleCallback), this);
    g_signal_connect(appsink, "new-preroll", G_CALLBACK(newPrerollCallback), this);

    return appsink;
}

gboolean appSinkSinkQuery(GstPad* pad, GstObject* parent, GstQuery* query)
{
    gboolean result = FALSE;
    auto* player = static_cast<MediaPlayerPrivateGStreamerBase*>(g_object_get_data(G_OBJECT(parent), "player"));

    switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_DRAIN: {
        player->clearCurrentBuffer();
        result = TRUE;
        break;
    }
    default:
        result = gst_pad_query_default(pad, parent, query);
        break;
    }

    return result;
}

GstElement* MediaPlayerPrivateGStreamerBase::createVideoSinkGL()
{
    // FIXME: Currently it's not possible to get the video frames and caps using this approach until
    // the pipeline gets into playing state. Due to this, trying to grab a frame and painting it by some
    // other mean (canvas or webgl) before playing state can result in a crash.
    // This is being handled in https://bugs.webkit.org/show_bug.cgi?id=159460.
    if (!webkitGstCheckVersion(1, 8, 0))
        return nullptr;

    gboolean result = TRUE;
    GstElement* videoSink = gst_bin_new("webkitvideosinkbin");
    GstElement* upload = gst_element_factory_make("glupload", nullptr);
    GstElement* colorconvert = gst_element_factory_make("glcolorconvert", nullptr);
    GstElement* appsink = createGLAppSink();

    if (!appsink || !upload || !colorconvert) {
        GST_WARNING("Failed to create GstGL elements");
        gst_object_unref(videoSink);

        if (upload)
            gst_object_unref(upload);
        if (colorconvert)
            gst_object_unref(colorconvert);
        if (appsink)
            gst_object_unref(appsink);

        return nullptr;
    }

    gst_bin_add_many(GST_BIN(videoSink), upload, colorconvert, appsink, nullptr);

    GRefPtr<GstCaps> caps = adoptGRef(gst_caps_from_string("video/x-raw(" GST_CAPS_FEATURE_MEMORY_GL_MEMORY "), format = (string) { RGBA }"));

    result &= gst_element_link_pads(upload, "src", colorconvert, "sink");
    result &= gst_element_link_pads_filtered(colorconvert, "src", appsink, "sink", caps.get());

    GRefPtr<GstPad> pad = adoptGRef(gst_element_get_static_pad(upload, "sink"));
    gst_element_add_pad(videoSink, gst_ghost_pad_new("sink", pad.get()));

    pad = adoptGRef(gst_element_get_static_pad(appsink, "sink"));
    gst_pad_add_probe (pad.get(), GST_PAD_PROBE_TYPE_EVENT_FLUSH, [] (GstPad*, GstPadProbeInfo* info,  gpointer userData) -> GstPadProbeReturn {
        if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_EVENT (info)) != GST_EVENT_FLUSH_START)
           return GST_PAD_PROBE_OK;

        auto* player = static_cast<MediaPlayerPrivateGStreamerBase*>(userData);
        player->clearCurrentBuffer();
        return GST_PAD_PROBE_OK;
     }, this, nullptr);
 
     g_object_set_data(G_OBJECT(appsink), "player", (gpointer) this);
     gst_pad_set_query_function(pad.get(), appSinkSinkQuery);

    if (!result) {
        GST_WARNING("Failed to link GstGL elements");
        gst_object_unref(videoSink);
        videoSink = nullptr;
    }
    return videoSink;
}
#endif

#if !USE(HOLE_PUNCH_GSTREAMER)
GstElement* MediaPlayerPrivateGStreamerBase::createVideoSink()
{
#if USE(GSTREAMER_GL)
    m_videoSink = createVideoSinkGL();
#endif

    if (!m_videoSink) {
        m_usingFallbackVideoSink = true;
        m_videoSink = webkitVideoSinkNew();
        m_repaintHandler = g_signal_connect_swapped(m_videoSink.get(), "repaint-requested", G_CALLBACK(repaintCallback), this);
    }

    GstElement* videoSink = nullptr;
    m_fpsSink = gst_element_factory_make("fpsdisplaysink", "sink");
    if (m_fpsSink) {
        g_object_set(m_fpsSink.get(), "silent", TRUE , nullptr);

        // Turn off text overlay unless logging is enabled.
#if LOG_DISABLED
        g_object_set(m_fpsSink.get(), "text-overlay", FALSE , nullptr);
#else
        if (!isLogChannelEnabled("Media"))
            g_object_set(m_fpsSink.get(), "text-overlay", FALSE , nullptr);
#endif // LOG_DISABLED

        if (g_object_class_find_property(G_OBJECT_GET_CLASS(m_fpsSink.get()), "video-sink")) {
            g_object_set(m_fpsSink.get(), "video-sink", m_videoSink.get(), nullptr);
            videoSink = m_fpsSink.get();
        } else
            m_fpsSink = nullptr;
    }

    if (!m_fpsSink)
        videoSink = m_videoSink.get();

    ASSERT(videoSink);

    return videoSink;
}
#endif

void MediaPlayerPrivateGStreamerBase::setStreamVolumeElement(GstStreamVolume* volume)
{
    ASSERT(!m_volumeElement);
    m_volumeElement = volume;

    // We don't set the initial volume because we trust the sink to keep it for us. See
    // https://bugs.webkit.org/show_bug.cgi?id=118974 for more information.
    if (!m_player->platformVolumeConfigurationRequired()) {
        GST_DEBUG("Setting stream volume to %f", m_player->volume());
        g_object_set(m_volumeElement.get(), "volume", m_player->volume(), NULL);
    } else
        GST_DEBUG("Not setting stream volume, trusting system one");

    GST_DEBUG("Setting stream muted %d",  m_player->muted());
    g_object_set(m_volumeElement.get(), "mute", m_player->muted(), NULL);

    g_signal_connect_swapped(m_volumeElement.get(), "notify::volume", G_CALLBACK(volumeChangedCallback), this);
    g_signal_connect_swapped(m_volumeElement.get(), "notify::mute", G_CALLBACK(muteChangedCallback), this);
}

unsigned MediaPlayerPrivateGStreamerBase::decodedFrameCount() const
{
    guint64 decodedFrames = 0;
    if (m_fpsSink)
        g_object_get(m_fpsSink.get(), "frames-rendered", &decodedFrames, NULL);
    return static_cast<unsigned>(decodedFrames);
}

unsigned MediaPlayerPrivateGStreamerBase::droppedFrameCount() const
{
    guint64 framesDropped = 0;
    if (m_fpsSink)
        g_object_get(m_fpsSink.get(), "frames-dropped", &framesDropped, NULL);
    return static_cast<unsigned>(framesDropped);
}

unsigned MediaPlayerPrivateGStreamerBase::audioDecodedByteCount() const
{
    GstQuery* query = gst_query_new_position(GST_FORMAT_BYTES);
    gint64 position = 0;

    if (audioSink() && gst_element_query(audioSink(), query))
        gst_query_parse_position(query, 0, &position);

    gst_query_unref(query);
    return static_cast<unsigned>(position);
}

unsigned MediaPlayerPrivateGStreamerBase::videoDecodedByteCount() const
{
    GstQuery* query = gst_query_new_position(GST_FORMAT_BYTES);
    gint64 position = 0;

    if (gst_element_query(m_videoSink.get(), query))
        gst_query_parse_position(query, 0, &position);

    gst_query_unref(query);
    return static_cast<unsigned>(position);
}

#if USE(PLAYREADY)
PlayreadySession* MediaPlayerPrivateGStreamerBase::prSession() const
{
    PlayreadySession* session = nullptr;
#if ENABLE(ENCRYPTED_MEDIA)
    session = m_prSession;
#elif ENABLE(ENCRYPTED_MEDIA_V2)
    if (m_cdmSession) {
        CDMPRSessionGStreamer* cdmSession = static_cast<CDMPRSessionGStreamer*>(m_cdmSession);
        session = static_cast<PlayreadySession*>(cdmSession);
    }
#endif
    return session;
}
#endif

#if USE(PLAYREADY)
void MediaPlayerPrivateGStreamerBase::emitSession()
{
    PlayreadySession* session = prSession();
    if (!session->ready())
        return;

    gst_element_send_event(m_pipeline.get(), gst_event_new_custom(GST_EVENT_CUSTOM_DOWNSTREAM_OOB,
        gst_structure_new("playready-session", "session", G_TYPE_POINTER, session, nullptr)));
}
#endif

void MediaPlayerPrivateGStreamerBase::emitOCDMSession()
{

    printf(" %s:%s:%d \n", __FILE__, __func__, __LINE__);
    if (!m_cdmSession)
        return;

    printf(" %s:%s:%d \n", __FILE__, __func__, __LINE__);
    CDMSessionEncKey* cdmSession = static_cast<CDMSessionEncKey*>(m_cdmSession);
    std::string sessionId = (cdmSession->sessionId()).utf8().data();
    if (sessionId.empty())
        return;

    printf("$$ %s:%s:%d SessionID = %s\n", __FILE__, __func__, __LINE__, sessionId.c_str());
    gst_element_send_event(m_pipeline.get(), gst_event_new_custom(GST_EVENT_CUSTOM_DOWNSTREAM_OOB,
        gst_structure_new("drm-session", "session", G_TYPE_STRING, sessionId.c_str(), nullptr)));
}

#if ENABLE(ENCRYPTED_MEDIA)
MediaPlayer::MediaKeyException MediaPlayerPrivateGStreamerBase::addKey(const String& keySystem, const unsigned char* keyData, unsigned keyLength, const unsigned char* /* initData */, unsigned /* initDataLength */ , const String& sessionID)
{
    GST_DEBUG("addKey system: %s, length: %u, session: %s", keySystem.utf8().data(), keyLength, sessionID.utf8().data());

#if USE(PLAYREADY)
    if (equalIgnoringASCIICase(keySystem, "com.microsoft.playready")
        || equalIgnoringASCIICase(keySystem, "com.youtube.playready")) {
        RefPtr<Uint8Array> key = Uint8Array::create(keyData, keyLength);
        RefPtr<Uint8Array> nextMessage;
        unsigned short errorCode;
        uint32_t systemCode;
        bool result = m_prSession->playreadyProcessKey(key.get(), nextMessage, errorCode, systemCode);

        if (errorCode || !result) {
            GST_DEBUG("Error processing key: errorCode: %u, result: %d", errorCode, result);
            return MediaPlayer::InvalidPlayerState;
        }

        // XXX: use nextMessage here and send a new keyMessage is ack is needed?
        emitSession();

        m_player->keyAdded(keySystem, sessionID);

        return MediaPlayer::NoError;
    }
#endif

    if (!equalIgnoringASCIICase(keySystem, "org.w3.clearkey"))
        return MediaPlayer::KeySystemNotSupported;

    GstBuffer* buffer = gst_buffer_new_wrapped(g_memdup(keyData, keyLength), keyLength);
    dispatchDecryptionKey(buffer);
    gst_buffer_unref(buffer);

    m_player->keyAdded(keySystem, sessionID);

    return MediaPlayer::NoError;
}

MediaPlayer::MediaKeyException MediaPlayerPrivateGStreamerBase::generateKeyRequest(const String& keySystem, const unsigned char* initDataPtr, unsigned initDataLength, const String& customData)
{
    GST_DEBUG("generating key request for system: %s", keySystem.utf8().data());
#if USE(PLAYREADY)
    if (equalIgnoringASCIICase(keySystem, "com.microsoft.playready")
        || equalIgnoringASCIICase(keySystem, "com.youtube.playready")) {
        if (!m_prSession)
            m_prSession = new PlayreadySession();
        if (m_prSession->ready()) {
            emitSession();
            return MediaPlayer::NoError;
        }

        unsigned short errorCode;
        uint32_t systemCode;
        RefPtr<Uint8Array> initData = Uint8Array::create(initDataPtr, initDataLength);
        String destinationURL;
        RefPtr<Uint8Array> result = m_prSession->playreadyGenerateKeyRequest(initData.get(), customData, destinationURL, errorCode, systemCode);
        if (errorCode) {
            GST_ERROR("the key request wasn't properly generated");
            return MediaPlayer::InvalidPlayerState;
        }

        if (m_prSession->ready()) {
            emitSession();
            return MediaPlayer::NoError;
        }
        URL url(URL(), destinationURL);
        m_player->keyMessage(keySystem, createCanonicalUUIDString(), result->data(), result->length(), url);
        return MediaPlayer::NoError;
    }
#endif

    if (!equalIgnoringASCIICase(keySystem, "org.w3.clearkey"))
        return MediaPlayer::KeySystemNotSupported;

    m_player->keyMessage(keySystem, createCanonicalUUIDString(), initDataPtr, initDataLength, URL());
    return MediaPlayer::NoError;
}

MediaPlayer::MediaKeyException MediaPlayerPrivateGStreamerBase::cancelKeyRequest(const String& /* keySystem */ , const String& /* sessionID */)
{
    GST_DEBUG("cancelKeyRequest");
    return MediaPlayer::KeySystemNotSupported;
}

void MediaPlayerPrivateGStreamerBase::needKey(const String& keySystem, const String& sessionId, const unsigned char* initData, unsigned initDataLength)
{
    if (!m_player->keyNeeded(keySystem, sessionId, initData, initDataLength))
        GST_DEBUG("no event handler for key needed");
}
#endif

#if ENABLE(ENCRYPTED_MEDIA_V2)
void MediaPlayerPrivateGStreamerBase::needKey(RefPtr<Uint8Array> initData)
{
    if (!m_player->keyNeeded(initData.get()))
        GST_DEBUG("no event handler for key needed");
}

void MediaPlayerPrivateGStreamerBase::setCDMSession(CDMSession* session)
{
    GST_DEBUG("setting CDM session to %p", session);
    m_cdmSession = session;
}

void MediaPlayerPrivateGStreamerBase::keyAdded()
{
#if USE(PLAYREADY)
    emitSession();
#endif

    if (m_cdmSession) {
       emitOCDMSession();
    }
}

std::unique_ptr<CDMSession> MediaPlayerPrivateGStreamerBase::createSession(const String& keySystem, CDMSessionClient* client)
{
    if (!supportsKeySystem(keySystem, emptyString()))
        return nullptr;

    GST_DEBUG("creating key session for %s", keySystem.utf8().data());
#if USE(PLAYREADY)
    if (equalIgnoringASCIICase(keySystem, "com.microsoft.playready")
        || equalIgnoringASCIICase(keySystem, "com.youtube.playready"))
        return std::make_unique<CDMPRSessionGStreamer>(client);
#endif

    if (CDMPrivateEncKey::supportsKeySystem(keySystem)) {
        return (CDMPrivateEncKey::createSession(client));
    }
    return nullptr;
}
#endif // ENABLE(ENCRYPTED_MEDIA_V2)

#if ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2)
void MediaPlayerPrivateGStreamerBase::dispatchDecryptionKey(GstBuffer* buffer)
{
    gst_element_send_event(m_pipeline.get(), gst_event_new_custom(GST_EVENT_CUSTOM_DOWNSTREAM_OOB,
        gst_structure_new("drm-cipher", "key", GST_TYPE_BUFFER, buffer, nullptr)));
}
#endif

bool MediaPlayerPrivateGStreamerBase::supportsKeySystem(const String& keySystem, const String& mimeType)
{
    GST_DEBUG("Checking for KeySystem support with %s and type %s", keySystem.utf8().data(), mimeType.utf8().data());

#if ENABLE(ENCRYPTED_MEDIA)
    if (equalIgnoringASCIICase(keySystem, "org.w3.clearkey"))
        return true;
#endif

#if USE(PLAYREADY) && (ENABLE(ENCRYPTED_MEDIA) || ENABLE(ENCRYPTED_MEDIA_V2))
    if (equalIgnoringASCIICase(keySystem, "com.microsoft.playready")
        || equalIgnoringASCIICase(keySystem, "com.youtube.playready"))
        return true;
#endif
#if ENABLE(ENCRYPTED_MEDIA_V2)
    if (CDMPrivateEncKey::supportsKeySystemAndMimeType(keySystem,mimeType)) {
        return true;
    }
#endif

    return false;
}

MediaPlayer::SupportsType MediaPlayerPrivateGStreamerBase::extendedSupportsType(const MediaEngineSupportParameters& parameters, MediaPlayer::SupportsType result)
{
#if ENABLE(ENCRYPTED_MEDIA)
    // From: <http://dvcs.w3.org/hg/html-media/raw-file/eme-v0.1b/encrypted-media/encrypted-media.html#dom-canplaytype>
    // In addition to the steps in the current specification, this method must run the following steps:

    // 1. Check whether the Key System is supported with the specified container and codec type(s) by following the steps for the first matching condition from the following list:
    //    If keySystem is null, continue to the next step.
    if (parameters.keySystem.isNull() || parameters.keySystem.isEmpty())
        return result;

    // If keySystem contains an unrecognized or unsupported Key System, return the empty string
    if (!supportsKeySystem(parameters.keySystem, emptyString()))
        result = MediaPlayer::IsNotSupported;
#else
    UNUSED_PARAM(parameters);
#endif
    return result;
}

}

#endif // USE(GSTREAMER)
