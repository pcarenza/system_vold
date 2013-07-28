/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <fcntl.h>
#include <dirent.h>
#include <fs_mgr.h>

#define LOG_TAG "Vold"

#include "cutils/klog.h"
#include "cutils/log.h"
#include "cutils/properties.h"

#include "VolumeManager.h"
#include "CommandListener.h"
#include "NetlinkManager.h"
#include "DirectVolume.h"
#include "AutoVolume.h"
#include "cryptfs.h"

static int process_config(VolumeManager *vm);
static void coldboot(const char *path);

#define FSTAB_PREFIX "/fstab."
struct fstab *fstab;

int main() {

    VolumeManager *vm;
    CommandListener *cl;
    NetlinkManager *nm;

    SLOGI("Vold 2.1 (the revenge) firing up");

    mkdir("/dev/block/vold", 0755);

    /* For when cryptfs checks and mounts an encrypted filesystem */
    klog_set_level(6);

    /* Create our singleton managers */
    if (!(vm = VolumeManager::Instance())) {
        SLOGE("Unable to create VolumeManager");
        exit(1);
    };

    if (!(nm = NetlinkManager::Instance())) {
        SLOGE("Unable to create NetlinkManager");
        exit(1);
    };


    cl = new CommandListener();
    vm->setBroadcaster((SocketListener *) cl);
    nm->setBroadcaster((SocketListener *) cl);

    if (vm->start()) {
        SLOGE("Unable to start VolumeManager (%s)", strerror(errno));
        exit(1);
    }

    if (process_config(vm)) {
        SLOGE("Error reading configuration (%s)... continuing anyways", strerror(errno));
    }

    if (nm->start()) {
        SLOGE("Unable to start NetlinkManager (%s)", strerror(errno));
        exit(1);
    }

    coldboot("/sys/block");
//    coldboot("/sys/class/switch");

    /*
     * Now that we're up, we can respond to commands
     */
    if (cl->startListener()) {
        SLOGE("Unable to start CommandListener (%s)", strerror(errno));
        exit(1);
    }

    // Eventually we'll become the monitoring thread
    while(1) {
        sleep(1000);
    }

    SLOGI("Vold exiting");
    exit(0);
}

static void do_coldboot(DIR *d, int lvl)
{
    struct dirent *de;
    int dfd, fd;

    dfd = dirfd(d);

    fd = openat(dfd, "uevent", O_WRONLY);
    if(fd >= 0) {
        write(fd, "add\n", 4);
        close(fd);
    }

    while((de = readdir(d))) {
        DIR *d2;

        if (de->d_name[0] == '.')
            continue;

        if (de->d_type != DT_DIR && lvl > 0)
            continue;

        fd = openat(dfd, de->d_name, O_RDONLY | O_DIRECTORY);
        if(fd < 0)
            continue;

        d2 = fdopendir(fd);
        if(d2 == 0)
            close(fd);
        else {
            do_coldboot(d2, lvl + 1);
            closedir(d2);
        }
    }
}

static void coldboot(const char *path)
{
    DIR *d = opendir(path);
    if(d) {
        do_coldboot(d, 0);
        closedir(d);
    }
}


static int process_config(VolumeManager *vm)
{
    char *save_ptr;
    int flags = 0;

    if (strcasestr(mount_flags, "encryptable")) {
        flags |= VOL_ENCRYPTABLE;
    }

    if (strcasestr(mount_flags, "nonremovable")) {
        flags |= VOL_NONREMOVABLE;
    }

    return flags;
}

static int process_config(VolumeManager *vm) {
    FILE *fp;
    int n = 0;
    char line[255];
    Volume *vol = 0;

    if ((fp = fopen("/proc/cmdline", "r"))) {
        while (fscanf(fp, "%s", line) > 0) {
            if (!strncmp(line, "SDCARD=", 7)) {
                const char *sdcard = line + 7;
                if (*sdcard) {
                    // FIXME: should not hardcode the label and mount_point
                    if ((vol = new AutoVolume(vm, "sdcard", "/mnt/sdcard", sdcard))) {
                        vm->addVolume(vol);
                        break;
                    }
                }
            }
        }
        fclose(fp);
    }

    if (!(fp = fopen("/etc/vold.fstab", "r"))) {
        // no volume added yet, create a AutoVolume object
        // to mount USB/MMC/SD automatically
        if (!vol) {
            // FIXME: should not hardcode the label and mount_point
            vol = new AutoVolume(vm, "sdcard", "/mnt/sdcard");
            if (vol)
                vm->addVolume(vol);
        }
        return vol ? 0 : -ENOMEM;
    }

    /* Loop through entries looking for ones that vold manages */
    for (i = 0; i < fstab->num_entries; i++) {
        if (fs_mgr_is_voldmanaged(&fstab->recs[i])) {
            DirectVolume *dv = NULL;
            flags = 0;

            dv = new DirectVolume(vm, fstab->recs[i].label,
                                  fstab->recs[i].mount_point,
                                  fstab->recs[i].partnum);

            const char *sdcard = 0;
            while ((sysfs_path = strtok_r(NULL, delim, &save_ptr))) {
                if ((sdcard = strncmp(sysfs_path, "SDCARD=", 7) ? 0 : sysfs_path + 7))
                    break;
                if (!dv) {
                     if (!strcmp(part, "auto")) {
                        dv = new DirectVolume(vm, label, mount_point, -1);
                    } else {
                        dv = new DirectVolume(vm, label, mount_point, atoi(part));
                    }
                }
                if (*sysfs_path != '/') {
                    /* If the first character is not a '/', it must be flags */
                    break;
                }
                if (dv->addPath(sysfs_path)) {
                    SLOGE("Failed to add devpath %s to volume %s", sysfs_path,
                         label);
                    goto out_fail;
                }
            }

            if (!dv) {
                dv = new AutoVolume(vm, label, mount_point, sdcard);
            }

            /* If sysfs_path is non-null at this point, then it contains
             * the optional flags for this volume
             */
            if (sysfs_path)
                flags = parse_mount_flags(sysfs_path);
            else
                flags = 0;
            dv->setFlags(flags);

            vm->addVolume(dv);
        }
    }

    ret = 0;

out_fail:
    return ret;
}
