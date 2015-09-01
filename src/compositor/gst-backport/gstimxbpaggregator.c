/* GStreamer aggregator base class
 * Copyright (C) 2014 Mathieu Duponchelle <mathieu.duponchelle@opencreed.com>
 * Copyright (C) 2014 Thibault Saunier <tsaunier@gnome.org>
 *
 * gstaggregator.c:
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
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
/**
 * SECTION: gstaggregator
 * @short_description: manages a set of pads with the purpose of
 * aggregating their buffers.
 * @see_also: gstcollectpads for historical reasons.
 *
 * Manages a set of pads with the purpose of aggregating their buffers.
 * Control is given to the subclass when all pads have data.
 * <itemizedlist>
 *  <listitem><para>
 *    Base class for mixers and muxers. Subclasses should at least implement
 *    the #GstImxBPAggregatorClass.aggregate() virtual method.
 *  </para></listitem>
 *  <listitem><para>
 *    When data is queued on all pads, tha aggregate vmethod is called.
 *  </para></listitem>
 *  <listitem><para>
 *    One can peek at the data on any given GstImxBPAggregatorPad with the
 *    gst_imxbp_aggregator_pad_get_buffer () method, and take ownership of it
 *    with the gst_imxbp_aggregator_pad_steal_buffer () method. When a buffer
 *    has been taken with steal_buffer (), a new buffer can be queued
 *    on that pad.
 *  </para></listitem>
 *  <listitem><para>
 *    If the subclass wishes to push a buffer downstream in its aggregate
 *    implementation, it should do so through the
 *    gst_imxbp_aggregator_finish_buffer () method. This method will take care
 *    of sending and ordering mandatory events such as stream start, caps
 *    and segment.
 *  </para></listitem>
 *  <listitem><para>
 *    Same goes for EOS events, which should not be pushed directly by the
 *    subclass, it should instead return GST_FLOW_EOS in its aggregate
 *    implementation.
 *  </para></listitem>
 *  <listitem><para>
 *    Note that the aggregator logic regarding gap event handling is to turn
 *    these into gap buffers with matching PTS and duration. It will also
 *    flag these buffers with GST_BUFFER_FLAG_GAP and GST_BUFFER_FLAG_DROPPABLE
 *    to ease their identification and subsequent processing.
 *  </para></listitem>
 * </itemizedlist>
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <string.h>             /* strlen */

#include "gstimxbpaggregator.h"


/*  Might become API */
static void gst_imxbp_aggregator_merge_tags (GstImxBPAggregator * aggregator,
    const GstTagList * tags, GstTagMergeMode mode);
static void gst_imxbp_aggregator_set_latency_property (GstImxBPAggregator * agg,
    gint64 latency);
static gint64 gst_imxbp_aggregator_get_latency_property (GstImxBPAggregator * agg);


/* Locking order, locks in this element must always be taken in this order
 *
 * standard sink pad stream lock -> GST_PAD_STREAM_LOCK (aggpad)
 * Aggregator pad flush lock -> PAD_FLUSH_LOCK(aggpad)
 * standard src pad stream lock -> GST_PAD_STREAM_LOCK (srcpad)
 * Aggregator src lock -> SRC_LOCK(agg) w/ SRC_WAIT/BROADCAST
 * standard element object lock -> GST_OBJECT_LOCK(agg)
 * Aggregator pad lock -> PAD_LOCK (aggpad) w/ PAD_WAIT/BROADCAST_EVENT(aggpad)
 * standard src pad object lock -> GST_OBJECT_LOCK(srcpad)
 * standard sink pad object lock -> GST_OBJECT_LOCK(aggpad)
 */


static GstClockTime gst_imxbp_aggregator_get_latency_unlocked (GstImxBPAggregator * self);

GST_DEBUG_CATEGORY_STATIC (aggregator_debug);
#define GST_CAT_DEFAULT aggregator_debug

/* GstImxBPAggregatorPad definitions */
#define PAD_LOCK(pad)   G_STMT_START {                                  \
  GST_TRACE_OBJECT (pad, "Taking PAD lock from thread %p",              \
        g_thread_self());                                               \
  g_mutex_lock(&pad->priv->lock);                                       \
  GST_TRACE_OBJECT (pad, "Took PAD lock from thread %p",                \
        g_thread_self());                                               \
  } G_STMT_END

#define PAD_UNLOCK(pad)  G_STMT_START {                                 \
  GST_TRACE_OBJECT (pad, "Releasing PAD lock from thread %p",           \
      g_thread_self());                                                 \
  g_mutex_unlock(&pad->priv->lock);                                     \
  GST_TRACE_OBJECT (pad, "Release PAD lock from thread %p",             \
        g_thread_self());                                               \
  } G_STMT_END


#define PAD_WAIT_EVENT(pad)   G_STMT_START {                            \
  GST_LOG_OBJECT (pad, "Waiting for buffer to be consumed thread %p",   \
        g_thread_self());                                               \
  g_cond_wait(&(((GstImxBPAggregatorPad* )pad)->priv->event_cond),           \
      (&((GstImxBPAggregatorPad*)pad)->priv->lock));                         \
  GST_LOG_OBJECT (pad, "DONE Waiting for buffer to be consumed on thread %p", \
        g_thread_self());                                               \
  } G_STMT_END

#define PAD_BROADCAST_EVENT(pad) G_STMT_START {                        \
  GST_LOG_OBJECT (pad, "Signaling buffer consumed from thread %p",     \
        g_thread_self());                                              \
  g_cond_broadcast(&(((GstImxBPAggregatorPad* )pad)->priv->event_cond));    \
  } G_STMT_END


#define PAD_FLUSH_LOCK(pad)     G_STMT_START {                          \
  GST_TRACE_OBJECT (pad, "Taking lock from thread %p",                  \
        g_thread_self());                                               \
  g_mutex_lock(&pad->priv->flush_lock);                                 \
  GST_TRACE_OBJECT (pad, "Took lock from thread %p",                    \
        g_thread_self());                                               \
  } G_STMT_END

#define PAD_FLUSH_UNLOCK(pad)   G_STMT_START {                          \
  GST_TRACE_OBJECT (pad, "Releasing lock from thread %p",               \
        g_thread_self());                                               \
  g_mutex_unlock(&pad->priv->flush_lock);                               \
  GST_TRACE_OBJECT (pad, "Release lock from thread %p",                 \
        g_thread_self());                                               \
  } G_STMT_END

#define SRC_LOCK(self)   G_STMT_START {                             \
  GST_TRACE_OBJECT (self, "Taking src lock from thread %p",         \
      g_thread_self());                                             \
  g_mutex_lock(&self->priv->src_lock);                              \
  GST_TRACE_OBJECT (self, "Took src lock from thread %p",           \
        g_thread_self());                                           \
  } G_STMT_END

#define SRC_UNLOCK(self)  G_STMT_START {                            \
  GST_TRACE_OBJECT (self, "Releasing src lock from thread %p",      \
        g_thread_self());                                           \
  g_mutex_unlock(&self->priv->src_lock);                            \
  GST_TRACE_OBJECT (self, "Released src lock from thread %p",       \
        g_thread_self());                                           \
  } G_STMT_END

#define SRC_WAIT(self) G_STMT_START {                               \
  GST_LOG_OBJECT (self, "Waiting for src on thread %p",             \
        g_thread_self());                                           \
  g_cond_wait(&(self->priv->src_cond), &(self->priv->src_lock));    \
  GST_LOG_OBJECT (self, "DONE Waiting for src on thread %p",        \
        g_thread_self());                                           \
  } G_STMT_END

#define SRC_BROADCAST(self) G_STMT_START {                          \
    GST_LOG_OBJECT (self, "Signaling src from thread %p",           \
        g_thread_self());                                           \
    if (self->priv->aggregate_id)                                   \
      gst_clock_id_unschedule (self->priv->aggregate_id);           \
    g_cond_broadcast(&(self->priv->src_cond));                      \
  } G_STMT_END

struct _GstImxBPAggregatorPadPrivate
{
  /* Following fields are protected by the PAD_LOCK */
  GstFlowReturn flow_return;
  gboolean pending_flush_start;
  gboolean pending_flush_stop;
  gboolean pending_eos;

  GstBuffer *buffer;
  gboolean eos;

  GMutex lock;
  GCond event_cond;
  /* This lock prevents a flush start processing happening while
   * the chain function is also happening.
   */
  GMutex flush_lock;
};

static gboolean
gst_imxbp_aggregator_pad_flush (GstImxBPAggregatorPad * aggpad, GstImxBPAggregator * agg)
{
  GstImxBPAggregatorPadClass *klass = GST_IMXBP_AGGREGATOR_PAD_GET_CLASS (aggpad);

  PAD_LOCK (aggpad);
  aggpad->priv->pending_eos = FALSE;
  aggpad->priv->eos = FALSE;
  aggpad->priv->flow_return = GST_FLOW_OK;
  PAD_UNLOCK (aggpad);

  if (klass->flush)
    return klass->flush (aggpad, agg);

  return TRUE;
}

/*************************************
 * GstImxBPAggregator implementation  *
 *************************************/
static GstElementClass *aggregator_parent_class = NULL;

/* All members are protected by the object lock unless otherwise noted */

struct _GstImxBPAggregatorPrivate
{
  gint padcount;

