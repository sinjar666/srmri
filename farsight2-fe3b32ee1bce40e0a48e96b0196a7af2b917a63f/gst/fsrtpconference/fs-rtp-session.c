/*
 * Farsight2 - Farsight RTP Session
 *
 * Copyright 2007 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * Copyright 2007 Nokia Corp.
 *
 * fs-rtp-session.c - A Farsight RTP Session gobject
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/**
 * SECTION:fs-rtp-session
 * @short_description: A  RTP session in a #FsRtpConference
 *
 * This object represents one session, it is created by called
 * fs_conference_new_session() on a #FsRtpConference. It can be either
 * Audio or Video. It also represents data send with one and only one
 * SSRC (although if there is a SSRC collision, that SSRC may change).
 * </para>
 * <refsect2><title>Codec profiles</title>
 * <para>
 * It is possible to define "codec profiles", that is non-autodetected
 * encoding and decoding pipelines for codecs. It is even possible to declare
 * entirely new codecs using this method.
 *
 * To create a profile for a codec, add it to the codec-preferences with
 * special optional parameters called "farsight-send-profile" and
 * "farsight-recv-profile", these should contain gst-launch style descriptions
 * of the encoding or decoding bin.
 *
 * As a special case, encoding profiles can have more than one unconnected
 * source pad, all of these pads should produce application/x-rtp of some kind.
 * The profile will be ignored if not ALL pads match currently negotiated
 * codecs.
 *
 * Also, it is possible to declare profiles with only a decoding pipeline,
 * you will only be able to receive from this codec, the encoding may be a
 * secondary pad of some other codec.
 * </para></refsect2><para>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include <gst/gst.h>

#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtcpbuffer.h>

#include <gst/farsight/fs-transmitter.h>

#include "fs-rtp-session.h"
#include "fs-rtp-stream.h"
#include "fs-rtp-participant.h"
#include "fs-rtp-discover-codecs.h"
#include "fs-rtp-codec-negotiation.h"
#include "fs-rtp-substream.h"
#include "fs-rtp-special-source.h"
#include "fs-rtp-specific-nego.h"

#define GST_CAT_DEFAULT fsrtpconference_debug

/* Signals */
enum
{
  LAST_SIGNAL
};

/* props */
enum
{
  PROP_0,
  PROP_MEDIA_TYPE,
  PROP_ID,
  PROP_SINK_PAD,
  PROP_CODEC_PREFERENCES,
  PROP_CODECS,
  PROP_CODECS_WITHOUT_CONFIG,
  PROP_CURRENT_SEND_CODEC,
  PROP_CODECS_READY,
  PROP_CONFERENCE,
  PROP_NO_RTCP_TIMEOUT
};

#define DEFAULT_NO_RTCP_TIMEOUT (7000)

struct _FsRtpSessionPrivate
{
  FsMediaType media_type;

  /* We dont need a reference to this one per our reference model
   * This Session object can only exist while its parent conference exists
   */
  FsRtpConference *conference;

  GHashTable *transmitters;

  /* We keep references to these elements
   */

  GstElement *media_sink_valve;
  GstElement *send_tee;
  GstElement *send_capsfilter;
  GstElement *transmitter_rtp_tee;
  GstElement *transmitter_rtcp_tee;
  GstElement *transmitter_rtp_funnel;
  GstElement *transmitter_rtcp_funnel;
  GstElement *transmitter_rtp_fakesink;
  GstElement *transmitter_rtcp_fakesink;

  GstElement *rtpmuxer;

  /* Request pads that are disposed of when the tee is disposed of */
  GstPad *send_tee_media_pad;
  GstPad *send_tee_discovery_pad;

  /* We dont keep explicit references to the pads, the Bin does that for us
   * only this element's methods can add/remote it
   */
  GstPad *media_sink_pad;

  GList *extra_send_capsfilters;

  /* The discovery elements are only created when codec parameter discovery is
   * under progress.
   * They are normally destroyed when the caps are found but may be destroyed
   * by the dispose function too, we hold refs to them
   */
  GstElement *discovery_fakesink;
  GstElement *discovery_capsfilter;
  GstElement *discovery_codecbin;
  FsCodec *discovery_codec;

  /* Request pad to release on dispose */
  GstPad *rtpbin_send_rtp_sink;
  GstPad *rtpbin_send_rtcp_src;

  GstPad *rtpbin_recv_rtp_sink;
  GstPad *rtpbin_recv_rtcp_sink;

  /* Protected by the session mutex */
  /* The codec bin is owned implicitely by the Conference bin for us */
  GstElement *send_codecbin;
  FsCodec *current_send_codec;
  FsCodec *requested_send_codec;

  /* These lists are protected by the session mutex */
  GList *streams;
  GList *free_substreams;

  /* The static list of all the blueprints */
  GList *blueprints;

  GList *codec_preferences;

  /* These are protected by the session mutex */
  GList *codec_associations;

  /* Protected by the session mutex */
  gint no_rtcp_timeout;

  GList *extra_sources;

  /* This is a ht of ssrc->streams
   * It is protected by the session mutex */
  GHashTable *ssrc_streams;

  GError *construction_error;

  gboolean disposed;
};

G_DEFINE_TYPE (FsRtpSession, fs_rtp_session, FS_TYPE_SESSION);

#define FS_RTP_SESSION_GET_PRIVATE(o)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((o), FS_TYPE_RTP_SESSION, FsRtpSessionPrivate))

static void fs_rtp_session_dispose (GObject *object);
static void fs_rtp_session_finalize (GObject *object);

static void fs_rtp_session_get_property (GObject *object,
    guint prop_id,
    GValue *value,
    GParamSpec *pspec);
static void fs_rtp_session_set_property (GObject *object,
    guint prop_id,
    const GValue *value,
    GParamSpec *pspec);

static void fs_rtp_session_constructed (GObject *object);

static FsStream *fs_rtp_session_new_stream (FsSession *session,
    FsParticipant *participant,
    FsStreamDirection direction,
    const gchar *transmitter,
    guint n_parameters,
    GParameter *parameters,
    GError **error);
static gboolean fs_rtp_session_start_telephony_event (FsSession *session,
    guint8 event,
    guint8 volume,
    FsDTMFMethod method);
static gboolean fs_rtp_session_stop_telephony_event (FsSession *session,
    FsDTMFMethod method);
static gboolean fs_rtp_session_set_send_codec (FsSession *session,
    FsCodec *send_codec,
    GError **error);
static gboolean fs_rtp_session_set_codec_preferences (FsSession *session,
    GList *codec_preferences,
    GError **error);
static gboolean fs_rtp_session_verify_send_codec_bin_locked (
    FsRtpSession *self,
    GError **error);
static void fs_rtp_session_send_codec_changed (FsRtpSession *self);

static gchar **fs_rtp_session_list_transmitters (FsSession *session);

static void _substream_no_rtcp_timedout_cb (FsRtpSubStream *substream,
    FsRtpSession *session);
static void _substream_blocked (FsRtpSubStream *substream, FsRtpStream *stream,
    FsRtpSession *session);

static gboolean _stream_new_remote_codecs (FsRtpStream *stream, GList *codecs,
    GError **error, gpointer user_data);


static FsStreamTransmitter *fs_rtp_session_get_new_stream_transmitter (
    FsRtpSession *self,
    const gchar *transmitter_name,
    FsParticipant *participant,
    guint n_parameters,
    GParameter *parameters,
    GError **error);

static gboolean fs_rtp_session_substream_set_codec_bin (FsRtpSession *session,
    FsRtpSubStream *substream,
    FsRtpStream *stream,
    guint32 ssrc,
    guint pt,
    GError **error);

static void _remove_stream (gpointer user_data,
    GObject *where_the_object_was);

static gboolean
fs_rtp_session_update_codecs (FsRtpSession *session,
    FsRtpStream *stream,
    GList *remote_codecs,
    GError **error);

static CodecAssociation *
fs_rtp_session_get_recv_codec_locked (FsRtpSession *session,
    guint pt,
    FsRtpStream *stream,
    GError **error);

static void
fs_rtp_session_start_codec_param_gathering (FsRtpSession *session);
static void
fs_rtp_session_stop_codec_param_gathering (FsRtpSession *session);

static void
fs_rtp_session_associate_free_substreams (FsRtpSession *session,
    FsRtpStream *stream, guint32 ssrc);

static void
_send_caps_changed (GstPad *pad, GParamSpec *pspec, FsRtpSession *session);
static void
_send_sink_pad_blocked_callback (GstPad *pad, gboolean blocked,
    gpointer user_data);


static GObjectClass *parent_class = NULL;

//static guint signals[LAST_SIGNAL] = { 0 };

