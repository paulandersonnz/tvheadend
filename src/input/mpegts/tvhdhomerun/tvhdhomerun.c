/*
 *  Tvheadend - HDHomeRun client
 *
 *  Copyright (C) 2014 Patric Karlström
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

#include "tvheadend.h"
#include "input.h"
#include "htsbuf.h"
#include "settings.h"
#include "tvhdhomerun.h"
#include "tvhdhomerun_private.h"

#include <arpa/inet.h>
#include <openssl/sha.h>

static void tvhdhomerun_device_discovery( void );

static void
tvhdhomerun_device_class_save ( idnode_t *in )
{
  tvhdhomerun_device_save((tvhdhomerun_device_t *)in);
}

static idnode_set_t *
tvhdhomerun_device_class_get_childs ( idnode_t *in )
{
  tvhdhomerun_device_t *hd = (tvhdhomerun_device_t *)in;
  idnode_set_t *is = idnode_set_create(0);
  tvhdhomerun_frontend_t *lfe;

  TAILQ_FOREACH(lfe, &hd->hd_frontends, hf_link)
    idnode_set_add(is, &lfe->ti_id, NULL);
  return is;
}

typedef struct tvhdhomerun_discovery {
  TAILQ_ENTRY(tvhdhomerun_discovery) disc_link;
} tvhdhomerun_discovery_t;

TAILQ_HEAD(tvhdhomerun_discovery_queue, tvhdhomerun_discovery);

static int tvhdhomerun_discoveries_count;
static struct tvhdhomerun_discovery_queue tvhdhomerun_discoveries;

static const char *
tvhdhomerun_device_class_get_title( idnode_t *in )
{
  static char buf[256];
  tvhdhomerun_device_t *hd = (tvhdhomerun_device_t *)in;
  snprintf(buf, sizeof(buf),
           "%s - %s", hd->hd_info.friendlyname, hd->hd_info.ip_address);
  return buf;
}

static htsmsg_t *
tvhdhomerun_device_class_override_enum( void * p )
{
  htsmsg_t *m = htsmsg_create_list();
  htsmsg_add_str(m, NULL, "DVB-T");
  htsmsg_add_str(m, NULL, "DVB-C");
  htsmsg_add_str(m, NULL, "ATSC");
  return m;
}

static int
tvhdhomerun_device_class_override_set( void *obj, const void * p )
{
  tvhdhomerun_device_t *hd = obj;
  const char *s = p;
  if ( s != NULL && strlen(s) > 0 ) {
    if ( hd->hd_override_type != NULL && strcmp(hd->hd_override_type,s) != 0 ) {
      free(hd->hd_override_type);
      hd->hd_override_type = strdup(p);
      tvhlog(LOG_INFO, "tvhdhomerun", "Setting override_type : %s", hd->hd_override_type);
      return 1;
    }
  }
  return 0;
}

static void
tvhdhomerun_device_class_override_notify( void * obj )
{
  tvhdhomerun_device_t *hd = obj;
  tvhdhomerun_frontend_t *hfe;
  dvb_fe_type_t type = dvb_str2type(hd->hd_override_type);
  struct hdhomerun_discover_device_t discover_info;
  unsigned int tuner;
  htsmsg_t *conf;

  conf = hts_settings_load("input/tvhdhomerun/adapters/%s", hd->hd_info.uuid);
  if (conf)
    conf = htsmsg_get_map(conf, "frontends");

  lock_assert(&global_lock);

  while ((hfe = TAILQ_FIRST(&hd->hd_frontends)) != NULL)
  {
    if (hfe->hf_type == type)
      break;

    discover_info.device_id = hdhomerun_device_get_device_id(hfe->hf_hdhomerun_tuner);
    discover_info.ip_addr =  hdhomerun_device_get_device_ip(hfe->hf_hdhomerun_tuner);
    tuner = hfe->hf_tunerNumber;

    tvhdhomerun_frontend_delete(hfe);

    tvhdhomerun_frontend_create(hd, &discover_info, conf, type, tuner);
  }
}

const idclass_t tvhdhomerun_device_class =
{
  .ic_class      = "tvhdhomerun_client",
  .ic_caption    = "tvhdhomerun Client",
  .ic_save       = tvhdhomerun_device_class_save,
  .ic_get_childs = tvhdhomerun_device_class_get_childs,
  .ic_get_title  = tvhdhomerun_device_class_get_title,
  .ic_properties = (const property_t[]){
    {
      .type     = PT_STR,
      .id       = "networkType",
      .name     = "Network",
      .opts     = PO_RDONLY | PO_NOSAVE,
      .off      = offsetof(tvhdhomerun_device_t, hd_override_type),
    },
    {
      .type     = PT_STR,
      .id       = "ip_address",
      .name     = "IP Address",
      .opts     = PO_RDONLY | PO_NOSAVE,
      .off      = offsetof(tvhdhomerun_device_t, hd_info.ip_address),
    },
    {
      .type     = PT_STR,
      .id       = "uuid",
      .name     = "UUID",
      .opts     = PO_RDONLY,
      .off      = offsetof(tvhdhomerun_device_t, hd_info.uuid),
    },
    {
      .type     = PT_STR,
      .id       = "friendly",
      .name     = "Friendly Name",
      .opts     = PO_RDONLY | PO_NOSAVE,
      .off      = offsetof(tvhdhomerun_device_t, hd_info.friendlyname),
    },
    {
      .type     = PT_STR,
      .id       = "deviceModel",
      .name     = "Device Model",
      .opts     = PO_RDONLY | PO_NOSAVE,
      .off      = offsetof(tvhdhomerun_device_t, hd_info.deviceModel),
    },
    {
      .type     = PT_STR,
      .id       = "fe_override",
      .name     = "Network Type",
      .opts     = PO_ADVANCED,
      .set      = tvhdhomerun_device_class_override_set,
      .notify   = tvhdhomerun_device_class_override_notify,
      .list     = tvhdhomerun_device_class_override_enum,
      .off      = offsetof(tvhdhomerun_device_t, hd_type),
    },
    {}
  }
};


void
tvhdhomerun_device_save( tvhdhomerun_device_t *hd )
{
  tvhdhomerun_frontend_t *lfe;
  htsmsg_t *m, *l;

  m = htsmsg_create_map();
  idnode_save(&hd->th_id, m);

  l = htsmsg_create_map();
  TAILQ_FOREACH(lfe, &hd->hd_frontends, hf_link)
    tvhdhomerun_frontend_save(lfe, l);
  htsmsg_add_msg(m, "frontends", l);

  htsmsg_add_str(m, "fe_override", hd->hd_override_type);

  hts_settings_save(m, "input/tvhdhomerun/adapters/%s",
                    idnode_uuid_as_str(&hd->th_id));
  htsmsg_destroy(m);
}

static void
tvhdhomerun_discovery_destroy(tvhdhomerun_discovery_t *d, int unlink)
{
  if (d == NULL)
    return;
  if (unlink) {
    tvhdhomerun_discoveries_count--;
    TAILQ_REMOVE(&tvhdhomerun_discoveries, d, disc_link);
  }
  free(d);
}

static void
tvhdhomerun_device_calc_bin_uuid( uint8_t *uuid, const uint32_t device_id )
{
  SHA_CTX sha1;

  SHA1_Init(&sha1);
  SHA1_Update(&sha1, (void*)&device_id, sizeof(device_id));
  SHA1_Final(uuid, &sha1);
}

static void
tvhdhomerun_device_calc_uuid( tvh_uuid_t *uuid, const uint32_t device_id )
{
  uint8_t uuidbin[20];

  tvhdhomerun_device_calc_bin_uuid(uuidbin, device_id);
  bin2hex(uuid->hex, sizeof(uuid->hex), uuidbin, sizeof(uuidbin));
}

static tvhdhomerun_device_t *
tvhdhomerun_device_find( uint32_t device_id )
{
  tvh_hardware_t *th;
  uint8_t binuuid[20];

  tvhdhomerun_device_calc_bin_uuid(binuuid, device_id);
  TVH_HARDWARE_FOREACH(th) {
    if (idnode_is_instance(&th->th_id, &tvhdhomerun_device_class) &&
        memcmp(th->th_id.in_uuid, binuuid, UUID_BIN_SIZE) == 0)
      return (tvhdhomerun_device_t *)th;
  }
  return NULL;
}


#define MAX_HDHOMERUN_DEVICES 8

static void tvhdhomerun_device_create(struct hdhomerun_discover_device_t *dInfo) {

  tvhdhomerun_device_t *hd = calloc(1, sizeof(tvhdhomerun_device_t));
  htsmsg_t *conf = NULL, *feconf = NULL;
  tvh_uuid_t uuid;
  int j, save = 0;
  struct hdhomerun_device_t *hdhomerun_tuner;
  dvb_fe_type_t type = DVB_TYPE_C;
  struct in_addr ip_addr;

  tvhdhomerun_device_calc_uuid(&uuid, dInfo->device_id);

  hdhomerun_tuner = hdhomerun_device_create(dInfo->device_id, dInfo->ip_addr, 0, NULL);
  {
    const char *deviceModel =  hdhomerun_device_get_model_str(hdhomerun_tuner);
    if(deviceModel != NULL) {
      hd->hd_info.deviceModel = strdup(deviceModel);
    }
    hdhomerun_device_destroy(hdhomerun_tuner);
  }

  conf = hts_settings_load("input/tvhdhomerun/adapters/%s", uuid.hex);

  if ( conf != NULL ) {
    const char *override_type = htsmsg_get_str(conf, "fe_override");
    if ( override_type != NULL) {
      type = dvb_str2type(override_type);
      if ( ! ( type == DVB_TYPE_C || type == DVB_TYPE_T  || type == DVB_TYPE_ATSC ) ) {
        type = DVB_TYPE_C;
      }
    }
  } else {
    if (strstr(hd->hd_info.deviceModel, "_atsc"))
      type = DVB_TYPE_ATSC;
  }

  hd->hd_override_type = strdup(dvb_type2str(type));
  tvhlog(LOG_INFO, "tvheadend","Using Network type : %s", hd->hd_override_type);

  /* some sane defaults */
  hd->hd_fullmux_ok  = 1;
  hd->hd_pids_len    = 127;
  hd->hd_pids_max    = 32;
  hd->hd_pids_deladd = 1;

  if (!tvh_hardware_create0((tvh_hardware_t*)hd, &tvhdhomerun_device_class,
                            uuid.hex, conf)) {
    free(hd);
    return;
  }

  TAILQ_INIT(&hd->hd_frontends);

  /* we may check if uuid matches, but the SHA hash should be enough */
  if (hd->hd_info.uuid)
    free(hd->hd_info.uuid);

  char fName[128];
  snprintf(fName, 128, "HDHomeRun(%08X)",dInfo->device_id);

  ip_addr.s_addr = htonl(dInfo->ip_addr);
  hd->hd_info.ip_address = strdup(inet_ntoa(ip_addr));
  hd->hd_info.uuid = strdup(uuid.hex);
  hd->hd_info.friendlyname = strdup(fName);

  if (conf)
    feconf = htsmsg_get_map(conf, "frontends");
  save = !conf || !feconf;

  for (j = 0; j < dInfo->tuner_count; ++j) {
      if (tvhdhomerun_frontend_create(hd, dInfo, feconf, type, j)) {
        tvhlog(LOG_INFO, "tvhdhomerun", "Created frontend %08X tuner %d", dInfo->device_id, j);
      } else {
        tvhlog(LOG_ERR, "tvhdhomerun", "Unable to create frontend-device. ( %08x-%d )", dInfo->device_id,j);
      }
  }


  if (save)
    tvhdhomerun_device_save(hd);

  htsmsg_destroy(conf);
}