  /* Our state is >= PAUSED */
  gboolean running;             /* protected by src_lock */

  gint seqnum;
  gboolean send_stream_start;   /* protected by srcpad stream lock */
  gboolean send_segment;
  gboolean flush_seeking;
  gboolean pending_flush_start;
  gboolean send_eos;            /* protected by srcpad stream lock */

  GstCaps *srccaps;             /* protected by the srcpad stream lock */

  GstTagList *tags;
  gboolean tags_changed;

  gboolean peer_latency_live;   /* protected by src_lock */
  GstClockTime peer_latency_min;        /* protected by src_lock */
  GstClockTime peer_latency_max;        /* protected by src_lock */
  gboolean has_peer_latency;

  GstClockTime sub_latency_min; /* protected by src_lock */
  GstClockTime sub_latency_max; /* protected by src_lock */

  /* aggregate */
  GstClockID aggregate_id;      /* protected by src_lock */
  GMutex src_lock;
  GCond src_cond;

  /* properties */
  gint64 latency;
};

typedef struct
{
  GstEvent *event;
  gboolean result;
  gboolean flush;

  gboolean one_actually_seeked;
} EventData;

#define DEFAULT_LATENCY        0

enum
{
  PROP_0,
  PROP_LATENCY,
  PROP_LAST
};

/**
 * gst_imxbp_aggregator_iterate_sinkpads:
 * @self: The #GstImxBPAggregator
 * @func: (scope call): The function to call.
 * @user_data: (closure): The data to pass to @func.
 *
 * Iterate the sinkpads of aggregator to call a function on them.
 *
 * This method guarantees that @func will be called only once for each
 * sink pad.
 */