static void
fs_rtp_session_class_init (FsRtpSessionClass *klass)
{
  GObjectClass *gobject_class;
  FsSessionClass *session_class;

  gobject_class = (GObjectClass *) klass;
  parent_class = g_type_class_peek_parent (klass);
  session_class = FS_SESSION_CLASS (klass);

  gobject_class->set_property = fs_rtp_session_set_property;
  gobject_class->get_property = fs_rtp_session_get_property;
  gobject_class->constructed = fs_rtp_session_constructed;

  session_class->new_stream = fs_rtp_session_new_stream;
  session_class->start_telephony_event = fs_rtp_session_start_telephony_event;
  session_class->stop_telephony_event = fs_rtp_session_stop_telephony_event;
  session_class->set_send_codec = fs_rtp_session_set_send_codec;
  session_class->set_codec_preferences =
    fs_rtp_session_set_codec_preferences;
  session_class->list_transmitters = fs_rtp_session_list_transmitters;

  g_object_class_override_property (gobject_class,
    PROP_MEDIA_TYPE, "media-type");
  g_object_class_override_property (gobject_class,
    PROP_ID, "id");
  g_object_class_override_property (gobject_class,
    PROP_SINK_PAD, "sink-pad");
  g_object_class_override_property (gobject_class,
    PROP_CODEC_PREFERENCES, "codec-preferences");
  g_object_class_override_property (gobject_class,
    PROP_CODECS, "codecs");
  g_object_class_override_property (gobject_class,
    PROP_CODECS_WITHOUT_CONFIG, "codecs-without-config");
  g_object_class_override_property (gobject_class,
    PROP_CURRENT_SEND_CODEC, "current-send-codec");
  g_object_class_override_property (gobject_class,
    PROP_CODECS_READY, "codecs-ready");

  g_object_class_install_property (gobject_class,
    PROP_CONFERENCE,
    g_param_spec_object ("conference",
      "The Conference this stream refers to",
      "This is a convience pointer for the Conference",
      FS_TYPE_RTP_CONFERENCE,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
      PROP_NO_RTCP_TIMEOUT,
      g_param_spec_int ("no-rtcp-timeout",
          "The timeout (in ms) before no RTCP is assumed",
          "This is the time (in ms) after which data received without RTCP"
          " is attached the FsStream, this only works if there is only one"
          " FsStream. -1 will wait forever. 0 will not wait for RTCP and"
          " attach it immediataly to the FsStream and prohibit the creation"
          " of a second FsStream",
          -1, G_MAXINT, DEFAULT_NO_RTCP_TIMEOUT,
          G_PARAM_READWRITE));

  gobject_class->dispose = fs_rtp_session_dispose;
  gobject_class->finalize = fs_rtp_session_finalize;

  g_type_class_add_private (klass, sizeof (FsRtpSessionPrivate));
}

static void
fs_rtp_session_init (FsRtpSession *self)
{
  /* member init */
  self->priv = FS_RTP_SESSION_GET_PRIVATE (self);
  self->priv->disposed = FALSE;
  self->priv->construction_error = NULL;

  self->priv->transmitters = g_hash_table_new_full (g_str_hash, g_str_equal,
    g_free, g_object_unref);

  g_static_rec_mutex_init (&self->mutex);

  self->priv->media_type = FS_MEDIA_TYPE_LAST + 1;

  self->priv->no_rtcp_timeout = DEFAULT_NO_RTCP_TIMEOUT;

  self->priv->ssrc_streams = g_hash_table_new (g_direct_hash, g_direct_equal);
}

static gboolean
_remove_transmitter (gpointer key, gpointer value, gpointer user_data)
{
  FsRtpSession *self = FS_RTP_SESSION (user_data);
  FsTransmitter *transmitter = FS_TRANSMITTER (value);
  GstElement *src, *sink;

  g_object_get (transmitter, "gst-sink", &sink, "gst-src", &src, NULL);

  gst_element_set_state (src, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (self->priv->conference), src);
  gst_element_set_state (src, GST_STATE_NULL);

  gst_element_set_state (sink, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (self->priv->conference), sink);
  gst_element_set_state (sink, GST_STATE_NULL);

  gst_object_unref (src);
  gst_object_unref (sink);

  return TRUE;
}

static void
_stop_transmitter_elem (gpointer key, gpointer value, gpointer elem_name)
{
  FsTransmitter *transmitter = FS_TRANSMITTER (value);
  GstElement *elem = NULL;

  g_object_get (transmitter, elem_name, &elem, NULL);

  gst_element_set_locked_state (elem, TRUE);
  gst_element_set_state (elem, GST_STATE_NULL);
}

static void
stop_and_remove (GstBin *conf, GstElement **element, gboolean unref)
{
  if (*element == NULL)
    return;

  gst_element_set_locked_state (*element, TRUE);
  gst_element_set_state (*element, GST_STATE_NULL);
  gst_bin_remove (conf, *element);
  if (unref)
    gst_object_unref (*element);
  *element = NULL;
}

static void
fs_rtp_session_dispose (GObject *object)
{
  FsRtpSession *self = FS_RTP_SESSION (object);
  GList *item = NULL;
  GstBin *conferencebin = NULL;

  if (self->priv->disposed)
    /* If dispose did already run, return. */
    return;

  conferencebin = GST_BIN (self->priv->conference);

  /* Lets stop all of the elements sink to source */

  /* First the send pipeline */
  if (self->priv->transmitters)
    g_hash_table_foreach (self->priv->transmitters, _stop_transmitter_elem,
      "gst-sink");

  stop_and_remove (conferencebin, &self->priv->transmitter_rtp_fakesink, TRUE);
  stop_and_remove (conferencebin, &self->priv->transmitter_rtcp_fakesink, TRUE);
  stop_and_remove (conferencebin, &self->priv->transmitter_rtp_tee, TRUE);
  stop_and_remove (conferencebin, &self->priv->transmitter_rtcp_tee, TRUE);

  if (self->priv->rtpbin_send_rtcp_src)
    gst_pad_set_active (self->priv->rtpbin_send_rtcp_src, FALSE);
  if (self->priv->rtpbin_send_rtp_sink)
    gst_pad_set_active (self->priv->rtpbin_send_rtp_sink, FALSE);

  fs_rtp_session_stop_codec_param_gathering (self);

  if (self->priv->send_tee_discovery_pad)
  {
    gst_object_unref (self->priv->send_tee_discovery_pad);
    self->priv->send_tee_discovery_pad = NULL;
  }

  if (self->priv->send_tee_media_pad)
  {
    gst_object_unref (self->priv->send_tee_media_pad);
    self->priv->send_tee_media_pad = NULL;
  }

  if (self->priv->send_capsfilter && self->priv->rtpmuxer)
  {
    GstPad *srcpad = gst_element_get_static_pad (self->priv->send_capsfilter,
        "src");
    if (srcpad)
    {
      GstPad *otherpad = gst_pad_get_peer (srcpad);
      if (otherpad)
      {
        gst_element_release_request_pad (self->priv->rtpmuxer, otherpad);
        gst_object_unref (otherpad);
      }
      gst_object_unref (srcpad);
    }
  }

  for (item = self->priv->extra_send_capsfilters;
       item;
       item = g_list_next (item))
  {
    GstElement *cf = item->data;
    GstPad *ourpad = gst_element_get_static_pad (cf, "src");
    GstPad *pad = NULL;

    if (ourpad)
    {
      pad = gst_pad_get_peer (ourpad);
      if (pad)
      {
        gst_element_release_request_pad (self->priv->rtpmuxer, pad);
        gst_object_unref (pad);
      }
      gst_object_unref (ourpad);
    }
  }

  stop_and_remove (conferencebin, &self->priv->rtpmuxer, TRUE);
  stop_and_remove (conferencebin, &self->priv->send_capsfilter, TRUE);
  stop_and_remove (conferencebin, &self->priv->send_codecbin, FALSE);
  stop_and_remove (conferencebin, &self->priv->send_tee, TRUE);
  stop_and_remove (conferencebin, &self->priv->media_sink_valve, TRUE);

  while (self->priv->extra_send_capsfilters)
  {
    GstElement *cf = self->priv->extra_send_capsfilters->data;

    stop_and_remove (conferencebin, &cf, FALSE);
    self->priv->extra_send_capsfilters = g_list_delete_link (
        self->priv->extra_send_capsfilters,
        self->priv->extra_send_capsfilters);
  }

  if (self->priv->media_sink_pad)
    gst_pad_set_active (self->priv->media_sink_pad, FALSE);


  /* Now the recv pipeline */
  if (self->priv->free_substreams)
    g_list_foreach (self->priv->free_substreams, (GFunc) fs_rtp_sub_stream_stop,
      NULL);
  if (self->priv->rtpbin_recv_rtp_sink)
    gst_pad_set_active (self->priv->rtpbin_recv_rtp_sink, FALSE);
  if (self->priv->rtpbin_recv_rtcp_sink)
    gst_pad_set_active (self->priv->rtpbin_recv_rtcp_sink, FALSE);

  stop_and_remove (conferencebin, &self->priv->transmitter_rtp_funnel, TRUE);
  stop_and_remove (conferencebin, &self->priv->transmitter_rtcp_funnel, TRUE);

  if (self->priv->transmitters)
    g_hash_table_foreach (self->priv->transmitters, _stop_transmitter_elem,
      "gst-src");

  self->priv->extra_sources =
    fs_rtp_special_sources_destroy (self->priv->extra_sources);

  /* Now they should all be stopped, we can remove them in peace */


  if (self->priv->media_sink_pad)
  {
    gst_pad_set_active (self->priv->media_sink_pad, FALSE);
    gst_element_remove_pad (GST_ELEMENT (self->priv->conference),
      self->priv->media_sink_pad);
    self->priv->media_sink_pad = NULL;
  }


  if (self->priv->rtpbin_send_rtcp_src)
  {
    /* Release pad generates a g_warning as of Nov 11 2008
    gst_pad_set_active (self->priv->rtpbin_send_rtcp_src, FALSE);
    gst_element_release_request_pad (self->priv->conference->gstrtpbin,
      self->priv->rtpbin_send_rtcp_src);
    */
    gst_object_unref (self->priv->rtpbin_send_rtcp_src);
    self->priv->rtpbin_send_rtcp_src = NULL;
  }

  if (self->priv->rtpbin_send_rtp_sink)
  {
    /* Release pad generates a g_warning as of Nov 11 2008
    gst_pad_set_active (self->priv->rtpbin_send_rtp_sink, FALSE);
    gst_element_release_request_pad (self->priv->conference->gstrtpbin,
      self->priv->rtpbin_send_rtp_sink);
    */
    gst_object_unref (self->priv->rtpbin_send_rtp_sink);
    self->priv->rtpbin_send_rtp_sink = NULL;
  }

  if (self->priv->rtpbin_recv_rtp_sink)
  {
    /* Release pad generates a g_warning as of Nov 11 2008
    gst_pad_set_active (self->priv->rtpbin_recv_rtp_sink, FALSE);
    gst_element_release_request_pad (self->priv->conference->gstrtpbin,
      self->priv->rtpbin_recv_rtp_sink);
    */
    gst_object_unref (self->priv->rtpbin_recv_rtp_sink);
    self->priv->rtpbin_recv_rtp_sink = NULL;
  }

  if (self->priv->rtpbin_recv_rtcp_sink)
  {
    /* Release pad generates a g_warning as of Nov 11 2008
    gst_pad_set_active (self->priv->rtpbin_recv_rtcp_sink, FALSE);
    gst_element_release_request_pad (self->priv->conference->gstrtpbin,
      self->priv->rtpbin_recv_rtcp_sink);
    */
    gst_object_unref (self->priv->rtpbin_recv_rtcp_sink);
    self->priv->rtpbin_recv_rtcp_sink = NULL;
  }

  if (self->priv->transmitters)
  {
    g_hash_table_foreach_remove (self->priv->transmitters, _remove_transmitter,
      self);

    g_hash_table_destroy (self->priv->transmitters);
    self->priv->transmitters = NULL;
  }

  if (self->priv->free_substreams)
  {
    g_list_foreach (self->priv->free_substreams, (GFunc) g_object_unref, NULL);
    g_list_free (self->priv->free_substreams);
    self->priv->free_substreams = NULL;
  }


  if (self->priv->blueprints)
  {
    fs_rtp_blueprints_unref (self->priv->media_type);
    self->priv->blueprints = NULL;
  }

  if (self->priv->conference)
  {
    g_object_unref (self->priv->conference);
    self->priv->conference = NULL;
  }

  for (item = g_list_first (self->priv->streams);
       item;
       item = g_list_next (item))
    g_object_weak_unref (G_OBJECT (item->data), _remove_stream, self);
  g_list_free (self->priv->streams);
  self->priv->streams = NULL;

  self->priv->disposed = TRUE;


  FS_RTP_SESSION_UNLOCK (self);

  /* MAKE sure dispose does not run twice. */
  self->priv->disposed = TRUE;

  parent_class->dispose (object);
}

static void
fs_rtp_session_finalize (GObject *object)
{
  FsRtpSession *self = FS_RTP_SESSION (object);

  g_static_rec_mutex_free (&self->mutex);

  if (self->priv->codec_preferences)
    fs_codec_list_destroy (self->priv->codec_preferences);

  if (self->priv->codec_associations)
    codec_association_list_destroy (self->priv->codec_associations);

  if (self->priv->current_send_codec)
    fs_codec_destroy (self->priv->current_send_codec);

  if (self->priv->requested_send_codec)
    fs_codec_destroy (self->priv->requested_send_codec);

  if (self->priv->ssrc_streams)
    g_hash_table_destroy (self->priv->ssrc_streams);

  parent_class->finalize (object);
}

static void
fs_rtp_session_get_property (GObject *object,
                             guint prop_id,
                             GValue *value,
                             GParamSpec *pspec)
{
  FsRtpSession *self = FS_RTP_SESSION (object);

  switch (prop_id)
  {
    case PROP_MEDIA_TYPE:
      g_value_set_enum (value, self->priv->media_type);
      break;
    case PROP_ID:
      g_value_set_uint (value, self->id);
      break;
    case PROP_SINK_PAD:
      g_value_set_object (value, self->priv->media_sink_pad);
      break;
    case PROP_CODEC_PREFERENCES:
      g_value_set_boxed (value, self->priv->codec_preferences);
      break;
    case PROP_CODECS:
      {
        GList *codecs = NULL;
        FS_RTP_SESSION_LOCK (self);
        codecs = codec_associations_to_codecs (self->priv->codec_associations,
            TRUE);
        FS_RTP_SESSION_UNLOCK (self);
        g_value_take_boxed (value, codecs);
      }
      break;
    case PROP_CODECS_WITHOUT_CONFIG:
      {
        GList *codecs = NULL;
        FS_RTP_SESSION_LOCK (self);
        codecs = codec_associations_to_codecs (self->priv->codec_associations,
            FALSE);
        FS_RTP_SESSION_UNLOCK (self);
        g_value_take_boxed (value, codecs);
      }
      break;
    case PROP_CODECS_READY:
      {
        GList *item = NULL;

        FS_RTP_SESSION_LOCK (self);
        for (item = g_list_first (self->priv->codec_associations);
             item;
             item = g_list_next (item))
        {
          CodecAssociation *ca = item->data;
          if (!ca->disable && ca->need_config)
            break;
        }
        FS_RTP_SESSION_UNLOCK (self);

        g_value_set_boolean (value, item == NULL);
      }
      break;
    case PROP_CONFERENCE:
      g_value_set_object (value, self->priv->conference);
      break;
    case PROP_CURRENT_SEND_CODEC:
      FS_RTP_SESSION_LOCK (self);
      g_value_set_boxed (value, self->priv->current_send_codec);
      FS_RTP_SESSION_UNLOCK (self);
      break;
    case PROP_NO_RTCP_TIMEOUT:
      FS_RTP_SESSION_LOCK (self);
      g_value_set_int (value, self->priv->no_rtcp_timeout);
      FS_RTP_SESSION_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
fs_rtp_session_set_property (GObject *object,
                             guint prop_id,
                             const GValue *value,
                             GParamSpec *pspec)
{
  FsRtpSession *self = FS_RTP_SESSION (object);

  switch (prop_id)
  {
    case PROP_MEDIA_TYPE:
      self->priv->media_type = g_value_get_enum (value);
      break;
    case PROP_ID:
      self->id = g_value_get_uint (value);
      break;
    case PROP_CONFERENCE:
      self->priv->conference = FS_RTP_CONFERENCE (g_value_dup_object (value));
      break;
    case PROP_NO_RTCP_TIMEOUT:
      FS_RTP_SESSION_LOCK (self);
      self->priv->no_rtcp_timeout = g_value_get_int (value);
      FS_RTP_SESSION_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
fs_rtp_session_constructed (GObject *object)
{
  FsRtpSession *self = FS_RTP_SESSION_CAST (object);
  GstElement *valve = NULL;
  GstElement *capsfilter = NULL;
  GstElement *tee = NULL;
  GstElement *funnel = NULL;
  GstElement *muxer = NULL;
  GstElement *fakesink = NULL;
  GstPad *tee_sink_pad = NULL;
  GstPad *valve_sink_pad = NULL;
  GstPad *funnel_src_pad = NULL;
  GstPad *muxer_src_pad = NULL;
  GstPad *transmitter_rtcp_tee_sink_pad;
  GstPad *pad1, *pad2;
  GstPadLinkReturn ret;
  gchar *tmp;

  if (self->id == 0)
  {
    g_error ("You can no instantiate this element directly, you MUST"
      " call fs_rtp_session_new ()");
    return;
  }

  self->priv->blueprints = fs_rtp_blueprints_get (self->priv->media_type,
    &self->priv->construction_error);

  if (!self->priv->blueprints)
  {
    if (!self->priv->construction_error)
      self->priv->construction_error = g_error_new (FS_ERROR,
        FS_ERROR_INTERNAL,
        "Unknown error while trying to discover codecs");
    return;
  }

  /* Create an initial list of local codec associations */
  self->priv->codec_associations = create_local_codec_associations (
      self->priv->blueprints, NULL, NULL);

  if (!self->priv->codec_associations)
  {
    self->priv->construction_error = g_error_new (FS_ERROR, FS_ERROR_INTERNAL,
        "Unable to create initial codec associations"
        " from the discovered codecs");
    return;
  }



  tmp = g_strdup_printf ("send_tee_%u", self->id);
  tee = gst_element_factory_make ("tee", tmp);
  g_free (tmp);

  if (!tee)
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not create the tee element");
    return;
  }

  if (!gst_bin_add (GST_BIN (self->priv->conference), tee))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not add the tee element to the FsRtpConference");
    gst_object_unref (tee);
    return;
  }

  gst_element_set_state (tee, GST_STATE_PLAYING);

  self->priv->send_tee = gst_object_ref (tee);


  tee_sink_pad = gst_element_get_static_pad (tee, "sink");

  tmp = g_strdup_printf ("sink_%u", self->id);
  self->priv->media_sink_pad = gst_ghost_pad_new (tmp, tee_sink_pad);
  g_free (tmp);

  if (!self->priv->media_sink_pad)
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
        FS_ERROR_CONSTRUCTION,
        "Could not create ghost pad for tee's sink pad");
    return;
  }

  gst_pad_set_active (self->priv->media_sink_pad, TRUE);
  if (!gst_element_add_pad (GST_ELEMENT (self->priv->conference),
          self->priv->media_sink_pad))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
        FS_ERROR_CONSTRUCTION,
        "Could not add ghost pad to the conference bin");
    gst_object_unref (self->priv->media_sink_pad);
    self->priv->media_sink_pad = NULL;
    return;
  }

  gst_object_unref (tee_sink_pad);

  self->priv->send_tee_discovery_pad = gst_element_get_request_pad (tee,
      "src%d");
  self->priv->send_tee_media_pad = gst_element_get_request_pad (tee,
      "src%d");

  if (!self->priv->send_tee_discovery_pad || !self->priv->send_tee_media_pad)
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
        FS_ERROR_CONSTRUCTION,
        "Could not create the send tee request src pads");
  }

  tmp = g_strdup_printf ("valve_send_%u", self->id);
  valve = gst_element_factory_make ("fsvalve", tmp);
  g_free (tmp);

  if (!valve)
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not create the fsvalve element");
    return;
  }

  if (!gst_bin_add (GST_BIN (self->priv->conference), valve))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not add the valve element to the FsRtpConference");
    gst_object_unref (valve);
    return;
  }

  g_object_set (G_OBJECT (valve), "drop", TRUE, NULL);
  gst_element_set_state (valve, GST_STATE_PLAYING);

  self->priv->media_sink_valve = gst_object_ref (valve);

  valve_sink_pad = gst_element_get_static_pad (valve, "sink");

  if (GST_PAD_LINK_FAILED (gst_pad_link (self->priv->send_tee_media_pad,
              valve_sink_pad)))
  {
    gst_object_unref (valve_sink_pad);

    self->priv->construction_error = g_error_new (FS_ERROR,
        FS_ERROR_CONSTRUCTION,
        "Could not link send tee and valve");
    return;
  }

  gst_object_unref (valve_sink_pad);




  /* Now create the transmitter RTP funnel */

  tmp = g_strdup_printf ("recv_rtp_funnel_%u", self->id);
  funnel = gst_element_factory_make ("fsfunnel", tmp);
  g_free (tmp);

  if (!funnel)
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not create the rtp funnel element");
    return;
  }

  if (!gst_bin_add (GST_BIN (self->priv->conference), funnel))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not add the rtp funnel element to the FsRtpConference");
    gst_object_unref (funnel);
    return;
  }

  self->priv->transmitter_rtp_funnel = gst_object_ref (funnel);

  tmp = g_strdup_printf ("recv_rtp_sink_%u", self->id);
  self->priv->rtpbin_recv_rtp_sink =
    gst_element_get_request_pad (self->priv->conference->gstrtpbin,
      tmp);
  g_free (tmp);

  funnel_src_pad = gst_element_get_static_pad (funnel, "src");

  ret = gst_pad_link (funnel_src_pad, self->priv->rtpbin_recv_rtp_sink);

  if (GST_PAD_LINK_FAILED (ret))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not link pad %s (%p) with pad %s (%p)",
      GST_PAD_NAME (funnel_src_pad), GST_PAD_CAPS (funnel_src_pad),
      GST_PAD_NAME (self->priv->rtpbin_recv_rtp_sink),
      GST_PAD_CAPS (self->priv->rtpbin_recv_rtp_sink));

    gst_object_unref (funnel_src_pad);
    return;
  }

  gst_object_unref (funnel_src_pad);

  gst_element_set_state (funnel, GST_STATE_PLAYING);


  /* Now create the transmitter RTCP funnel */

  tmp = g_strdup_printf ("recv_rtcp_funnel_%u", self->id);
  funnel = gst_element_factory_make ("fsfunnel", tmp);
  g_free (tmp);

  if (!funnel)
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not create the rtcp funnel element");
    return;
  }

  if (!gst_bin_add (GST_BIN (self->priv->conference), funnel))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not add the rtcp funnel element to the FsRtcpConference");
    gst_object_unref (funnel);
    return;
  }

  self->priv->transmitter_rtcp_funnel = gst_object_ref (funnel);

  tmp = g_strdup_printf ("recv_rtcp_sink_%u", self->id);
  self->priv->rtpbin_recv_rtcp_sink =
    gst_element_get_request_pad (self->priv->conference->gstrtpbin,
      tmp);
  g_free (tmp);

  funnel_src_pad = gst_element_get_static_pad (funnel, "src");

  ret = gst_pad_link (funnel_src_pad, self->priv->rtpbin_recv_rtcp_sink);

  if (GST_PAD_LINK_FAILED (ret))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not link pad %s (%p) with pad %s (%p)",
      GST_PAD_NAME (funnel_src_pad), GST_PAD_CAPS (funnel_src_pad),
      GST_PAD_NAME (self->priv->rtpbin_recv_rtcp_sink),
      GST_PAD_CAPS (self->priv->rtpbin_recv_rtcp_sink));

    gst_object_unref (funnel_src_pad);
    return;
  }

  gst_object_unref (funnel_src_pad);

  gst_element_set_state (funnel, GST_STATE_PLAYING);

  /* Lets now create the RTP muxer */

  tmp = g_strdup_printf ("send_rtp_muxer_%u", self->id);
  muxer = gst_element_factory_make ("rtpdtmfmux", tmp);
  g_free (tmp);

  if (!muxer)
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not create the rtp muxer element");
    return;
  }

  if (!gst_bin_add (GST_BIN (self->priv->conference), muxer))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not add the rtp muxer element to the FsRtpConference");
    gst_object_unref (muxer);
    return;
  }

  self->priv->rtpmuxer = gst_object_ref (muxer);

  tmp = g_strdup_printf ("send_rtp_sink_%u", self->id);
  self->priv->rtpbin_send_rtp_sink =
    gst_element_get_request_pad (self->priv->conference->gstrtpbin,
      tmp);
  g_free (tmp);

  muxer_src_pad = gst_element_get_static_pad (muxer, "src");

  ret = gst_pad_link (muxer_src_pad, self->priv->rtpbin_send_rtp_sink);

  if (GST_PAD_LINK_FAILED (ret))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not link pad %s (%p) with pad %s (%p)",
      GST_PAD_NAME (muxer_src_pad), GST_PAD_CAPS (muxer_src_pad),
      GST_PAD_NAME (self->priv->rtpbin_send_rtp_sink),
      GST_PAD_CAPS (self->priv->rtpbin_send_rtp_sink));

    gst_object_unref (muxer_src_pad);
    return;
  }

  gst_object_unref (muxer_src_pad);

  gst_element_set_state (muxer, GST_STATE_PLAYING);


  /* Now create the transmitter RTP tee */

  tmp = g_strdup_printf ("send_rtp_tee_%u", self->id);
  tee = gst_element_factory_make ("tee", tmp);
  g_free (tmp);

  if (!tee)
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not create the rtp tee element");
    return;
  }

  if (!gst_bin_add (GST_BIN (self->priv->conference), tee))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not add the rtp tee element to the FsRtpConference");
    gst_object_unref (tee);
    return;
  }

  gst_element_set_state (tee, GST_STATE_PLAYING);

  self->priv->transmitter_rtp_tee = gst_object_ref (tee);

  tmp = g_strdup_printf ("send_rtp_src_%u", self->id);
  if (!gst_element_link_pads (
          self->priv->conference->gstrtpbin, tmp,
          self->priv->transmitter_rtp_tee, "sink"))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
        FS_ERROR_CONSTRUCTION,
        "Could not link rtpbin %s pad to tee sink", tmp);
    g_free (tmp);
    return;
  }
  g_free (tmp);

  /* Now add a fakesink to the RTP tee */

  tmp = g_strdup_printf ("sink_rtp_fakesink_%u", self->id);
  fakesink = gst_element_factory_make ("fakesink", tmp);
  g_free (tmp);

  if (!fakesink)
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not create the rtp fakesink element");
    return;
  }

  if (!gst_bin_add (GST_BIN (self->priv->conference), fakesink))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not add the rtp fakesink element to the FsRtpConference");
    gst_object_unref (tee);
    return;
  }

  g_object_set (fakesink, "sync", FALSE, "async", FALSE, NULL);

  gst_element_set_state (fakesink, GST_STATE_PLAYING);

  self->priv->transmitter_rtp_fakesink = gst_object_ref (fakesink);

  pad1 = gst_element_get_request_pad (tee, "src%d");
  pad2 = gst_element_get_static_pad (fakesink, "sink");

  ret = gst_pad_link (pad1, pad2);

  gst_object_unref (pad2);
  gst_object_unref (pad1);

  if (GST_PAD_LINK_FAILED (ret))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
        "Could not link the rtp tee to its fakesink");
    return;
  }

  /* Now create the transmitter RTCP tee */

  tmp = g_strdup_printf ("send_rtcp_tee_%u", self->id);
  tee = gst_element_factory_make ("tee", tmp);
  g_free (tmp);

  if (!tee)
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not create the rtcp tee element");
    return;
  }

  if (!gst_bin_add (GST_BIN (self->priv->conference), tee))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not add the rtcp tee element to the FsRtpConference");
    gst_object_unref (tee);
    return;
  }

  gst_element_set_state (tee, GST_STATE_PLAYING);

  self->priv->transmitter_rtcp_tee = gst_object_ref (tee);

  tmp = g_strdup_printf ("send_rtcp_src_%u", self->id);
  self->priv->rtpbin_send_rtcp_src =
    gst_element_get_request_pad (self->priv->conference->gstrtpbin, tmp);

  if (!self->priv->rtpbin_send_rtcp_src)
  {
     self->priv->construction_error = g_error_new (FS_ERROR,
         FS_ERROR_CONSTRUCTION,
         "Could not get %s request pad from the gstrtpbin", tmp);
    g_free (tmp);
    return;
  }
  g_free (tmp);


  transmitter_rtcp_tee_sink_pad =
    gst_element_get_static_pad (self->priv->transmitter_rtcp_tee, "sink");
  g_assert (transmitter_rtcp_tee_sink_pad);

  ret = gst_pad_link (self->priv->rtpbin_send_rtcp_src,
    transmitter_rtcp_tee_sink_pad);

  gst_object_unref (transmitter_rtcp_tee_sink_pad);

  if (GST_PAD_LINK_FAILED (ret))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
        FS_ERROR_CONSTRUCTION,
        "Could not link rtpbin network rtcp src to tee");
    return;
  }

  /* Now add a fakesink to the RTCP tee */

  tmp = g_strdup_printf ("sink_rtcp_fakesink_%u", self->id);
  fakesink = gst_element_factory_make ("fakesink", tmp);
  g_free (tmp);

  if (!fakesink)
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not create the rtcp fakesink element");
    return;
  }

  if (!gst_bin_add (GST_BIN (self->priv->conference), fakesink))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not add the rtcp fakesink element to the FsRtcpConference");
    gst_object_unref (tee);
    return;
  }

  g_object_set (fakesink, "sync", FALSE, "async", FALSE, NULL);

  gst_element_set_state (fakesink, GST_STATE_PLAYING);

  self->priv->transmitter_rtcp_fakesink = gst_object_ref (fakesink);

  pad1 = gst_element_get_request_pad (tee, "src%d");
  pad2 = gst_element_get_static_pad (fakesink, "sink");

  ret = gst_pad_link (pad1, pad2);

  gst_object_unref (pad2);
  gst_object_unref (pad1);

  if (GST_PAD_LINK_FAILED (ret))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
        "Could not link the rtcp tee to its fakesink");
    return;
  }

  /* Lets now do the send_capsfilter */

  tmp = g_strdup_printf ("send_rtp_capsfilter_%u", self->id);
  capsfilter = gst_element_factory_make ("capsfilter", tmp);
  g_free (tmp);

  if (!capsfilter)
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not create the rtp capsfilter element");
    return;
  }

  if (!gst_bin_add (GST_BIN (self->priv->conference), capsfilter))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
      FS_ERROR_CONSTRUCTION,
      "Could not add the rtp capsfilter element to the FsRtpConference");
    gst_object_unref (capsfilter);
    return;
  }

  pad1 = gst_element_get_static_pad (capsfilter, "src");
  g_signal_connect (pad1, "notify::caps", G_CALLBACK (_send_caps_changed),
      self);
  gst_object_unref (pad1);

  self->priv->send_capsfilter = gst_object_ref (capsfilter);

  if (!gst_element_link_pads (capsfilter, "src", muxer, NULL))
  {
    self->priv->construction_error = g_error_new (FS_ERROR,
        FS_ERROR_CONSTRUCTION,
        "Could not link pad capsfilter src pad to the rtpmux");
    return;
  }

  gst_element_set_state (capsfilter, GST_STATE_PLAYING);

  fs_rtp_session_start_codec_param_gathering (self);

  GST_CALL_PARENT (G_OBJECT_CLASS, constructed, (object));
}


