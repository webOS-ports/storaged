/*
 *      Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file volumes.c
 *
 * How full each mounted volume is, and whether it is encrypted.
 *
 * Neither question had an answer anywhere on the bus. luna-prefs has
 * storageCapacity and storageFreeSpace, but those are one statfs of
 * /media/internal each, with no way to ask about the root filesystem or an SD
 * card, and no units or device to go with them. com.webos.service.pdm knows
 * about attached USB and SD devices but nothing about the internal ones. So a
 * settings panel had no way to show where the space had gone.
 *
 * storaged is the right place for it: it is the daemon that already owns
 * partitions on this device - it mounts and unmounts them for mass storage
 * mode and erases them - and it is dynamic, so answering a question here
 * costs nothing while nobody is asking.
 *
 * Both methods walk /proc/self/mountinfo. mountinfo rather than /proc/mounts
 * because it carries the mount id and parent, so a bind mount of something
 * already listed can be recognised and dropped rather than counted twice.
 */

#include <errno.h>
#include <glib.h>
#include <json.h>
#include <luna-service2/lunaservice.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
/* major()/minor() live here on glibc, not in sys/stat.h. */
#include <sys/sysmacros.h>
#include <unistd.h>

#include "util.h"
#include "volumes.h"

/* A mount worth reporting: something backed by a block device. */
typedef struct
{
    char *mountPoint;
    char *device;       /* the mount source, e.g. /dev/mmcblk0p12  */
    char *fsType;
    gboolean readOnly;
} Volume;

static void
volume_free(gpointer data)
{
    Volume *volume = (Volume *) data;

    if (!volume)
        return;

    g_free(volume->mountPoint);
    g_free(volume->device);
    g_free(volume->fsType);
    g_free(volume);
}

/**
 * @brief Undo the octal escapes the kernel writes into mountinfo.
 *
 * Paths in mountinfo have space, tab, newline and backslash written as \040,
 * \011, \012 and \134. A mount point with a space in it is not exotic - every
 * SD card labelled "My Card" produces one - and left escaped it would be
 * reported as a path that does not exist.
 */
static char *
unescape_mountinfo(const char *escaped)
{
    GString *out = g_string_sized_new(strlen(escaped));

    while (*escaped)
    {
        if (escaped[0] == '\\' && g_ascii_isdigit(escaped[1]) &&
            g_ascii_isdigit(escaped[2]) && g_ascii_isdigit(escaped[3]))
        {
            int value = (escaped[1] - '0') * 64 + (escaped[2] - '0') * 8 +
                        (escaped[3] - '0');
            g_string_append_c(out, (char) value);
            escaped += 4;
        }
        else
        {
            g_string_append_c(out, *escaped++);
        }
    }

    return g_string_free(out, FALSE);
}

/**
 * @brief Is this a filesystem holding anything a person put there?
 *
 * Everything the kernel invents - proc, sysfs, cgroups, the tmpfs on /run and
 * /var/volatile - has a size and a free figure, and reporting them would bury
 * the two or three volumes that actually matter in a page of noise. What is
 * left is what is backed by a real block device.
 *
 * Loop devices and the read-only image filesystems are dropped with them.
 * They are backed by a block device and so pass the test above, but they are
 * a file mounted as a disk - Halium's system image, a waydroid image, and on
 * a desktop every installed snap. Measured on this machine: twenty-five
 * volumes, twenty-one of them squashfs images of applications, which is not a
 * list anybody wants to read to find out how full their phone is.
 */
static gboolean
is_image_filesystem(const char *fsType)
{
    return g_strcmp0(fsType, "squashfs") == 0 ||
           g_strcmp0(fsType, "erofs") == 0 ||
           g_strcmp0(fsType, "iso9660") == 0;
}

static gboolean
is_interesting_source(const char *source)
{
    if (!source || source[0] != '/')
        return FALSE;

    if (g_str_has_prefix(source, "/dev/loop"))
        return FALSE;

    /* Covers /dev/mmcblk0p1, /dev/sda1 and /dev/mapper/<name> alike. */
    return g_str_has_prefix(source, "/dev/");
}

/**
 * @brief Read the mounted volumes out of /proc/self/mountinfo.
 *
 * Returns a GList of Volume*, in mount order, with bind mounts of a device
 * that is already in the list left out - a device is reported once, under the
 * first place it is mounted.
 */
