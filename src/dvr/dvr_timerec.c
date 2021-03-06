/*
 *  tvheadend, Automatic time-based recording
 *  Copyright (C) 2014 Jaroslav Kysela <perex@perex.cz>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>
#include <ctype.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#include "tvheadend.h"
#include "settings.h"
#include "dvr.h"
#include "epg.h"

struct dvr_timerec_entry_queue timerec_entries;

static gtimer_t dvr_timerec_timer;

/**
 *
 */
static time_t
dvr_timerec_timecorrection(time_t clk, int hm, struct tm *tm)
{
  time_t r;
  int isdst;

  localtime_r(&clk, tm);
  tm->tm_min = hm % 60;
  tm->tm_hour = hm / 60;
  tm->tm_sec = 0;
  isdst = tm->tm_isdst;
  r = mktime(tm);
  if (tm->tm_isdst != isdst) {
    tm->tm_min = hm % 60;
    tm->tm_hour = hm / 60;
    tm->tm_sec = 0;
    r = mktime(tm);
  }
  return r;
}

/**
 * Unlink - and remove any unstarted
 */
static void
dvr_timerec_purge_spawn(dvr_timerec_entry_t *dte, int delconf)
{
  dvr_entry_t *de = dte->dte_spawn;

  if (de && de->de_timerec) {
    dte->dte_spawn = NULL;
    de->de_timerec = NULL;
    if (delconf) {
      if (de->de_sched_state == DVR_SCHEDULED)
        dvr_entry_cancel(de);
      else
        dvr_entry_save(de);
    }
  }
}

/**
 * Title
 */
static const char *
dvr_timerec_title(dvr_timerec_entry_t *dte, struct tm *start)
{
  static char buf[256];
  size_t len;

  if (dte->dte_title == NULL)
    return "Unknown";
  len = strftime(buf, sizeof(buf) - 1, dte->dte_title, start);
  buf[len] = '\0';
  return buf;
}

/**
 * handle the timerec entry
 */
void
dvr_timerec_check(dvr_timerec_entry_t *dte)
{
  dvr_entry_t *de;
  time_t start, stop, limit;
  struct tm tm_start, tm_stop;
  const char *title;
  char buf[200];

  if(dte->dte_enabled == 0 || dte->dte_weekdays == 0)
    goto fail;
  if(dte->dte_start < 0 || dte->dte_start >= 24*60 ||
     dte->dte_stop < 0 || dte->dte_stop >= 24*60)
    goto fail;
  if(dte->dte_channel == NULL)
    goto fail;

  limit = dispatch_clock - 600;
  start = dvr_timerec_timecorrection(dispatch_clock, dte->dte_start, &tm_start);
  stop  = dvr_timerec_timecorrection(dispatch_clock, dte->dte_stop,  &tm_stop);
  if (start < limit && stop < limit) {
    /* next day */
    start = dvr_timerec_timecorrection(dispatch_clock + 24*60*60,
                                       dte->dte_start,
                                       &tm_start);
    stop  = dvr_timerec_timecorrection(dispatch_clock + 24*60*60,
                                       dte->dte_stop,
                                       &tm_stop);
  }
  /* day boundary correction */
  if (start > stop)
    stop += 24 * 60 * 60;
  assert(start < stop);

  if(dte->dte_weekdays != 0x7f) {
    localtime_r(&start, &tm_start);
    if(!((1 << ((tm_start.tm_wday ?: 7) - 1)) & dte->dte_weekdays))
      goto fail;
  }

  /* purge the old entry */
  de = dte->dte_spawn;
  if (de) {
    if (de->de_start == start && de->de_stop == stop)
      return;
    dvr_timerec_purge_spawn(dte, 1);
  }

  title = dvr_timerec_title(dte, &tm_start);
  snprintf(buf, sizeof(buf), "Time recording%s%s",
           dte->dte_creator ? " by: " : "",
           dte->dte_creator ?: "");
  de = dvr_entry_create_(idnode_uuid_as_str(&dte->dte_config->dvr_id),
                         NULL, dte->dte_channel,
                         start, stop, 0, 0, title,
                         NULL, NULL, NULL, buf,
                         NULL, dte, dte->dte_pri, dte->dte_retention);

  return;

fail:
  dvr_timerec_purge_spawn(dte, 1);
}