static void
_stream_known_source_packet_received (FsRtpStream *stream, guint component,
    GstBuffer *buffer, gpointer user_data)
{
  guint32 ssrc;
  FsRtpSession *self = FS_RTP_SESSION_CAST (user_data);

  if (component == 1)
  {
    if (gst_rtp_buffer_validate (buffer))
    {
      ssrc = gst_rtp_buffer_get_ssrc (buffer);
      goto ok;
    }
  }
  else if (component == 2)
  {
    GstRTCPPacket rtcppacket;

    if (gst_rtcp_buffer_validate (buffer))
    {
      if (gst_rtcp_buffer_get_first_packet (buffer, &rtcppacket))
      {
        do {
          if (gst_rtcp_packet_get_type (&rtcppacket) == GST_RTCP_TYPE_SDES)
          {
            ssrc = gst_rtcp_packet_sdes_get_ssrc (&rtcppacket);
            goto ok;
          }
        } while (gst_rtcp_packet_move_to_next (&rtcppacket));
      }
    }
  }

  /* We would have jumped to OK if we had a valid packet */
  return;

 ok:

  FS_RTP_SESSION_LOCK (self);

  if (!g_hash_table_lookup (self->priv->ssrc_streams,  GUINT_TO_POINTER (ssrc)))
  {
    GST_DEBUG ("Associating SSRC %x in session %d", ssrc, self->id);
    g_hash_table_insert (self->priv->ssrc_streams, GUINT_TO_POINTER (ssrc),
        stream);

    FS_RTP_SESSION_UNLOCK (self);

    fs_rtp_session_associate_free_substreams (self, stream, ssrc);
  }
  else
  {
    FS_RTP_SESSION_UNLOCK (self);
  }

}

static gboolean
_remove_stream_from_ht (gpointer key, gpointer value, gpointer user_data)
{
  return (value == user_data);
}

static void
_remove_stream (gpointer user_data,
    GObject *where_the_object_was)
{
  FsRtpSession *self = FS_RTP_SESSION (user_data);

  FS_RTP_SESSION_LOCK (self);
  self->priv->streams =
    g_list_remove_all (self->priv->streams, where_the_object_was);

  g_hash_table_foreach_remove (self->priv->ssrc_streams, _remove_stream_from_ht,
      where_the_object_was);
  FS_RTP_SESSION_UNLOCK (self);
}

/**
 * fs_rtp_session_new_stream:
 * @session: an #FsRtpSession
 * @participant: #FsParticipant of a participant for the new stream
 * @direction: #FsStreamDirection describing the direction of the new stream that will
 * be created for this participant
 * @transmitter: Name of the type of transmitter to use for this session
 * @error: location of a #GError, or NULL if no error occured
 *
 * This function creates a stream for the given participant into the active session.
 *
 * Returns: the new #FsStream that has been created. User must unref the
 * #FsStream when the stream is ended. If an error occured, returns NULL.
 */
static FsStream *
fs_rtp_session_new_stream (FsSession *session,
    FsParticipant *participant,
    FsStreamDirection direction,
    const gchar *transmitter,
    guint n_parameters,
    GParameter *parameters,
    GError **error)
{
  FsRtpSession *self = FS_RTP_SESSION (session);
  FsRtpParticipant *rtpparticipant = NULL;
  FsStream *new_stream = NULL;
  FsStreamTransmitter *st;

  if (!FS_IS_RTP_PARTICIPANT (participant))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
      "You have to provide a participant of type RTP");
    return NULL;
  }
  rtpparticipant = FS_RTP_PARTICIPANT (participant);

  st = fs_rtp_session_get_new_stream_transmitter (self, transmitter,
      participant, n_parameters, parameters, error);

  if (!st)
    return NULL;

  new_stream = FS_STREAM_CAST (fs_rtp_stream_new (self, rtpparticipant,
          direction, st, _stream_new_remote_codecs,
          _stream_known_source_packet_received, self, error));

  FS_RTP_SESSION_LOCK (self);
  self->priv->streams = g_list_append (self->priv->streams, new_stream);
  FS_RTP_SESSION_UNLOCK (self);

  g_object_weak_ref (G_OBJECT (new_stream), _remove_stream, self);

  return new_stream;
}

/**
 * fs_rtp_session_start_telephony_event:
 * @session: an #FsRtpSession
 * @event: A #FsStreamDTMFEvent or another number defined at
 * http://www.iana.org/assignments/audio-telephone-event-registry
 * @volume: The volume in dBm0 without the negative sign. Should be between
 * 0 and 36. Higher values mean lower volume
 * @method: The method used to send the event
 *
 * This function will start sending a telephony event (such as a DTMF
 * tone) on the #FsRtpSession. You have to call the function
 * #fs_rtp_session_stop_telephony_event () to stop it.
 * This function will use any available method, if you want to use a specific
 * method only, use #fs_rtp_session_start_telephony_event_full ()
 *
 * Returns: %TRUE if sucessful, it can return %FALSE if the #FsStream
 * does not support this telephony event.
 */
