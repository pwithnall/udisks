/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2007-2010 David Zeuthen <zeuthen@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"
#include <glib/gi18n-lib.h>

#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <mntent.h>

#include <glib/gstdio.h>

#include "storagedlogging.h"
#include "storagedlinuxprovider.h"
#include "storagedlinuxmdraidobject.h"
#include "storagedlinuxmdraid.h"
#include "storagedlinuxblockobject.h"
#include "storageddaemon.h"
#include "storagedstate.h"
#include "storageddaemonutil.h"
#include "storagedlinuxdevice.h"
#include "storagedlinuxblock.h"

/**
 * SECTION:storagedlinuxmdraid
 * @title: StoragedLinuxMDRaid
 * @short_description: Linux implementation of #StoragedMDRaid
 *
 * This type provides an implementation of the #StoragedMDRaid interface
 * on Linux.
 */

typedef struct _StoragedLinuxMDRaidClass   StoragedLinuxMDRaidClass;

/**
 * StoragedLinuxMDRaid:
 *
 * The #StoragedLinuxMDRaid structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _StoragedLinuxMDRaid
{
  StoragedMDRaidSkeleton parent_instance;

  guint polling_timeout;
};

struct _StoragedLinuxMDRaidClass
{
  StoragedMDRaidSkeletonClass parent_class;
};

static void ensure_polling (StoragedLinuxMDRaid  *mdraid,
                            gboolean              polling_on);

static void mdraid_iface_init (StoragedMDRaidIface *iface);

G_DEFINE_TYPE_WITH_CODE (StoragedLinuxMDRaid, storaged_linux_mdraid, STORAGED_TYPE_MDRAID_SKELETON,
                         G_IMPLEMENT_INTERFACE (STORAGED_TYPE_MDRAID, mdraid_iface_init));

/* ---------------------------------------------------------------------------------------------------- */

