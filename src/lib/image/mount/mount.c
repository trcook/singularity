/* 
 * Copyright (c) 2015-2016, Gregory M. Kurtzer. All rights reserved.
 * 
 * “Singularity” Copyright (c) 2016, The Regents of the University of California,
 * through Lawrence Berkeley National Laboratory (subject to receipt of any
 * required approvals from the U.S. Dept. of Energy).  All rights reserved.
 * 
 * This software is licensed under a customized 3-clause BSD license.  Please
 * consult LICENSE file distributed with the sources of this project regarding
 * your rights to use or distribute this software.
 * 
 * NOTICE.  This Software was developed under funding from the U.S. Department of
 * Energy and the U.S. Government consequently retains certain rights. As such,
 * the U.S. Government has been granted for itself and others acting on its
 * behalf a paid-up, nonexclusive, irrevocable, worldwide license in the Software
 * to reproduce, distribute copies to the public, prepare derivative works, and
 * perform publicly and display publicly, and to permit other to do so. 
 * 
*/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <unistd.h>
#include <stdlib.h>
#include <libgen.h>
#include <linux/limits.h>

#include "util/file.h"
#include "util/util.h"
#include "lib/message.h"
#include "lib/config_parser.h"
#include "lib/privilege.h"

#include "../image.h"

#include "./image/image.h"
#include "./dir/dir.h"
#include "./squashfs/squashfs.h"

#define ROOTFS_IMAGE    1
#define ROOTFS_DIR      2
#define ROOTFS_SQUASHFS 3

#define ROOTFS_SOURCE   "/source"
#define OVERLAY_MOUNT   "/overlay"
#define OVERLAY_UPPER   "/overlay/upper"
#define OVERLAY_WORK    "/overlay/work"
#define OVERLAY_FINAL   "/final"


static int module = -1;
static int overlay_enabled = -1;
static char *mount_point = NULL;


int _singularity_image_mount_overlayfs(void) {
    return(overlay_enabled);
}

char *_singularity_image_mount_path(void) {
    return(joinpath(mount_point, OVERLAY_FINAL));
}