gboolean
gst_imxbp_aggregator_iterate_sinkpads (GstImxBPAggregator * self,
    GstImxBPAggregatorPadForeachFunc func, gpointer user_data)
{
  gboolean result = FALSE;
  GstIterator *iter;
  gboolean done = FALSE;
  GValue item = { 0, };
  GList *seen_pads = NULL;

  iter = gst_element_iterate_sink_pads (GST_ELEMENT (self));

  if (!iter)
    goto no_iter;

  while (!done) {
    switch (gst_iterator_next (iter, &item)) {
      case GST_ITERATOR_OK:
      {
        GstImxBPAggregatorPad *pad;

        pad = g_value_get_object (&item);

        /* if already pushed, skip. FIXME, find something faster to tag pads */
        if (pad == NULL || g_list_find (seen_pads, pad)) {
          g_value_reset (&item);
          break;
        }

        GST_LOG_OBJECT (pad, "calling function %s on pad",
            GST_DEBUG_FUNCPTR_NAME (func));

        result = func (self, pad, user_data);

        done = !result;

        seen_pads = g_list_prepend (seen_pads, pad);

        g_value_reset (&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync (iter);
        break;
      case GST_ITERATOR_ERROR:
        GST_ERROR_OBJECT (self,
            "Could not iterate over internally linked pads");
        done = TRUE;
        break;
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
    }
  }
  g_value_unset (&item);
  gst_iterator_free (iter);

  if (seen_pads == NULL) {
    GST_DEBUG_OBJECT (self, "No pad seen");
    return FALSE;
  }

  g_list_free (seen_pads);

no_iter:
  return result;
}

static gboolean
gst_imxbp_aggregator_check_pads_ready (GstImxBPAggregator * self)
{
  GstImxBPAggregatorPad *pad;
  GList *l, *sinkpads;

  GST_LOG_OBJECT (self, "checking pads");

  GST_OBJECT_LOCK (self);

  sinkpads = GST_ELEMENT_CAST (self)->sinkpads;
  if (sinkpads == NULL)
    goto no_sinkpads;

  for (l = sinkpads; l != NULL; l = l->next) {
    pad = l->data;

    PAD_LOCK (pad);
    if (pad->priv->buffer == NULL && !pad->priv->eos) {
      PAD_UNLOCK (pad);
      goto pad_not_ready;
    }
    PAD_UNLOCK (pad);

  }

  GST_OBJECT_UNLOCK (self);
  GST_LOG_OBJECT (self, "pads are ready");
  return TRUE;

no_sinkpads:
  {
    GST_LOG_OBJECT (self, "pads not ready: no sink pads");
    GST_OBJECT_UNLOCK (self);
    return FALSE;
  }
pad_not_ready:
  {
    GST_LOG_OBJECT (pad, "pad not ready to be aggregated yet");
    GST_OBJECT_UNLOCK (self);
    return FALSE;
  }
}

static void
gst_imxbp_aggregator_reset_flow_values (GstImxBPAggregator * self)
{
  GST_OBJECT_LOCK (self);
  self->priv->send_stream_start = TRUE;
  self->priv->send_segment = TRUE;
  gst_segment_init (&self->segment, GST_FORMAT_TIME);
  GST_OBJECT_UNLOCK (self);
}

static inline void
gst_imxbp_aggregator_push_mandatory_events (GstImxBPAggregator * self)
{
  GstImxBPAggregatorPrivate *priv = self->priv;
  GstEvent *segment = NULL;
  GstEvent *tags = NULL;

  if (self->priv->send_stream_start) {
    gchar s_id[32];

    GST_INFO_OBJECT (self, "pushing stream start");
    /* stream-start (FIXME: create id based on input ids) */
    g_snprintf (s_id, sizeof (s_id), "agg-%08x", g_random_int ());
    if (!gst_pad_push_event (self->srcpad, gst_event_new_stream_start (s_id))) {
      GST_WARNING_OBJECT (self->srcpad, "Sending stream start event failed");
    }
    self->priv->send_stream_start = FALSE;
  }

  if (self->priv->srccaps) {

    GST_INFO_OBJECT (self, "pushing caps: %" GST_PTR_FORMAT,
        self->priv->srccaps);
    if (!gst_pad_push_event (self->srcpad,
            gst_event_new_caps (self->priv->srccaps))) {
      GST_WARNING_OBJECT (self->srcpad, "Sending caps event failed");
    }
    gst_caps_unref (self->priv->srccaps);
    self->priv->srccaps = NULL;
  }

  GST_OBJECT_LOCK (self);
  if (self->priv->send_segment && !self->priv->flush_seeking) {
    segment = gst_event_new_segment (&self->segment);

    if (!self->priv->seqnum)
      self->priv->seqnum = gst_event_get_seqnum (segment);
    else
      gst_event_set_seqnum (segment, self->priv->seqnum);
    self->priv->send_segment = FALSE;

    GST_DEBUG_OBJECT (self, "pushing segment %" GST_PTR_FORMAT, segment);
  }

  if (priv->tags && priv->tags_changed && !self->priv->flush_seeking) {
    tags = gst_event_new_tag (gst_tag_list_ref (priv->tags));
    priv->tags_changed = FALSE;
  }
  GST_OBJECT_UNLOCK (self);

  if (segment)
    gst_pad_push_event (self->srcpad, segment);
  if (tags)
    gst_pad_push_event (self->srcpad, tags);

}

/**
 * gst_imxbp_aggregator_set_src_caps:
 * @self: The #GstImxBPAggregator
 * @caps: The #GstCaps to set on the src pad.
 *
 * Sets the caps to be used on the src pad.
 */
void
gst_imxbp_aggregator_set_src_caps (GstImxBPAggregator * self, GstCaps * caps)
{
  GST_PAD_STREAM_LOCK (self->srcpad);
  gst_caps_replace (&self->priv->srccaps, caps);
  gst_imxbp_aggregator_push_mandatory_events (self);
  GST_PAD_STREAM_UNLOCK (self->srcpad);
}

/**
 * gst_imxbp_aggregator_finish_buffer:
 * @self: The #GstImxBPAggregator
 * @buffer: (transfer full): the #GstBuffer to push.
 *
 * This method will push the provided output buffer downstream. If needed,
 * mandatory events such as stream-start, caps, and segment events will be
 * sent before pushing the buffer.
 */
GstFlowReturn
gst_imxbp_aggregator_finish_buffer (GstImxBPAggregator * self, GstBuffer * buffer)
{
  gst_imxbp_aggregator_push_mandatory_events (self);

  GST_OBJECT_LOCK (self);
  if (!self->priv->flush_seeking && gst_pad_is_active (self->srcpad)) {
    GST_TRACE_OBJECT (self, "pushing buffer %" GST_PTR_FORMAT, buffer);
    GST_OBJECT_UNLOCK (self);
    return gst_pad_push (self->srcpad, buffer);
  } else {
    GST_INFO_OBJECT (self, "Not pushing (active: %i, flushing: %i)",
        self->priv->flush_seeking, gst_pad_is_active (self->srcpad));
    GST_OBJECT_UNLOCK (self);
    gst_buffer_unref (buffer);
    return GST_FLOW_OK;
  }
}

static void
gst_imxbp_aggregator_push_eos (GstImxBPAggregator * self)
{
  GstEvent *event;
  gst_imxbp_aggregator_push_mandatory_events (self);

  event = gst_event_new_eos ();

  GST_OBJECT_LOCK (self);
  self->priv->send_eos = FALSE;
  gst_event_set_seqnum (event, self->priv->seqnum);
  GST_OBJECT_UNLOCK (self);

  gst_pad_push_event (self->srcpad, event);
}

static GstClockTime
gst_imxbp_aggregator_get_next_time (GstImxBPAggregator * self)
{
  GstImxBPAggregatorClass *klass = GST_IMXBP_AGGREGATOR_GET_CLASS (self);

  if (klass->get_next_time)
    return klass->get_next_time (self);

  return GST_CLOCK_TIME_NONE;
}

static gboolean
gst_imxbp_aggregator_wait_and_check (GstImxBPAggregator * self, gboolean * timeout)
{
  GstClockTime latency;
  GstClockTime start;
  gboolean res;

  *timeout = FALSE;

  SRC_LOCK (self);

  latency = gst_imxbp_aggregator_get_latency_unlocked (self);

  if (gst_imxbp_aggregator_check_pads_ready (self)) {
    GST_DEBUG_OBJECT (self, "all pads have data");
    SRC_UNLOCK (self);

    return TRUE;
  }

  /* Before waiting, check if we're actually still running */
  if (!self->priv->running || !self->priv->send_eos) {
    SRC_UNLOCK (self);

    return FALSE;
  }

  start = gst_imxbp_aggregator_get_next_time (self);

  if (!GST_CLOCK_TIME_IS_VALID (latency) ||
      !GST_IS_CLOCK (GST_ELEMENT_CLOCK (self)) ||
      !GST_CLOCK_TIME_IS_VALID (start)) {
    /* We wake up here when something happened, and below
     * then check if we're ready now. If we return FALSE,
     * we will be directly called again.
     */
    SRC_WAIT (self);
  } else {
    GstClockTime base_time, time;
    GstClock *clock;
    GstClockReturn status;
    GstClockTimeDiff jitter;

    GST_DEBUG_OBJECT (self, "got subclass start time: %" GST_TIME_FORMAT,
        GST_TIME_ARGS (start));

    GST_OBJECT_LOCK (self);
    base_time = GST_ELEMENT_CAST (self)->base_time;
    clock = GST_ELEMENT_CLOCK (self);
    if (clock)
      gst_object_ref (clock);
    GST_OBJECT_UNLOCK (self);

    time = base_time + start;
    time += latency;

    GST_DEBUG_OBJECT (self, "possibly waiting for clock to reach %"
        GST_TIME_FORMAT " (base %" GST_TIME_FORMAT " start %" GST_TIME_FORMAT
        " latency %" GST_TIME_FORMAT " current %" GST_TIME_FORMAT ")",
        GST_TIME_ARGS (time),
        GST_TIME_ARGS (GST_ELEMENT_CAST (self)->base_time),
        GST_TIME_ARGS (start), GST_TIME_ARGS (latency),
        GST_TIME_ARGS (gst_clock_get_time (clock)));


    self->priv->aggregate_id = gst_clock_new_single_shot_id (clock, time);
    gst_object_unref (clock);
    SRC_UNLOCK (self);

    jitter = 0;
    status = gst_clock_id_wait (self->priv->aggregate_id, &jitter);

    SRC_LOCK (self);
    if (self->priv->aggregate_id) {
      gst_clock_id_unref (self->priv->aggregate_id);
      self->priv->aggregate_id = NULL;
    }

    GST_DEBUG_OBJECT (self,
        "clock returned %d (jitter: %s%" GST_TIME_FORMAT ")",
        status, (jitter < 0 ? "-" : " "),
        GST_TIME_ARGS ((jitter < 0 ? -jitter : jitter)));

    /* we timed out */
    if (status == GST_CLOCK_OK || status == GST_CLOCK_EARLY) {
      SRC_UNLOCK (self);
      *timeout = TRUE;
      return TRUE;
    }
  }

  res = gst_imxbp_aggregator_check_pads_ready (self);
  SRC_UNLOCK (self);

  return res;
}

static void
gst_imxbp_aggregator_pad_set_flushing (GstImxBPAggregatorPad * aggpad,
    GstFlowReturn flow_return)
{
  PAD_LOCK (aggpad);
  if (flow_return == GST_FLOW_NOT_LINKED)
    aggpad->priv->flow_return = MIN (flow_return, aggpad->priv->flow_return);
  else
    aggpad->priv->flow_return = flow_return;
  gst_buffer_replace (&aggpad->priv->buffer, NULL);
  PAD_BROADCAST_EVENT (aggpad);
  PAD_UNLOCK (aggpad);
}

static void
gst_imxbp_aggregator_aggregate_func (GstImxBPAggregator * self)
{
  GstImxBPAggregatorPrivate *priv = self->priv;
  GstImxBPAggregatorClass *klass = GST_IMXBP_AGGREGATOR_GET_CLASS (self);
  gboolean timeout = FALSE;

  if (self->priv->running == FALSE) {
    GST_DEBUG_OBJECT (self, "Not running anymore");
    return;
  }

  GST_LOG_OBJECT (self, "Checking aggregate");
  while (priv->send_eos && priv->running) {
    GstFlowReturn flow_return;

    if (!gst_imxbp_aggregator_wait_and_check (self, &timeout))
      continue;

    GST_TRACE_OBJECT (self, "Actually aggregating!");

    flow_return = klass->aggregate (self, timeout);

    GST_OBJECT_LOCK (self);
    if (flow_return == GST_FLOW_FLUSHING && priv->flush_seeking) {
      /* We don't want to set the pads to flushing, but we want to
       * stop the thread, so just break here */
      GST_OBJECT_UNLOCK (self);
      break;
    }
    GST_OBJECT_UNLOCK (self);

    if (flow_return == GST_FLOW_EOS || flow_return == GST_FLOW_ERROR) {
      gst_imxbp_aggregator_push_eos (self);
    }

    GST_LOG_OBJECT (self, "flow return is %s", gst_flow_get_name (flow_return));

    if (flow_return != GST_FLOW_OK) {
      GList *item;

      GST_OBJECT_LOCK (self);
      for (item = GST_ELEMENT (self)->sinkpads; item; item = item->next) {
        GstImxBPAggregatorPad *aggpad = GST_IMXBP_AGGREGATOR_PAD (item->data);

        gst_imxbp_aggregator_pad_set_flushing (aggpad, flow_return);
      }
      GST_OBJECT_UNLOCK (self);
      break;
    }
  }

  /* Pause the task here, the only ways to get here are:
   * 1) We're stopping, in which case the task is stopped anyway
   * 2) We got a flow error above, in which case it might take
   *    some time to forward the flow return upstream and we
   *    would otherwise call the task function over and over
   *    again without doing anything
   */
  gst_pad_pause_task (self->srcpad);
}

static gboolean
gst_imxbp_aggregator_start (GstImxBPAggregator * self)
{
  GstImxBPAggregatorClass *klass;
  gboolean result;

  self->priv->running = TRUE;
  self->priv->send_stream_start = TRUE;
  self->priv->send_segment = TRUE;
  self->priv->send_eos = TRUE;
  self->priv->srccaps = NULL;

  klass = GST_IMXBP_AGGREGATOR_GET_CLASS (self);

  if (klass->start)
    result = klass->start (self);
  else
    result = TRUE;

  return result;
}

static gboolean
_check_pending_flush_stop (GstImxBPAggregatorPad * pad)
{
  gboolean res;

  PAD_LOCK (pad);
  res = (!pad->priv->pending_flush_stop && !pad->priv->pending_flush_start);
  PAD_UNLOCK (pad);

  return res;
}

static gboolean
gst_imxbp_aggregator_stop_srcpad_task (GstImxBPAggregator * self, GstEvent * flush_start)
{
  gboolean res = TRUE;

  GST_INFO_OBJECT (self, "%s srcpad task",
      flush_start ? "Pausing" : "Stopping");

  SRC_LOCK (self);
  self->priv->running = FALSE;
  SRC_BROADCAST (self);
  SRC_UNLOCK (self);

  if (flush_start) {
    res = gst_pad_push_event (self->srcpad, flush_start);
  }

  gst_pad_stop_task (self->srcpad);

  return res;
}

static void
gst_imxbp_aggregator_start_srcpad_task (GstImxBPAggregator * self)
{
  GST_INFO_OBJECT (self, "Starting srcpad task");

  self->priv->running = TRUE;
  gst_pad_start_task (GST_PAD (self->srcpad),
      (GstTaskFunction) gst_imxbp_aggregator_aggregate_func, self, NULL);
}

static GstFlowReturn
gst_imxbp_aggregator_flush (GstImxBPAggregator * self)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstImxBPAggregatorPrivate *priv = self->priv;
  GstImxBPAggregatorClass *klass = GST_IMXBP_AGGREGATOR_GET_CLASS (self);

  GST_DEBUG_OBJECT (self, "Flushing everything");
  GST_OBJECT_LOCK (self);
  priv->send_segment = TRUE;
  priv->flush_seeking = FALSE;
  priv->tags_changed = FALSE;
  GST_OBJECT_UNLOCK (self);
  if (klass->flush)
    ret = klass->flush (self);

  return ret;
}


/* Called with GstImxBPAggregator's object lock held */

static gboolean
gst_imxbp_aggregator_all_flush_stop_received_locked (GstImxBPAggregator * self)
{
  GList *tmp;
  GstImxBPAggregatorPad *tmppad;

  for (tmp = GST_ELEMENT (self)->sinkpads; tmp; tmp = tmp->next) {
    tmppad = (GstImxBPAggregatorPad *) tmp->data;

    if (_check_pending_flush_stop (tmppad) == FALSE) {
      GST_DEBUG_OBJECT (tmppad, "Is not last %i -- %i",
          tmppad->priv->pending_flush_start, tmppad->priv->pending_flush_stop);
      return FALSE;
    }
  }

  return TRUE;
}

static void
gst_imxbp_aggregator_flush_start (GstImxBPAggregator * self, GstImxBPAggregatorPad * aggpad,
    GstEvent * event)
{
  GstImxBPAggregatorPrivate *priv = self->priv;
  GstImxBPAggregatorPadPrivate *padpriv = aggpad->priv;

  gst_imxbp_aggregator_pad_set_flushing (aggpad, GST_FLOW_FLUSHING);

  PAD_FLUSH_LOCK (aggpad);
  PAD_LOCK (aggpad);
  if (padpriv->pending_flush_start) {
    GST_DEBUG_OBJECT (aggpad, "Expecting FLUSH_STOP now");

    padpriv->pending_flush_start = FALSE;
    padpriv->pending_flush_stop = TRUE;
  }
  PAD_UNLOCK (aggpad);

  GST_OBJECT_LOCK (self);
  if (priv->flush_seeking) {
    /* If flush_seeking we forward the first FLUSH_START */
    if (priv->pending_flush_start) {
      priv->pending_flush_start = FALSE;
      GST_OBJECT_UNLOCK (self);

      GST_INFO_OBJECT (self, "Flushing, pausing srcpad task");
      gst_imxbp_aggregator_stop_srcpad_task (self, event);

      GST_INFO_OBJECT (self, "Getting STREAM_LOCK while seeking");
      GST_PAD_STREAM_LOCK (self->srcpad);
      GST_LOG_OBJECT (self, "GOT STREAM_LOCK");
      event = NULL;
    } else {
      GST_OBJECT_UNLOCK (self);
      gst_event_unref (event);
    }
  } else {
    GST_OBJECT_UNLOCK (self);
    gst_event_unref (event);
  }
  PAD_FLUSH_UNLOCK (aggpad);

  gst_imxbp_aggregator_pad_drop_buffer (aggpad);
}

/* GstImxBPAggregator vmethods default implementations */
static gboolean
gst_imxbp_aggregator_default_sink_event (GstImxBPAggregator * self,
    GstImxBPAggregatorPad * aggpad, GstEvent * event)
{
  gboolean res = TRUE;
  GstPad *pad = GST_PAD (aggpad);
  GstImxBPAggregatorPrivate *priv = self->priv;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
    {
      gst_imxbp_aggregator_flush_start (self, aggpad, event);
      /* We forward only in one case: right after flush_seeking */
      event = NULL;
      goto eat;
    }
    case GST_EVENT_FLUSH_STOP:
    {
      GST_DEBUG_OBJECT (aggpad, "Got FLUSH_STOP");

      gst_imxbp_aggregator_pad_flush (aggpad, self);
      GST_OBJECT_LOCK (self);
      if (priv->flush_seeking) {
        g_atomic_int_set (&aggpad->priv->pending_flush_stop, FALSE);
        if (gst_imxbp_aggregator_all_flush_stop_received_locked (self)) {
          GST_OBJECT_UNLOCK (self);
          /* That means we received FLUSH_STOP/FLUSH_STOP on
           * all sinkpads -- Seeking is Done... sending FLUSH_STOP */
          gst_imxbp_aggregator_flush (self);
          gst_pad_push_event (self->srcpad, event);
          event = NULL;
          SRC_LOCK (self);
          priv->send_eos = TRUE;
          SRC_BROADCAST (self);
          SRC_UNLOCK (self);

          GST_INFO_OBJECT (self, "Releasing source pad STREAM_LOCK");
          GST_PAD_STREAM_UNLOCK (self->srcpad);
          gst_imxbp_aggregator_start_srcpad_task (self);
        } else {
          GST_OBJECT_UNLOCK (self);
        }
      } else {
        GST_OBJECT_UNLOCK (self);
      }

      /* We never forward the event */
      goto eat;
    }
    case GST_EVENT_EOS:
    {
      GST_DEBUG_OBJECT (aggpad, "EOS");

      /* We still have a buffer, and we don't want the subclass to have to
       * check for it. Mark pending_eos, eos will be set when steal_buffer is
       * called
       */
      SRC_LOCK (self);
      PAD_LOCK (aggpad);
      if (!aggpad->priv->buffer) {
        aggpad->priv->eos = TRUE;
      } else {
        aggpad->priv->pending_eos = TRUE;
      }
      PAD_UNLOCK (aggpad);

      SRC_BROADCAST (self);
      SRC_UNLOCK (self);
      goto eat;
    }
    case GST_EVENT_SEGMENT:
    {
      GST_OBJECT_LOCK (aggpad);
      gst_event_copy_segment (event, &aggpad->segment);
      GST_OBJECT_UNLOCK (aggpad);

      GST_OBJECT_LOCK (self);
      self->priv->seqnum = gst_event_get_seqnum (event);
      GST_OBJECT_UNLOCK (self);
      goto eat;
    }
    case GST_EVENT_STREAM_START:
    {
      goto eat;
    }
    case GST_EVENT_GAP:
    {
      GstClockTime pts;
      GstClockTime duration;
      GstBuffer *gapbuf;

      gst_event_parse_gap (event, &pts, &duration);
      gapbuf = gst_buffer_new ();

      GST_BUFFER_PTS (gapbuf) = pts;
      GST_BUFFER_DURATION (gapbuf) = duration;
      GST_BUFFER_FLAG_SET (gapbuf, GST_BUFFER_FLAG_GAP);
      GST_BUFFER_FLAG_SET (gapbuf, GST_BUFFER_FLAG_DROPPABLE);

      if (gst_pad_chain (pad, gapbuf) != GST_FLOW_OK) {
        GST_WARNING_OBJECT (self, "Failed to chain gap buffer");
        res = FALSE;
      }

      goto eat;
    }
    case GST_EVENT_TAG:
    {
      GstTagList *tags;

      gst_event_parse_tag (event, &tags);

      if (gst_tag_list_get_scope (tags) == GST_TAG_SCOPE_STREAM) {
        gst_imxbp_aggregator_merge_tags (self, tags, GST_TAG_MERGE_REPLACE);
        gst_event_unref (event);
        event = NULL;
        goto eat;
      }
      break;
    }
    default:
    {
      break;
    }
  }

  GST_DEBUG_OBJECT (pad, "Forwarding event: %" GST_PTR_FORMAT, event);
  return gst_pad_event_default (pad, GST_OBJECT (self), event);

eat:
  GST_DEBUG_OBJECT (pad, "Eating event: %" GST_PTR_FORMAT, event);
  if (event)
    gst_event_unref (event);

  return res;
}