static gboolean
fs_rtp_session_start_telephony_event (FsSession *session, guint8 event,
                                      guint8 volume, FsDTMFMethod method)
{
  FsRtpSession *self = FS_RTP_SESSION (session);
  gboolean ret = FALSE;

  FS_RTP_SESSION_LOCK (self);
  ret = fs_rtp_special_sources_start_telephony_event (
      self->priv->extra_sources, event, volume, method);
  FS_RTP_SESSION_UNLOCK (self);

  return ret;
}

/**
 * fs_rtp_session_stop_telephony_event:
 * @session: an #FsRtpSession
 * @method: The method used to send the event
 *
 * This function will stop sending a telephony event started by
 * #fs_rtp_session_start_telephony_event (). If the event was being sent
 * for less than 50ms, it will be sent for 50ms minimum. If the
 * duration was a positive and the event is not over, it will cut it
 * short.
 *
 * Returns: %TRUE if sucessful, it can return %FALSE if the #FsRtpSession
 * does not support telephony events or if no telephony event is being sent
 */
static gboolean
fs_rtp_session_stop_telephony_event (FsSession *session, FsDTMFMethod method)
{
  FsRtpSession *self = FS_RTP_SESSION (session);
  gboolean ret = FALSE;

  FS_RTP_SESSION_LOCK (self);
  ret = fs_rtp_special_sources_stop_telephony_event (
      self->priv->extra_sources, method);
  FS_RTP_SESSION_UNLOCK (self);

  return ret;
}

/**
 * fs_rtp_session_set_send_codec:
 * @session: an #FsRtpSession
 * @send_codec: an #FsCodec representing the codec to send
 * @error: location of a #GError, or NULL if no error occured
 *
 * This function will set the currently being sent codec for all streams in this
 * session. The given #FsCodec must be taken directly from the #FsSession:codecs
 * property of the session. If the given codec is not in the codecs
 * list, @error will be set and %FALSE will be returned. The @send_codec will be
 * copied so it must be free'd using fs_codec_destroy () when done.
 *
 * Returns: %FALSE if the send codec couldn't be set.
 */
static gboolean
fs_rtp_session_set_send_codec (FsSession *session, FsCodec *send_codec,
                               GError **error)
{
  FsRtpSession *self = FS_RTP_SESSION (session);
  gboolean ret = FALSE;

  FS_RTP_SESSION_LOCK (self);

  if (lookup_codec_association_by_codec_without_config (
          self->priv->codec_associations, send_codec))
  {
    if (self->priv->requested_send_codec)
      fs_codec_destroy (self->priv->requested_send_codec);

    self->priv->requested_send_codec = fs_codec_copy (send_codec);

    ret = fs_rtp_session_verify_send_codec_bin_locked (self, error);
  }
  else
  {
    g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
        "The passed codec is not part of the list of codecs");
  }

  FS_RTP_SESSION_UNLOCK (self);

  return ret;
}

static gboolean
fs_rtp_session_set_codec_preferences (FsSession *session,
    GList *codec_preferences,
    GError **error)
{
  FsRtpSession *self = FS_RTP_SESSION (session);
  GList *old_codec_prefs = NULL;
  GList *new_codec_prefs = fs_codec_list_copy (codec_preferences);
  gboolean ret;

  new_codec_prefs =
    validate_codecs_configuration (
        self->priv->media_type, self->priv->blueprints,
        new_codec_prefs);

  if (new_codec_prefs == NULL)
    GST_DEBUG ("None of the new codec preferences passed are usable,"
        " this will restore the original list of detected codecs");

  FS_RTP_SESSION_LOCK (self);

  old_codec_prefs = self->priv->codec_preferences;

  self->priv->codec_preferences = new_codec_prefs;

  ret = fs_rtp_session_update_codecs (self, NULL, NULL, error);
  if (ret)
  {
    fs_codec_list_destroy (old_codec_prefs);

    g_object_notify ((GObject*) self, "codecs");
    g_object_notify ((GObject*) self, "codecs-without-config");
    g_object_notify ((GObject*) self, "codec-preferences");

    gst_element_post_message (GST_ELEMENT (self->priv->conference),
        gst_message_new_element (GST_OBJECT (self->priv->conference),
            gst_structure_new ("farsight-codecs-changed",
                "session", FS_TYPE_SESSION, self,
                NULL)));

  }
  else
  {
    fs_codec_list_destroy (new_codec_prefs);
    self->priv->codec_preferences = old_codec_prefs;
    GST_WARNING ("Invalid new codec preferences");
  }

  FS_RTP_SESSION_UNLOCK (self);

  return ret;
}

FsRtpSession *
fs_rtp_session_new (FsMediaType media_type, FsRtpConference *conference,
                    guint id, GError **error)
{
  FsRtpSession *session = g_object_new (FS_TYPE_RTP_SESSION,
    "media-type", media_type,
    "conference", conference,
    "id", id,
    NULL);

  if (session->priv->construction_error)
  {
    g_propagate_error (error, session->priv->construction_error);
    g_object_unref (session);
    return NULL;
  }

  return session;
}


GstCaps *
fs_rtp_session_request_pt_map (FsRtpSession *session, guint pt)
{
  GstCaps *caps = NULL;
  CodecAssociation *ca = NULL;

  FS_RTP_SESSION_LOCK (session);

  ca = lookup_codec_association_by_pt (
      session->priv->codec_associations, pt);

  if (ca)
  {
    FsCodec *tmpcodec = codec_copy_without_config (ca->codec);
    caps = fs_codec_to_gst_caps (tmpcodec);
    fs_codec_destroy (tmpcodec);
  }

  FS_RTP_SESSION_UNLOCK (session);

  if (!caps)
    GST_WARNING ("Could not get caps for payload type %u in session %d",
        pt, session->id);

  return caps;
}

static gboolean
_get_request_pad_and_link (GstElement *tee_funnel, const gchar *tee_funnel_name,
  GstElement *sinksrc, const gchar *sinksrc_padname, GstPadDirection direction,
  GError **error)
{
  GstPad *requestpad = NULL;
  GstPad *transpad = NULL;
  GstPadLinkReturn ret;
  gchar *requestpad_name = (direction == GST_PAD_SINK) ? "src%d" : "sink%d";

  /* The transmitter will only be removed when the whole session is disposed,
   * then the
   */
  requestpad = gst_element_get_request_pad (tee_funnel, requestpad_name);


  if (!requestpad)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
      "Can not get the %s pad from the transmitter %s element",
      requestpad_name, tee_funnel_name);
    return FALSE;
  }

  transpad = gst_element_get_static_pad (sinksrc, sinksrc_padname);

  if (direction == GST_PAD_SINK)
    ret = gst_pad_link (requestpad, transpad);
  else
    ret = gst_pad_link (transpad, requestpad);

  gst_object_unref (requestpad);
  gst_object_unref (transpad);

  if (GST_PAD_LINK_FAILED (ret))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
      "Can not link the %s to the transmitter %s", tee_funnel_name,
      (direction == GST_PAD_SINK) ? "sink" : "src");
    return FALSE;
  }

  return TRUE;
}

/**
 * fs_rtp_session_get_new_stream_transmitter:
 * @self: a #FsRtpSession
 * @transmitter_name: The name of the transmitter to create a stream for
 * @participant: The #FsRtpParticipant for this stream
 * @n_parameters: the number of parameters
 * @parameters: a table of n_parameters #GParameter structs
 *
 * This function will create a new #FsStreamTransmitter, possibly creating
 * and inserting into the pipeline its parent #FsTransmitter
 *
 * Returns: a newly allocated #FsStreamTransmitter
 */

static FsStreamTransmitter *
fs_rtp_session_get_new_stream_transmitter (FsRtpSession *self,
  const gchar *transmitter_name, FsParticipant *participant, guint n_parameters,
  GParameter *parameters, GError **error)
{
  FsTransmitter *transmitter;
  GstElement *src, *sink;

  transmitter = g_hash_table_lookup (self->priv->transmitters,
    transmitter_name);

  if (transmitter)
  {
    return fs_transmitter_new_stream_transmitter (transmitter, participant,
      n_parameters, parameters, error);
  }

  transmitter = fs_transmitter_new (transmitter_name, 2, error);
  if (!transmitter)
    return NULL;

  g_object_get (transmitter, "gst-sink", &sink, "gst-src", &src, NULL);

  if (!gst_bin_add (GST_BIN (self->priv->conference), sink))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
      "Could not add the transmitter sink for %s to the conference",
      transmitter_name);
    goto error;
  }

  if (!gst_bin_add (GST_BIN (self->priv->conference), src))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
      "Could not add the transmitter src for %s to the conference",
      transmitter_name);
    goto error;
  }

  if (!_get_request_pad_and_link (self->priv->transmitter_rtp_tee,
      "rtp tee", sink, "sink1", GST_PAD_SINK, error))
    goto error;

  if (!_get_request_pad_and_link (self->priv->transmitter_rtcp_tee,
      "rtcp tee", sink, "sink2", GST_PAD_SINK, error))
    goto error;

  if (!_get_request_pad_and_link (self->priv->transmitter_rtp_funnel,
      "rtp funnel", src, "src1", GST_PAD_SRC, error))
    goto error;

  if (!_get_request_pad_and_link (self->priv->transmitter_rtcp_funnel,
      "rtcp funnel", src, "src2", GST_PAD_SRC, error))
    goto error;

  gst_element_sync_state_with_parent (src);
  gst_element_sync_state_with_parent (sink);

  g_hash_table_insert (self->priv->transmitters, g_strdup (transmitter_name),
    transmitter);

  gst_object_unref (src);
  gst_object_unref (sink);

  return fs_transmitter_new_stream_transmitter (transmitter, participant,
    n_parameters, parameters, error);

 error:
  if (src)
    gst_object_unref (src);
  if (sink)
    gst_object_unref (sink);
  if (transmitter)
    g_object_unref (transmitter);

  return NULL;
}


/**
 * fs_rtp_session_get_stream_by_ssrc_locked
 * @self: The #FsRtpSession
 * @stream_ssrc: The stream ssrc
 *
 * Gets the #FsRtpStream for the SSRC or %NULL if it doesnt exist
 *
 * Return value: A #FsRtpStream (unref after use) or %NULL if it doesn't exist
 */
static FsRtpStream *
fs_rtp_session_get_stream_by_ssrc (FsRtpSession *self,
    guint32 ssrc)
{
  FsRtpStream *stream = NULL;

  FS_RTP_SESSION_LOCK (self);

  stream = g_hash_table_lookup (self->priv->ssrc_streams,
      GUINT_TO_POINTER (ssrc));

  if (stream)
    g_object_ref (stream);

  FS_RTP_SESSION_UNLOCK (self);

  return stream;
}


/**
 * fs_rtp_session_verify_recv_codecs
 * @session: A #FsRtpSession
 *
 * Verifies that the various substreams still have the right codec, otherwise
 * re-sets it.
 */

static void
fs_rtp_session_verify_recv_codecs (FsRtpSession *session)
{
  GList *item, *item2;

  FS_RTP_SESSION_LOCK (session);

  for (item = g_list_first (session->priv->free_substreams);
       item;
       item = g_list_next (item))
    fs_rtp_sub_stream_verify_codec_locked (item->data);

  for (item = g_list_first (session->priv->streams);
       item;
       item = g_list_next (item))
  {
    FsRtpStream *stream = item->data;

    for (item2 = g_list_first (stream->substreams);
         item2;
         item2 = g_list_next (item2))
      fs_rtp_sub_stream_verify_codec_locked (item2->data);

  }

  FS_RTP_SESSION_UNLOCK (session);
}

/**
 * fs_rtp_session_distribute_recv_codecs:
 * @session: a #FsRtpSession
 * @force_stream: The #FsRtpStream to which the new remote codecs belong
 * @forced_remote_codecs: The #GList of remote codecs to use for that stream
 *
 * This function distributes the codecs to the streams including their
 * own config data.
 *
 * If a stream is specified, it will use the specified remote codecs
 * instead of the ones currently in the stream.
 */


static void
fs_rtp_session_distribute_recv_codecs (FsRtpSession *session,
    FsRtpStream *force_stream,
    GList *forced_remote_codecs)
{
  GList *item = NULL;

  FS_RTP_SESSION_LOCK (session);

  for (item = session->priv->streams;
       item;
       item = g_list_next (item))
  {
    FsRtpStream *stream = item->data;
    GList *remote_codecs = NULL;

    if (stream == force_stream)
      remote_codecs = forced_remote_codecs;
    else
      g_object_get (stream, "remote-codecs", &remote_codecs, NULL);

    if (remote_codecs)
    {
      GList *new_codecs = codec_associations_to_codecs (
          session->priv->codec_associations, FALSE);
      GList *item2 = NULL;

      for (item2 = new_codecs;
           item2;
           item2 = g_list_next (item2))
      {
        FsCodec *codec = item2->data;
        GList *item3 = NULL;
        FsCodec *remote_codec = NULL;

        for (item3 = remote_codecs; item3; item3 = g_list_next (item3))
        {
          FsCodec *tmpcodec = NULL;
          remote_codec = item3->data;

          tmpcodec = sdp_is_compat (codec, remote_codec, TRUE);
          if (tmpcodec)
          {
            fs_codec_destroy (tmpcodec);
            break;
          }
        }

        if (item3 == NULL)
          remote_codec = NULL;

        GST_LOG ("Adding codec to stream %p " FS_CODEC_FORMAT, stream,
            FS_CODEC_ARGS (codec));

        if (remote_codec)
        {
          for (item3 = remote_codec->optional_params; item3;
               item3 = g_list_next (item3))
          {
            FsCodecParameter *param = item3->data;
            if (codec_has_config_data_named (codec, param->name))
            {
              GST_LOG ("Adding parameter to stream %p %s=%s", stream,
                  param->name, param->value);
              fs_codec_add_optional_parameter (codec, param->name,
                  param->value);
            }
          }
        }
      }

      fs_rtp_stream_set_negotiated_codecs (stream, new_codecs);
    }

    if (stream != force_stream)
      fs_codec_list_destroy (remote_codecs);
  }

  FS_RTP_SESSION_UNLOCK (session);
}


/**
 * fs_rtp_session_negotiate_codecs:
 * @session: a #FsRtpSession
 * @stream: The #FsRtpStream to which the new remote codecs belong
 * @remote_codecs: The #GList of remote codecs to use for that stream
 * @has_remotes: Set to %TRUE if at least one stream has remote codecs
 *  set to %FALSE otherwise
 *
 * Negotiates the codecs using the current (stored) codecs
 * and the remote codecs from each stream.
 * If a stream is specified, it will use the specified remote codecs
 * instead of the ones currently in the stream
 *
 * MT safe
 *
 * Returns: the newly negotiated codec associations or %NULL on error
 */