/**
 *
 */
dvr_timerec_entry_t *
dvr_timerec_create(const char *uuid, htsmsg_t *conf)
{
  dvr_timerec_entry_t *dte;

  dte = calloc(1, sizeof(*dte));

  if (idnode_insert(&dte->dte_id, uuid, &dvr_timerec_entry_class, 0)) {
    if (uuid)
      tvhwarn("dvr", "invalid timerec entry uuid '%s'", uuid);
    free(dte);
    return NULL;
  }

  dte->dte_title = strdup("Time-%x-%R");
  dte->dte_weekdays = 0x7f;
  dte->dte_pri = DVR_PRIO_NORMAL;
  dte->dte_start = -1;
  dte->dte_stop = -1;
  dte->dte_config = dvr_config_find_by_name_default(NULL);
  LIST_INSERT_HEAD(&dte->dte_config->dvr_timerec_entries, dte, dte_config_link);

  TAILQ_INSERT_TAIL(&timerec_entries, dte, dte_link);

  idnode_load(&dte->dte_id, conf);

  return dte;
}

/**
 *
 */
static void
timerec_entry_destroy(dvr_timerec_entry_t *dte, int delconf)
{
  dvr_timerec_purge_spawn(dte, delconf);

  if (delconf)
    hts_settings_remove("dvr/timerec/%s", idnode_uuid_as_str(&dte->dte_id));

  TAILQ_REMOVE(&timerec_entries, dte, dte_link);
  idnode_unlink(&dte->dte_id);

  if(dte->dte_config != NULL)
    LIST_REMOVE(dte, dte_config_link);

  free(dte->dte_name);
  free(dte->dte_title);
  free(dte->dte_creator);
  free(dte->dte_comment);

  if(dte->dte_channel != NULL)
    LIST_REMOVE(dte, dte_channel_link);

  free(dte);
}

/**
 *
 */
void
dvr_timerec_save(dvr_timerec_entry_t *dte)
{
  htsmsg_t *m = htsmsg_create_map();

  lock_assert(&global_lock);

  idnode_save(&dte->dte_id, m);
  hts_settings_save(m, "dvr/timerec/%s", idnode_uuid_as_str(&dte->dte_id));
  htsmsg_destroy(m);
}

/* **************************************************************************
 * DVR Autorec Entry Class definition
 * **************************************************************************/

static void
dvr_timerec_entry_class_save(idnode_t *self)
{
  dvr_timerec_entry_t *dte = (dvr_timerec_entry_t *)self;
  dvr_timerec_save(dte);
  dvr_timerec_check(dte);
}

static void
dvr_timerec_entry_class_delete(idnode_t *self)
{
  timerec_entry_destroy((dvr_timerec_entry_t *)self, 1);
}

static const char *
dvr_timerec_entry_class_get_title (idnode_t *self)
{
  dvr_timerec_entry_t *dte = (dvr_timerec_entry_t *)self;
  const char *s = "";
  if (dte->dte_name && dte->dte_name[0] != '\0')
    s = dte->dte_name;
  else if (dte->dte_comment && dte->dte_comment[0] != '\0')
    s = dte->dte_comment;
  return s;
}

static int
dvr_timerec_entry_class_channel_set(void *o, const void *v)
{
  dvr_timerec_entry_t *dte = (dvr_timerec_entry_t *)o;
  channel_t *ch = v ? channel_find_by_uuid(v) : NULL;
  if (ch == NULL) ch = v ? channel_find_by_name(v) : NULL;
  if (ch == NULL) {
    if (dte->dte_channel) {
      LIST_REMOVE(dte, dte_channel_link);
      dte->dte_channel = NULL;
      return 1;
    }
  } else if (dte->dte_channel != ch) {
    if (dte->dte_channel)
      LIST_REMOVE(dte, dte_channel_link);
    dte->dte_channel = ch;
    LIST_INSERT_HEAD(&ch->ch_timerecs, dte, dte_channel_link);
    return 1;
  }
  return 0;
}