static inline gboolean
gst_imxbp_aggregator_stop_pad (GstImxBPAggregator * self, GstImxBPAggregatorPad * pad,
    G_GNUC_UNUSED gpointer unused_udata)
{
  gst_imxbp_aggregator_pad_flush (pad, self);

  return TRUE;
}

static gboolean
gst_imxbp_aggregator_stop (GstImxBPAggregator * agg)
{
  GstImxBPAggregatorClass *klass;
  gboolean result;

  gst_imxbp_aggregator_reset_flow_values (agg);

  gst_imxbp_aggregator_iterate_sinkpads (agg, gst_imxbp_aggregator_stop_pad, NULL);

  klass = GST_IMXBP_AGGREGATOR_GET_CLASS (agg);

  if (klass->stop)
    result = klass->stop (agg);
  else
    result = TRUE;

  agg->priv->has_peer_latency = FALSE;
  agg->priv->peer_latency_live = FALSE;
  agg->priv->peer_latency_min = agg->priv->peer_latency_max = FALSE;

  if (agg->priv->tags)
    gst_tag_list_unref (agg->priv->tags);
  agg->priv->tags = NULL;

  return result;
}

/* GstElement vmethods implementations */
static GstStateChangeReturn
gst_imxbp_aggregator_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstImxBPAggregator *self = GST_IMXBP_AGGREGATOR (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      if (!gst_imxbp_aggregator_start (self))
        goto error_start;
      break;
    default:
      break;
  }

  if ((ret =
          GST_ELEMENT_CLASS (aggregator_parent_class)->change_state (element,
              transition)) == GST_STATE_CHANGE_FAILURE)
    goto failure;


  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (!gst_imxbp_aggregator_stop (self)) {
        /* What to do in this case? Error out? */
        GST_ERROR_OBJECT (self, "Subclass failed to stop.");
      }
      break;
    default:
      break;
  }

  return ret;

/* ERRORS */
failure:
  {
    GST_ERROR_OBJECT (element, "parent failed state change");
    return ret;
  }
error_start:
  {
    GST_ERROR_OBJECT (element, "Subclass failed to start");
    return GST_STATE_CHANGE_FAILURE;
  }
}

static void
gst_imxbp_aggregator_release_pad (GstElement * element, GstPad * pad)
{
  GstImxBPAggregator *self = GST_IMXBP_AGGREGATOR (element);
  GstImxBPAggregatorPad *aggpad = GST_IMXBP_AGGREGATOR_PAD (pad);

  GST_INFO_OBJECT (pad, "Removing pad");

  SRC_LOCK (self);
  gst_imxbp_aggregator_pad_set_flushing (aggpad, GST_FLOW_FLUSHING);
  gst_element_remove_pad (element, pad);

  self->priv->has_peer_latency = FALSE;
  SRC_BROADCAST (self);
  SRC_UNLOCK (self);
}