static void
storaged_linux_mdraid_finalize (GObject *object)
{
  StoragedLinuxMDRaid *mdraid = STORAGED_LINUX_MDRAID (object);

  ensure_polling (mdraid, FALSE);

  if (G_OBJECT_CLASS (storaged_linux_mdraid_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (storaged_linux_mdraid_parent_class)->finalize (object);
}

static void
storaged_linux_mdraid_init (StoragedLinuxMDRaid *mdraid)
{
  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (mdraid),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
}

static void
storaged_linux_mdraid_class_init (StoragedLinuxMDRaidClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize     = storaged_linux_mdraid_finalize;
}

/**
 * storaged_linux_mdraid_new:
 *
 * Creates a new #StoragedLinuxMDRaid instance.
 *
 * Returns: A new #StoragedLinuxMDRaid. Free with g_object_unref().
 */
StoragedMDRaid *
storaged_linux_mdraid_new (void)
{
  return STORAGED_MDRAID (g_object_new (STORAGED_TYPE_LINUX_MDRAID,
                                          NULL));
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar *
read_sysfs_attr (GUdevDevice *device,
                 const gchar *attr)
{
  gchar *ret = NULL;
  gchar *path = NULL;
  GError *error = NULL;

  g_return_val_if_fail (G_UDEV_IS_DEVICE (device), NULL);

  path = g_strdup_printf ("%s/%s", g_udev_device_get_sysfs_path (device), attr);
  if (!g_file_get_contents (path, &ret, NULL /* size */, &error))
    {
      storaged_warning ("Error reading sysfs attr `%s': %s (%s, %d)",
                      path, error->message, g_quark_to_string (error->domain), error->code);
      g_clear_error (&error);
      goto out;
    }

 out:
  g_free (path);
  return ret;
}

static gint
read_sysfs_attr_as_int (GUdevDevice *device,
                        const gchar *attr)
{
  gint ret = 0;
  gchar *str = NULL;

  str = read_sysfs_attr (device, attr);
  if (str == NULL)
    goto out;

  ret = atoi (str);
  g_free (str);

 out:
  return ret;
}

static guint64
read_sysfs_attr_as_uint64 (GUdevDevice *device,
                           const gchar *attr)
{
  guint64 ret = 0;
  gchar *str = NULL;

  str = read_sysfs_attr (device, attr);
  if (str == NULL)
    goto out;

  ret = atoll (str);
  g_free (str);

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
on_polling_timout (gpointer user_data)
{
  StoragedLinuxMDRaid *mdraid = STORAGED_LINUX_MDRAID (user_data);
  StoragedLinuxMDRaidObject *object = NULL;
  StoragedLinuxDevice *raid_device;

  /* storaged_debug ("polling timeout"); */

  object = storaged_daemon_util_dup_object (mdraid, NULL);
  if (object == NULL)
    goto out;

  /* synthesize uevent */
  raid_device = storaged_linux_mdraid_object_get_device (object);
  if (raid_device != NULL)
    {
      storaged_linux_mdraid_object_uevent (object, "change", raid_device, FALSE);
      g_object_unref (raid_device);
    }

 out:
  g_clear_object (&object);
  return TRUE; /* keep timeout around */
}

static void
ensure_polling (StoragedLinuxMDRaid  *mdraid,
                gboolean              polling_on)
{
  if (polling_on)
    {
      if (mdraid->polling_timeout == 0)
        {
          mdraid->polling_timeout = g_timeout_add_seconds (1,
                                                           on_polling_timout,
                                                           mdraid);
        }
    }
  else
    {
      if (mdraid->polling_timeout != 0)
        {
          g_source_remove (mdraid->polling_timeout);
          mdraid->polling_timeout = 0;
        }
    }
}

static gint
member_cmpfunc (GVariant **a,
                GVariant **b)
{
  gint slot_a;
  gint slot_b;
  const gchar *objpath_a;
  const gchar *objpath_b;

  g_return_val_if_fail (a != NULL, 0);
  g_return_val_if_fail (b != NULL, 0);

  g_variant_get (*a, "(&oiasta{sv})", &objpath_a, &slot_a, NULL, NULL, NULL);
  g_variant_get (*b, "(&oiasta{sv})", &objpath_b, &slot_b, NULL, NULL, NULL);

  if (slot_a == slot_b)
    return g_strcmp0 (objpath_a, objpath_b);

  return slot_a - slot_b;
}

/**
 * storaged_linux_mdraid_update:
 * @mdraid: A #StoragedLinuxMDRaid.
 * @object: The enclosing #StoragedLinuxMDRaidObject instance.
 *
 * Updates the interface.
 *
 * Returns: %TRUE if configuration has changed, %FALSE otherwise.
 */
gboolean
storaged_linux_mdraid_update (StoragedLinuxMDRaid       *mdraid,
                              StoragedLinuxMDRaidObject *object)
{
  StoragedMDRaid *iface = STORAGED_MDRAID (mdraid);
  gboolean ret = FALSE;
  guint num_devices = 0;
  guint64 size = 0;
  StoragedLinuxDevice *raid_device = NULL;
  GList *member_devices = NULL;
  StoragedLinuxDevice *device = NULL;
  const gchar *level = NULL;
  const gchar *uuid = NULL;
  const gchar *name = NULL;
  gchar *sync_action = NULL;
  gchar *sync_completed = NULL;
  gchar *bitmap_location = NULL;
  guint degraded = 0;
  guint64 chunk_size = 0;
  gdouble sync_completed_val = 0.0;
  guint64 sync_rate = 0;
  guint64 sync_remaining_time = 0;
  GVariantBuilder builder;
  StoragedDaemon *daemon = NULL;
  gboolean has_redundancy = FALSE;
  gboolean has_stripes = FALSE;

  daemon = storaged_linux_mdraid_object_get_daemon (object);

  member_devices = storaged_linux_mdraid_object_get_members (object);
  raid_device = storaged_linux_mdraid_object_get_device (object);

  if (member_devices == NULL && raid_device == NULL)
    {
      /* this should never happen */
      storaged_warning ("No members and no RAID device - bailing");
      goto out;
    }

  /* it doesn't matter where we get the MD_ properties from - it can be
   * either a member device or the raid device (/dev/md*) - prefer the
   * former, if available
   */
  if (member_devices != NULL)
    {
      device = STORAGED_LINUX_DEVICE (member_devices->data);
      num_devices = g_udev_device_get_property_as_int (device->udev_device, "STORAGED_MD_MEMBER_DEVICES");
      level = g_udev_device_get_property (device->udev_device, "STORAGED_MD_MEMBER_LEVEL");
      uuid = g_udev_device_get_property (device->udev_device, "STORAGED_MD_MEMBER_UUID");
      name = g_udev_device_get_property (device->udev_device, "STORAGED_MD_MEMBER_NAME");
    }
  else
    {
      device = STORAGED_LINUX_DEVICE (raid_device);
      num_devices = g_udev_device_get_property_as_int (device->udev_device, "STORAGED_MD_DEVICES");
      level = g_udev_device_get_property (device->udev_device, "STORAGED_MD_LEVEL");
      uuid = g_udev_device_get_property (device->udev_device, "STORAGED_MD_UUID");
      name = g_udev_device_get_property (device->udev_device, "STORAGED_MD_NAME");
    }

  /* figure out size */
  if (raid_device != NULL)
    {
      size = 512 * g_udev_device_get_sysfs_attr_as_uint64 (raid_device->udev_device, "size");
    }
  else
    {
      /* TODO: need MD_ARRAY_SIZE, see https://bugs.freedesktop.org/show_bug.cgi?id=53239#c5 */
    }

  storaged_mdraid_set_uuid (iface, uuid);
  storaged_mdraid_set_name (iface, name);
  storaged_mdraid_set_level (iface, level);
  storaged_mdraid_set_num_devices (iface, num_devices);
  storaged_mdraid_set_size (iface, size);

  if (g_strcmp0 (level, "raid1") == 0 ||
      g_strcmp0 (level, "raid4") == 0 ||
      g_strcmp0 (level, "raid5") == 0 ||
      g_strcmp0 (level, "raid6") == 0 ||
      g_strcmp0 (level, "raid10") == 0)
    has_redundancy = TRUE;

  if (g_strcmp0 (level, "raid0") == 0 ||
      g_strcmp0 (level, "raid4") == 0 ||
      g_strcmp0 (level, "raid5") == 0 ||
      g_strcmp0 (level, "raid6") == 0 ||
      g_strcmp0 (level, "raid10") == 0)
    has_stripes = TRUE;

  if (raid_device != NULL)
    {
      if (has_redundancy)
        {
          /* Can't use GUdevDevice methods as they cache the result and these variables vary */
          degraded = read_sysfs_attr_as_int (raid_device->udev_device, "md/degraded");
          sync_action = read_sysfs_attr (raid_device->udev_device, "md/sync_action");
          if (sync_action != NULL)
            g_strstrip (sync_action);
          sync_completed = read_sysfs_attr (raid_device->udev_device, "md/sync_completed");
          if (sync_completed != NULL)
            g_strstrip (sync_completed);
          bitmap_location = read_sysfs_attr (raid_device->udev_device, "md/bitmap/location");
          if (bitmap_location != NULL)
            g_strstrip (bitmap_location);
        }

      if (has_stripes)
        {
          chunk_size = read_sysfs_attr_as_uint64 (raid_device->udev_device, "md/chunk_size");
        }
    }
  storaged_mdraid_set_degraded (iface, degraded);
  storaged_mdraid_set_sync_action (iface, sync_action);
  storaged_mdraid_set_bitmap_location (iface, bitmap_location);
  storaged_mdraid_set_chunk_size (iface, chunk_size);

  if (sync_completed != NULL && g_strcmp0 (sync_completed, "none") != 0)
    {
      guint64 completed_sectors = 0;
      guint64 num_sectors = 1;
      if (sscanf (sync_completed, "%" G_GUINT64_FORMAT " / %" G_GUINT64_FORMAT,
                  &completed_sectors, &num_sectors) == 2)
        {
          if (num_sectors != 0)
            sync_completed_val = ((gdouble) completed_sectors) / ((gdouble) num_sectors);
        }

      /* this is KiB/s (see drivers/md/md.c:sync_speed_show() */
      sync_rate = read_sysfs_attr_as_uint64 (raid_device->udev_device, "md/sync_speed") * 1024;
      if (sync_rate > 0)
        {
          guint64 num_bytes_remaining = (num_sectors - completed_sectors) * 512ULL;
          sync_remaining_time = ((guint64) G_USEC_PER_SEC) * num_bytes_remaining / sync_rate;
        }
    }
  storaged_mdraid_set_sync_completed (iface, sync_completed_val);
  storaged_mdraid_set_sync_rate (iface, sync_rate);
  storaged_mdraid_set_sync_remaining_time (iface, sync_remaining_time);

  /* ensure we poll, exactly when we need to */
  if (g_strcmp0 (sync_action, "resync") == 0 ||
      g_strcmp0 (sync_action, "recover") == 0 ||
      g_strcmp0 (sync_action, "check") == 0 ||
      g_strcmp0 (sync_action, "repair") == 0)
    {
      ensure_polling (mdraid, TRUE);
    }
  else
    {
      ensure_polling (mdraid, FALSE);
    }

  /* figure out active devices */
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(oiasta{sv})"));
  if (raid_device != NULL)
    {
      gchar *md_dir_name = NULL;
      GDir *md_dir;
      GPtrArray *p;
      guint n;

      /* First build an array of variants, then sort it, then build
       *
       * Why sort it? Because directory traversal does not preserve
       * the order and we want the same order every time to avoid
       * spurious property changes on MDRaid:ActiveDevices
       */
      p = g_ptr_array_new ();
      md_dir_name = g_strdup_printf ("%s/md", g_udev_device_get_sysfs_path (raid_device->udev_device));
      md_dir = g_dir_open (md_dir_name, 0, NULL);
      if (md_dir != NULL)
        {
          gchar buf[256];
          const gchar *file_name;
          while ((file_name = g_dir_read_name (md_dir)) != NULL)
            {
              gchar *block_sysfs_path = NULL;
              StoragedObject *member_object = NULL;
              gchar *member_state = NULL;
              gchar **member_state_elements = NULL;
              gchar *member_slot = NULL;
              gint member_slot_as_int = -1;
              guint64 member_errors = 0;

              if (!g_str_has_prefix (file_name, "dev-"))
                goto member_done;

              snprintf (buf, sizeof (buf), "%s/block", file_name);
              block_sysfs_path = storaged_daemon_util_resolve_link (md_dir_name, buf);
              if (block_sysfs_path == NULL)
                {
                  storaged_warning ("Unable to resolve %s/%s symlink", md_dir_name, buf);
                  goto member_done;
                }

              member_object = storaged_daemon_find_block_by_sysfs_path (daemon, block_sysfs_path);
              if (member_object == NULL)
                {
                  /* TODO: only warn on !coldplug */
                  /* storaged_warning ("No object for block device with sysfs path %s", block_sysfs_path); */
                  goto member_done;
                }

              snprintf (buf, sizeof (buf), "md/%s/state", file_name);
              member_state = read_sysfs_attr (raid_device->udev_device, buf);
              if (member_state != NULL)
                {
                  g_strstrip (member_state);
                  member_state_elements = g_strsplit (member_state, ",", 0);
                }

              snprintf (buf, sizeof (buf), "md/%s/slot", file_name);
              member_slot = read_sysfs_attr (raid_device->udev_device, buf);
              if (member_slot != NULL)
                {
                  g_strstrip (member_slot);
                  if (g_strcmp0 (member_slot, "none") != 0)
                    member_slot_as_int = atoi (member_slot);
                }

              snprintf (buf, sizeof (buf), "md/%s/errors", file_name);
              member_errors = read_sysfs_attr_as_uint64 (raid_device->udev_device, buf);

              g_ptr_array_add (p,
                               g_variant_new ("(oi^asta{sv})",
                                              g_dbus_object_get_object_path (G_DBUS_OBJECT (member_object)),
                                              member_slot_as_int,
                                              member_state_elements,
                                              member_errors,
                                              NULL)); /* expansion, unused for now */

            member_done:
              g_free (member_slot);
              g_free (member_state);
              g_strfreev (member_state_elements);
              g_clear_object (&member_object);
              g_free (block_sysfs_path);

            } /* for all dev- directories */

          /* ... and sort */
          g_ptr_array_sort (p, (GCompareFunc) member_cmpfunc);

          /* ... and finally build (builder consumes each GVariant instance) */
          for (n = 0; n < p->len; n++)
            g_variant_builder_add_value (&builder, p->pdata[n]);
          g_ptr_array_free (p, TRUE);

          g_dir_close (md_dir);
        }
      g_free (md_dir_name);

    }
  storaged_mdraid_set_active_devices (iface, g_variant_builder_end (&builder));

  storaged_mdraid_set_child_configuration (iface,
                                           storaged_linux_find_child_configuration (daemon,
                                                                                    uuid));

 out:
  g_free (sync_completed);
  g_free (sync_action);
  g_free (bitmap_location);
  g_list_free_full (member_devices, g_object_unref);
  g_clear_object (&raid_device);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static StoragedObject *
wait_for_md_block_object (StoragedDaemon *daemon,
                          gpointer        user_data)
{
  StoragedLinuxMDRaidObject *mdraid_object = STORAGED_LINUX_MDRAID_OBJECT (user_data);
  StoragedObject *ret = NULL;
  GList *objects, *l;

  objects = storaged_daemon_get_objects (daemon);
  for (l = objects; l != NULL; l = l->next)
    {
      StoragedObject *object = STORAGED_OBJECT (l->data);
      StoragedBlock *block = NULL;

      block = storaged_object_get_block (object);
      if (block != NULL)
        {
          if (g_strcmp0 (storaged_block_get_mdraid (block),
                         g_dbus_object_get_object_path (G_DBUS_OBJECT (mdraid_object))) == 0)
            {
              g_object_unref (block);
              ret = g_object_ref (object);
              goto out;
            }
          g_object_unref (block);
        }
    }

 out:
  g_list_free_full (objects, g_object_unref);
  return ret;
}

static gboolean
handle_start (StoragedMDRaid           *_mdraid,
              GDBusMethodInvocation    *invocation,
              GVariant                 *options)
{
  StoragedLinuxMDRaid *mdraid = STORAGED_LINUX_MDRAID (_mdraid);
  StoragedDaemon *daemon;
  StoragedState *state;
  StoragedLinuxMDRaidObject *object;
  const gchar *action_id;
  const gchar *message;
  uid_t caller_uid;
  gid_t caller_gid;
  StoragedLinuxDevice *raid_device = NULL;
  GList *member_devices = NULL;
  gchar *raid_device_file = NULL;
  GError *error = NULL;
  gchar *error_message = NULL;
  gchar *escaped_uuid = NULL;
  gboolean opt_start_degraded = FALSE;
  struct stat statbuf;
  dev_t raid_device_num;
  StoragedObject *block_object = NULL;
  StoragedBlock *block = NULL;

  object = storaged_daemon_util_dup_object (mdraid, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = storaged_linux_mdraid_object_get_daemon (object);
  state = storaged_daemon_get_state (daemon);

  g_variant_lookup (options, "start-degraded", "b", &opt_start_degraded);

  error = NULL;
  if (!storaged_daemon_util_get_caller_uid_sync (daemon,
                                                 invocation,
                                                 NULL /* GCancellable */,
                                                 &caller_uid,
                                                 &caller_gid,
                                                 NULL,
                                                 &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
      goto out;
    }

  raid_device = storaged_linux_mdraid_object_get_device (object);
  if (raid_device != NULL)
    {
      g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                             "RAID Array is already running");
      goto out;
    }

  member_devices = storaged_linux_mdraid_object_get_members (object);
  if (member_devices == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                             "No member devices");
      goto out;
    }

  /* Translators: Shown in authentication dialog when the user
   * attempts to start a RAID Array.
   */
  /* TODO: variables */
  message = N_("Authentication is required to start a RAID array");
  action_id = "org.storaged.Storaged.manage-md-raid";
  if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                      STORAGED_OBJECT (object),
                                                      action_id,
                                                      options,
                                                      message,
                                                      invocation))
    goto out;

  escaped_uuid = storaged_daemon_util_escape_and_quote (storaged_mdraid_get_uuid (STORAGED_MDRAID (mdraid)));

  if (!storaged_daemon_launch_spawned_job_sync (daemon,
                                                STORAGED_OBJECT (object),
                                                "md-raid-start", caller_uid,
                                                NULL, /* GCancellable */
                                                0,    /* uid_t run_as_uid */
                                                0,    /* uid_t run_as_euid */
                                                NULL, /* gint *out_status */
                                                &error_message,
                                                NULL,  /* input_string */
                                                "mdadm --assemble%s --scan --uuid %s",
                                                opt_start_degraded ? " --run" : " ",
                                                escaped_uuid))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             STORAGED_ERROR,
                                             STORAGED_ERROR_FAILED,
                                             "Error starting RAID array: %s",
                                             error_message);
      goto out;
    }

  /* ... then, sit and wait for MD block device to show up */
  block_object = storaged_daemon_wait_for_object_sync (daemon,
                                                     wait_for_md_block_object,
                                                     object,
                                                     NULL,
                                                     10, /* timeout_seconds */
                                                     &error);
  if (block_object == NULL)
    {
      g_prefix_error (&error,
                      "Error waiting for MD block device after starting array");
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }
  block = storaged_object_get_block (block_object);
  if (block == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             STORAGED_ERROR,
                                             STORAGED_ERROR_FAILED,
                                             "No block interface for object");
      goto out;
    }
  raid_device_file = storaged_block_dup_device (block);

  /* Check that it's a block device */
  if (stat (raid_device_file, &statbuf) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             STORAGED_ERROR,
                                             STORAGED_ERROR_FAILED,
                                             "Error calling stat(2) on %s: %m",
                                             raid_device_file);
      goto out;
    }
  if (!S_ISBLK (statbuf.st_mode))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             STORAGED_ERROR,
                                             STORAGED_ERROR_FAILED,
                                             "Device file %s is not a block device",
                                             raid_device_file);
      goto out;
    }
  raid_device_num = statbuf.st_rdev;

  /* update the mdraid file */
  storaged_state_add_mdraid (state,
                             raid_device_num,
                             caller_uid);

  /* TODO: wait for array to actually show up in storaged? Probably */

  storaged_mdraid_complete_start (_mdraid, invocation);

 out:
  g_clear_object (&block);
  g_clear_object (&block_object);
  g_list_free_full (member_devices, g_object_unref);
  g_free (error_message);
  g_free (raid_device_file);
  g_free (escaped_uuid);
  g_clear_object (&raid_device);
  g_clear_object (&object);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