int _singularity_image_mount(void) {
    char *rootfs_source = joinpath(mount_point, ROOTFS_SOURCE);
    char *overlay_mount = joinpath(mount_point, OVERLAY_MOUNT);
    char *overlay_upper = joinpath(mount_point, OVERLAY_UPPER);
    char *overlay_work  = joinpath(mount_point, OVERLAY_WORK);
    char *overlay_final = joinpath(mount_point, OVERLAY_FINAL);

    singularity_message(DEBUG, "Checking on container source type\n");

    if ( _singularity_image_mount_image_check(source) == 0 ) {
        module = ROOTFS_IMAGE;
    } else if ( _singularity_image_mount_squashfs_check(source) == 0 ) {
        module = ROOTFS_SQUASHFS;
    } else if ( _singularity_image_mount_dir_check(source) == 0 ) {
        module = ROOTFS_DIR;
    } else {
        singularity_message(ERROR, "Could not identify image format type: %s\n", source);
        ABORT(255);
    }

    singularity_message(DEBUG, "Checking 'container dir' mount location: %s\n", mount_point);
    if ( is_dir(mount_point) < 0 ) {
        singularity_priv_escalate();
        singularity_message(VERBOSE, "Creating container dir: %s\n", mount_point);
        if ( s_mkpath(mount_point, 0755) < 0 ) {
            singularity_message(ERROR, "Could not create directory: %s\n", mount_point);
            ABORT(255);
        }
        singularity_priv_drop();
    }

    singularity_message(DEBUG, "Checking for rootfs_source directory: %s\n", rootfs_source);
    if ( is_dir(rootfs_source) < 0 ) {
        singularity_priv_escalate();
        singularity_message(VERBOSE, "Creating container destination dir: %s\n", rootfs_source);
        if ( s_mkpath(rootfs_source, 0755) < 0 ) {
            singularity_message(ERROR, "Could not create directory: %s\n", rootfs_source);
            ABORT(255);
        }
        singularity_priv_drop();
    }

    singularity_message(DEBUG, "Checking for overlay_mount directory: %s\n", overlay_mount);
    if ( is_dir(overlay_mount) < 0 ) {
        singularity_priv_escalate();
        singularity_message(VERBOSE, "Creating container mount dir: %s\n", overlay_mount);
        if ( s_mkpath(overlay_mount, 0755) < 0 ) {
            singularity_message(ERROR, "Could not create directory: %s\n", overlay_mount);
            ABORT(255);
        }
        singularity_priv_drop();
    }

    singularity_message(DEBUG, "Checking for overlay_final directory: %s\n", overlay_final);
    if ( is_dir(overlay_final) < 0 ) {
        singularity_priv_escalate();
        singularity_message(VERBOSE, "Creating overlay final dir: %s\n", overlay_final);
        if ( s_mkpath(overlay_final, 0755) < 0 ) {
            singularity_message(ERROR, "Could not create directory: %s\n", overlay_final);
            ABORT(255);
        }
        singularity_priv_drop();
    }

    if ( module == ROOTFS_IMAGE ) {
        if ( _singularity_image_mount_image_mount(source, joinpath(mount_point, ROOTFS_SOURCE)) < 0 ) {
            singularity_message(ERROR, "Failed mounting image, aborting...\n");
            ABORT(255);
        }
    } else if ( module == ROOTFS_DIR ) {
        if ( _singularity_image_mount_dir_mount(source, joinpath(mount_point, ROOTFS_SOURCE)) < 0 ) {
            singularity_message(ERROR, "Failed mounting directory, aborting...\n");
            ABORT(255);
        }
    } else if ( module == ROOTFS_SQUASHFS ) {
        if ( _singularity_image_mount_squashfs_mount(source, joinpath(mount_point, ROOTFS_SOURCE)) < 0 ) {
            singularity_message(ERROR, "Failed mounting SquashFS, aborting...\n");
            ABORT(255);
        }
    } else {
        singularity_message(ERROR, "Internal error, no rootfs type defined\n");
        ABORT(255);
    }

    if ( singularity_config_get_bool(ENABLE_OVERLAY) <= 0 ) {
        singularity_message(VERBOSE3, "Not enabling overlayFS via configuration\n");
    } else if ( envar_defined("SINGULARITY_DISABLE_OVERLAYFS") == TRUE ) {
        singularity_message(VERBOSE3, "Not enabling overlayFS via environment\n");
    } else if ( envar_defined("SINGULARITY_WRITABLE") == TRUE ) {
        singularity_message(VERBOSE3, "Not enabling overlayFS, image mounted writablable\n");
    } else {
#ifdef SINGULARITY_OVERLAYFS
        int overlay_options_len = strlength(rootfs_source, PATH_MAX) + strlength(overlay_upper, PATH_MAX) + strlength(overlay_work, PATH_MAX) + 50;
        char *overlay_options = (char *) malloc(overlay_options_len);

        singularity_message(DEBUG, "OverlayFS enabled by host build\n");

        snprintf(overlay_options, overlay_options_len, "lowerdir=%s,upperdir=%s,workdir=%s", rootfs_source, overlay_upper, overlay_work); // Flawfinder: ignore

        singularity_priv_escalate();
        singularity_message(DEBUG, "Mounting overlay tmpfs: %s\n", overlay_mount);
        if ( mount("tmpfs", overlay_mount, "tmpfs", MS_NOSUID, "size=1m") < 0 ){
            singularity_message(ERROR, "Failed to mount overlay tmpfs %s: %s\n", overlay_mount, strerror(errno));
            ABORT(255);
        }

        singularity_message(DEBUG, "Creating upper overlay directory: %s\n", overlay_upper);
        if ( s_mkpath(overlay_upper, 0755) < 0 ) {
            singularity_message(ERROR, "Failed creating upper overlay directory %s: %s\n", overlay_upper, strerror(errno));
            ABORT(255);
        }

        singularity_message(DEBUG, "Creating overlay work directory: %s\n", overlay_work);
        if ( s_mkpath(overlay_work, 0755) < 0 ) {
            singularity_message(ERROR, "Failed creating overlay work directory %s: %s\n", overlay_work, strerror(errno));
            ABORT(255);
        }

        singularity_message(VERBOSE, "Mounting overlay with options: %s\n", overlay_options);
        if ( mount("overlay", overlay_final, "overlay", MS_NOSUID, overlay_options) < 0 ){
            singularity_message(ERROR, "Could not create overlay: %s\n", strerror(errno));
            ABORT(255); 
        }
        free(overlay_options);
        singularity_priv_drop();

        overlay_enabled = 1;
#else /* SINGULARITY_OVERLAYFS */
        singularity_message(VERBOSE, "OverlayFS not supported by host build\n");
#endif /* SINGULARITY_OVERLAYFS */
    }

    if ( overlay_enabled != 1 ) {
        singularity_priv_escalate();
        singularity_message(VERBOSE3, "Binding the ROOTFS_SOURCE to OVERLAY_FINAL (%s->%s)\n", joinpath(mount_point, ROOTFS_SOURCE), joinpath(mount_point, OVERLAY_FINAL));
        if ( mount(joinpath(mount_point, ROOTFS_SOURCE), joinpath(mount_point, OVERLAY_FINAL), NULL, MS_BIND|MS_NOSUID|MS_REC, NULL) < 0 ) {
            singularity_message(ERROR, "There was an error binding the path %s: %s\n", joinpath(mount_point, ROOTFS_SOURCE), strerror(errno));
            ABORT(255);
        }
        singularity_priv_drop();
    }

    return(0);
}