static GstPad *
gst_imxbp_aggregator_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * req_name, G_GNUC_UNUSED const GstCaps * caps)
{
  GstImxBPAggregator *self;
  GstImxBPAggregatorPad *agg_pad;

  GstElementClass *klass = GST_ELEMENT_GET_CLASS (element);
  GstImxBPAggregatorPrivate *priv = GST_IMXBP_AGGREGATOR (element)->priv;

  self = GST_IMXBP_AGGREGATOR (element);

  if (templ == gst_element_class_get_pad_template (klass, "sink_%u")) {
    gint serial = 0;
    gchar *name = NULL;

    GST_OBJECT_LOCK (element);
    if (req_name == NULL || strlen (req_name) < 6
        || !g_str_has_prefix (req_name, "sink_")) {
      /* no name given when requesting the pad, use next available int */
      priv->padcount++;
    } else {
      /* parse serial number from requested padname */
      serial = g_ascii_strtoull (&req_name[5], NULL, 10);
      if (serial >= priv->padcount)
        priv->padcount = serial;
    }

    name = g_strdup_printf ("sink_%u", priv->padcount);
    agg_pad = g_object_new (GST_IMXBP_AGGREGATOR_GET_CLASS (self)->sinkpads_type,
        "name", name, "direction", GST_PAD_SINK, "template", templ, NULL);
    g_free (name);

    GST_OBJECT_UNLOCK (element);

  } else {
    return NULL;
  }

  GST_DEBUG_OBJECT (element, "Adding pad %s", GST_PAD_NAME (agg_pad));
  self->priv->has_peer_latency = FALSE;

  if (priv->running)
    gst_pad_set_active (GST_PAD (agg_pad), TRUE);

  /* add the pad to the element */
  gst_element_add_pad (element, GST_PAD (agg_pad));

  return GST_PAD (agg_pad);
}

/* Must be called with SRC_LOCK held */

static gboolean
gst_imxbp_aggregator_query_latency_unlocked (GstImxBPAggregator * self, GstQuery * query)
{
  gboolean query_ret, live;
  GstClockTime our_latency, min, max;

  query_ret = gst_pad_query_default (self->srcpad, GST_OBJECT (self), query);

  if (!query_ret) {
    GST_WARNING_OBJECT (self, "Latency query failed");
    return FALSE;
  }

  gst_query_parse_latency (query, &live, &min, &max);

  our_latency = self->priv->latency;

  if (G_UNLIKELY (!GST_CLOCK_TIME_IS_VALID (min))) {
    GST_ERROR_OBJECT (self, "Invalid minimum latency %" GST_TIME_FORMAT
        ". Please file a bug at " PACKAGE_BUGREPORT ".", GST_TIME_ARGS (min));
    return FALSE;
  }

  if (min > max && GST_CLOCK_TIME_IS_VALID (max)) {
    GST_ELEMENT_WARNING (self, CORE, CLOCK, (NULL),
        ("Impossible to configure latency: max %" GST_TIME_FORMAT " < min %"
            GST_TIME_FORMAT ". Add queues or other buffering elements.",
            GST_TIME_ARGS (max), GST_TIME_ARGS (min)));
    return FALSE;
  }

  self->priv->peer_latency_live = live;
  self->priv->peer_latency_min = min;
  self->priv->peer_latency_max = max;
  self->priv->has_peer_latency = TRUE;

  /* add our own */
  min += our_latency;
  min += self->priv->sub_latency_min;
  if (GST_CLOCK_TIME_IS_VALID (self->priv->sub_latency_max)
      && GST_CLOCK_TIME_IS_VALID (max))
    max += self->priv->sub_latency_max;
  else
    max = GST_CLOCK_TIME_NONE;

  if (live && min > max) {
    GST_ELEMENT_WARNING (self, CORE, NEGOTIATION,
        ("%s", "Latency too big"),
        ("The requested latency value is too big for the current pipeline. "
            "Limiting to %" G_GINT64_FORMAT, max));
    min = max;
    /* FIXME: This could in theory become negative, but in
     * that case all is lost anyway */
    self->priv->latency -= min - max;
    /* FIXME: shouldn't we g_object_notify() the change here? */
  }

  SRC_BROADCAST (self);

  GST_DEBUG_OBJECT (self, "configured latency live:%s min:%" G_GINT64_FORMAT
      " max:%" G_GINT64_FORMAT, live ? "true" : "false", min, max);

  gst_query_set_latency (query, live, min, max);

  return query_ret;
}

/*
 * MUST be called with the src_lock held.
 *
 * See  gst_imxbp_aggregator_get_latency() for doc
 */
static GstClockTime
gst_imxbp_aggregator_get_latency_unlocked (GstImxBPAggregator * self)
{
  GstClockTime latency;

  g_return_val_if_fail (GST_IS_AGGREGATOR (self), 0);

  if (!self->priv->has_peer_latency) {
    GstQuery *query = gst_query_new_latency ();
    gboolean ret;

    ret = gst_imxbp_aggregator_query_latency_unlocked (self, query);
    gst_query_unref (query);
    if (!ret)
      return GST_CLOCK_TIME_NONE;
  }

  if (!self->priv->has_peer_latency || !self->priv->peer_latency_live)
    return GST_CLOCK_TIME_NONE;

  /* latency_min is never GST_CLOCK_TIME_NONE by construction */
  latency = self->priv->peer_latency_min;

  /* add our own */
  latency += self->priv->latency;
  latency += self->priv->sub_latency_min;

  return latency;
}

/**
 * gst_imxbp_aggregator_get_latency:
 * @self: a #GstImxBPAggregator
 *
 * Retrieves the latency values reported by @self in response to the latency
 * query, or %GST_CLOCK_TIME_NONE if there is not live source connected and the element
 * will not wait for the clock.
 *
 * Typically only called by subclasses.
 *
 * Returns: The latency or %GST_CLOCK_TIME_NONE if the element does not sync
 */
GstClockTime
gst_imxbp_aggregator_get_latency (GstImxBPAggregator * self)
{
  GstClockTime ret;

  SRC_LOCK (self);
  ret = gst_imxbp_aggregator_get_latency_unlocked (self);
  SRC_UNLOCK (self);

  return ret;
}

static gboolean
gst_imxbp_aggregator_send_event (GstElement * element, GstEvent * event)
{
  GstImxBPAggregator *self = GST_IMXBP_AGGREGATOR (element);

  GST_STATE_LOCK (element);
  if (GST_EVENT_TYPE (event) == GST_EVENT_SEEK &&
      GST_STATE (element) < GST_STATE_PAUSED) {
    gdouble rate;
    GstFormat fmt;
    GstSeekFlags flags;
    GstSeekType start_type, stop_type;
    gint64 start, stop;

    gst_event_parse_seek (event, &rate, &fmt, &flags, &start_type,
        &start, &stop_type, &stop);

    GST_OBJECT_LOCK (self);
    gst_segment_do_seek (&self->segment, rate, fmt, flags, start_type, start,
        stop_type, stop, NULL);
    self->priv->seqnum = gst_event_get_seqnum (event);
    GST_OBJECT_UNLOCK (self);

    GST_DEBUG_OBJECT (element, "Storing segment %" GST_PTR_FORMAT, event);
  }
  GST_STATE_UNLOCK (element);


  return GST_ELEMENT_CLASS (aggregator_parent_class)->send_event (element,
      event);
}

static gboolean
gst_imxbp_aggregator_default_src_query (GstImxBPAggregator * self, GstQuery * query)
{
  gboolean res = TRUE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_SEEKING:
    {
      GstFormat format;

      /* don't pass it along as some (file)sink might claim it does
       * whereas with a collectpads in between that will not likely work */
      gst_query_parse_seeking (query, &format, NULL, NULL, NULL);
      gst_query_set_seeking (query, format, FALSE, 0, -1);
      res = TRUE;

      break;
    }
    case GST_QUERY_LATENCY:
      SRC_LOCK (self);
      res = gst_imxbp_aggregator_query_latency_unlocked (self, query);
      SRC_UNLOCK (self);
      break;
    default:
      return gst_pad_query_default (self->srcpad, GST_OBJECT (self), query);
  }

  return res;
}

static gboolean
gst_imxbp_aggregator_event_forward_func (GstPad * pad, gpointer user_data)
{
  EventData *evdata = user_data;
  gboolean ret = TRUE;
  GstPad *peer = gst_pad_get_peer (pad);
  GstImxBPAggregatorPad *aggpad = GST_IMXBP_AGGREGATOR_PAD (pad);

  if (peer) {
    ret = gst_pad_send_event (peer, gst_event_ref (evdata->event));
    GST_DEBUG_OBJECT (pad, "return of event push is %d", ret);
    gst_object_unref (peer);
  }

  if (ret == FALSE) {
    if (GST_EVENT_TYPE (evdata->event) == GST_EVENT_SEEK)
      GST_ERROR_OBJECT (pad, "Event %" GST_PTR_FORMAT " failed", evdata->event);

    if (GST_EVENT_TYPE (evdata->event) == GST_EVENT_SEEK) {
      GstQuery *seeking = gst_query_new_seeking (GST_FORMAT_TIME);

      if (gst_pad_query (peer, seeking)) {
        gboolean seekable;

        gst_query_parse_seeking (seeking, NULL, &seekable, NULL, NULL);

        if (seekable == FALSE) {
          GST_INFO_OBJECT (pad,
              "Source not seekable, We failed but it does not matter!");

          ret = TRUE;
        }
      } else {
        GST_ERROR_OBJECT (pad, "Query seeking FAILED");
      }

      gst_query_unref (seeking);
    }

    if (evdata->flush) {
      PAD_LOCK (aggpad);
      aggpad->priv->pending_flush_start = FALSE;
      aggpad->priv->pending_flush_stop = FALSE;
      PAD_UNLOCK (aggpad);
    }
  } else {
    evdata->one_actually_seeked = TRUE;
  }

  evdata->result &= ret;

  /* Always send to all pads */
  return FALSE;
}