static GList *
fs_rtp_session_negotiate_codecs (FsRtpSession *session,
    FsRtpStream *stream,
    GList *remote_codecs,
    gboolean *has_remotes,
    GError **error)
{
  gint streams_with_codecs = 0;
  gboolean has_many_streams = FALSE;
  GList *new_negotiated_codec_associations = NULL;
  GList *item;

  *has_remotes = FALSE;

  FS_RTP_SESSION_LOCK (session);

  for (item = g_list_first (session->priv->streams);
       item;
       item = g_list_next (item))
  {
    FsRtpStream *mystream = item->data;
    if (mystream == stream)
    {
      if (remote_codecs)
        streams_with_codecs ++;
    }
    else if (mystream->remote_codecs)
    {
      streams_with_codecs ++;
    }
  }

  if (streams_with_codecs >= 2)
    has_many_streams = TRUE;

  new_negotiated_codec_associations = create_local_codec_associations (
      session->priv->blueprints, session->priv->codec_preferences,
      session->priv->codec_associations);

  if (!new_negotiated_codec_associations)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_NO_CODECS_LEFT,
        "Codec config would leave no valid local codecs");
    goto error;
  }

  for (item = g_list_first (session->priv->streams);
       item;
       item = g_list_next (item))
  {
    FsRtpStream *mystream = item->data;
    GList *codecs = NULL;

    if (mystream == stream)
      codecs = remote_codecs;
    else
      codecs = mystream->remote_codecs;

    if (codecs)
    {
      GList *tmp_codec_associations = NULL;

      *has_remotes = TRUE;

      tmp_codec_associations = negotiate_stream_codecs (codecs,
          new_negotiated_codec_associations, has_many_streams);

      codec_association_list_destroy (new_negotiated_codec_associations);
      new_negotiated_codec_associations = tmp_codec_associations;

      if (!new_negotiated_codec_associations)
        break;
    }
  }

  if (!new_negotiated_codec_associations)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_NEGOTIATION_FAILED,
        "There was no intersection between the remote codecs"
        " and the local ones");
    goto error;
  }

  new_negotiated_codec_associations = finish_codec_negotiation (
      session->priv->codec_associations,
      new_negotiated_codec_associations);


  FS_RTP_SESSION_UNLOCK (session);

  return new_negotiated_codec_associations;

 error:

  FS_RTP_SESSION_UNLOCK (session);

  return NULL;
}



/**
 * fs_rtp_session_update_codecs:
 * @session: a #FsRtpSession
 * @stream: The #FsRtpStream to which the new remote codecs belong
 * @remote_codecs: The #GList of remote codecs to use for that stream
 *
 * Negotiates the codecs using the current (stored) codecs
 * and the remote codecs from each stream.
 * If a stream is specified, it will use the specified remote codecs
 * instead of the ones currently in the stream
 *
 * MT safe
 *
 * Returns: TRUE if the negotiation succeeds, FALSE otherwise
 */

static gboolean
fs_rtp_session_update_codecs (FsRtpSession *session,
    FsRtpStream *stream,
    GList *remote_codecs,
    GError **error)
{
  GList *new_negotiated_codec_associations = NULL;
  gboolean is_new = TRUE;
  GList *old_negotiated_codec_associations;
  gboolean has_remotes = FALSE;

  FS_RTP_SESSION_LOCK (session);

  old_negotiated_codec_associations =
    session->priv->codec_associations;

  new_negotiated_codec_associations = fs_rtp_session_negotiate_codecs (
      session, stream, remote_codecs, &has_remotes, error);

  if (!new_negotiated_codec_associations)
  {
    FS_RTP_SESSION_UNLOCK (session);
    return FALSE;
  }

  session->priv->codec_associations = new_negotiated_codec_associations;

  if (old_negotiated_codec_associations)
  {
    is_new = ! codec_associations_list_are_equal (
        old_negotiated_codec_associations,
        new_negotiated_codec_associations);

    codec_association_list_destroy (old_negotiated_codec_associations);
  }

  fs_rtp_session_distribute_recv_codecs (session, stream, remote_codecs);

  fs_rtp_session_verify_recv_codecs (session);

  if (is_new)
    g_signal_emit_by_name (session->priv->conference->gstrtpbin,
        "clear-pt-map");

  fs_rtp_session_start_codec_param_gathering (session);

  if (has_remotes)
  {
    if (!fs_rtp_session_verify_send_codec_bin_locked (session, error))
    {
      FS_RTP_SESSION_UNLOCK (session);
      return FALSE;
    }
  }

  FS_RTP_SESSION_UNLOCK (session);

  if (is_new)
  {
    g_object_notify (G_OBJECT (session), "codecs");
    g_object_notify (G_OBJECT (session), "codecs-without-config");

    gst_element_post_message (GST_ELEMENT (session->priv->conference),
        gst_message_new_element (GST_OBJECT (session->priv->conference),
            gst_structure_new ("farsight-codecs-changed",
                "session", FS_TYPE_SESSION, session,
                NULL)));
  }

  return TRUE;
}

static gboolean
_stream_new_remote_codecs (FsRtpStream *stream,
    GList *codecs, GError **error, gpointer user_data)
{
  FsRtpSession *session = FS_RTP_SESSION_CAST (user_data);

  return fs_rtp_session_update_codecs (session, stream, codecs, error);
}


static void
_substream_error (FsRtpSubStream *substream,
    gint errorno,
    gchar *error_msg,
    gchar *debug_msg,
    gpointer user_data)
{
  FsSession *session = FS_SESSION (user_data);

  fs_session_emit_error (session, errorno, error_msg, debug_msg);
}

/**
 * fs_rtp_session_new_recv_pad:
 * @session: a #FsSession
 * @new_pad: the newly created pad
 * @ssrc: the ssrc for this new pad
 * @pt: the pt for this new pad
 *
 * This function is called by the #FsRtpConference when a new src pad appears.
 * It can will be called on the streaming thread.
 *
 * MT safe.
 */

void
fs_rtp_session_new_recv_pad (FsRtpSession *session, GstPad *new_pad,
  guint32 ssrc, guint pt)
{
  FsRtpSubStream *substream = NULL;
  FsRtpStream *stream = NULL;
  GError *error = NULL;
  gint no_rtcp_timeout;

  FS_RTP_SESSION_LOCK (session);
  no_rtcp_timeout = session->priv->no_rtcp_timeout;
  FS_RTP_SESSION_UNLOCK (session);

  substream = fs_rtp_sub_stream_new (session->priv->conference, session,
      new_pad, ssrc, pt, no_rtcp_timeout, &error);

  if (substream == NULL)
  {
    if (error && error->domain == FS_ERROR)
      fs_session_emit_error (FS_SESSION (session), error->code,
        "Could not create a substream for the new pad", error->message);
    else
      fs_session_emit_error (FS_SESSION (session), FS_ERROR_CONSTRUCTION,
        "Could not create a substream for the new pad",
        "No error details returned");

    g_clear_error (&error);
    return;
  }

  g_signal_connect (substream, "blocked", G_CALLBACK (_substream_blocked),
      session);

  /* Lets find the FsRtpStream for this substream, if no Stream claims it
   * then we just store it
   */

  FS_RTP_SESSION_LOCK (session);
  stream = fs_rtp_session_get_stream_by_ssrc (session, ssrc);

  if (stream)
    GST_DEBUG ("Already have a stream with SSRC %x, using it", ssrc);

  /* Add the substream directly if the no_rtcp_timeout is 0 and
   * there is only one stream */
  if (!stream)
  {
    if (no_rtcp_timeout == 0 &&
        g_list_length (session->priv->streams) == 1)
    {
      stream = g_object_ref (g_list_first (session->priv->streams)->data);
      GST_DEBUG ("No RTCP timeout and only one stream, giving it substream"
          " for SSRC %x in session %u", ssrc, session->id);
    }
    else
    {
      session->priv->free_substreams =
        g_list_prepend (session->priv->free_substreams, substream);

      g_signal_connect (substream, "error", G_CALLBACK (_substream_error),
          session);

      if (no_rtcp_timeout > 0)
      {
        g_signal_connect (substream, "no-rtcp-timedout",
            G_CALLBACK (_substream_no_rtcp_timedout_cb), session);
        GST_DEBUG ("No stream for SSRC %x, waiting for %d ms before associating"
            "in session %u", ssrc, no_rtcp_timeout, session->id);
      }
      else if (no_rtcp_timeout < 0)
      {
        GST_DEBUG ("No RTCP timeout is < 0, we will wait forever for an"
            " RTCP SDES to arrive for SSRC %x in session %u",
            ssrc, session->id);
      }
      else
      {
        GST_WARNING ("No RTCP timeout is 0, but there is more than one stream,"
            " we will wait forever for an RTCP SDES to arrive for SSRC %u in"
            " session %u", ssrc, session->id);
      }
    }
  }


  if (!fs_rtp_session_substream_set_codec_bin (session, substream, stream,
          ssrc, pt, &error))
  {
    if (error)
      fs_session_emit_error (FS_SESSION (session), error->code,
          "Could not add the codec bin to the new substream", error->message);
    else
      fs_session_emit_error (FS_SESSION (session), FS_ERROR_CONSTRUCTION,
          "Could not add the codec bin to the new substream",
          "No error details returned");
  }

  g_clear_error (&error);


  FS_RTP_SESSION_UNLOCK (session);

  if (stream)
  {
    if (!fs_rtp_stream_add_substream (stream, substream, &error))
      fs_session_emit_error (FS_SESSION (session), error->code,
          "Could not add the output ghostpad to the new substream",
          error->message);

    g_clear_error (&error);
    g_object_unref (stream);
  }
}



static gboolean
_g_object_has_property (GObject *object, const gchar *property)
{
 GObjectClass *klass;

  klass = G_OBJECT_GET_CLASS (object);
  return NULL != g_object_class_find_property (klass, property);
}


static gboolean
_create_ghost_pad (GstElement *current_element, const gchar *padname, GstElement
  *codec_bin, GError **error)
{
  GstPad *ghostpad;
  GstPad *pad = gst_element_get_static_pad (current_element, padname);
  gboolean ret = FALSE;

  if (!pad)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
      "Could not find the %s pad on the element", padname);
      return FALSE;
  }

  ghostpad = gst_ghost_pad_new (padname, pad);
  if (!ghostpad)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
      "Could not create a ghost pad for pad %s", padname);
    goto done;
  }

  if (!gst_pad_set_active (ghostpad, TRUE))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
      "Could not active ghostpad %s", padname);
    gst_object_unref (ghostpad);
    goto done;
  }

  if (!gst_element_add_pad (codec_bin, ghostpad))
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
      "Could not add ghostpad %s to the codec bin", padname);

  ret = TRUE;
 done:
  gst_object_unref (pad);

  return ret;
}

static gboolean
validate_src_pads (gpointer item, GValue *ret, gpointer user_data)
{
  GstPad *pad = item;
  GList *codecs = user_data;
  GstCaps *caps;
  GList *listitem = NULL;

  caps = gst_pad_get_caps (pad);

  if (gst_caps_is_empty (caps))
  {
    GST_WARNING_OBJECT (pad, "Caps on pad are empty");
    goto error;
  }

  for (listitem = codecs; listitem; listitem = g_list_next (listitem))
  {
    FsCodec *codec = listitem->data;
    GstCaps *tmpcaps = fs_codec_to_gst_caps (codec);
    GstCaps *intersect = gst_caps_intersect (tmpcaps, caps);
    gst_caps_unref (tmpcaps);

    if (!gst_caps_is_empty (intersect))
    {
      GST_LOG_OBJECT (pad, "Pad matches " FS_CODEC_FORMAT,
          FS_CODEC_ARGS (codec));
      gst_object_unref (pad);
      gst_caps_unref (caps);
      gst_caps_unref (intersect);
      return TRUE;
    }
    gst_caps_unref (intersect);
  }

 error:

  gst_object_unref (pad);
  gst_caps_unref (caps);
  g_value_set_boolean (ret, FALSE);
  return FALSE;
}

/*
 * Builds a codec bin in the specified direction for the specified codec
 * using the specified blueprint
 */

static GstElement *
_create_codec_bin (const CodecAssociation *ca, const FsCodec *codec,
    const gchar *name, gboolean is_send, GList *codecs,
    GError **error)
{
  GList *pipeline_factory = NULL;
  GList *walk = NULL;
  GstElement *codec_bin = NULL;
  GstElement *current_element = NULL;
  GstElement *previous_element = NULL;
  gchar *direction_str = (is_send == TRUE) ? "send" : "receive";
  gchar *profile = NULL;

  if (is_send)
    profile = ca->send_profile;
  else
    profile = ca->recv_profile;

  if (ca->blueprint)
  {
    if (is_send)
      pipeline_factory = ca->blueprint->send_pipeline_factory;
    else
      pipeline_factory = ca->blueprint->receive_pipeline_factory;
  }

  if (profile)
  {
    GError *tmperror = NULL;
    guint src_pad_count = 0, sink_pad_count = 0;

    codec_bin = gst_parse_bin_from_description (profile, TRUE, &tmperror);
    codec_bin = parse_bin_from_description_all_linked (profile,
        &src_pad_count, &sink_pad_count, &tmperror);

    if (codec_bin)
    {
      if (sink_pad_count != 1 || src_pad_count == 0)
      {
        GST_ERROR ("Invalid pad count (src:%u sink:%u)"
            " from codec profile: %s", src_pad_count, sink_pad_count, profile);
        gst_object_unref (codec_bin);
        codec_bin = NULL;
        goto try_factory;
      }

      if (codecs && src_pad_count > 1)
      {
        GstIterator *iter;
        GValue valid = {0};
        GstIteratorResult res;

        iter = gst_element_iterate_src_pads (codec_bin);
        g_value_init (&valid, G_TYPE_BOOLEAN);
        g_value_set_boolean (&valid, TRUE);
        res = gst_iterator_fold (iter, validate_src_pads, &valid,
            codecs);
        gst_iterator_free (iter);

        if (!g_value_get_boolean (&valid) || res == GST_ITERATOR_ERROR)
        {
          gst_object_unref (codec_bin);
          codec_bin = NULL;
          goto try_factory;
        }
      }

      GST_DEBUG ("creating %s codec bin for id %d, profile: %s",
          direction_str, codec->id, profile);
      gst_element_set_name (codec_bin, name);
      return codec_bin;
    }
    else if (!pipeline_factory)
    {
      g_propagate_error (error, tmperror);
      return NULL;
    }
  }

 try_factory:

  if (!pipeline_factory)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_UNKNOWN_CODEC,
        "The %s codec %s does not have a pipeline,"
        " its probably a special codec",
        fs_media_type_to_string (codec->media_type),
        codec->encoding_name);
    return NULL;
  }


  GST_DEBUG ("creating %s codec bin for id %d, pipeline_factory %p",
    direction_str, codec->id, pipeline_factory);
  codec_bin = gst_bin_new (name);

  for (walk = g_list_first (pipeline_factory); walk; walk = g_list_next (walk))
  {
    if (g_list_next (g_list_first (walk->data)))
    {
      /* We have to check some kind of configuration to see if we have a
         favorite */
      current_element = gst_element_factory_make ("fsselector", NULL);

      if (!current_element)
      {
        g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
          "Could not create fsselector element");
        goto error;
      }

      g_object_set (current_element, "factories", walk->data, NULL);
    } else {
      current_element =
        gst_element_factory_create (
            GST_ELEMENT_FACTORY (g_list_first (walk->data)->data), NULL);
      if (!current_element)
      {
        g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
          "Could not create element for pt %d", codec->id);
        goto error;
      }
    }

    if (!gst_bin_add (GST_BIN (codec_bin), current_element))
    {
      g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not add new element to %s codec_bin for pt %d",
        direction_str, codec->id);
      goto error;
    }

    /* queue delay to 0 on all depayloaders until I remove that property
     * all-together */

    if (_g_object_has_property (G_OBJECT (current_element), "queue-delay"))
      g_object_set (G_OBJECT (current_element), "queue-delay", 0, NULL);

    if (_g_object_has_property (G_OBJECT (current_element), "pt"))
      g_object_set (current_element, "pt", codec->id,
        NULL);

    /* Lets create the ghost pads on the codec bin */

    if (g_list_previous (walk) == NULL)
      /* if its the first element of the codec bin */
      if (!_create_ghost_pad (current_element,
              is_send ? "src" : "sink", codec_bin, error))
        goto error;

    if (g_list_next (walk) == NULL)
      /* if its the last element of the codec bin */
      if (!_create_ghost_pad (current_element,
              is_send ? "sink" : "src" , codec_bin, error))
        goto error;


    /* let's link them together using the specified media_caps if any
     * this will ensure that multi-codec encoders/decoders will select the
     * appropriate codec based on caps negotiation */
    if (previous_element)
    {
      GstPad *sinkpad;
      GstPad *srcpad;
      GstPadLinkReturn ret;

      if (is_send)
        sinkpad = gst_element_get_static_pad (previous_element, "sink");
      else
        sinkpad = gst_element_get_static_pad (current_element, "sink");

      if (!sinkpad)
      {
        g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
          "Could not get the sink pad one of the elements in the %s codec bin"
          " for pt %d", direction_str, codec->id);
        goto error;
      }


      if (is_send)
        srcpad = gst_element_get_static_pad (current_element, "src");
      else
        srcpad = gst_element_get_static_pad (previous_element, "src");

      if (!srcpad)
      {
        g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
          "Could not get the src pad one of the elements in the %s codec bin"
          " for pt %d", direction_str, codec->id);
        gst_object_unref (sinkpad);
        goto error;
      }

      ret = gst_pad_link (srcpad, sinkpad);

      gst_object_unref (srcpad);
      gst_object_unref (sinkpad);

      if (GST_PAD_LINK_FAILED (ret))
      {
        g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
          "Could not link element inside the %s codec bin for pt %d",
          direction_str, codec->id);
        goto error;
      }
    }

    previous_element = current_element;
  }

  return codec_bin;

 error:
  gst_object_unref (codec_bin);
  return NULL;
}