storaged_linux_mdraid_stop (StoragedMDRaid           *_mdraid,
                            GDBusMethodInvocation    *invocation,
                            GVariant                 *options,
                            GError                  **error)
{
  StoragedLinuxMDRaid *mdraid = STORAGED_LINUX_MDRAID (_mdraid);
  StoragedDaemon *daemon;
  StoragedState *state;
  StoragedLinuxMDRaidObject *object;
  uid_t started_by_uid;
  uid_t caller_uid;
  gid_t caller_gid;
  StoragedLinuxDevice *raid_device = NULL;
  const gchar *device_file = NULL;
  gchar *escaped_device_file = NULL;
  gchar *error_message = NULL;
  gboolean ret;

  object = storaged_daemon_util_dup_object (mdraid, error);
  if (object == NULL)
    {
      ret = FALSE;
      goto out;
    }

  daemon = storaged_linux_mdraid_object_get_daemon (object);
  state = storaged_daemon_get_state (daemon);

  if (!storaged_daemon_util_get_caller_uid_sync (daemon,
                                                 invocation,
                                                 NULL /* GCancellable */,
                                                 &caller_uid,
                                                 &caller_gid,
                                                 NULL,
                                                 error))
    {
      ret = FALSE;
      goto out;
    }

  raid_device = storaged_linux_mdraid_object_get_device (object);
  if (raid_device == NULL)
    {
      g_set_error (error, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                   "RAID Array is not running");
      ret = FALSE;
      goto out;
    }

  if (!storaged_state_has_mdraid (state,
                                  g_udev_device_get_device_number (raid_device->udev_device),
                                  &started_by_uid))
    {
      /* allow stopping arrays stuff not mentioned in mounted-fs, but treat it like root mounted it */
      started_by_uid = 0;
    }

  if (caller_uid != 0 && (caller_uid != started_by_uid))
    {
      const gchar *action_id;
      const gchar *message;

      /* Translators: Shown in authentication dialog when the user
       * attempts to stop a RAID Array.
       */
      /* TODO: variables */
      message = N_("Authentication is required to stop a RAID array");
      action_id = "org.storaged.Storaged.manage-md-raid";
      if (!storaged_daemon_util_check_authorization_sync_with_error (daemon,
                                                                     STORAGED_OBJECT (object),
                                                                     action_id,
                                                                     options,
                                                                     message,
                                                                     invocation,
                                                                     error))
        ret = FALSE;
        goto out;
    }

  device_file = g_udev_device_get_device_file (raid_device->udev_device);
  escaped_device_file = storaged_daemon_util_escape_and_quote (g_udev_device_get_device_file (raid_device->udev_device));

  if (!storaged_daemon_launch_spawned_job_sync (daemon,
                                                STORAGED_OBJECT (object),
                                                "md-raid-stop", caller_uid,
                                                NULL, /* GCancellable */
                                                0,    /* uid_t run_as_uid */
                                                0,    /* uid_t run_as_euid */
                                                NULL, /* gint *out_status */
                                                &error_message,
                                                NULL,  /* input_string */
                                                "mdadm --stop %s",
                                                escaped_device_file))
    {
      g_set_error (error,
                   STORAGED_ERROR,
                   STORAGED_ERROR_FAILED,
                   "Error stopping RAID array %s: %s",
                   device_file,
                   error_message);
      ret = FALSE;
      goto out;
    }

  ret = TRUE;

 out:
  g_free (error_message);
  g_free (escaped_device_file);
  g_clear_object (&raid_device);
  g_clear_object (&object);
  return ret;
}