static GList *
read_volumes(void)
{
    GList *volumes = NULL;
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    char *contents = NULL;
    GError *error = NULL;

    if (!g_file_get_contents("/proc/self/mountinfo", &contents, NULL, &error))
    {
        SHOW_ERROR(error);
        g_hash_table_destroy(seen);
        return NULL;
    }

    char **lines = g_strsplit(contents, "\n", -1);

    for (int i = 0; lines[i]; i++)
    {
        if (lines[i][0] == '\0')
            continue;

        /*
         * mountinfo is: id parent major:minor root mountPoint options
         * [optional fields...] - fsType source superOptions
         *
         * The optional fields are what makes this awkward: there may be any
         * number of them, and the single "-" is the only marker of where they
         * stop. Split on that first, then the fixed fields fall out of each
         * half.
         */
        char **halves = g_strsplit(lines[i], " - ", 2);
        if (!halves[0] || !halves[1])
        {
            g_strfreev(halves);
            continue;
        }

        char **before = g_strsplit(halves[0], " ", -1);
        char **after = g_strsplit(halves[1], " ", -1);

        /* before: id parent major:minor root mountPoint options... */
        /* after:  fsType source superOptions                       */
        if (g_strv_length(before) >= 6 && g_strv_length(after) >= 2 &&
            is_interesting_source(after[1]) && !is_image_filesystem(after[0]))
        {
            char *source = unescape_mountinfo(after[1]);

            if (!g_hash_table_contains(seen, source))
            {
                Volume *volume = g_new0(Volume, 1);

                volume->mountPoint = unescape_mountinfo(before[4]);
                volume->device = source;   /* the table takes ownership */
                volume->fsType = g_strdup(after[0]);
                volume->readOnly = g_str_has_prefix(before[5], "ro,") ||
                                   g_strcmp0(before[5], "ro") == 0;

                g_hash_table_add(seen, volume->device);
                volumes = g_list_append(volumes, volume);
            }
            else
            {
                g_free(source);
            }
        }

        g_strfreev(before);
        g_strfreev(after);
        g_strfreev(halves);
    }

    g_strfreev(lines);
    g_free(contents);
    g_hash_table_destroy(seen);

    return volumes;
}

/**
 * @brief The kernel name of whatever block device backs a path.
 *
 * /dev/mapper/home is a symlink to /dev/dm-0, and it is dm-0 that has the
 * sysfs directory saying what it is. Going through stat() rather than
 * readlink() means this works whichever name the mount table happens to
 * carry.
 */
static char *
kernel_block_name(const char *devicePath)
{
    struct stat info;

    if (stat(devicePath, &info) != 0 || !S_ISBLK(info.st_mode))
        return NULL;

    char *sysPath = g_strdup_printf("/sys/dev/block/%u:%u",
                                    major(info.st_rdev), minor(info.st_rdev));
    char *resolved = realpath(sysPath, NULL);
    g_free(sysPath);

    if (!resolved)
        return NULL;

    char *name = g_path_get_basename(resolved);
    free(resolved);

    return name;
}

/**
 * @brief What kind of encryption, if any, is under a block device.
 *
 * A device-mapper target says what it is in its sysfs "dm/uuid", which for
 * anything cryptsetup made reads CRYPT-LUKS2-<uuid>-<name>, or CRYPT-PLAIN-
 * for a plain dm-crypt mapping with no header. That is enough to answer the
 * only question a settings panel can honestly ask - is what is on this
 * volume readable without a key - without linking libcryptsetup into a daemon
 * that has no other use for it.
 *
 * Returns NULL when the volume is not encrypted; otherwise "LUKS1", "LUKS2",
 * "PLAIN" or "unknown", never freed by the caller of the table below.
 */
static char *
encryption_type(const char *devicePath)
{
    char *name = kernel_block_name(devicePath);

    if (!name)
        return NULL;

    char *uuidPath = g_strdup_printf("/sys/class/block/%s/dm/uuid", name);
    char *uuid = NULL;
    char *type = NULL;

    g_free(name);

    if (g_file_get_contents(uuidPath, &uuid, NULL, NULL))
    {
        g_strstrip(uuid);

        if (g_str_has_prefix(uuid, "CRYPT-"))
        {
            if (g_str_has_prefix(uuid, "CRYPT-LUKS2-"))
                type = g_strdup("LUKS2");
            else if (g_str_has_prefix(uuid, "CRYPT-LUKS1-"))
                type = g_strdup("LUKS1");
            else if (g_str_has_prefix(uuid, "CRYPT-PLAIN-"))
                type = g_strdup("PLAIN");
            else
                type = g_strdup("unknown");
        }
    }

    g_free(uuid);
    g_free(uuidPath);

    return type;
}