/**
 * fs_rtp_session_get_recv_codec_locked:
 * @session: a #FsRtpSession
 * @pt: The payload type to find the codec for
 * @stream: an optional #FsRtpStream for which this data is received
 *
 * This function returns the #CodecAssociation that will be used to receive
 * data on a specific payload type, optionally from a specific stream.
 *
 * MUST be called with the FsRtpSession lock held
 *
 * Returns: a #CodecAssociation, the caller doesn't own it
 */

static CodecAssociation *
fs_rtp_session_get_recv_codec_locked (FsRtpSession *session,
    guint pt,
    FsRtpStream *stream,
    GError **error)
{
  FsCodec *recv_codec = NULL;
  CodecAssociation *ca = NULL;
  GList *item = NULL;

  if (!session->priv->codec_associations)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_INTERNAL,
        "No codecs yet");
    return NULL;
  }

  ca = lookup_codec_association_by_pt (session->priv->codec_associations, pt);

  if (!ca)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_UNKNOWN_CODEC,
      "There is no negotiated codec with pt %d", pt);
    return NULL;
  }

  if (stream)
  {
    GList *stream_codecs = NULL;

    g_object_get (stream, "negotiated-codecs", &stream_codecs, NULL);

    for(item = stream_codecs; item; item = g_list_next (item))
    {
      recv_codec = item->data;
      if (recv_codec->id == pt)
        break;
    }

    if (item)
    {
      GST_DEBUG ("Receiving on stream codec " FS_CODEC_FORMAT,
          FS_CODEC_ARGS (recv_codec));
      stream_codecs = g_list_remove_all (stream_codecs, recv_codec);
    }
    else
    {
      GST_DEBUG ("Have stream, but it does not have negotiatied codec");
      recv_codec = NULL;
    }

    fs_codec_list_destroy (stream_codecs);
  }

  if (!recv_codec)
  {
    recv_codec = codec_copy_without_config (ca->codec);
    GST_DEBUG ("Receiving on session codec " FS_CODEC_FORMAT,
        FS_CODEC_ARGS (recv_codec));
  }

  return ca;
}

/**
 * fs_rtp_session_substream_set_codec_bin:
 * @session: a #FsRtpSession
 * @substream: a #FsRtpSubStream
 * @ssrc: the ssrc of the substream
 * @pt: the payload type of the substream
 * @error: location of a #GError, or NULL if no error occured
 *
 * Sets a codecbin on a substream according to the currently negotiated codecs
 *
 * Returns: %TRUE on success, %FALSE on error
 */

static gboolean
fs_rtp_session_substream_set_codec_bin (FsRtpSession *session,
    FsRtpSubStream *substream,
    FsRtpStream *stream,
    guint32 ssrc,
    guint pt,
    GError **error)
{
  gboolean ret = FALSE;
  GstElement *codecbin = NULL;
  gchar *name;
  FsCodec *current_codec = NULL;
  CodecAssociation *ca = NULL;

  FS_RTP_SESSION_LOCK (session);

  ca = fs_rtp_session_get_recv_codec_locked (session, pt, stream, error);

  if (!ca)
    goto out;

  g_object_get (substream,
      "codec", &current_codec,
      NULL);

  if (fs_codec_are_equal (ca->codec, current_codec))
  {
    ret = TRUE;
    goto out;
  }

  name = g_strdup_printf ("recv_%d_%u_%d", session->id, ssrc, pt);
  codecbin = _create_codec_bin (ca, ca->codec, name, FALSE, NULL, error);
  g_free (name);

  if (!codecbin)
    goto out;

  ret = fs_rtp_sub_stream_set_codecbin (substream, ca->codec, codecbin, error);

 out:

  fs_codec_destroy (current_codec);

  FS_RTP_SESSION_UNLOCK (session);

  return ret;
}


/**
 * fs_rtp_session_select_send_codec_locked:
 * @session: the #FsRtpSession
 *
 * This function selects the codec to send using either the user preference
 * or the remote preference (from the negotiation).
 *
 * You MUST own the FsRtpSession mutex to call this function
 *
 * Returns: a #CodecAssociation, the caller doesn't own it
 */

static CodecAssociation *
fs_rtp_session_select_send_codec_locked (FsRtpSession *session,
    GError **error)
{
  CodecAssociation *ca = NULL;
  GList *ca_e = NULL;

  if (!session->priv->codec_associations)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_INTERNAL,
        "Tried to call fs_rtp_session_select_send_codec_bin before the codec"
        " negotiation has taken place");
    return NULL;
  }

  if (session->priv->requested_send_codec)
  {
    ca = lookup_codec_association_by_codec_without_config (
        session->priv->codec_associations,
        session->priv->requested_send_codec);
    if (ca)
    {
      if (!codec_association_is_valid_for_sending (ca))
      {
        fs_codec_destroy (session->priv->requested_send_codec);
        session->priv->requested_send_codec = NULL;
        ca = NULL;
        GST_DEBUG_OBJECT (session->priv->conference,
            "The current requested codec is not a valid main send codec,"
            " ignoring");
      }
      else
      {
        /* We have a valid codec, lets use it */
        goto out;
      }
    }
    else
    {
      /* The requested send codec no longer exists */
      fs_codec_destroy (session->priv->requested_send_codec);
      session->priv->requested_send_codec = NULL;

      GST_WARNING_OBJECT (session->priv->conference,
          "The current requested codec no longer exists, resetting");
    }
  }

  /*
   * We don't have a requested codec, or it was not valid, lets use the first
   * codec from the list
   */
  for (ca_e = g_list_first (session->priv->codec_associations);
       ca_e;
       ca_e = g_list_next (ca_e))
  {
    if (codec_association_is_valid_for_sending (ca_e->data))
    {
      ca = ca_e->data;
      break;
    }
  }

  if (ca == NULL)
    g_set_error (error, FS_ERROR, FS_ERROR_NEGOTIATION_FAILED,
        "Could not get a valid send codec");

out:

  return ca;
}

struct link_data {
  FsRtpSession *session;
  GstCaps *caps;
  FsCodec *codec;

  GList *all_codecs;

  GError **error;
};

/*
 * This is a  GstIteratorFoldFunction
 * It returns FALSE when it wants to stop the iteration
 */

static gboolean
link_main_pad (gpointer item, GValue *ret, gpointer user_data)
{
  GstPad *pad = item;
  struct link_data *data = user_data;
  GstCaps *caps, *intersect;
  GstPad *other_pad;

  caps = gst_pad_get_caps (pad);
  intersect = gst_caps_intersect (caps, data->caps);
  gst_caps_unref (caps);

  if (gst_caps_is_empty (intersect))
  {
    gst_caps_unref (intersect);
    gst_object_unref (pad);
    return TRUE;
  }
  gst_caps_unref (intersect);

  other_pad = gst_element_get_static_pad (data->session->priv->send_capsfilter,
      "sink");

  if (!other_pad)
  {
    g_set_error (data->error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not get the sink pad of the send capsfilter");
    goto error;
  }

  if (GST_PAD_LINK_SUCCESSFUL(gst_pad_link (pad, other_pad)))
    g_value_set_boolean (ret, TRUE);
  else
    g_set_error (data->error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not link the send codec bin for pt %d to the send capsfilter",
        data->codec->id);

 error:

  gst_object_unref (other_pad);
  gst_object_unref (pad);

  return FALSE;
}


/*
 * This is a  GstIteratorFoldFunction
 * It returns FALSE when it wants to stop the iteration
 */

static gboolean
link_other_pads (gpointer item, GValue *ret, gpointer user_data)
{
  GstPad *pad = item;
  struct link_data *data = user_data;
  GstCaps *caps;
  GstCaps *filter_caps = NULL;
  GList *listitem;
  GstElement *capsfilter;
  GstPad *otherpad;

  if (gst_pad_is_linked (pad))
  {
    gst_object_unref (pad);
    return TRUE;
  }

  caps = gst_pad_get_caps (pad);

  if (gst_caps_is_empty (caps))
  {
    GST_WARNING_OBJECT (pad, "Caps on pad are empty");
    return TRUE;
  }

  for (listitem = data->all_codecs; listitem; listitem = g_list_next (listitem))
  {
    FsCodec *codec = listitem->data;
    GstCaps *intersect;

    filter_caps = fs_codec_to_gst_caps (codec);
    intersect = gst_caps_intersect (filter_caps, caps);

    if (!gst_caps_is_empty (intersect))
    {
      GST_LOG_OBJECT (pad, "Pad matches " FS_CODEC_FORMAT,
          FS_CODEC_ARGS (codec));
      gst_caps_unref (intersect);
      break;
    }
    gst_caps_unref (filter_caps);
    gst_caps_unref (intersect);
  }

  gst_caps_unref (caps);

  if (!listitem)
  {
    g_set_error (data->error, FS_ERROR, FS_ERROR_INTERNAL,
        "Could not find codec that matches the src pad");
    g_value_set_boolean (ret, FALSE);
    gst_object_unref (pad);
    return FALSE;
  }

  capsfilter = gst_element_factory_make ("capsfilter", NULL);
  g_object_set (capsfilter, "caps", filter_caps, NULL);
  gst_caps_unref (filter_caps);

  if (!gst_bin_add (GST_BIN (data->session->priv->conference), capsfilter))
  {
    g_set_error (data->error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not add send capsfilter to the conference");
    gst_object_unref (capsfilter);
    goto error;
  }

  data->session->priv->extra_send_capsfilters =
    g_list_append (data->session->priv->extra_send_capsfilters,
        capsfilter);

  otherpad = gst_element_get_static_pad (capsfilter, "sink");

  if (!otherpad)
  {
    g_set_error (data->error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not get sink pad on capsfilter");
    goto error;
  }

  if (GST_PAD_LINK_FAILED (gst_pad_link (pad, otherpad)))
  {
    g_set_error (data->error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not get sink pad on capsfilter");
    gst_object_unref (otherpad);
    goto error;
  }
  gst_object_unref (otherpad);
  gst_object_unref (pad);
  pad = NULL;

  if (!gst_element_link (capsfilter, data->session->priv->rtpmuxer))
  {
    g_set_error (data->error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not an extra capsfilter to the muxer");
    g_value_set_boolean (ret, FALSE);
    return FALSE;
  }

  return TRUE;

 error:

  g_value_set_boolean (ret, FALSE);
  gst_bin_remove (GST_BIN (data->session->priv->conference), capsfilter);
  data->session->priv->extra_send_capsfilters =
    g_list_remove (data->session->priv->extra_send_capsfilters,
        capsfilter);
  gst_object_unref (pad);

  return FALSE;
}

/**
 * fs_rtp_session_add_send_codec_bin:
 * @session: a #FsRtpSession
 * @codec: a #FsCodec
 * @ca: the #CodecAssociation to use
 *
 * This function creates, adds and links a codec bin for the current send remote
 * codec
 *
 * Needs the Session lock to be held.
 *
 * Returns: The new codec bin (or NULL if there is an error)
 */

static GstElement *
fs_rtp_session_add_send_codec_bin (FsRtpSession *session,
    const FsCodec *codec,
    const CodecAssociation *ca,
    GError **error)
{
  GstElement *codecbin = NULL;
  gchar *name;
  GstCaps *sendcaps;
  GList *codecs;
  GstIterator *iter;
  GValue link_rv = {0};
  struct link_data data;

  GST_DEBUG ("Trying to add send codecbin for " FS_CODEC_FORMAT,
      FS_CODEC_ARGS (codec));

  name = g_strdup_printf ("send_%d_%d", session->id, codec->id);
  codecs = codec_associations_to_codecs (session->priv->codec_associations,
      FALSE);
  codecbin = _create_codec_bin (ca, codec, name, TRUE, codecs, error);
  g_free (name);

  if (!codecbin)
  {
    fs_codec_list_destroy (codecs);
    return NULL;
  }

  if (!gst_bin_add (GST_BIN (session->priv->conference), codecbin))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not add the send codec bin for pt %d to the pipeline",
        codec->id);
    gst_object_unref (codecbin);
    fs_codec_list_destroy (codecs);
    return NULL;
  }

  if (!gst_element_link_pads (session->priv->media_sink_valve, "src",
          codecbin, "sink"))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not get the valve sink for the send codec bin");
    gst_bin_remove (GST_BIN (session->priv->conference), (codecbin));
    fs_codec_list_destroy (codecs);
    return NULL;
  }

  sendcaps = fs_codec_to_gst_caps (codec);

  g_object_set (G_OBJECT (session->priv->send_capsfilter),
      "caps", sendcaps, NULL);

  iter = gst_element_iterate_src_pads (codecbin);

  g_value_init (&link_rv, G_TYPE_BOOLEAN);
  g_value_set_boolean (&link_rv, FALSE);

  data.session = session;
  data.caps = sendcaps;
  data.error = NULL;
  data.all_codecs = codecs;

  if (gst_iterator_fold (iter, link_main_pad, &link_rv, &data) ==
      GST_ITERATOR_ERROR)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not iterate over the src pads of the send codec bin to link"
        " the main pad for pt %d", codec->id);
    goto error;
  }

  gst_caps_unref (sendcaps);

  if (!g_value_get_boolean (&link_rv))
  {
    gst_iterator_free (iter);
    goto error;
  }

  gst_iterator_resync (iter);

  if (gst_iterator_fold (iter, link_other_pads, &link_rv, &data) ==
      GST_ITERATOR_ERROR)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not iterate over the src pads of the send codec bin to link"
        " the main pad for pt %d", codec->id);
    gst_iterator_free (iter);
    goto error;
  }

  gst_iterator_free (iter);

  if (!g_value_get_boolean (&link_rv))
    goto error;


  if (!gst_element_sync_state_with_parent (codecbin))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not sync the state of the codec bin for pt %d with the state"
        " of the conference", codec->id);
    goto error;
  }

  g_object_set (session->priv->media_sink_valve, "drop", FALSE, NULL);

  session->priv->send_codecbin = codecbin;
  session->priv->current_send_codec = fs_codec_copy (codec);

  fs_rtp_session_send_codec_changed (session);

  fs_codec_list_destroy (codecs);

  return codecbin;

 error:
  gst_element_set_state (codecbin, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (session->priv->conference), codecbin);
  fs_codec_list_destroy (codecs);
  return NULL;
}