static gboolean
handle_stop (StoragedMDRaid           *_mdraid,
             GDBusMethodInvocation    *invocation,
             GVariant                 *options)
{
  GError *error = NULL;

  if (!storaged_linux_mdraid_stop (_mdraid,
                                   invocation,
                                   options,
                                   &error))
    g_dbus_method_invocation_take_error (invocation, error);
  else
    storaged_mdraid_complete_stop (_mdraid, invocation);

  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar **
find_member_states (StoragedLinuxMDRaid *mdraid,
                    const gchar         *member_device_objpath)
{
  GVariantIter iter;
  GVariant *active_devices = NULL;
  const gchar *iter_objpath;
  const gchar **iter_state;
  gchar **ret = NULL;

  active_devices = storaged_mdraid_dup_active_devices (STORAGED_MDRAID (mdraid));
  if (active_devices == NULL)
    goto out;

  g_variant_iter_init (&iter, active_devices);
  while (g_variant_iter_next (&iter, "(&oi^a&sta{sv})", &iter_objpath, NULL, &iter_state, NULL, NULL))
    {
      if (g_strcmp0 (iter_objpath, member_device_objpath) == 0)
        {
          guint n;
          /* we only own the container, so need to dup the values */
          ret = (gchar **) iter_state;
          for (n = 0; ret[n] != NULL; n++)
            ret[n] = g_strdup (ret[n]);
          goto out;
        }
      else
        {
          g_free (iter_state);
        }
    }

 out:
  if (active_devices != NULL)
    g_variant_unref (active_devices);
  return ret;
}

static gboolean
has_state (gchar **states, const gchar *state)
{
  gboolean ret = FALSE;
  guint n;

  if (states == NULL)
    goto out;

  for (n = 0; states[n] != NULL; n++)
    {
      if (g_strcmp0 (states[n], state) == 0)
        {
          ret = TRUE;
          goto out;
        }
    }
 out:
  return ret;
}

static gboolean
handle_remove_device (StoragedMDRaid           *_mdraid,
                      GDBusMethodInvocation    *invocation,
                      const gchar              *member_device_objpath,
                      GVariant                 *options)
{
  StoragedLinuxMDRaid *mdraid = STORAGED_LINUX_MDRAID (_mdraid);
  StoragedDaemon *daemon;
  StoragedState *state;
  StoragedLinuxMDRaidObject *object;
  const gchar *action_id;
  const gchar *message;
  uid_t started_by_uid;
  uid_t caller_uid;
  gid_t caller_gid;
  StoragedLinuxDevice *raid_device = NULL;
  const gchar *device_file = NULL;
  gchar *escaped_device_file = NULL;
  const gchar *member_device_file = NULL;
  gchar *escaped_member_device_file = NULL;
  GError *error = NULL;
  gchar *error_message = NULL;
  StoragedObject *member_device_object = NULL;
  StoragedBlock *member_device = NULL;
  gchar **member_states = NULL;
  gboolean opt_wipe = FALSE;

  object = storaged_daemon_util_dup_object (mdraid, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = storaged_linux_mdraid_object_get_daemon (object);
  state = storaged_daemon_get_state (daemon);

  g_variant_lookup (options, "wipe", "b", &opt_wipe);

  error = NULL;
  if (!storaged_daemon_util_get_caller_uid_sync (daemon,
                                                 invocation,
                                                 NULL /* GCancellable */,
                                                 &caller_uid,
                                                 &caller_gid,
                                                 NULL,
                                                 &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
      goto out;
    }

  raid_device = storaged_linux_mdraid_object_get_device (object);
  if (raid_device == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                             "RAID Array is not running");
      goto out;
    }

  member_device_object = storaged_daemon_find_object (daemon, member_device_objpath);
  if (member_device_object == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                             "No device for given object path");
      goto out;
    }

  member_device = storaged_object_get_block (member_device_object);
  if (member_device == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                             "No block interface on given object");
      goto out;
    }

  member_states = find_member_states (mdraid, member_device_objpath);
  if (member_states == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                             "Cannot determine member state of given object");
      goto out;
    }

  if (!storaged_state_has_mdraid (state,
                                  g_udev_device_get_device_number (raid_device->udev_device),
                                  &started_by_uid))
    {
      /* allow stopping arrays stuff not mentioned in mounted-fs, but treat it like root mounted it */
      started_by_uid = 0;
    }

  if (caller_uid != 0 && (caller_uid != started_by_uid))
    {
      /* Translators: Shown in authentication dialog when the user
       * attempts to remove a device from a RAID Array.
       */
      /* TODO: variables */
      message = N_("Authentication is required to remove a device from a RAID array");
      action_id = "org.storaged.Storaged.manage-md-raid";
      if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                          STORAGED_OBJECT (object),
                                                          action_id,
                                                          options,
                                                          message,
                                                          invocation))
        goto out;
    }

  device_file = g_udev_device_get_device_file (raid_device->udev_device);
  escaped_device_file = storaged_daemon_util_escape_and_quote (device_file);

  member_device_file = storaged_block_get_device (member_device);
  escaped_member_device_file = storaged_daemon_util_escape_and_quote (member_device_file);

  /* if necessary, mark as faulty first */
  if (has_state (member_states, "in_sync"))
    {
      if (!storaged_daemon_launch_spawned_job_sync (daemon,
                                                    STORAGED_OBJECT (object),
                                                    "md-raid-fault-device", caller_uid,
                                                    NULL, /* GCancellable */
                                                    0,    /* uid_t run_as_uid */
                                                    0,    /* uid_t run_as_euid */
                                                    NULL, /* gint *out_status */
                                                    &error_message,
                                                    NULL,  /* input_string */
                                                    "mdadm --manage %s --set-faulty %s",
                                                    escaped_device_file,
                                                    escaped_member_device_file))
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 STORAGED_ERROR,
                                                 STORAGED_ERROR_FAILED,
                                                 "Error marking %s as faulty in RAID array %s: %s",
                                                 member_device_file,
                                                 device_file,
                                                 error_message);
          goto out;
        }
    }

  if (!storaged_daemon_launch_spawned_job_sync (daemon,
                                              STORAGED_OBJECT (object),
                                              "md-raid-remove-device", caller_uid,
                                              NULL, /* GCancellable */
                                              0,    /* uid_t run_as_uid */
                                              0,    /* uid_t run_as_euid */
                                              NULL, /* gint *out_status */
                                              &error_message,
                                              NULL,  /* input_string */
                                              "mdadm --manage %s --remove %s",
                                              escaped_device_file,
                                              escaped_member_device_file))
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 STORAGED_ERROR,
                                                 STORAGED_ERROR_FAILED,
                                                 "Error removing %s from RAID array %s: %s",
                                                 member_device_file,
                                                 device_file,
                                                 error_message);
          goto out;
        }

  if (opt_wipe)
    {
      if (!storaged_daemon_launch_spawned_job_sync (daemon,
                                                    STORAGED_OBJECT (member_device_object),
                                                    "format-erase", caller_uid,
                                                    NULL, /* GCancellable */
                                                    0,    /* uid_t run_as_uid */
                                                    0,    /* uid_t run_as_euid */
                                                    NULL, /* gint *out_status */
                                                    &error_message,
                                                    NULL,  /* input_string */
                                                    "wipefs -a %s",
                                                    escaped_member_device_file))
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 STORAGED_ERROR,
                                                 STORAGED_ERROR_FAILED,
                                                 "Error wiping  %s after removal from RAID array %s: %s",
                                                 member_device_file,
                                                 device_file,
                                                 error_message);
          goto out;
        }
    }

  storaged_mdraid_complete_remove_device (_mdraid, invocation);

 out:
  g_free (error_message);
  g_free (escaped_device_file);
  g_free (escaped_member_device_file);
  g_free (member_states);
  g_clear_object (&member_device_object);
  g_clear_object (&member_device);
  g_clear_object (&raid_device);
  g_clear_object (&object);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_add_device (StoragedMDRaid           *_mdraid,
                   GDBusMethodInvocation    *invocation,
                   const gchar              *new_member_device_objpath,
                   GVariant                 *options)
{
  StoragedLinuxMDRaid *mdraid = STORAGED_LINUX_MDRAID (_mdraid);
  StoragedDaemon *daemon;
  StoragedState *state;
  StoragedLinuxMDRaidObject *object;
  const gchar *action_id;
  const gchar *message;
  uid_t started_by_uid;
  uid_t caller_uid;
  gid_t caller_gid;
  StoragedLinuxDevice *raid_device = NULL;
  const gchar *device_file = NULL;
  gchar *escaped_device_file = NULL;
  const gchar *new_member_device_file = NULL;
  gchar *escaped_new_member_device_file = NULL;
  GError *error = NULL;
  gchar *error_message = NULL;
  StoragedObject *new_member_device_object = NULL;
  StoragedBlock *new_member_device = NULL;

  object = storaged_daemon_util_dup_object (mdraid, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = storaged_linux_mdraid_object_get_daemon (object);
  state = storaged_daemon_get_state (daemon);

  error = NULL;
  if (!storaged_daemon_util_get_caller_uid_sync (daemon,
                                                 invocation,
                                                 NULL /* GCancellable */,
                                                 &caller_uid,
                                                 &caller_gid,
                                                 NULL,
                                                 &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
      goto out;
    }

  raid_device = storaged_linux_mdraid_object_get_device (object);
  if (raid_device == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                             "RAID Array is not running");
      goto out;
    }

  new_member_device_object = storaged_daemon_find_object (daemon, new_member_device_objpath);
  if (new_member_device_object == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                             "No device for given object path");
      goto out;
    }

  new_member_device = storaged_object_get_block (new_member_device_object);
  if (new_member_device == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                             "No block interface on given object");
      goto out;
    }

  if (!storaged_state_has_mdraid (state,
                                  g_udev_device_get_device_number (raid_device->udev_device),
                                  &started_by_uid))
    {
      /* allow stopping arrays stuff not mentioned in mounted-fs, but treat it like root mounted it */
      started_by_uid = 0;
    }

  /* First check the user is authorized to manage RAID */
  if (caller_uid != 0 && (caller_uid != started_by_uid))
    {
      /* Translators: Shown in authentication dialog when the user
       * attempts to add a device to a RAID Array.
       */
      /* TODO: variables */
      message = N_("Authentication is required to add a device to a RAID array");
      action_id = "org.storaged.Storaged.manage-md-raid";
      if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                          STORAGED_OBJECT (object),
                                                          action_id,
                                                          options,
                                                          message,
                                                          invocation))
        goto out;
    }

  device_file = g_udev_device_get_device_file (raid_device->udev_device);
  escaped_device_file = storaged_daemon_util_escape_and_quote (device_file);

  new_member_device_file = storaged_block_get_device (new_member_device);
  escaped_new_member_device_file = storaged_daemon_util_escape_and_quote (new_member_device_file);

  if (!storaged_daemon_launch_spawned_job_sync (daemon,
                                                STORAGED_OBJECT (object),
                                                "md-raid-add-device", caller_uid,
                                                NULL, /* GCancellable */
                                                0,    /* uid_t run_as_uid */
                                                0,    /* uid_t run_as_euid */
                                                NULL, /* gint *out_status */
                                                &error_message,
                                                NULL,  /* input_string */
                                                "mdadm --manage %s --add %s",
                                                escaped_device_file,
                                                escaped_new_member_device_file))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             STORAGED_ERROR,
                                             STORAGED_ERROR_FAILED,
                                             "Error adding %s to RAID array %s: %s",
                                             new_member_device_file,
                                             device_file,
                                             error_message);
      goto out;
    }

  storaged_mdraid_complete_add_device (_mdraid, invocation);

 out:
  g_free (error_message);
  g_free (escaped_device_file);
  g_free (escaped_new_member_device_file);
  g_clear_object (&new_member_device_object);
  g_clear_object (&new_member_device);
  g_clear_object (&raid_device);
  g_clear_object (&object);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_set_bitmap_location (StoragedMDRaid           *_mdraid,
                            GDBusMethodInvocation    *invocation,
                            const gchar              *value,
                            GVariant                 *options)
{
  StoragedLinuxMDRaid *mdraid = STORAGED_LINUX_MDRAID (_mdraid);
  StoragedDaemon *daemon;
  StoragedState *state;
  StoragedLinuxMDRaidObject *object;
  const gchar *action_id;
  const gchar *message;
  uid_t started_by_uid;
  uid_t caller_uid;
  gid_t caller_gid;
  StoragedLinuxDevice *raid_device = NULL;
  const gchar *device_file = NULL;
  gchar *escaped_device_file = NULL;
  GError *error = NULL;
  gchar *error_message = NULL;

  object = storaged_daemon_util_dup_object (mdraid, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = storaged_linux_mdraid_object_get_daemon (object);
  state = storaged_daemon_get_state (daemon);

  error = NULL;
  if (!storaged_daemon_util_get_caller_uid_sync (daemon,
                                                 invocation,
                                                 NULL /* GCancellable */,
                                                 &caller_uid,
                                                 &caller_gid,
                                                 NULL,
                                                 &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
      goto out;
    }

  if (!(g_strcmp0 (value, "none") == 0 || g_strcmp0 (value, "internal") == 0))
    {
      g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                             "Only values 'none' and 'internal' are currently supported.");
      goto out;
    }

  raid_device = storaged_linux_mdraid_object_get_device (object);
  if (raid_device == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                             "RAID Array is not running");
      goto out;
    }

  if (!storaged_state_has_mdraid (state,
                                  g_udev_device_get_device_number (raid_device->udev_device),
                                  &started_by_uid))
    {
      /* allow stopping arrays stuff not mentioned in mounted-fs, but treat it like root mounted it */
      started_by_uid = 0;
    }

  /* First check the user is authorized to manage RAID */
  if (caller_uid != 0 && (caller_uid != started_by_uid))
    {
      /* Translators: Shown in authentication dialog when the user
       * attempts to change whether it has a write-intent bitmap
       */
      /* TODO: variables */
      message = N_("Authentication is required to configure the write-intent bitmap on a RAID array");
      action_id = "org.storaged.Storaged.manage-md-raid";
      if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                        STORAGED_OBJECT (object),
                                                        action_id,
                                                        options,
                                                        message,
                                                        invocation))
        goto out;
    }

  device_file = g_udev_device_get_device_file (raid_device->udev_device);
  escaped_device_file = storaged_daemon_util_escape_and_quote (device_file);

  if (!storaged_daemon_launch_spawned_job_sync (daemon,
                                                STORAGED_OBJECT (object),
                                                "md-raid-set-bitmap", caller_uid,
                                                NULL, /* GCancellable */
                                                0,    /* uid_t run_as_uid */
                                                0,    /* uid_t run_as_euid */
                                                NULL, /* gint *out_status */
                                                &error_message,
                                                NULL,  /* input_string */
                                                "mdadm --grow %s --bitmap %s",
                                                escaped_device_file,
                                                value))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             STORAGED_ERROR,
                                             STORAGED_ERROR_FAILED,
                                             "Error setting bitmap on RAID array %s: %s",
                                             device_file,
                                             error_message);
      goto out;
    }

  storaged_mdraid_complete_add_device (_mdraid, invocation);

 out:
  g_free (error_message);
  g_free (escaped_device_file);
  g_clear_object (&raid_device);
  g_clear_object (&object);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_request_sync_action (StoragedMDRaid           *_mdraid,
                            GDBusMethodInvocation    *invocation,
                            const gchar              *sync_action,
                            GVariant                 *options)
{
  StoragedLinuxMDRaid *mdraid = STORAGED_LINUX_MDRAID (_mdraid);
  StoragedDaemon *daemon;
  StoragedState *state;
  StoragedLinuxMDRaidObject *object;
  const gchar *action_id;
  const gchar *message;
  uid_t started_by_uid;
  uid_t caller_uid;
  gid_t caller_gid;
  StoragedLinuxDevice *raid_device = NULL;
  GError *error = NULL;
  gchar *sync_action_path = NULL;
  FILE *f;

  object = storaged_daemon_util_dup_object (mdraid, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = storaged_linux_mdraid_object_get_daemon (object);
  state = storaged_daemon_get_state (daemon);

  error = NULL;
  if (!storaged_daemon_util_get_caller_uid_sync (daemon,
                                                 invocation,
                                                 NULL /* GCancellable */,
                                                 &caller_uid,
                                                 &caller_gid,
                                                 NULL,
                                                 &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
      goto out;
    }

  if (!(g_strcmp0 (sync_action, "check") == 0 ||
        g_strcmp0 (sync_action, "repair") == 0 ||
        g_strcmp0 (sync_action, "idle") == 0))
    {
      g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                             "Only values 'check', 'repair' and 'idle' are currently supported.");
      goto out;
    }

  raid_device = storaged_linux_mdraid_object_get_device (object);
  if (raid_device == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                             "RAID Array is not running");
      goto out;
    }

  if (!storaged_state_has_mdraid (state,
                                  g_udev_device_get_device_number (raid_device->udev_device),
                                  &started_by_uid))
    {
      /* allow stopping arrays stuff not mentioned in mounted-fs, but treat it like root mounted it */
      started_by_uid = 0;
    }

  /* First check the user is authorized to manage RAID */
  if (caller_uid != 0 && (caller_uid != started_by_uid))
    {
      /* Translators: Shown in authentication dialog when the user
       * attempts to start/stop data scrubbing operations
       */
      /* TODO: variables */
      message = N_("Authentication is required to start/stop data scrubbing of a RAID array");
      action_id = "org.storaged.Storaged.manage-md-raid";
      if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                          STORAGED_OBJECT (object),
                                                          action_id,
                                                          options,
                                                          message,
                                                          invocation))
        goto out;
    }

  sync_action_path = g_strdup_printf ("%s/md/sync_action", g_udev_device_get_sysfs_path (raid_device->udev_device));
  f = fopen (sync_action_path, "w");
  if (f == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             STORAGED_ERROR,
                                             STORAGED_ERROR_FAILED,
                                             "Error opening %s: %m",
                                             sync_action_path);
      goto out;
    }
  else
    {
      if (fwrite (sync_action, 1, strlen (sync_action), f) != strlen (sync_action))
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 STORAGED_ERROR,
                                                 STORAGED_ERROR_FAILED,
                                                 "Error writing to sysfs file %s: %m",
                                                 sync_action_path);
          fclose (f);
          goto out;
        }
    }
  fclose (f);

  storaged_mdraid_complete_request_sync_action (_mdraid, invocation);

 out:
  g_free (sync_action_path);
  g_clear_object (&raid_device);
  g_clear_object (&object);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