static struct json_object *
volume_to_json(const Volume *volume, gboolean withSpace, gboolean withEncryption)
{
    struct json_object *entry = json_object_new_object();

    json_object_object_add(entry, "mountPoint",
                           json_object_new_string(volume->mountPoint));
    json_object_object_add(entry, "device",
                           json_object_new_string(volume->device));
    json_object_object_add(entry, "fsType",
                           json_object_new_string(volume->fsType));
    json_object_object_add(entry, "readOnly",
                           json_object_new_boolean(volume->readOnly));

    if (withSpace)
    {
        struct statvfs space;

        if (statvfs(volume->mountPoint, &space) == 0)
        {
            /*
             * f_frsize, not f_bsize: f_bsize is the preferred I/O size and is
             * not what the block counts are in. Reported in bytes so nothing
             * downstream has to know the block size.
             *
             * "free" and "available" are two different numbers and both are
             * worth having - a filesystem keeps a reserve only root may use,
             * so "available" is what a person can still fill and "free" is
             * what is left at all.
             */
            guint64 unit = (guint64) space.f_frsize;

            json_object_object_add(entry, "sizeBytes",
                    json_object_new_int64((gint64) (space.f_blocks * unit)));
            json_object_object_add(entry, "freeBytes",
                    json_object_new_int64((gint64) (space.f_bfree * unit)));
            json_object_object_add(entry, "availableBytes",
                    json_object_new_int64((gint64) (space.f_bavail * unit)));
        }
    }

    if (withEncryption)
    {
        char *type = encryption_type(volume->device);

        json_object_object_add(entry, "encrypted",
                               json_object_new_boolean(type != NULL));
        if (type)
        {
            json_object_object_add(entry, "encryptionType",
                                   json_object_new_string(type));
            g_free(type);
        }
    }

    return entry;
}

static bool
reply_with_volumes(LSHandle *lsh, LSMessage *message, gboolean withSpace,
                   gboolean withEncryption)
{
    LSError lserror;
    LSErrorInit(&lserror);

    GList *volumes = read_volumes();
    struct json_object *reply = json_object_new_object();
    struct json_object *array = json_object_new_array();

    for (GList *entry = volumes; entry; entry = entry->next)
    {
        json_object_array_add(array,
                volume_to_json((const Volume *) entry->data, withSpace,
                               withEncryption));
    }

    json_object_object_add(reply, "returnValue", json_object_new_boolean(TRUE));
    json_object_object_add(reply, "volumes", array);

    if (!LSMessageReply(lsh, message, json_object_to_json_string(reply),
                        &lserror))
    {
        LSREPORT(lserror);
    }

    json_object_put(reply);
    g_list_free_full(volumes, volume_free);
    LSErrorFree(&lserror);

    return true;
}

//->Start of API documentation comment block
/**
@page com_palm_storage com.palm.storage
@{
@section com_palm_storage_volumes_getSpaceInfo getSpaceInfo

How large each mounted volume is and how much of it is left.

@par Parameters
None.

@par Returns(Call)
Name        | Required | Type    | Description
------------|----------|---------|------------
returnValue | yes      | Boolean | True
volumes     | yes      | Array   | One entry per volume: mountPoint, device, fsType, readOnly, sizeBytes, freeBytes, availableBytes

@par Returns(Subscription)
None
@}
*/
//->End of API documentation comment block
static bool
handle_get_space_info(LSHandle *lsh, LSMessage *message, void *user_data)
{
    LSTRACE_LSMESSAGE(message);

    return reply_with_volumes(lsh, message, TRUE, FALSE);
}

//->Start of API documentation comment block
/**
@page com_palm_storage com.palm.storage
@{
@section com_palm_storage_volumes_getEncryptionStatus getEncryptionStatus

Whether each mounted volume is behind device-mapper encryption.

@par Parameters
None.

@par Returns(Call)
Name        | Required | Type    | Description
------------|----------|---------|------------
returnValue | yes      | Boolean | True
volumes     | yes      | Array   | One entry per volume: mountPoint, device, fsType, readOnly, encrypted, and encryptionType ("LUKS1", "LUKS2" or "PLAIN") where it is

@par Returns(Subscription)
None
@}
*/
//->End of API documentation comment block
static bool
handle_get_encryption_status(LSHandle *lsh, LSMessage *message, void *user_data)
{
    LSTRACE_LSMESSAGE(message);

    return reply_with_volumes(lsh, message, TRUE, TRUE);
}

static LSMethod volumes_mthds[] = {
    { "getSpaceInfo", handle_get_space_info },
    { "getEncryptionStatus", handle_get_encryption_status },
    { },
};

int
VolumesInit(GMainLoop *loop, LSHandle *handle)
{
    LSError lserror;
    LSErrorInit(&lserror);

    if (!LSRegisterCategory(handle, "/volumes", volumes_mthds, NULL, NULL,
                            &lserror))
    {
        LSREPORT(lserror);
    }

    LSErrorFree(&lserror);

    return 0;
}