/**
 * fs_rtp_session_send_codec_changed:
 * @self: The #FsRtpSession that changed its codec
 *
 * Call this function when the value of the #FsSession:current-send-codec
 * changes
 */

static void
fs_rtp_session_send_codec_changed (FsRtpSession *self)
{
  FsCodec *codec = NULL;

  FS_RTP_SESSION_LOCK (self);
  codec = fs_codec_copy (self->priv->current_send_codec);
  FS_RTP_SESSION_UNLOCK (self);

  g_object_notify (G_OBJECT (self), "current-send-codec");

  gst_element_post_message (GST_ELEMENT (self->priv->conference),
      gst_message_new_element (GST_OBJECT (self->priv->conference),
          gst_structure_new ("farsight-send-codec-changed",
              "session", FS_TYPE_SESSION, self,
              "codec", FS_TYPE_CODEC, codec,
              NULL)));

  fs_codec_destroy (codec);
}

static void
pad_block_do_nothing (GstPad *pad, gboolean blocked, gpointer user_data)
{
}

/**
 * _send_src_pad_blocked_callback:
 *
 * This is the callback for the pad blocking on the media src pad
 * It is used to replace the codec bin when the send codec has been changed.
 */

static void
_send_src_pad_blocked_callback (GstPad *pad, gboolean blocked,
    gpointer user_data)
{
  FsRtpSession *self = FS_RTP_SESSION (user_data);
  CodecAssociation *ca = NULL;
  FsCodec *codec_without_config = NULL;
  GError *error = NULL;

  FS_RTP_SESSION_LOCK (self);
  ca = fs_rtp_session_select_send_codec_locked (self, &error);

  if (!ca)
  {
    fs_session_emit_error (FS_SESSION (self), error->code,
        "Could not select a new send codec", error->message);
    goto done;
  }

  codec_without_config = codec_copy_without_config (ca->codec);

  g_clear_error (&error);

  if (fs_codec_are_equal (codec_without_config, self->priv->current_send_codec))
    goto done;


  gst_element_set_locked_state (self->priv->send_codecbin, TRUE);
  if (gst_element_set_state (self->priv->send_codecbin, GST_STATE_NULL) !=
      GST_STATE_CHANGE_SUCCESS)
  {
    fs_session_emit_error (FS_SESSION (self), FS_ERROR_INTERNAL,
        "Could not stop the codec bin",
        "Setting the codec bin to NULL did not succeed" );
    goto done;
  }

  gst_bin_remove (GST_BIN (self->priv->conference), self->priv->send_codecbin);
  self->priv->send_codecbin = NULL;

  fs_codec_destroy (self->priv->current_send_codec);
  self->priv->current_send_codec = NULL;

  while (self->priv->extra_send_capsfilters)
  {
    GstElement *cf = self->priv->extra_send_capsfilters->data;
    GstPad *ourpad = gst_element_get_static_pad (cf, "src");
    GstPad *pad = NULL;

    if (ourpad)
    {
      pad = gst_pad_get_peer (ourpad);
      if (pad)
      {
        gst_pad_set_active (pad, FALSE);
        gst_element_release_request_pad (self->priv->rtpmuxer, pad);
        gst_object_unref (pad);
      }
      gst_object_unref (ourpad);
    }

    gst_element_set_state (cf, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (self->priv->conference), cf);

    self->priv->extra_send_capsfilters = g_list_delete_link (
        self->priv->extra_send_capsfilters,
        self->priv->extra_send_capsfilters);
  }


  self->priv->extra_sources = fs_rtp_special_sources_remove (
      self->priv->extra_sources,
      self->priv->codec_associations, codec_without_config,
      GST_ELEMENT (self->priv->conference),
      self->priv->rtpmuxer);
  if (error)
  {
    fs_session_emit_error (FS_SESSION (self), FS_ERROR_INTERNAL,
        "Could not remove unused special sources",
        "Could not remove unused special sources");
    goto done;
  }

  /*
   * Lets reset the clock-rate (because rtpmuxer saves it.. )
   */
  g_object_set (self->priv->rtpmuxer, "clock-rate", 0, NULL);


  if (!fs_rtp_session_add_send_codec_bin (self, codec_without_config,
          ca, &error))
  {
    fs_session_emit_error (FS_SESSION (self), error->code,
        "Could not build a new send codec bin", error->message);
  }


  self->priv->extra_sources = fs_rtp_special_sources_create (
      self->priv->extra_sources,
      self->priv->codec_associations, codec_without_config,
      GST_ELEMENT (self->priv->conference),
      self->priv->rtpmuxer);
  if (error)
  {
    fs_session_emit_error (FS_SESSION (self), FS_ERROR_INTERNAL,
        "Could not create special sources",
        "Could not create special sources");
    goto done;
  }


 done:
  g_clear_error (&error);

  /* If we have a codec bin, the required/preferred caps may have changed,
   * in this case, we need to drop the current buffer and wait for a buffer
   * with the right caps to come in. Only then can we drop the pad block
   */

  fs_codec_destroy (codec_without_config);

  gst_pad_set_blocked_async (pad, FALSE, pad_block_do_nothing, NULL);

  FS_RTP_SESSION_UNLOCK (self);
}

/**
 * fs_rtp_session_verify_send_codec_bin_locked:
 *
 * Verify that the current send codec is still valid and if it is not
 * do whats required to have the right one be used.
 *
 * Must be called with the FsRtpSession lock taken
 *
 * Returns: TRUE if it succeeds, FALSE on an error
 */

static gboolean
fs_rtp_session_verify_send_codec_bin_locked (FsRtpSession *self, GError **error)
{
  CodecAssociation *ca = NULL;
  FsCodec *codec_without_config = NULL;

  ca = fs_rtp_session_select_send_codec_locked (self, error);
  if (!ca)
    return FALSE;

  codec_without_config = codec_copy_without_config (ca->codec);

  if (self->priv->current_send_codec)
  {
    if (fs_codec_are_equal (codec_without_config,
            self->priv->current_send_codec))
      goto done;

    /* If we have to change an already made pipeline,
     * we have to make sure that is it blocked
     */


    gst_pad_set_blocked_async (self->priv->send_tee_media_pad, TRUE,
        _send_src_pad_blocked_callback, self);
  }
  else
  {
    /* The codec does exist yet, lets just create it */

    if (!fs_rtp_session_add_send_codec_bin (self, codec_without_config,
            ca, error))
      /* We have an error !! */
      goto error;

    self->priv->extra_sources = fs_rtp_special_sources_create (
        self->priv->extra_sources,
        self->priv->codec_associations, codec_without_config,
        GST_ELEMENT (self->priv->conference),
        self->priv->rtpmuxer);
  }

 done:

  fs_codec_destroy (codec_without_config);
  return TRUE;

 error:
  fs_codec_destroy (codec_without_config);
  return FALSE;
}

/*
 * This callback is called when the pad of a substream has been locked because
 * the codec needs to be changed. It will see if there is a new codec to be set,
 * if there is, it will change the codec bin.
 */

static void
_substream_blocked (FsRtpSubStream *substream, FsRtpStream *stream,
    FsRtpSession *session)
{
  GError *error = NULL;
  gint pt;
  guint32 ssrc;

  g_object_get (substream,
      "pt", &pt,
      "ssrc", &ssrc,
      NULL);

  GST_DEBUG ("Substream blocked for codec change (session:%d SSRC:%x pt:%d)",
      session->id, ssrc, pt);

  if (!fs_rtp_session_substream_set_codec_bin (session, substream, stream,
          ssrc, pt, &error))
  {
    gchar *str = g_strdup_printf ("Could not add the new recv codec bin for"
        " ssrc %u and payload type %d to the state NULL", ssrc, pt);

    if (stream)
      fs_stream_emit_error (FS_STREAM (stream), FS_ERROR_CONSTRUCTION,
          "Could not add the new recv codec bin", error->message);
    else
      fs_session_emit_error (FS_SESSION (session), FS_ERROR_CONSTRUCTION,
          "Could not add the new recv codec bin", error->message);
    g_free (str);
    goto done;
  }

 done:

  g_clear_error (&error);
}

static void
fs_rtp_session_associate_free_substreams (FsRtpSession *session,
    FsRtpStream *stream, guint32 ssrc)
{
  gboolean added = FALSE;

  for (;;)
  {
    FsRtpSubStream *substream = NULL;
    GList *item = NULL;
    GError *error = NULL;

    FS_RTP_SESSION_LOCK (session);

    for (item = g_list_first (session->priv->free_substreams);
         item;
         item = g_list_next (item))
    {
      FsRtpSubStream *localsubstream = item->data;
      guint32 localssrc;

      g_object_get (localsubstream, "ssrc", &localssrc, NULL);
      GST_LOG ("Have substream with ssrc %x, looking for %x", localssrc, ssrc);
      if (ssrc == localssrc)
      {
        substream = localsubstream;
        session->priv->free_substreams = g_list_delete_link (
            session->priv->free_substreams, item);
        break;
      }
    }
    FS_RTP_SESSION_UNLOCK (session);

    if (!substream)
      break;

    added = TRUE;

    while (
        g_signal_handlers_disconnect_by_func (substream, "error", session) > 0);
    while (
        g_signal_handlers_disconnect_by_func (substream, "no-rtcp-timedout",
            session) > 0);

    if (fs_rtp_stream_add_substream (stream, substream, &error))
    {
      fs_rtp_sub_stream_verify_codec_locked (substream);
      GST_DEBUG ("Associated SSRC %x in session %u", ssrc, session->id);
    }
    else
    {
      GST_ERROR ("Could not associate a substream with its stream",
          error->message);
      fs_session_emit_error (FS_SESSION (session), error->code,
          "Could not associate a substream with its stream",
          error->message);
    }
    g_clear_error (&error);
  }

  if (added == FALSE)
    GST_DEBUG ("No free substream with SSRC %x in session %u",
        ssrc, session->id);
}

void
fs_rtp_session_associate_ssrc_cname (FsRtpSession *session,
    guint32 ssrc,
    const gchar *cname)
{
  FsRtpStream *stream = NULL;
  GList *item = NULL;

  FS_RTP_SESSION_LOCK (session);
  for (item = g_list_first (session->priv->streams);
       item;
       item = g_list_next (item))
  {
    FsRtpStream *localstream = item->data;
    FsRtpParticipant *participant = NULL;
    gchar *localcname = NULL;

    g_object_get (localstream, "participant", &participant, NULL);
    g_object_get (participant, "cname", &localcname, NULL);
    g_object_unref (participant);

    g_assert (localcname);

    if (!strcmp (localcname, cname))
    {
      stream = localstream;
      g_free (localcname);
      break;
    }
    g_free (localcname);
  }

  if (!stream)
  {
    gchar *str = g_strdup_printf ("There is no particpant with cname %s for"
        " ssrc %u", cname, ssrc);
    fs_session_emit_error (FS_SESSION (session), FS_ERROR_UNKNOWN_CNAME,
        str, str);
    g_free (str);
    FS_RTP_SESSION_UNLOCK (session);
    return;
  }

  g_hash_table_insert (session->priv->ssrc_streams, GUINT_TO_POINTER (ssrc),
      stream);

  FS_RTP_SESSION_UNLOCK (session);

  fs_rtp_session_associate_free_substreams (session, stream, ssrc);
}

static void
_substream_no_rtcp_timedout_cb (FsRtpSubStream *substream,
    FsRtpSession *session)
{
  GError *error = NULL;

  FS_RTP_SESSION_LOCK (session);

  if (g_list_length (session->priv->streams) != 1)
  {
    guint ssrc, pt;
    gint timeout;
    g_object_get (substream,
        "ssrc", &ssrc,
        "pt", &pt,
        "no-rtcp-timeout", &timeout,
        NULL);
    GST_WARNING ("The substream for SSRC %x and pt %u did not receive RTCP"
        " for %d milliseconds, but we have more than one stream so we can"
        " not associate it.", ssrc, pt, timeout);
    goto done;
  }

  if (!g_list_find (session->priv->free_substreams, substream))
  {
    GST_WARNING ("Could not find substream %p in the list of free substreams",
        substream);
    goto done;
  }

  session->priv->free_substreams =
    g_list_remove (session->priv->free_substreams,
        substream);

  while (
      g_signal_handlers_disconnect_by_func (substream, "error", session) > 0);
  while (
      g_signal_handlers_disconnect_by_func (substream, "no-rtcp-timedout", session) > 0);

  if (fs_rtp_stream_add_substream (
          g_list_first (session->priv->streams)->data,
          substream, &error))
    fs_rtp_sub_stream_verify_codec_locked (substream);
  else
    fs_session_emit_error (FS_SESSION (session),
        error ? error->code : FS_ERROR_INTERNAL,
        "Could not link the substream to a stream",
        error ? error->message : "No error message");
  g_clear_error (&error);

 done:

  FS_RTP_SESSION_UNLOCK (session);
}

/**
 * fs_rtp_session_bye_ssrc:
 * @session: a #FsRtpSession
 * @ssrc: The ssrc
 *
 * This function is called when a RTCP BYE is received
 */
void
fs_rtp_session_bye_ssrc (FsRtpSession *session,
    guint32 ssrc)
{

  /* First remove it from the known SSRCs */

  FS_RTP_SESSION_LOCK (session);
  g_hash_table_remove (session->priv->ssrc_streams, GUINT_TO_POINTER (ssrc));
  FS_RTP_SESSION_UNLOCK (session);

  /*
   * TODO:
   *
   * Remove running streams with that SSRC .. lets also check if they
   * come from the right ip/port/etc ??
   */
}