storaged_linux_mdraid_delete (StoragedMDRaid           *mdraid,
                              GDBusMethodInvocation    *invocation,
                              GVariant                 *options,
                              GError                  **error)
{
  StoragedLinuxMDRaidObject *object;
  StoragedDaemon *daemon;
  uid_t caller_uid;
  gid_t caller_gid;
  const gchar *message;
  const gchar *action_id;
  gboolean teardown_flag = FALSE;
  GList *member_devices = NULL;
  GList *l;
  StoragedLinuxDevice *raid_device = NULL;
  gboolean ret;

  g_variant_lookup (options, "tear-down", "b", &teardown_flag);

  /* Delete is just stop followed by wiping of all members.
   */

  object = storaged_daemon_util_dup_object (mdraid, error);
  if (object == NULL)
    {
      ret = FALSE;
      goto out;
    }

  daemon = storaged_linux_mdraid_object_get_daemon (object);

  if (!storaged_daemon_util_get_caller_uid_sync (daemon,
                                                 invocation,
                                                 NULL /* GCancellable */,
                                                 &caller_uid,
                                                 &caller_gid,
                                                 NULL,
                                                 error))
    {
      ret = FALSE;
      goto out;
    }

  message = N_("Authentication is required to delete a RAID array");
  action_id = "org.storaged.Storaged.manage-md-raid";
  if (!storaged_daemon_util_check_authorization_sync_with_error (daemon,
                                                                 NULL,
                                                                 action_id,
                                                                 options,
                                                                 message,
                                                                 invocation,
                                                                 error))
    {
      ret = FALSE;
      goto out;
    }

  member_devices = storaged_linux_mdraid_object_get_members (object);
  raid_device = storaged_linux_mdraid_object_get_device (object);

  if (teardown_flag)
    {
      message = N_("Authentication is required to modify the system configuration");
      action_id = "org.storaged.Storaged.modify-system-configuration";
      if (!storaged_daemon_util_check_authorization_sync_with_error (daemon,
                                                                     NULL,
                                                                     action_id,
                                                                     options,
                                                                     message,
                                                                     invocation,
                                                                     error))
        {
          ret = FALSE;
          goto out;
        }

      if (raid_device)
        {
          /* The array is running, teardown its block device.
          */
          StoragedObject *block_object =
            storaged_daemon_find_block_by_device_file (daemon,
                                                       g_udev_device_get_device_file (raid_device->udev_device));
          StoragedBlock *block = block_object? storaged_object_peek_block (block_object) : NULL;
          if (block &&
              !storaged_linux_block_teardown (block,
                                              invocation,
                                              options,
                                              error))
            {
              g_clear_object (&block_object);
              ret = FALSE;
              goto out;
            }

          g_clear_object (&block_object);
        }
      else
        {
          /* The array is not running, remove the ChildConfiguration.
          */
          if (!storaged_linux_remove_configuration (storaged_mdraid_get_child_configuration (mdraid),
                                                    error))
            {
              ret = FALSE;
              goto out;
            }
        }
    }

  if (raid_device &&
      !storaged_linux_mdraid_stop (mdraid,
                                   invocation,
                                   options,
                                   error))
    {
      ret = FALSE;
      goto out;
    }

  for (l = member_devices; l; l = l->next)
    {
      StoragedLinuxDevice *member_device = STORAGED_LINUX_DEVICE (l->data);
      const gchar *device = g_udev_device_get_device_file (member_device->udev_device);
      gchar *escaped_device = storaged_daemon_util_escape_and_quote (device);
      gchar *error_message = NULL;
      int status;

      if (!storaged_daemon_launch_spawned_job_sync (daemon,
                                                    STORAGED_OBJECT (object),
                                                    "format-erase", caller_uid,
                                                    NULL, /* cancellable */
                                                    0,    /* uid_t run_as_uid */
                                                    0,    /* uid_t run_as_euid */
                                                    &status,
                                                    &error_message,
                                                    NULL, /* input_string */
                                                    "wipefs -a %s",
                                                    escaped_device))
        {
          g_set_error (error,
                       STORAGED_ERROR,
                       STORAGED_ERROR_FAILED,
                       "Error wiping device: %s",
                       error_message);
          ret = FALSE;
          g_free (escaped_device);
          g_free (error_message);
          goto out;
        }

      g_free (escaped_device);
      g_free (error_message);
    }

  ret = TRUE;

 out:
  g_list_free_full (member_devices, g_object_unref);
  g_clear_object (&raid_device);
  return ret;
}

static gboolean
handle_delete (StoragedMDRaid           *_mdraid,
               GDBusMethodInvocation    *invocation,
               GVariant                 *options)
{
  GError *error = NULL;

  if (!storaged_linux_mdraid_delete (_mdraid,
                                     invocation,
                                     options,
                                     &error))
    g_dbus_method_invocation_take_error (invocation, error);
  else
    storaged_mdraid_complete_delete (_mdraid, invocation);

  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
mdraid_iface_init (StoragedMDRaidIface *iface)
{
  iface->handle_start = handle_start;
  iface->handle_stop = handle_stop;
  iface->handle_remove_device = handle_remove_device;
  iface->handle_add_device = handle_add_device;
  iface->handle_set_bitmap_location = handle_set_bitmap_location;
  iface->handle_request_sync_action = handle_request_sync_action;
  iface->handle_delete = handle_delete;
}