static EventData
gst_imxbp_aggregator_forward_event_to_all_sinkpads (GstImxBPAggregator * self,
    GstEvent * event, gboolean flush)
{
  EventData evdata;

  evdata.event = event;
  evdata.result = TRUE;
  evdata.flush = flush;
  evdata.one_actually_seeked = FALSE;

  /* We first need to set all pads as flushing in a first pass
   * as flush_start flush_stop is sometimes sent synchronously
   * while we send the seek event */
  if (flush) {
    GList *l;

    GST_OBJECT_LOCK (self);
    for (l = GST_ELEMENT_CAST (self)->sinkpads; l != NULL; l = l->next) {
      GstImxBPAggregatorPad *pad = l->data;

      PAD_LOCK (pad);
      pad->priv->pending_flush_start = TRUE;
      pad->priv->pending_flush_stop = FALSE;
      PAD_UNLOCK (pad);
    }
    GST_OBJECT_UNLOCK (self);
  }

  gst_pad_forward (self->srcpad, gst_imxbp_aggregator_event_forward_func, &evdata);

  gst_event_unref (event);

  return evdata;
}

static gboolean
gst_imxbp_aggregator_do_seek (GstImxBPAggregator * self, GstEvent * event)
{
  gdouble rate;
  GstFormat fmt;
  GstSeekFlags flags;
  GstSeekType start_type, stop_type;
  gint64 start, stop;
  gboolean flush;
  EventData evdata;
  GstImxBPAggregatorPrivate *priv = self->priv;

  gst_event_parse_seek (event, &rate, &fmt, &flags, &start_type,
      &start, &stop_type, &stop);

  GST_INFO_OBJECT (self, "starting SEEK");

  flush = flags & GST_SEEK_FLAG_FLUSH;

  GST_OBJECT_LOCK (self);
  if (flush) {
    priv->pending_flush_start = TRUE;
    priv->flush_seeking = TRUE;
  }

  gst_segment_do_seek (&self->segment, rate, fmt, flags, start_type, start,
      stop_type, stop, NULL);
  GST_OBJECT_UNLOCK (self);

  /* forward the seek upstream */
  evdata = gst_imxbp_aggregator_forward_event_to_all_sinkpads (self, event, flush);
  event = NULL;

  if (!evdata.result || !evdata.one_actually_seeked) {
    GST_OBJECT_LOCK (self);
    priv->flush_seeking = FALSE;
    priv->pending_flush_start = FALSE;
    GST_OBJECT_UNLOCK (self);
  }

  GST_INFO_OBJECT (self, "seek done, result: %d", evdata.result);

  return evdata.result;
}

static gboolean
gst_imxbp_aggregator_default_src_event (GstImxBPAggregator * self, GstEvent * event)
{
  EventData evdata;
  gboolean res = TRUE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
    {
      gst_event_ref (event);
      res = gst_imxbp_aggregator_do_seek (self, event);
      gst_event_unref (event);
      event = NULL;
      goto done;
    }
    case GST_EVENT_NAVIGATION:
    {
      /* navigation is rather pointless. */
      res = FALSE;
      gst_event_unref (event);
      goto done;
    }
    default:
    {
      break;
    }
  }

  evdata = gst_imxbp_aggregator_forward_event_to_all_sinkpads (self, event, FALSE);
  res = evdata.result;

done:
  return res;
}

static gboolean
gst_imxbp_aggregator_src_pad_event_func (G_GNUC_UNUSED GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstImxBPAggregatorClass *klass = GST_IMXBP_AGGREGATOR_GET_CLASS (parent);

  return klass->src_event (GST_IMXBP_AGGREGATOR (parent), event);
}

static gboolean
gst_imxbp_aggregator_src_pad_query_func (G_GNUC_UNUSED GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  GstImxBPAggregatorClass *klass = GST_IMXBP_AGGREGATOR_GET_CLASS (parent);

  return klass->src_query (GST_IMXBP_AGGREGATOR (parent), query);
}

static gboolean
gst_imxbp_aggregator_src_pad_activate_mode_func (GstPad * pad,
    GstObject * parent, GstPadMode mode, gboolean active)
{
  GstImxBPAggregator *self = GST_IMXBP_AGGREGATOR (parent);
  GstImxBPAggregatorClass *klass = GST_IMXBP_AGGREGATOR_GET_CLASS (parent);

  if (klass->src_activate) {
    if (klass->src_activate (self, mode, active) == FALSE) {
      return FALSE;
    }
  }

  if (active == TRUE) {
    switch (mode) {
      case GST_PAD_MODE_PUSH:
      {
        GST_INFO_OBJECT (pad, "Activating pad!");
        gst_imxbp_aggregator_start_srcpad_task (self);
        return TRUE;
      }
      default:
      {
        GST_ERROR_OBJECT (pad, "Only supported mode is PUSH");
        return FALSE;
      }
    }
  }

  /* deactivating */
  GST_INFO_OBJECT (self, "Deactivating srcpad");
  gst_imxbp_aggregator_stop_srcpad_task (self, FALSE);

  return TRUE;
}

static gboolean
gst_imxbp_aggregator_default_sink_query (GstImxBPAggregator * self,
    GstImxBPAggregatorPad * aggpad, GstQuery * query)
{
  GstPad *pad = GST_PAD (aggpad);

  return gst_pad_query_default (pad, GST_OBJECT (self), query);
}

static void
gst_imxbp_aggregator_finalize (GObject * object)
{
  GstImxBPAggregator *self = (GstImxBPAggregator *) object;

  g_mutex_clear (&self->priv->src_lock);
  g_cond_clear (&self->priv->src_cond);

  G_OBJECT_CLASS (aggregator_parent_class)->finalize (object);
}

/*
 * gst_imxbp_aggregator_set_latency_property:
 * @agg: a #GstImxBPAggregator
 * @latency: the new latency value (in nanoseconds).
 *
 * Sets the new latency value to @latency. This value is used to limit the
 * amount of time a pad waits for data to appear before considering the pad
 * as unresponsive.
 */
static void
gst_imxbp_aggregator_set_latency_property (GstImxBPAggregator * self, gint64 latency)
{
  gboolean changed;
  GstClockTime min, max;

  g_return_if_fail (GST_IS_AGGREGATOR (self));
  g_return_if_fail (GST_CLOCK_TIME_IS_VALID (latency));

  SRC_LOCK (self);
  if (self->priv->peer_latency_live) {
    min = self->priv->peer_latency_min;
    max = self->priv->peer_latency_max;
    /* add our own */
    min += latency;
    min += self->priv->sub_latency_min;
    if (GST_CLOCK_TIME_IS_VALID (self->priv->sub_latency_max)
        && GST_CLOCK_TIME_IS_VALID (max))
      max += self->priv->sub_latency_max;
    else
      max = GST_CLOCK_TIME_NONE;

    if (GST_CLOCK_TIME_IS_VALID (max) && min > max) {
      GST_ELEMENT_WARNING (self, CORE, NEGOTIATION,
          ("%s", "Latency too big"),
          ("The requested latency value is too big for the latency in the "
              "current pipeline.  Limiting to %" G_GINT64_FORMAT, max));
      /* FIXME: This could in theory become negative, but in
       * that case all is lost anyway */
      latency -= min - max;
      /* FIXME: shouldn't we g_object_notify() the change here? */
    }
  }

  changed = (self->priv->latency != latency);
  self->priv->latency = latency;

  if (changed)
    SRC_BROADCAST (self);
  SRC_UNLOCK (self);

  if (changed)
    gst_element_post_message (GST_ELEMENT_CAST (self),
        gst_message_new_latency (GST_OBJECT_CAST (self)));
}

/*
 * gst_imxbp_aggregator_get_latency_property:
 * @agg: a #GstImxBPAggregator
 *
 * Gets the latency value. See gst_imxbp_aggregator_set_latency for
 * more details.
 *
 * Returns: The time in nanoseconds to wait for data to arrive on a sink pad 
 * before a pad is deemed unresponsive. A value of -1 means an
 * unlimited time.
 */
static gint64
gst_imxbp_aggregator_get_latency_property (GstImxBPAggregator * agg)
{
  gint64 res;

  g_return_val_if_fail (GST_IS_AGGREGATOR (agg), -1);

  GST_OBJECT_LOCK (agg);
  res = agg->priv->latency;
  GST_OBJECT_UNLOCK (agg);

  return res;
}