static const void *
dvr_timerec_entry_class_channel_get(void *o)
{
  static const char *ret;
  dvr_timerec_entry_t *dte = (dvr_timerec_entry_t *)o;
  if (dte->dte_channel)
    ret = idnode_uuid_as_str(&dte->dte_channel->ch_id);
  else
    ret = "";
  return &ret;
}

static char *
dvr_timerec_entry_class_channel_rend(void *o)
{
  dvr_timerec_entry_t *dte = (dvr_timerec_entry_t *)o;
  if (dte->dte_channel)
    return strdup(channel_get_name(dte->dte_channel));
  return NULL;
}

static int
dvr_timerec_entry_class_time_set(void *o, const void *v, int *tm)
{
  const char *s = v;
  int t;

  if(s == NULL || s[0] == '\0' || !isdigit(s[0]))
    t = -1;
  else if(strchr(s, ':') != NULL)
    // formatted time string - convert
    t = (atoi(s) * 60) + atoi(s + 3);
  else {
    t = atoi(s);
  }
  if (t >= 24 * 60)
    t = -1;
  if (t != *tm) {
    *tm = t;
    return 1;
  }
  return 0;
}

static int
dvr_timerec_entry_class_start_set(void *o, const void *v)
{
  dvr_timerec_entry_t *dte = (dvr_timerec_entry_t *)o;
  return dvr_timerec_entry_class_time_set(o, v, &dte->dte_start);
}

static int
dvr_timerec_entry_class_stop_set(void *o, const void *v)
{
  dvr_timerec_entry_t *dte = (dvr_timerec_entry_t *)o;
  return dvr_timerec_entry_class_time_set(o, v, &dte->dte_stop);
}

static const void *
dvr_timerec_entry_class_time_get(void *o, int tm)
{
  static const char *ret;
  static char buf[16];
  if (tm >= 0)
    snprintf(buf, sizeof(buf), "%02d:%02d", tm / 60, tm % 60);
  else
    strcpy(buf, "Any");
  ret = buf;
  return &ret;
}

static const void *
dvr_timerec_entry_class_start_get(void *o)
{
  dvr_timerec_entry_t *dte = (dvr_timerec_entry_t *)o;
  return dvr_timerec_entry_class_time_get(o, dte->dte_start);
}

static const void *
dvr_timerec_entry_class_stop_get(void *o)
{
  dvr_timerec_entry_t *dte = (dvr_timerec_entry_t *)o;
  return dvr_timerec_entry_class_time_get(o, dte->dte_stop);
}

static htsmsg_t *
dvr_timerec_entry_class_time_list(void *o)
{
  return dvr_autorec_entry_class_time_list(o, "Invalid");
}

static int
dvr_timerec_entry_class_config_name_set(void *o, const void *v)
{
  dvr_timerec_entry_t *dte = (dvr_timerec_entry_t *)o;
  dvr_config_t *cfg = v ? dvr_config_find_by_uuid(v) : NULL;
  if (cfg == NULL) cfg = v ? dvr_config_find_by_name_default(v): NULL;
  if (cfg == NULL && dte->dte_config) {
    dte->dte_config = NULL;
    LIST_REMOVE(dte, dte_config_link);
    return 1;
  } else if (cfg != dte->dte_config) {
    if (dte->dte_config)
      LIST_REMOVE(dte, dte_config_link);
    LIST_INSERT_HEAD(&cfg->dvr_timerec_entries, dte, dte_config_link);
    dte->dte_config = cfg;
    return 1;
  }
  return 0;
}

static const void *
dvr_timerec_entry_class_config_name_get(void *o)
{
  static const char *buf;
  dvr_timerec_entry_t *dte = (dvr_timerec_entry_t *)o;
  if (dte->dte_config)
    buf = idnode_uuid_as_str(&dte->dte_config->dvr_id);
  else
    buf = "";
  return &buf;
}