static gboolean
gather_caps_parameters (CodecAssociation *ca, GstCaps *caps)
{
  GstStructure *s = NULL;
  int i;
  gboolean old_need_config = FALSE;

  s = gst_caps_get_structure (caps, 0);

  for (i = 0; i < gst_structure_n_fields (s); i++)
  {
    const gchar *name = gst_structure_nth_field_name (s, i);
    if (name)
    {
      const gchar *value = gst_structure_get_string (s, name);
      if (value)
      {
        if (codec_has_config_data_named (ca->codec, name))
        {
          GList *item = NULL;

          for (item = ca->codec->optional_params; item;
               item = g_list_next (item))
          {
            FsCodecParameter *param = item->data;
            if (!g_ascii_strcasecmp (param->name, name))
            {
              if (!g_ascii_strcasecmp (param->value, value))
                break;

              GST_DEBUG ("%d/%s: replacing param %s=%s with %s",
                  ca->codec->id, ca->codec->encoding_name, name, param->value, value);

              /* replace the value if its different */
              fs_codec_remove_optional_parameter (ca->codec, param);
              fs_codec_add_optional_parameter (ca->codec, name, value);
              break;
            }
          }

          /* Add it if it wasn't there */
          if (item == NULL)
          {
            GST_DEBUG ("%d/%s: adding param %s=%s",
                ca->codec->id, ca->codec->encoding_name, name, value);

            fs_codec_add_optional_parameter (ca->codec, name, value);
          }
        }
      }
    }
  }

  old_need_config = ca->need_config;
  ca->need_config = FALSE;

  return old_need_config;
}

static void
_send_caps_changed (GstPad *pad, GParamSpec *pspec, FsRtpSession *session)
{
  GstCaps *caps = NULL;
  CodecAssociation *ca = NULL;

  g_object_get (pad, "caps", &caps, NULL);

  if (!caps)
    return;

  g_return_if_fail (GST_CAPS_IS_SIMPLE(caps));

  FS_RTP_SESSION_LOCK (session);

  if (!session->priv->current_send_codec)
    goto out;

  ca = lookup_codec_association_by_codec (session->priv->codec_associations,
      session->priv->current_send_codec);

  if (!ca)
    goto out;

  /*
   * Emit farsight-codecs-changed if the sending thread finds the config
   * for the last codec that needed it
   */
  if (gather_caps_parameters (ca, caps))
  {
    GList *item = NULL;

    for (item = g_list_first (session->priv->codec_associations);
         item;
         item = g_list_next (item))
    {
      ca = item->data;
      if (ca->need_config)
        break;
    }
    if (!item)
    {
      g_object_notify (G_OBJECT (session), "codecs-ready");
      g_object_notify (G_OBJECT (session), "codecs");
      gst_element_post_message (GST_ELEMENT (session->priv->conference),
          gst_message_new_element (GST_OBJECT (session->priv->conference),
              gst_structure_new ("farsight-codecs-changed",
                  "session", FS_TYPE_SESSION, session,
                  NULL)));

      goto out;
    }

  }

 out:

  FS_RTP_SESSION_UNLOCK (session);

  gst_caps_unref (caps);
}

static void
_discovery_caps_changed (GstPad *pad, GParamSpec *pspec, FsRtpSession *session)
{
  CodecAssociation *ca = NULL;
  GstCaps *caps = NULL;

  g_object_get (pad, "caps", &caps, NULL);

  if (!caps)
    return;

  g_return_if_fail (GST_CAPS_IS_SIMPLE(caps));

  FS_RTP_SESSION_LOCK (session);

  if (!session->priv->discovery_codec)
  {
    fs_session_emit_error (FS_SESSION (session), FS_ERROR_INTERNAL,
        "Internal error while discovering codecs configurations",
        "Got notify::caps signal on the discovery codecs whith no codecs"
        " being discovered");
    goto out;
  }

  ca = lookup_codec_association_by_codec (session->priv->codec_associations,
      session->priv->discovery_codec);

  fs_codec_destroy (session->priv->discovery_codec);
  session->priv->discovery_codec = NULL;

  if (ca && ca->need_config)
  {
    gather_caps_parameters (ca, caps);
    session->priv->discovery_codec = fs_codec_copy (ca->codec);
  }

 out:

  FS_RTP_SESSION_UNLOCK (session);

  gst_caps_unref (caps);

  gst_pad_set_blocked_async (session->priv->send_tee_discovery_pad, TRUE,
      _send_sink_pad_blocked_callback, session);

}

/**
 * fs_rtp_session_get_codec_params:
 * @session: a #FsRtpSession
 * @ca: the #CodecAssociaton to get params for
 *
 * Gets the parameters for the specified #CodecAssociation
 *
 * Returns: %TRUE on success, %FALSE on error
 */

static gboolean
fs_rtp_session_get_codec_params (FsRtpSession *session, CodecAssociation *ca,
    GError **error)
{
  GstPad *pad = NULL;
  gchar *tmp;
  GstCaps *caps;

  FS_RTP_SESSION_LOCK (session);

  GST_LOG ("Gathering params for codec " FS_CODEC_FORMAT,
      FS_CODEC_ARGS (ca->codec));

  if (session->priv->discovery_codecbin)
  {
    gst_element_set_locked_state (session->priv->discovery_codecbin, TRUE);
    gst_element_set_state (session->priv->discovery_codecbin, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (session->priv->conference),
        session->priv->discovery_codecbin);
    session->priv->discovery_codecbin = NULL;
  }

  fs_codec_destroy (session->priv->discovery_codec);
  session->priv->discovery_codec = NULL;

  /* They must both exist or neither exists, anything else is wrong */
  if ((session->priv->discovery_fakesink == NULL ||
          session->priv->discovery_capsfilter == NULL) &&
      session->priv->discovery_fakesink != session->priv->discovery_capsfilter)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_INTERNAL,
        "Capsfilter and fakesink not synchronized, fakesink:%p capsfilter:%p",
        session->priv->discovery_fakesink, session->priv->discovery_capsfilter);
    goto error;
  }

  if (session->priv->discovery_fakesink == NULL &&
      session->priv->discovery_capsfilter == NULL)
  {

    tmp = g_strdup_printf ("discovery_fakesink_%d", session->id);
    session->priv->discovery_fakesink =
      gst_element_factory_make ("fakesink", tmp);
    g_free (tmp);
    if (!session->priv->discovery_fakesink)
    {
      g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
          "Could not make fakesink element");
      goto error;
    }
    g_object_set (session->priv->discovery_fakesink,
        "sync", FALSE,
        "async", FALSE,
        NULL);

    if (!gst_bin_add (GST_BIN (session->priv->conference),
            session->priv->discovery_fakesink))
    {
      g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
          "Could not add the discovery fakesink to the bin");
      goto error;
    }

    if (!gst_element_sync_state_with_parent (session->priv->discovery_fakesink))
    {
      g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
          "Could not sync the discovery fakesink's state with its parent");
      goto error;
    }

    tmp = g_strdup_printf ("discovery_capsfilter_%d", session->id);
    session->priv->discovery_capsfilter =
      gst_element_factory_make ("capsfilter", tmp);
    g_free (tmp);
    if (!session->priv->discovery_capsfilter)
    {
      g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
          "Could not make capsfilter element");
      goto error;
    }

    if (!gst_bin_add (GST_BIN (session->priv->conference),
            session->priv->discovery_capsfilter))
    {
      g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
          "Could not add the discovery capsfilter to the bin");
      goto error;
    }

    if (!gst_element_sync_state_with_parent (
            session->priv->discovery_capsfilter))
    {
      g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
          "Could not sync the discovery capsfilter's state with its parent");
      goto error;
    }

    if (!gst_element_link_pads (session->priv->discovery_capsfilter, "src",
            session->priv->discovery_fakesink, "sink"))
    {
      g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
          "Could not link discovery capsfilter and fakesink");
      goto error;
    }

    pad = gst_element_get_static_pad (session->priv->discovery_capsfilter,
        "src");
    g_signal_connect (pad, "notify::caps", G_CALLBACK (_discovery_caps_changed),
        session);
    gst_object_unref (pad);
  }

  tmp = g_strdup_printf ("discover_%d_%d", session->id, ca->codec->id);
  session->priv->discovery_codecbin = _create_codec_bin (ca, ca->codec,
      tmp, TRUE, NULL, error);
  g_free (tmp);

  if (!session->priv->discovery_codecbin)
    goto error;

  if (!gst_bin_add (GST_BIN (session->priv->conference),
            session->priv->discovery_codecbin))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not add the discovery codecbin to the bin");
    goto error;
  }

  if (!gst_element_sync_state_with_parent (session->priv->discovery_codecbin))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not sync the discovery codecbin's state with its parent");
    goto error;
  }

  caps = fs_codec_to_gst_caps (ca->codec);
  g_object_set (session->priv->discovery_capsfilter,
      "caps", caps,
      NULL);
  gst_caps_unref (caps);


  if (!gst_element_link_pads (session->priv->discovery_codecbin, "src",
            session->priv->discovery_capsfilter, "sink"))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not link discovery codecbin and capsfilter");
    goto error;
  }

  pad = gst_element_get_static_pad (session->priv->discovery_codecbin, "sink");

  if (GST_PAD_LINK_FAILED (gst_pad_link (session->priv->send_tee_discovery_pad,
              pad)))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not link the tee and the discovery codecbin");
    gst_object_unref (pad);
    goto error;
  }

  gst_object_unref (pad);

  session->priv->discovery_codec = fs_codec_copy (ca->codec);

  FS_RTP_SESSION_UNLOCK (session);

  return TRUE;

 error:

  if (session->priv->discovery_fakesink)
  {
    gst_element_set_locked_state (session->priv->discovery_fakesink, TRUE);
    gst_element_set_state (session->priv->discovery_fakesink, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (session->priv->conference),
        session->priv->discovery_fakesink);
    session->priv->discovery_fakesink = NULL;
  }

  if (session->priv->discovery_capsfilter)
  {
    gst_element_set_locked_state (session->priv->discovery_capsfilter, TRUE);
    gst_element_set_state (session->priv->discovery_capsfilter, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (session->priv->conference),
        session->priv->discovery_capsfilter);
    session->priv->discovery_capsfilter = NULL;
  }

  if (session->priv->discovery_codecbin)
  {
    gst_element_set_locked_state (session->priv->discovery_codecbin, TRUE);
    gst_element_set_state (session->priv->discovery_codecbin, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (session->priv->conference),
        session->priv->discovery_codecbin);
    session->priv->discovery_codecbin = NULL;
  }

  FS_RTP_SESSION_UNLOCK (session);

  return FALSE;
}

/**
 * _send_sink_pad_blocked_callback:
 *
 * This is the callback to change the discovery codecbin
 */

static void
_send_sink_pad_blocked_callback (GstPad *pad, gboolean blocked,
    gpointer user_data)
{
  FsRtpSession *session = user_data;
  GError *error = NULL;
  GList *item = NULL;
  CodecAssociation *ca = NULL;

  FS_RTP_SESSION_LOCK (session);

  /* Find out if there is a codec that needs the config to be fetched */
  for (item = g_list_first (session->priv->codec_associations);
       item;
       item = g_list_next (item))
  {
    ca = item->data;
    if (ca->need_config)
      break;
  }
  if (!item)
  {
    fs_rtp_session_stop_codec_param_gathering (session);
    g_object_notify (G_OBJECT (session), "codecs-ready");
    gst_element_post_message (GST_ELEMENT (session->priv->conference),
        gst_message_new_element (GST_OBJECT (session->priv->conference),
            gst_structure_new ("farsight-codecs-changed",
                "session", FS_TYPE_SESSION, session,
                NULL)));

    goto out;
  }

  if (fs_codec_are_equal (ca->codec, session->priv->discovery_codec))
    goto out;

  if (!fs_rtp_session_get_codec_params (session, ca, &error))
  {
    fs_rtp_session_stop_codec_param_gathering (session);
    fs_session_emit_error (FS_SESSION (session), error->code,
        "Error while discovering codec data, discovery cancelled",
        error->message);
  }

 out:

  g_clear_error (&error);

  FS_RTP_SESSION_UNLOCK (session);

  gst_pad_set_blocked_async (pad, FALSE, pad_block_do_nothing, NULL);
}

/**
 * fs_rtp_session_start_codec_param_gathering
 * @session: a #FsRtpSession
 *
 * Check if there is any codec associations that requires codec discovery and
 * if there is, starts the gathering process by adding a pad block to the
 * tee's discovery src pad
 */

static void
fs_rtp_session_start_codec_param_gathering (FsRtpSession *session)
{
  GList *item = NULL;

  FS_RTP_SESSION_LOCK (session);

  /* Find out if there is a codec that needs the config to be fetched */
  for (item = g_list_first (session->priv->codec_associations);
       item;
       item = g_list_next (item))
  {
    CodecAssociation *ca = item->data;
    if (ca->need_config)
      break;
  }
  if (!item)
    goto out;

  GST_DEBUG ("Starting Codec Param discovery for session %d", session->id);

  gst_pad_set_blocked_async (session->priv->send_tee_discovery_pad, TRUE,
      _send_sink_pad_blocked_callback, session);

 out:

  FS_RTP_SESSION_UNLOCK (session);
}


/**
 * fs_rtp_session_stop_codec_param_gathering
 * @session: a #FsRtpSession
 *
 * Stop the codec config gathering and remove the elements used for that
 */

static void
fs_rtp_session_stop_codec_param_gathering (FsRtpSession *session)
{

  FS_RTP_SESSION_LOCK (session);

  GST_DEBUG ("Stopping Codec Param discovery for session %d", session->id);

  if (session->priv->discovery_codec)
  {
    fs_codec_destroy (session->priv->discovery_codec);
    session->priv->discovery_codec = NULL;
  }

  if (session->priv->discovery_fakesink)
  {
    gst_element_set_locked_state (session->priv->discovery_fakesink, TRUE);
    gst_element_set_state (session->priv->discovery_fakesink, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (session->priv->conference),
        session->priv->discovery_fakesink);
    session->priv->discovery_fakesink = NULL;
  }

  if (session->priv->discovery_capsfilter)
  {
    gst_element_set_locked_state (session->priv->discovery_capsfilter, TRUE);
    gst_element_set_state (session->priv->discovery_capsfilter, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (session->priv->conference),
        session->priv->discovery_capsfilter);
    session->priv->discovery_capsfilter = NULL;
  }

  if (session->priv->discovery_codecbin)
  {
    gst_element_set_locked_state (session->priv->discovery_codecbin, TRUE);
    gst_element_set_state (session->priv->discovery_codecbin, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (session->priv->conference),
        session->priv->discovery_codecbin);
    session->priv->discovery_codecbin = NULL;
  }

  FS_RTP_SESSION_UNLOCK (session);
}

static gchar **
fs_rtp_session_list_transmitters (FsSession *session)
{
  gchar **rv;

  g_return_val_if_fail (FS_IS_RTP_SESSION (session), NULL);

  rv = fs_transmitter_list_available ();

  if (!rv)
    rv = g_malloc0 (1);

  return rv;
}