static void
tvhdhomerun_device_discovery( void )
{
  struct hdhomerun_discover_device_t result_list[MAX_HDHOMERUN_DEVICES];

  if (!tvheadend_running)
    return;

  int numDevices = hdhomerun_discover_find_devices_custom(0,
                                                          HDHOMERUN_DEVICE_TYPE_TUNER,
                                                          HDHOMERUN_DEVICE_ID_WILDCARD,
                                                          result_list,
                                                          MAX_HDHOMERUN_DEVICES);

  if (numDevices > 0)
  {
    while (numDevices > 0 ) {
      numDevices--;
      struct hdhomerun_discover_device_t* cDev = &result_list[numDevices];
      if ( cDev->device_type == HDHOMERUN_DEVICE_TYPE_TUNER ) {
        if ( !tvhdhomerun_device_find(cDev->device_id) ) {
          tvhlog(LOG_INFO, "tvhdhomerun","Found HDHomerun device %08x with %d tuners", cDev->device_id, cDev->tuner_count);
          tvhdhomerun_device_create(cDev);
        }
      }
    }
  }
}

void tvhdhomerun_init ( void )
{
  hdhomerun_debug_obj = hdhomerun_debug_create();

  hdhomerun_debug_set_filename(hdhomerun_debug_obj, "/tmp/tvheadend_hdhomerun_errors.log");
  hdhomerun_debug_enable(hdhomerun_debug_obj);
  TAILQ_INIT(&tvhdhomerun_discoveries);
  tvhdhomerun_device_discovery();
}