static char *
dvr_timerec_entry_class_config_name_rend(void *o)
{
  dvr_timerec_entry_t *dte = (dvr_timerec_entry_t *)o;
  if (dte->dte_config)
    return strdup(dte->dte_config->dvr_config_name);
  return NULL;
}

static int
dvr_timerec_entry_class_weekdays_set(void *o, const void *v)
{
  dvr_timerec_entry_t *dte = (dvr_timerec_entry_t *)o;
  htsmsg_field_t *f;
  uint32_t u32, bits = 0;

  HTSMSG_FOREACH(f, (htsmsg_t *)v)
    if (!htsmsg_field_get_u32(f, &u32) && u32 > 0 && u32 < 8)
      bits |= (1 << (u32 - 1));

  if (bits != dte->dte_weekdays) {
    dte->dte_weekdays = bits;
    return 1;
  }
  return 0;
}

static const void *
dvr_timerec_entry_class_weekdays_get(void *o)
{
  dvr_timerec_entry_t *dte = (dvr_timerec_entry_t *)o;
  return dvr_autorec_entry_class_weekdays_get(dte->dte_weekdays);
}

static htsmsg_t *
dvr_timerec_entry_class_weekdays_default(void)
{
  return dvr_autorec_entry_class_weekdays_get(0x7f);
}

static char *
dvr_timerec_entry_class_weekdays_rend(void *o)
{
  dvr_timerec_entry_t *dte = (dvr_timerec_entry_t *)o;
  return dvr_autorec_entry_class_weekdays_rend(dte->dte_weekdays);
}

const idclass_t dvr_timerec_entry_class = {
  .ic_class      = "dvrtimerec",
  .ic_caption    = "DVR Time-Record Entry",
  .ic_event      = "dvrtimerec",
  .ic_save       = dvr_timerec_entry_class_save,
  .ic_get_title  = dvr_timerec_entry_class_get_title,
  .ic_delete     = dvr_timerec_entry_class_delete,
  .ic_properties = (const property_t[]) {
    {
      .type     = PT_BOOL,
      .id       = "enabled",
      .name     = "Enabled",
      .off      = offsetof(dvr_timerec_entry_t, dte_enabled),
    },
    {
      .type     = PT_STR,
      .id       = "name",
      .name     = "Name",
      .off      = offsetof(dvr_timerec_entry_t, dte_name),
    },
    {
      .type     = PT_STR,
      .id       = "title",
      .name     = "Title",
      .off      = offsetof(dvr_timerec_entry_t, dte_title),
      .def.s    = "Time-%x-%R",
    },
    {
      .type     = PT_STR,
      .id       = "channel",
      .name     = "Channel",
      .set      = dvr_timerec_entry_class_channel_set,
      .get      = dvr_timerec_entry_class_channel_get,
      .rend     = dvr_timerec_entry_class_channel_rend,
      .list     = channel_class_get_list,
    },
    {
      .type     = PT_STR,
      .id       = "start",
      .name     = "Start",
      .set      = dvr_timerec_entry_class_start_set,
      .get      = dvr_timerec_entry_class_start_get,
      .list     = dvr_timerec_entry_class_time_list,
      .def.s    = "12:00",
      .opts     = PO_SORTKEY,
    },
    {
      .type     = PT_STR,
      .id       = "stop",
      .name     = "Stop",
      .set      = dvr_timerec_entry_class_stop_set,
      .get      = dvr_timerec_entry_class_stop_get,
      .list     = dvr_timerec_entry_class_time_list,
      .def.s    = "12:00",
      .opts     = PO_SORTKEY,
    },
    {
      .type     = PT_U32,
      .islist   = 1,
      .id       = "weekdays",
      .name     = "Week Days",
      .set      = dvr_timerec_entry_class_weekdays_set,
      .get      = dvr_timerec_entry_class_weekdays_get,
      .list     = dvr_autorec_entry_class_weekdays_list,
      .rend     = dvr_timerec_entry_class_weekdays_rend,
      .def.list = dvr_timerec_entry_class_weekdays_default
    },
    {
      .type     = PT_U32,
      .id       = "pri",
      .name     = "Priority",
      .list     = dvr_entry_class_pri_list,
      .def.i    = DVR_PRIO_NORMAL,
      .off      = offsetof(dvr_timerec_entry_t, dte_pri),
      .opts     = PO_SORTKEY,
    },
    {
      .type     = PT_INT,
      .id       = "retention",
      .name     = "Retention",
      .off      = offsetof(dvr_timerec_entry_t, dte_retention),
    },
    {
      .type     = PT_STR,
      .id       = "config_name",
      .name     = "DVR Configuration",
      .set      = dvr_timerec_entry_class_config_name_set,
      .get      = dvr_timerec_entry_class_config_name_get,
      .rend     = dvr_timerec_entry_class_config_name_rend,
      .list     = dvr_entry_class_config_name_list,
    },
    {
      .type     = PT_STR,
      .id       = "creator",
      .name     = "Creator",
      .off      = offsetof(dvr_timerec_entry_t, dte_creator),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_STR,
      .id       = "comment",
      .name     = "Comment",
      .off      = offsetof(dvr_timerec_entry_t, dte_comment),
    },
    {}
  }
};