/*

int singularity_rootfs_check(void) {

    singularity_message(DEBUG, "Checking if container has /bin/sh...\n");
    if ( ( is_exec(joinpath(joinpath(mount_point, OVERLAY_FINAL), "/bin/sh")) < 0 ) && ( is_link(joinpath(joinpath(mount_point, OVERLAY_FINAL), "/bin/sh")) < 0 ) ) {
        singularity_message(ERROR, "Container does not have a valid /bin/sh\n");
        ABORT(255);
    }

    return(0);
}


int singularity_rootfs_chroot(void) {
    
    singularity_priv_escalate();
    singularity_message(VERBOSE, "Entering container file system root: %s\n", joinpath(mount_point, OVERLAY_FINAL));
    if ( chroot(joinpath(mount_point, OVERLAY_FINAL)) < 0 ) { // Flawfinder: ignore (yep, yep, yep... we know!)
        singularity_message(ERROR, "failed enter container at: %s\n", joinpath(mount_point, OVERLAY_FINAL));
        ABORT(255);
    }
    singularity_priv_drop();

    singularity_message(DEBUG, "Changing dir to '/' within the new root\n");
    if ( chdir("/") < 0 ) {
        singularity_message(ERROR, "Could not chdir after chroot to /: %s\n", strerror(errno));
        ABORT(1);
    }

    return(0);
}



int singularity_rootfs_init(char *source) {
    char *containername = basename(strdup(source));

    singularity_message(DEBUG, "Checking on container source type\n");

    if ( containername != NULL ) {
        setenv("SINGULARITY_CONTAINER", containername, 1);
    } else {
        setenv("SINGULARITY_CONTAINER", "unknown", 1);
    }

    singularity_message(DEBUG, "Figuring out where to mount Singularity container\n");

    mount_point = strdup(singularity_config_get_value(CONTAINER_DIR));
    singularity_message(VERBOSE3, "Set image mount path to: %s\n", mount_point);

    if ( is_file(source) == 0 ) {
        int len = strlength(source, PATH_MAX);
        if ( strcmp(&source[len - 5], ".sqsh") == 0 ) {
            module = ROOTFS_SQUASHFS;
            return(rootfs_squashfs_init(source, joinpath(mount_point, ROOTFS_SOURCE)));
        } else { // Assume it is a standard Singularity image
            module = ROOTFS_IMAGE;
            return(rootfs_image_init(source, joinpath(mount_point, ROOTFS_SOURCE)));
        }
    } else if ( is_dir(source) == 0 ) {
        module = ROOTFS_DIR;
        return(rootfs_dir_init(source, joinpath(mount_point, ROOTFS_SOURCE)));
    }

    singularity_message(ERROR, "Container not found: %s\n", source);
    ABORT(255);
    return(-1);
}


*/