void tvhdhomerun_done ( void )
{
  tvh_hardware_t *th, *n;
  tvhdhomerun_discovery_t *d, *nd;

  pthread_mutex_lock(&global_lock);
  for (th = LIST_FIRST(&tvh_hardware); th != NULL; th = n) {
    n = LIST_NEXT(th, th_link);
    if (idnode_is_instance(&th->th_id, &tvhdhomerun_device_class)) {
      tvhdhomerun_device_destroy((tvhdhomerun_device_t *)th);
    }
  }
  for (d = TAILQ_FIRST(&tvhdhomerun_discoveries); d != NULL; d = nd) {
    nd = TAILQ_NEXT(d, disc_link);
    tvhdhomerun_discovery_destroy(d, 1);
  }
  pthread_mutex_unlock(&global_lock);
  hdhomerun_debug_destroy(hdhomerun_debug_obj);
}

void
tvhdhomerun_device_destroy( tvhdhomerun_device_t *hd )
{
  tvhdhomerun_frontend_t *lfe;

  lock_assert(&global_lock);

  gtimer_disarm(&hd->hd_destroy_timer);

  tvhlog(LOG_INFO, "tvhdhomerun", "Releasing locks for devices");
  while ((lfe = TAILQ_FIRST(&hd->hd_frontends)) != NULL) {
    tvhdhomerun_frontend_delete(lfe);
  }

#define FREEM(x) free(hd->hd_info.x)
  FREEM(ip_address);
  FREEM(friendlyname);
  FREEM(uuid);
  FREEM(deviceModel);
#undef FREEM

  tvh_hardware_delete((tvh_hardware_t*)hd);

#define FREEM(x) free(hd->x)
  FREEM(hd_override_type);
#undef FREEM

  free(hd);
}