/**
 *
 */
void
dvr_timerec_init(void)
{
  htsmsg_t *l, *c;
  htsmsg_field_t *f;

  TAILQ_INIT(&timerec_entries);
  if((l = hts_settings_load("dvr/timerec")) != NULL) {
    HTSMSG_FOREACH(f, l) {
      if((c = htsmsg_get_map_by_field(f)) == NULL)
        continue;
      (void)dvr_timerec_create(f->hmf_name, c);
    }
    htsmsg_destroy(l);
  }
}

void
dvr_timerec_done(void)
{
  dvr_timerec_entry_t *dte;

  pthread_mutex_lock(&global_lock);
  while ((dte = TAILQ_FIRST(&timerec_entries)) != NULL)
    timerec_entry_destroy(dte, 0);
  pthread_mutex_unlock(&global_lock);
}

static void
dvr_timerec_timer_cb(void *aux)
{
  dvr_timerec_entry_t *dte;

  tvhtrace("dvr", "timerec update");

  /* check all entries */
  TAILQ_FOREACH(dte, &timerec_entries, dte_link)
    dvr_timerec_check(dte);

  /* load the timer */
  gtimer_arm(&dvr_timerec_timer, dvr_timerec_timer_cb, NULL, 3550);
}

void
dvr_timerec_update(void)
{
  /* check all timerec entries and load the timer */
  dvr_timerec_timer_cb(NULL);
}

/**
 *
 */
void
timerec_destroy_by_channel(channel_t *ch, int delconf)
{
  dvr_timerec_entry_t *dte;

  while((dte = LIST_FIRST(&ch->ch_timerecs)) != NULL)
    timerec_entry_destroy(dte, delconf);
}

/**
 *
 */
void
timerec_destroy_by_id(const char *id, int delconf)
{
  dvr_timerec_entry_t *dte;
  dte = dvr_timerec_find_by_uuid(id);

  if (dte)
    timerec_entry_destroy(dte, delconf);
}

/**
 *
 */
void
timerec_destroy_by_config(dvr_config_t *kcfg, int delconf)
{
  dvr_timerec_entry_t *dte;
  dvr_config_t *cfg = NULL;

  while((dte = LIST_FIRST(&kcfg->dvr_timerec_entries)) != NULL) {
    LIST_REMOVE(dte, dte_config_link);
    if (cfg == NULL && delconf)
      cfg = dvr_config_find_by_name_default(NULL);
    if (cfg)
      LIST_INSERT_HEAD(&cfg->dvr_timerec_entries, dte, dte_config_link);
    dte->dte_config = cfg;
    if (delconf)
      dvr_timerec_save(dte);
  }
}