static void
gst_imxbp_aggregator_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstImxBPAggregator *agg = GST_IMXBP_AGGREGATOR (object);

  switch (prop_id) {
    case PROP_LATENCY:
      gst_imxbp_aggregator_set_latency_property (agg, g_value_get_int64 (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_imxbp_aggregator_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstImxBPAggregator *agg = GST_IMXBP_AGGREGATOR (object);

  switch (prop_id) {
    case PROP_LATENCY:
      g_value_set_int64 (value, gst_imxbp_aggregator_get_latency_property (agg));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* GObject vmethods implementations */
static void
gst_imxbp_aggregator_class_init (GstImxBPAggregatorClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;

  aggregator_parent_class = g_type_class_peek_parent (klass);
  g_type_class_add_private (klass, sizeof (GstImxBPAggregatorPrivate));

  GST_DEBUG_CATEGORY_INIT (aggregator_debug, "aggregator",
      GST_DEBUG_FG_MAGENTA, "GstImxBPAggregator");

  klass->sinkpads_type = GST_TYPE_AGGREGATOR_PAD;

  klass->sink_event = gst_imxbp_aggregator_default_sink_event;
  klass->sink_query = gst_imxbp_aggregator_default_sink_query;

  klass->src_event = gst_imxbp_aggregator_default_src_event;
  klass->src_query = gst_imxbp_aggregator_default_src_query;

  gstelement_class->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_imxbp_aggregator_request_new_pad);
  gstelement_class->send_event = GST_DEBUG_FUNCPTR (gst_imxbp_aggregator_send_event);
  gstelement_class->release_pad =
      GST_DEBUG_FUNCPTR (gst_imxbp_aggregator_release_pad);
  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_imxbp_aggregator_change_state);

  gobject_class->set_property = gst_imxbp_aggregator_set_property;
  gobject_class->get_property = gst_imxbp_aggregator_get_property;
  gobject_class->finalize = gst_imxbp_aggregator_finalize;

  g_object_class_install_property (gobject_class, PROP_LATENCY,
      g_param_spec_int64 ("latency", "Buffer latency",
          "Additional latency in live mode to allow upstream "
          "to take longer to produce buffers for the current "
          "position (in nanoseconds)", 0,
          (G_MAXLONG == G_MAXINT64) ? G_MAXINT64 : (G_MAXLONG * GST_SECOND - 1),
          DEFAULT_LATENCY, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  GST_DEBUG_REGISTER_FUNCPTR (gst_imxbp_aggregator_stop_pad);
}

static void
gst_imxbp_aggregator_init (GstImxBPAggregator * self, GstImxBPAggregatorClass * klass)
{
  GstPadTemplate *pad_template;
  GstImxBPAggregatorPrivate *priv;

  g_return_if_fail (klass->aggregate != NULL);

  self->priv =
      G_TYPE_INSTANCE_GET_PRIVATE (self, GST_TYPE_AGGREGATOR,
      GstImxBPAggregatorPrivate);

  priv = self->priv;

  pad_template =
      gst_element_class_get_pad_template (GST_ELEMENT_CLASS (klass), "src");
  g_return_if_fail (pad_template != NULL);

  priv->padcount = -1;
  priv->tags_changed = FALSE;

  self->priv->peer_latency_live = FALSE;
  self->priv->peer_latency_min = self->priv->sub_latency_min = 0;
  self->priv->peer_latency_max = self->priv->sub_latency_max = 0;
  self->priv->has_peer_latency = FALSE;
  gst_imxbp_aggregator_reset_flow_values (self);

  self->srcpad = gst_pad_new_from_template (pad_template, "src");

  gst_pad_set_event_function (self->srcpad,
      GST_DEBUG_FUNCPTR (gst_imxbp_aggregator_src_pad_event_func));
  gst_pad_set_query_function (self->srcpad,
      GST_DEBUG_FUNCPTR (gst_imxbp_aggregator_src_pad_query_func));
  gst_pad_set_activatemode_function (self->srcpad,
      GST_DEBUG_FUNCPTR (gst_imxbp_aggregator_src_pad_activate_mode_func));

  gst_element_add_pad (GST_ELEMENT (self), self->srcpad);

  self->priv->latency = DEFAULT_LATENCY;

  g_mutex_init (&self->priv->src_lock);
  g_cond_init (&self->priv->src_cond);
}

/* we can't use G_DEFINE_ABSTRACT_TYPE because we need the klass in the _init
 * method to get to the padtemplates */
GType
gst_imxbp_aggregator_get_type (void)
{
  static volatile gsize type = 0;

  if (g_once_init_enter (&type)) {
    GType _type;
    static const GTypeInfo info = {
      sizeof (GstImxBPAggregatorClass),
      NULL,
      NULL,
      (GClassInitFunc) gst_imxbp_aggregator_class_init,
      NULL,
      NULL,
      sizeof (GstImxBPAggregator),
      0,
      (GInstanceInitFunc) gst_imxbp_aggregator_init,
      NULL
    };

    _type = g_type_register_static (GST_TYPE_ELEMENT,
        "GstImxBPAggregator", &info, G_TYPE_FLAG_ABSTRACT);
    g_once_init_leave (&type, _type);
  }
  return type;
}

static GstFlowReturn
gst_imxbp_aggregator_pad_chain (GstPad * pad, GstObject * object, GstBuffer * buffer)
{
  GstBuffer *actual_buf = buffer;
  GstImxBPAggregator *self = GST_IMXBP_AGGREGATOR (object);
  GstImxBPAggregatorPad *aggpad = GST_IMXBP_AGGREGATOR_PAD (pad);
  GstImxBPAggregatorClass *aggclass = GST_IMXBP_AGGREGATOR_GET_CLASS (object);
  GstFlowReturn flow_return;

  GST_DEBUG_OBJECT (aggpad, "Start chaining a buffer %" GST_PTR_FORMAT, buffer);

  PAD_FLUSH_LOCK (aggpad);

  PAD_LOCK (aggpad);
  flow_return = aggpad->priv->flow_return;
  if (flow_return != GST_FLOW_OK)
    goto flushing;

  if (aggpad->priv->pending_eos == TRUE)
    goto eos;

  while (aggpad->priv->buffer && aggpad->priv->flow_return == GST_FLOW_OK)
    PAD_WAIT_EVENT (aggpad);

  flow_return = aggpad->priv->flow_return;
  if (flow_return != GST_FLOW_OK)
    goto flushing;

  PAD_UNLOCK (aggpad);

  if (aggclass->clip) {
    aggclass->clip (self, aggpad, buffer, &actual_buf);
  }

  SRC_LOCK (self);
  PAD_LOCK (aggpad);
  if (aggpad->priv->buffer)
    gst_buffer_unref (aggpad->priv->buffer);
  aggpad->priv->buffer = actual_buf;

  flow_return = aggpad->priv->flow_return;

  PAD_UNLOCK (aggpad);
  PAD_FLUSH_UNLOCK (aggpad);

  SRC_BROADCAST (self);
  SRC_UNLOCK (self);

  GST_DEBUG_OBJECT (aggpad, "Done chaining");

  return flow_return;

flushing:
  PAD_UNLOCK (aggpad);
  PAD_FLUSH_UNLOCK (aggpad);

  gst_buffer_unref (buffer);
  GST_DEBUG_OBJECT (aggpad, "Pad is %s, dropping buffer",
      gst_flow_get_name (flow_return));

  return flow_return;

eos:
  PAD_UNLOCK (aggpad);
  PAD_FLUSH_UNLOCK (aggpad);

  gst_buffer_unref (buffer);
  GST_DEBUG_OBJECT (pad, "We are EOS already...");

  return GST_FLOW_EOS;
}

static gboolean
gst_imxbp_aggregator_pad_query_func (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  GstImxBPAggregatorPad *aggpad = GST_IMXBP_AGGREGATOR_PAD (pad);
  GstImxBPAggregatorClass *klass = GST_IMXBP_AGGREGATOR_GET_CLASS (parent);

  if (GST_QUERY_IS_SERIALIZED (query)) {
    PAD_LOCK (aggpad);

    while (aggpad->priv->buffer && aggpad->priv->flow_return == GST_FLOW_OK)
      PAD_WAIT_EVENT (aggpad);

    if (aggpad->priv->flow_return != GST_FLOW_OK)
      goto flushing;

    PAD_UNLOCK (aggpad);
  }

  return klass->sink_query (GST_IMXBP_AGGREGATOR (parent),
      GST_IMXBP_AGGREGATOR_PAD (pad), query);

flushing:
  GST_DEBUG_OBJECT (aggpad, "Pad is %s, dropping query",
      gst_flow_get_name (aggpad->priv->flow_return));
  PAD_UNLOCK (aggpad);
  return FALSE;
}

static gboolean
gst_imxbp_aggregator_pad_event_func (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstImxBPAggregatorPad *aggpad = GST_IMXBP_AGGREGATOR_PAD (pad);
  GstImxBPAggregatorClass *klass = GST_IMXBP_AGGREGATOR_GET_CLASS (parent);

  if (GST_EVENT_IS_SERIALIZED (event) && GST_EVENT_TYPE (event) != GST_EVENT_EOS
      && GST_EVENT_TYPE (event) != GST_EVENT_SEGMENT_DONE) {
    PAD_LOCK (aggpad);


    while (aggpad->priv->buffer && aggpad->priv->flow_return == GST_FLOW_OK)
      PAD_WAIT_EVENT (aggpad);

    if (aggpad->priv->flow_return != GST_FLOW_OK
        && GST_EVENT_TYPE (event) != GST_EVENT_FLUSH_STOP)
      goto flushing;

    PAD_UNLOCK (aggpad);
  }

  return klass->sink_event (GST_IMXBP_AGGREGATOR (parent),
      GST_IMXBP_AGGREGATOR_PAD (pad), event);

flushing:
  GST_DEBUG_OBJECT (aggpad, "Pad is %s, dropping event",
      gst_flow_get_name (aggpad->priv->flow_return));
  PAD_UNLOCK (aggpad);
  if (GST_EVENT_IS_STICKY (event))
    gst_pad_store_sticky_event (pad, event);
  gst_event_unref (event);
  return FALSE;
}

static gboolean
gst_imxbp_aggregator_pad_activate_mode_func (GstPad * pad,
    G_GNUC_UNUSED GstObject * parent, G_GNUC_UNUSED GstPadMode mode, gboolean active)
{
  GstImxBPAggregatorPad *aggpad = GST_IMXBP_AGGREGATOR_PAD (pad);

  if (active == FALSE) {
    gst_imxbp_aggregator_pad_set_flushing (aggpad, GST_FLOW_FLUSHING);
  } else {
    PAD_LOCK (aggpad);
    aggpad->priv->flow_return = GST_FLOW_OK;
    PAD_BROADCAST_EVENT (aggpad);
    PAD_UNLOCK (aggpad);
  }

  return TRUE;
}

/***********************************
 * GstImxBPAggregatorPad implementation  *
 ************************************/
G_DEFINE_TYPE (GstImxBPAggregatorPad, gst_imxbp_aggregator_pad, GST_TYPE_PAD);

static void
gst_imxbp_aggregator_pad_constructed (GObject * object)
{
  GstPad *pad = GST_PAD (object);

  gst_pad_set_chain_function (pad,
      GST_DEBUG_FUNCPTR (gst_imxbp_aggregator_pad_chain));
  gst_pad_set_event_function (pad,
      GST_DEBUG_FUNCPTR (gst_imxbp_aggregator_pad_event_func));
  gst_pad_set_query_function (pad,
      GST_DEBUG_FUNCPTR (gst_imxbp_aggregator_pad_query_func));
  gst_pad_set_activatemode_function (pad,
      GST_DEBUG_FUNCPTR (gst_imxbp_aggregator_pad_activate_mode_func));
}

static void
gst_imxbp_aggregator_pad_finalize (GObject * object)
{
  GstImxBPAggregatorPad *pad = (GstImxBPAggregatorPad *) object;

  g_cond_clear (&pad->priv->event_cond);
  g_mutex_clear (&pad->priv->flush_lock);
  g_mutex_clear (&pad->priv->lock);

  G_OBJECT_CLASS (gst_imxbp_aggregator_pad_parent_class)->finalize (object);
}

static void
gst_imxbp_aggregator_pad_dispose (GObject * object)
{
  GstImxBPAggregatorPad *pad = (GstImxBPAggregatorPad *) object;

  gst_imxbp_aggregator_pad_drop_buffer (pad);

  G_OBJECT_CLASS (gst_imxbp_aggregator_pad_parent_class)->dispose (object);
}

static void
gst_imxbp_aggregator_pad_class_init (GstImxBPAggregatorPadClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  g_type_class_add_private (klass, sizeof (GstImxBPAggregatorPadPrivate));

  gobject_class->constructed = gst_imxbp_aggregator_pad_constructed;
  gobject_class->finalize = gst_imxbp_aggregator_pad_finalize;
  gobject_class->dispose = gst_imxbp_aggregator_pad_dispose;
}

static void
gst_imxbp_aggregator_pad_init (GstImxBPAggregatorPad * pad)
{
  pad->priv =
      G_TYPE_INSTANCE_GET_PRIVATE (pad, GST_TYPE_AGGREGATOR_PAD,
      GstImxBPAggregatorPadPrivate);

  pad->priv->buffer = NULL;
  g_cond_init (&pad->priv->event_cond);

  g_mutex_init (&pad->priv->flush_lock);
  g_mutex_init (&pad->priv->lock);
}

/**
 * gst_imxbp_aggregator_pad_steal_buffer:
 * @pad: the pad to get buffer from
 *
 * Steal the ref to the buffer currently queued in @pad.
 *
 * Returns: (transfer full): The buffer in @pad or NULL if no buffer was
 *   queued. You should unref the buffer after usage.
 */
GstBuffer *
gst_imxbp_aggregator_pad_steal_buffer (GstImxBPAggregatorPad * pad)
{
  GstBuffer *buffer = NULL;

  PAD_LOCK (pad);
  if (pad->priv->buffer) {
    GST_TRACE_OBJECT (pad, "Consuming buffer");
    buffer = pad->priv->buffer;
    pad->priv->buffer = NULL;
    if (pad->priv->pending_eos) {
      pad->priv->pending_eos = FALSE;
      pad->priv->eos = TRUE;
    }
    PAD_BROADCAST_EVENT (pad);
    GST_DEBUG_OBJECT (pad, "Consumed: %" GST_PTR_FORMAT, buffer);
  }
  PAD_UNLOCK (pad);

  return buffer;
}

/**
 * gst_imxbp_aggregator_pad_drop_buffer:
 * @pad: the pad where to drop any pending buffer
 *
 * Drop the buffer currently queued in @pad.
 *
 * Returns: TRUE if there was a buffer queued in @pad, or FALSE if not.
 */
gboolean
gst_imxbp_aggregator_pad_drop_buffer (GstImxBPAggregatorPad * pad)
{
  GstBuffer *buf;

  buf = gst_imxbp_aggregator_pad_steal_buffer (pad);

  if (buf == NULL)
    return FALSE;

  gst_buffer_unref (buf);
  return TRUE;
}

/**
 * gst_imxbp_aggregator_pad_get_buffer:
 * @pad: the pad to get buffer from
 *
 * Returns: (transfer full): A reference to the buffer in @pad or
 * NULL if no buffer was queued. You should unref the buffer after
 * usage.
 */
GstBuffer *
gst_imxbp_aggregator_pad_get_buffer (GstImxBPAggregatorPad * pad)
{
  GstBuffer *buffer = NULL;

  PAD_LOCK (pad);
  if (pad->priv->buffer)
    buffer = gst_buffer_ref (pad->priv->buffer);
  PAD_UNLOCK (pad);

  return buffer;
}

gboolean
gst_imxbp_aggregator_pad_is_eos (GstImxBPAggregatorPad * pad)
{
  gboolean is_eos;

  PAD_LOCK (pad);
  is_eos = pad->priv->eos;
  PAD_UNLOCK (pad);

  return is_eos;
}

/**
 * gst_imxbp_aggregator_merge_tags:
 * @self: a #GstImxBPAggregator
 * @tags: a #GstTagList to merge
 * @mode: the #GstTagMergeMode to use
 *
 * Adds tags to so-called pending tags, which will be processed
 * before pushing out data downstream.
 *
 * Note that this is provided for convenience, and the subclass is
 * not required to use this and can still do tag handling on its own.
 *
 * MT safe.
 */
void
gst_imxbp_aggregator_merge_tags (GstImxBPAggregator * self,
    const GstTagList * tags, GstTagMergeMode mode)
{
  GstTagList *otags;

  g_return_if_fail (GST_IS_AGGREGATOR (self));
  g_return_if_fail (tags == NULL || GST_IS_TAG_LIST (tags));

  /* FIXME Check if we can use OBJECT lock here! */
  GST_OBJECT_LOCK (self);
  if (tags)
    GST_DEBUG_OBJECT (self, "merging tags %" GST_PTR_FORMAT, tags);
  otags = self->priv->tags;
  self->priv->tags = gst_tag_list_merge (self->priv->tags, tags, mode);
  if (otags)
    gst_tag_list_unref (otags);
  self->priv->tags_changed = TRUE;
  GST_OBJECT_UNLOCK (self);
}

/**
 * gst_imxbp_aggregator_set_latency:
 * @self: a #GstImxBPAggregator
 * @min_latency: minimum latency
 * @max_latency: maximum latency
 *
 * Lets #GstImxBPAggregator sub-classes tell the baseclass what their internal
 * latency is. Will also post a LATENCY message on the bus so the pipeline
 * can reconfigure its global latency.
 */
void
gst_imxbp_aggregator_set_latency (GstImxBPAggregator * self,
    GstClockTime min_latency, GstClockTime max_latency)
{
  gboolean changed = FALSE;

  g_return_if_fail (GST_IS_AGGREGATOR (self));
  g_return_if_fail (GST_CLOCK_TIME_IS_VALID (min_latency));
  g_return_if_fail (max_latency >= min_latency);

  SRC_LOCK (self);
  if (self->priv->sub_latency_min != min_latency) {
    self->priv->sub_latency_min = min_latency;
    changed = TRUE;
  }
  if (self->priv->sub_latency_max != max_latency) {
    self->priv->sub_latency_max = max_latency;
    changed = TRUE;
  }

  if (changed)
    SRC_BROADCAST (self);
  SRC_UNLOCK (self);

  if (changed) {
    gst_element_post_message (GST_ELEMENT_CAST (self),
        gst_message_new_latency (GST_OBJECT_CAST (self)));
  }
}