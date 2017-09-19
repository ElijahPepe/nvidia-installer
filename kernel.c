/*
 * nvidia-installer: A tool for installing NVIDIA software packages on
 * Unix and Linux systems.
 *
 * Copyright (C) 2003-2009 NVIDIA Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses>.
 */

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <ctype.h>
#include <stdlib.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <limits.h>
#include <fts.h>

#include "nvidia-installer.h"
#include "kernel.h"
#include "user-interface.h"
#include "files.h"
#include "misc.h"
#include "precompiled.h"
#include "crc.h"
#include "conflicting-kernel-modules.h"

/* local prototypes */

static char *default_kernel_module_installation_path(Options *op);
static char *default_kernel_source_path(Options *op);
static int check_for_loaded_kernel_module(Options *op, const char *);
static void check_for_warning_messages(Options *op);
static int fbdev_check(Options *op, Package *p);
static int xen_check(Options *op, Package *p);

static PrecompiledInfo *scan_dir(Options *op, Package *p,
                                 const char *directory_name,
                                 const char *output_filename,
                                 const char *proc_version_string);

static char *build_distro_precompiled_kernel_interface_dir(Options *op);
static char *convert_include_path_to_source_path(const char *inc);
static char *guess_kernel_module_filename(Options *op);
static char *get_machine_arch(Options *op);
static void load_kernel_module_quiet(Options *op, const char *module_name);
static void modprobe_remove_kernel_module_quiet(Options *op, const char *name);

/*
 * Message text that is used by several error messages.
 */

static const char install_your_kernel_source[] = 
"Please make sure you have installed the kernel source files for "
"your kernel and that they are properly configured; on Red Hat "
"Linux systems, for example, be sure you have the 'kernel-source' "
"or 'kernel-devel' RPM installed.  If you know the correct kernel "
"source files are installed, you may specify the kernel source "
"path with the '--kernel-source-path' command line option.";

 


/*
 * determine_kernel_module_installation_path() - get the installation
 * path for the kernel module.  The order is:
 *
 * - if op->kernel_module_installation_path is non-NULL, then it must
 *   have been initialized by the commandline parser, and therefore we
 *   should obey that (so just return).
 *
 * - get the default installation path
 *
 * - if in expert mode, ask the user, and use what they gave, if
 *   non-NULL
 */

int determine_kernel_module_installation_path(Options *op)
{
    char *result;
    int count = 0;
    
    if (op->kernel_module_installation_path) return TRUE;
    
    op->kernel_module_installation_path =
        default_kernel_module_installation_path(op);
    
    if (!op->kernel_module_installation_path) return FALSE;

    if (op->expert) {

    ask_for_kernel_install_path:

        result = ui_get_input(op, op->kernel_module_installation_path,
                              "Kernel module installation path");
        if (result && result[0]) {
            free(op->kernel_module_installation_path);
            op->kernel_module_installation_path = result;
            if (!confirm_path(op, op->kernel_module_installation_path)) {
                return FALSE;
            }
        } else {
            if (result) free(result);
            
            if (++count < NUM_TIMES_QUESTIONS_ASKED) {
                ui_warn(op, "Invalid kernel module installation path.");
                goto ask_for_kernel_install_path;
            } else {
                ui_error(op, "Unable to determine kernel module "
                         "installation path.");

                return FALSE;
            }
        }
    }

    if (!mkdir_recursive(op, op->kernel_module_installation_path, 0755))
        return FALSE;

    ui_expert(op, "Kernel module installation path: %s",
              op->kernel_module_installation_path);

    return TRUE;
    
} /* determine_kernel_module_installation_path() */



/*
 * determine_kernel_source_path() - find the qualified path to the
 * kernel source tree.  This is called from install_from_cwd() if we
 * need to compile the kernel interface files.  Assigns
 * op->kernel_source_path and returns TRUE if successful.  Returns
 * FALSE if no kernel source tree was found.
 */

int determine_kernel_source_path(Options *op, Package *p)
{
    char *CC, *cmd, *result;
    char *source_files[2], *source_path;
    char *arch;
    int ret, count = 0;
    
    /* determine the kernel source path */
    
    op->kernel_source_path = default_kernel_source_path(op);
    
    if (op->expert) {
        
    ask_for_kernel_source_path:
        
        result = ui_get_input(op, op->kernel_source_path,
                              "Kernel source path");
        if (result && result[0]) {
            if (!directory_exists(op, result)) {
                ui_warn(op, "Kernel source path '%s' does not exist.",
                        result);
                free(result);
                
                if (++count < NUM_TIMES_QUESTIONS_ASKED) {
                    goto ask_for_kernel_source_path;
                } else {
                    op->kernel_source_path = NULL;
                }
            } else {
                op->kernel_source_path = result;
            }
        } else {
            ui_warn(op, "Invalid kernel source path.");
            if (result) free(result);
            
            if (++count < NUM_TIMES_QUESTIONS_ASKED) {
                goto ask_for_kernel_source_path;
            } else {
                op->kernel_source_path = NULL;
            }
        }
    }

    /* if we STILL don't have a kernel source path, give up */
    
    if (!op->kernel_source_path) {
        ui_error(op, "Unable to find the kernel source tree for the "
                 "currently running kernel.  %s", install_your_kernel_source);
        
        /*
         * I suppose we could ask them here for the kernel source
         * path, but we've already given users multiple methods of
         * specifying their kernel source tree.
         */
        
        return FALSE;
    }

    /* reject /usr as an invalid kernel source path */

    if (!strcmp(op->kernel_source_path, "/usr") ||
            !strcmp(op->kernel_source_path, "/usr/")) {
        ui_error (op, "The kernel source path '%s' is invalid.  %s",
                  op->kernel_source_path, install_your_kernel_source);
        op->kernel_source_path = NULL;
        return FALSE;
    }

    /* check that the kernel source path exists */

    if (!directory_exists(op, op->kernel_source_path)) {
        ui_error (op, "The kernel source path '%s' does not exist.  %s",
                  op->kernel_source_path, install_your_kernel_source);
        op->kernel_source_path = NULL;
        return FALSE;
    }

    /* check that <path>/include/linux/kernel.h exists */

    result = nvstrcat(op->kernel_source_path, "/include/linux/kernel.h", NULL);
    if (access(result, F_OK) == -1) {
        ui_error(op, "The kernel header file '%s' does not exist.  "
                 "The most likely reason for this is that the kernel source "
                 "path '%s' is incorrect.  %s", result,
                 op->kernel_source_path, install_your_kernel_source);
        free(result);
        return FALSE;
    }
    free(result);

    arch = get_machine_arch(op);
    if (!arch)
        return FALSE;
    if (!determine_kernel_output_path(op)) return FALSE;

    CC = getenv("CC");
    if (!CC) CC = "cc";
    
    cmd = nvstrcat("sh ", p->kernel_module_build_directory,
                   "/conftest.sh ", CC, " ", CC, " ", arch, " ",
                   op->kernel_source_path, " ",
                   op->kernel_output_path, " ",
                   "get_uname", NULL);
    
    ret = run_command(op, cmd, &result, FALSE, 0, TRUE);
    nvfree(cmd);

    if (ret != 0) {
        ui_error(op, "Unable to determine the version of the kernel "
                 "sources located in '%s'.  %s",
                 op->kernel_source_path, install_your_kernel_source);
        free(result);
        return FALSE;
    }

    if (strncmp(result, "2.4", 3) == 0) {
        source_files[0] = nvstrcat(op->kernel_source_path,
                                   "/include/linux/version.h", NULL);
        source_files[1] = NULL;
        source_path = op->kernel_source_path;
    } else {
        source_files[0] = nvstrcat(op->kernel_output_path,
                                   "/include/linux/version.h", NULL);
        source_files[1] = nvstrcat(op->kernel_output_path,
                                   "/include/generated/uapi/linux/version.h",
                                   NULL);
        source_path = op->kernel_output_path;
    }
    free(result);

    if (access(source_files[0], F_OK) != 0) {
        if (!source_files[1]) {
            ui_error(op, "The kernel header file '%s' does not exist.  "
                     "The most likely reason for this is that the kernel "
                     "source files in '%s' have not been configured.",
                     source_files[0], source_path);
            return FALSE;
        } else if (access(source_files[1], F_OK) != 0) {
            ui_error(op, "Neither the '%s' nor the '%s' kernel header "
                     "file exists.  The most likely reason for this "
                     "is that the kernel source files in '%s' have not been "
                     "configured.",
                     source_files[0], source_files[1], source_path);
            return FALSE;
        }
    }

    /* OK, we seem to have a path to a configured kernel source tree */
    
    ui_log(op, "Kernel source path: '%s'\n", op->kernel_source_path);
    ui_log(op, "Kernel output path: '%s'\n", op->kernel_output_path);
    
    return TRUE;
    
} /* determine_kernel_source_path() */


/*
 * determine_kernel_output_path() - determine the kernel output
 * path; unless specified, the kernel output path is assumed to be
 * the same as the kernel source path.
 */

int determine_kernel_output_path(Options *op)
{
    char *str, *tmp;
    int len;

    /* check --kernel-output-path */

    if (op->kernel_output_path) {
        ui_log(op, "Using the kernel output path '%s' as specified by the "
               "'--kernel-output-path' commandline option.",
               op->kernel_output_path);

        if (!directory_exists(op, op->kernel_output_path)) {
            ui_error(op, "The kernel output path '%s' does not exist.",
                     op->kernel_output_path);
            op->kernel_output_path = NULL;
            return FALSE;
        }

        return TRUE;
    }

    /* check SYSOUT */

    str = getenv("SYSOUT");
    if (str) {
        ui_log(op, "Using the kernel output path '%s', as specified by the "
               "SYSOUT environment variable.", str);
        op->kernel_output_path = str;

        if (!directory_exists(op, op->kernel_output_path)) {
            ui_error(op, "The kernel output path '%s' does not exist.",
                     op->kernel_output_path);
            op->kernel_output_path = NULL;
            return FALSE;
        }

        return TRUE;
    }

    /* check /lib/modules/`uname -r`/{source,build} */

    tmp = get_kernel_name(op);

    if (tmp) {
        str = nvstrcat("/lib/modules/", tmp, "/source", NULL);
        len = strlen(str);

        if (!strncmp(op->kernel_source_path, str, len)) {
            nvfree(str);
            str = nvstrcat("/lib/modules/", tmp, "/build", NULL);

            if (directory_exists(op, str)) {
                op->kernel_output_path = str;
                return TRUE;
            }
        }

        nvfree(str);
    }

    op->kernel_output_path = op->kernel_source_path;
    return TRUE;
}


/*
 * link_kernel_module() - link the prebuilt kernel interface against
 * the binary-only core of the kernel module.  This results in a
 * complete kernel module, ready for installation.
 *
 *
 * ld -r -o nvidia.o nv-linux.o nv-kernel.o
 */

int link_kernel_module(Options *op, Package *p)
{
    char *cmd, *result;
    int ret;
    
    p->kernel_module_filename = guess_kernel_module_filename(op);

    cmd = nvstrcat("cd ", p->kernel_module_build_directory,
                   "; ", op->utils[LD],
                   " ", LD_OPTIONS,
                   " -o ", p->kernel_module_filename,
                   " ", PRECOMPILED_KERNEL_INTERFACE_FILENAME,
                   " nv-kernel.o", NULL);
    
    ret = run_command(op, cmd, &result, TRUE, 0, TRUE);
    
    free(cmd);

    if (ret != 0) {
        ui_error(op, "Unable to link kernel module.");
        return FALSE;
    }

    ui_log(op, "Kernel module linked successfully.");

    return TRUE;
   
} /* link_kernel_module() */


/*
 * build_kernel_module() - determine the kernel include directory,
 * copy the kernel module source files into a temporary directory, and
 * compile nvidia.o.
 */

int build_kernel_module(Options *op, Package *p)
{
    char *CC, *result, *cmd, *tmp;
    char *arch;
    int len, ret;
    
    arch = get_machine_arch(op);
    if (!arch)
        return FALSE;

    /*
     * touch all the files in the build directory to avoid make time
     * skew messages
     */
    
    touch_directory(op, p->kernel_module_build_directory);
    
    CC = getenv("CC");
    if (!CC) CC = "cc";
    
    /*
     * Check if conftest.sh can determine the Makefile, there's
     * no hope for the make rules if this fails.
     */
    cmd = nvstrcat("sh ", p->kernel_module_build_directory,
                   "/conftest.sh ", CC, " ", CC, " ", arch, " ",
                   op->kernel_source_path, " ",
                   op->kernel_output_path, " ",
                   "select_makefile just_msg", NULL);

    ret = run_command(op, cmd, &result, FALSE, 0, TRUE);
    nvfree(cmd);

    if (ret != 0) {
        ui_error(op, "%s", result); /* display conftest.sh's error message */
        nvfree(result);
        return FALSE;
    }

    if (!fbdev_check(op, p)) return FALSE;
    if (!xen_check(op, p)) return FALSE;

    cmd = nvstrcat("cd ", p->kernel_module_build_directory,
                   "; make print-module-filename",
                   " SYSSRC=", op->kernel_source_path,
                   " SYSOUT=", op->kernel_output_path, NULL);
    
    ret = run_command(op, cmd, &p->kernel_module_filename, FALSE, 0, FALSE);
    
    free(cmd);

    if (ret != 0) {
        ui_error(op, "Unable to determine the NVIDIA kernel module filename.");
        nvfree(result);
        return FALSE;
    }
    
    ui_log(op, "Cleaning kernel module build directory.");
    
    len = strlen(p->kernel_module_build_directory) + 32;
    cmd = nvalloc(len);
    
    snprintf(cmd, len, "cd %s; make clean", p->kernel_module_build_directory);

    ret = run_command(op, cmd, &result, TRUE, 0, TRUE);
    free(result);
    free(cmd);
    
    ui_status_begin(op, "Building kernel module:", "Building");

    cmd = nvstrcat("cd ", p->kernel_module_build_directory,
                   "; make module",
                   " SYSSRC=", op->kernel_source_path,
                   " SYSOUT=", op->kernel_output_path, NULL);
    
    ret = run_command(op, cmd, &result, TRUE, 25, TRUE);

    free(cmd);

    if (ret != 0) {
        ui_status_end(op, "Error.");
        ui_error(op, "Unable to build the NVIDIA kernel module.");
        /* XXX need more descriptive error message */
        return FALSE;
    }
    
    /* check that the file actually exists */

    tmp = nvstrcat(p->kernel_module_build_directory, "/",
                   p->kernel_module_filename, NULL);
    if (access(tmp, F_OK) == -1) {
        free(tmp);
        ui_status_end(op, "Error.");
        ui_error(op, "The NVIDIA kernel module was not created.");
        return FALSE;
    }
    free(tmp);
    
    ui_status_end(op, "done.");

    ui_log(op, "Kernel module compilation complete.");

    return TRUE;
    
} /* build_kernel_module() */



/*
 * build_kernel_interface() - build the kernel interface, and place it
 * here:
 *
 * "%s/%s", p->kernel_module_build_directory,
 * PRECOMPILED_KERNEL_INTERFACE_FILENAME
 *
 * This is done by copying the sources to a temporary working
 * directory, building, and copying the kernel interface back to the
 * kernel module source directory.  The tmpdir is removed when
 * complete.
 *
 * XXX this and build_kernel_module() should be merged.
 */

int build_kernel_interface(Options *op, Package *p)
{
    char *tmpdir = NULL;
    char *cmd = NULL;
    char *kernel_interface = NULL;
    char *dstfile = NULL;
    int ret = FALSE;
    int command_ret;

    /* create a temporary directory */

    tmpdir = make_tmpdir(op);

    if (!tmpdir) {
        ui_error(op, "Unable to create a temporary build directory.");
        return FALSE;
    }
    
    /* copy the kernel module sources to it */
    
    ui_log(op, "Copying kernel module sources to temporary directory.");

    if (!copy_directory_contents
        (op, p->kernel_module_build_directory, tmpdir)) {
        ui_error(op, "Unable to copy the kernel module sources to temporary "
                 "directory '%s'.", tmpdir);
        goto failed;
    }
    
    /*
     * touch the contents of the build directory, to avoid make time
     * skew error messages
     */

    touch_directory(op, p->kernel_module_build_directory);

    /* build the kernel interface */

    ui_status_begin(op, "Building kernel interface:", "Building");
    
    cmd = nvstrcat("cd ", tmpdir, "; make ", p->kernel_interface_filename,
                   " SYSSRC=", op->kernel_source_path, NULL);
    
    command_ret = run_command(op, cmd, NULL, TRUE, 25 /* XXX */, TRUE);

    if (command_ret != 0) {
        ui_status_end(op, "Error.");
        ui_error(op, "Unable to build the NVIDIA kernel module interface.");
        /* XXX need more descriptive error message */
        goto failed;
    }
    
    /* check that the file exists */

    kernel_interface = nvstrcat(tmpdir, "/",
                                p->kernel_interface_filename, NULL);
    
    if (access(kernel_interface, F_OK) == -1) {
        ui_status_end(op, "Error.");
        ui_error(op, "The NVIDIA kernel module interface was not created.");
        goto failed;
    }

    ui_status_end(op, "done.");

    ui_log(op, "Kernel module interface compilation complete.");

    /* copy the kernel interface from the tmpdir back to the srcdir */
    
    dstfile = nvstrcat(p->kernel_module_build_directory, "/",
                       PRECOMPILED_KERNEL_INTERFACE_FILENAME, NULL);

    if (!copy_file(op, kernel_interface, dstfile, 0644)) goto failed;
    
    ret = TRUE;
    
 failed:
    
    remove_directory(op, tmpdir);

    if (tmpdir) nvfree(tmpdir);
    if (cmd) nvfree(cmd);
    if (kernel_interface) nvfree(kernel_interface);
    if (dstfile) nvfree(dstfile);

    return ret;

} /* build_kernel_interface() */



/*
 * check_for_warning_messages() - check if the kernel module detected
 * problems with the target system and registered warning messages
 * for us with the Linux /proc interface. If yes, show these messages
 * to the user.
 */

void check_for_warning_messages(Options *op)
{
    char *paths[2] = { "/proc/driver/nvidia/warnings", NULL };
    FTS *fts;
    FTSENT *ent;
    char *buf = NULL;

    fts = fts_open(paths, FTS_LOGICAL, NULL);
    if (!fts) return;

    while ((ent = fts_read(fts)) != NULL) {
        switch (ent->fts_info) {
            case FTS_F:
                if ((strlen(ent->fts_name) == 6) &&
                    !strncmp("README", ent->fts_name, 6))
                    break;
                if (read_text_file(ent->fts_path, &buf)) {
                    ui_warn(op, "%s", buf);
                    nvfree(buf);
                }
                break;
            default:
                /* ignore this file entry */
                break;
        }
    }

    fts_close(fts);

} /* check_for_warning_messages() */



#define PRINTK_LOGLEVEL_KERN_ALERT 1

/*
 * Attempt to set the printk loglevel, first using the /proc/sys interface,
 * and falling back to the deprecated sysctl if that fails. Pass the previous
 * loglevel back to the caller and return TRUE on success, or FALSE on failure.
 */
static int set_loglevel(int level, int *old_level)
{
    FILE *fp;
    int loglevel_set = FALSE;

    fp = fopen("/proc/sys/kernel/printk", "r+");
    if (fp) {
        if (!old_level || fscanf(fp, "%d ", old_level) == 1) {
            char *strlevel = nvasprintf("%d", level);

            fseek(fp, 0, SEEK_SET);
            if (fwrite(strlevel, strlen(strlevel), 1, fp) == 1) {
                loglevel_set = TRUE;
            }

            nvfree(strlevel);
        }
        fclose(fp);
    }

    if (!loglevel_set) {
        /*
         * Explicitly initialize the value of len, even though it looks like the
         * syscall should do that, since in practice it doesn't always actually
         * set the value of the pointed-to length parameter.
         */
        size_t len = sizeof(int);
        int name[] = { CTL_KERN, KERN_PRINTK };

        if (!old_level ||
            sysctl(name, ARRAY_LEN(name), old_level, &len, NULL, 0) == 0) {
            if (sysctl(name, ARRAY_LEN(name), NULL, 0, &level, len) == 0) {
                loglevel_set = TRUE;
            }
        }
    }

    return loglevel_set;
}



int test_kernel_module(Options *op, Package *p)
{
    char *cmd = NULL, *data, *kernel_module_fullpath;
    int ret, i, old_loglevel, loglevel_set;
    const char *depmods[] = { "agpgart", "i2c-core", "drm" };

    /* SELinux type labels to add to kernel modules */
    static const char *selinux_kmod_types[] = {
        "modules_object_t",
        NULL
    };

    /* 
     * If we're building/installing for a different kernel, then we
     * can't test the module now.
     */

    if (op->kernel_name) return TRUE;

    /*
     * Attempt to load modules that nvidia.ko might depend on.  Silently ignore
     * failures: if nvidia.ko doesn't depend on the module that failed, the test
     * load below will succeed and it doesn't matter that the load here failed.
     */
    if (strncmp(get_kernel_name(op), "2.4", 3) != 0) {
        for (i = 0; i < ARRAY_LEN(depmods); i++) {
            load_kernel_module_quiet(op, depmods[i]);
        }
    }

    kernel_module_fullpath = nvstrcat(p->kernel_module_build_directory, "/",
                                      p->kernel_module_filename, NULL);

    /* Some SELinux policies require specific file type labels for inserting
     * kernel modules. Attempt to set known types until one succeeds. If all
     * types fail, hopefully it's just because no type is needed. */
    for (i = 0; selinux_kmod_types[i]; i++) {
        if (set_security_context(op, kernel_module_fullpath,
                                 selinux_kmod_types[i])) {
            break;

        }
    }

    cmd = nvstrcat(op->utils[INSMOD], " ",
                   kernel_module_fullpath,
                   " NVreg_DeviceFileUID=0 NVreg_DeviceFileGID=0"
                   " NVreg_DeviceFileMode=0 NVreg_ModifyDeviceFiles=0",
                   NULL);

    nvfree(kernel_module_fullpath);

    loglevel_set = set_loglevel(PRINTK_LOGLEVEL_KERN_ALERT, &old_loglevel);

    /* only output the result of the test if in expert mode */

    ret = run_command(op, cmd, &data, op->expert, 0, TRUE);

    if (loglevel_set) {
        set_loglevel(old_loglevel, NULL);
    }

    if (ret != 0) {
        ui_error(op, "Unable to load the kernel module '%s'.  This "
                 "happens most frequently when this kernel module was "
                 "built against the wrong or improperly configured "
                 "kernel sources, with a version of gcc that differs "
                 "from the one used to build the target kernel, or "
                 "if a driver such as rivafb, nvidiafb, or nouveau is present "
                 "and prevents the NVIDIA kernel module from obtaining "
                 "ownership of the NVIDIA graphics device(s), or "
                 "NVIDIA GPU installed in this system is not supported "  
                 "by this NVIDIA Linux graphics driver release.\n\n" 
                 "Please see the log entries 'Kernel module load "
                 "error' and 'Kernel messages' at the end of the file "
                 "'%s' for more information.",
                 p->kernel_module_filename, op->log_file_name);

        /*
         * if in expert mode, run_command() would have caused this to
         * be written to the log file; so if not in expert mode, print
         * the output now.
         */

        if (!op->expert) ui_log(op, "Kernel module load error: %s", data);
        ret = FALSE;

    } else {
        /*
         * check if the kernel module detected problems with this
         * system's kernel and display any warning messages it may
         * have prepared for us.
         */

        check_for_warning_messages(op);

        ret = TRUE;
    }
   
    nvfree(cmd); 
    nvfree(data);

    rmmod_kernel_module(op, p->kernel_module_name);

    /*
     * display/log the last few lines of the kernel ring buffer
     * to provide further details in case of a load failure or
     * to capture NVRM warning messages, if any.
     */
    cmd = nvstrcat(op->utils[DMESG], " | ",
                   op->utils[TAIL], " -n 25", NULL);

    if (!run_command(op, cmd, &data, FALSE, 0, TRUE))
        ui_log(op, "Kernel messages:\n%s", data);

    nvfree(cmd);
    nvfree(data);

    /*
     * Unload dependencies that might have been loaded earlier.
     */
    if (strncmp(get_kernel_name(op), "2.4", 3) != 0) {
        for (i = 0; i < ARRAY_LEN(depmods); i++) {
           modprobe_remove_kernel_module_quiet(op, depmods[i]);
        }
    }

    return ret;
    
} /* test_kernel_module() */



/*
 * modprobe_helper() - run modprobe; used internally by other functions.
 *
 * module_name: the name of the kernel module to modprobe
 * quiet:       load/unload the kernel module silently if TRUE
 * unload:      remove a kernel module instead of loading it if TRUE
 *              (Note: unlike `rmmod`, `modprobe -r` handles dependencies.
 */

static int modprobe_helper(Options *op, const char *module_name,
                           int quiet, int unload)
{
    char *cmd, *data;
    int ret, old_loglevel, loglevel_set;

    if (op->skip_module_load) {
        return TRUE;
    }

    cmd = nvstrcat(op->utils[MODPROBE],
                   quiet ? " -q" : "",
                   unload ? " -r" : "",
                   " ", module_name,
                   NULL);

    loglevel_set = set_loglevel(PRINTK_LOGLEVEL_KERN_ALERT, &old_loglevel);
    
    ret = run_command(op, cmd, &data, FALSE, 0, TRUE);

    if (loglevel_set) {
        set_loglevel(old_loglevel, NULL);
    }

    if (!quiet && ret != 0) {
        if (op->expert) {
            ui_error(op, "Unable to %s the kernel module: '%s'",
                     unload ? "unload" : "load",
                     data);
        } else {
            ui_error(op, "Unable to %s the kernel module.",
                     unload ? "unload" : "load");
        }
        ret = FALSE;
    } else {
        ret = TRUE;
    }

    nvfree(cmd);
    nvfree(data);
    
    return ret;

} /* load_kernel_module() */

int load_kernel_module(Options *op, Package *p)
{
    return modprobe_helper(op, p->kernel_module_name, FALSE, FALSE);
}

static void load_kernel_module_quiet(Options *op, const char *module_name)
{
    modprobe_helper(op, module_name, TRUE, FALSE);
}

static void modprobe_remove_kernel_module_quiet(Options *op, const char *name)
{
    modprobe_helper(op, name, TRUE, TRUE);
}


/*
 * check_for_unloaded_kernel_module() - test if any of the "bad"
 * kernel modules are loaded; if they are, then try to unload it.  If
 * we can't unload it, then report an error and return FALSE;
 */

int check_for_unloaded_kernel_module(Options *op)
{
    int n;
    int loaded = FALSE;
    unsigned long long int bits = 0;
    
    /*
     * We can skip this check if we are installing for a non-running
     * kernel and only installing a kernel module.
     */
    
    if (op->kernel_module_only && op->kernel_name) {
        ui_log(op, "Only installing a kernel module for a non-running "
               "kernel; skipping the \"is an NVIDIA kernel module loaded?\" "
               "test.");
        return TRUE;
    }

    /*
     * We can also skip this check if we aren't installing a kernel
     * module at all.
     */

    if (op->no_kernel_module) {
        ui_log(op, "Not installing a kernel module; skipping the \"is an "
               "NVIDIA kernel module loaded?\" test.");
        return TRUE;
    }

    for (n = 0; n < num_conflicting_kernel_modules; n++) {
        if (check_for_loaded_kernel_module(op, conflicting_kernel_modules[n])) {
            loaded = TRUE;
            bits |= (1 << n);
        }
    }

    if (!loaded) return TRUE;
    
    /* one or more kernel modules is loaded... try to unload them */

    for (n = 0; n < num_conflicting_kernel_modules; n++) {
        if (!(bits & (1 << n))) {
            continue;
        }
        
        rmmod_kernel_module(op, conflicting_kernel_modules[n]);

        /* check again */
        
        if (check_for_loaded_kernel_module(op, conflicting_kernel_modules[n])) {
            ui_error(op,  "An NVIDIA kernel module '%s' appears to already "
                     "be loaded in your kernel.  This may be because it is "
                     "in use (for example, by the X server), but may also "
                     "happen if your kernel was configured without support "
                     "for module unloading.  Please be sure you have exited "
                     "X before attempting to upgrade your driver.  If you "
                     "have exited X, know that your kernel supports module "
                     "unloading, and still receive this message, then an "
                     "error may have occured that has corrupted the NVIDIA "
                     "kernel module's usage count; the simplest remedy is "
                     "to reboot your computer.",
                     conflicting_kernel_modules[n]);
    
            return FALSE;
        }
    }

    return TRUE;

} /* check_for_unloaded_kernel_module() */





/*
 * find_precompiled_kernel_interface() - do assorted black magic to
 * determine if the given package contains a precompiled kernel interface
 * for the kernel on this system.
 *
 * XXX it would be nice to extend this so that a kernel module could
 * be installed for a kernel other than the currently running one.
 */

int find_precompiled_kernel_interface(Options *op, Package *p)
{
    char *proc_version_string, *output_filename, *tmp;
    PrecompiledInfo *info = NULL;

    /* allow the user to completely skip this search */
    
    if (op->no_precompiled_interface) {
        ui_log(op, "Not probing for precompiled kernel interfaces.");
        return FALSE;
    }
    
    /* retrieve the proc version string for the running kernel */

    proc_version_string = read_proc_version(op);
    
    if (!proc_version_string) goto failed;
    
    /* make sure the target directory exists */
    
    if (!mkdir_recursive(op, p->kernel_module_build_directory, 0755))
        goto failed;
    
    /* build the output filename */
    
    output_filename = nvstrcat(p->kernel_module_build_directory, "/",
                               PRECOMPILED_KERNEL_INTERFACE_FILENAME, NULL);
    
    /*
     * if the --precompiled-kernel-interfaces-path option was
     * specified, search that directory, first
     */
    
    if (op->precompiled_kernel_interfaces_path) {
        info = scan_dir(op, p, op->precompiled_kernel_interfaces_path,
                        output_filename, proc_version_string);
    }
    
    /*
     * If we didn't find a match, search for distro-provided
     * precompiled kernel interfaces
     */

    if (!info) {
        tmp = build_distro_precompiled_kernel_interface_dir(op);
        if (tmp) {
            info = scan_dir(op, p, tmp, output_filename, proc_version_string);
            nvfree(tmp);
        }
    }
    
    /*
     * if we still haven't found a match, search in
     * p->precompiled_kernel_interface_directory (the directory
     * containing the precompiled kernel interfaces shipped with the
     * package)
     */

    if (!info) {
        info = scan_dir(op, p, p->precompiled_kernel_interface_directory,
                        output_filename, proc_version_string);
    }

    /* If we found one, ask expert users if they really want to use it */

    if (info && op->expert) {
        if (!ui_yes_no(op, TRUE, "A precompiled kernel interface for the "
                       "kernel '%s' has been found.  Would you like to "
                       "use this? (answering 'no' will require the "
                       "installer to compile the interface)",
                       info->description)) {
            /* XXX free info */
            info = NULL;
        }
    }

    if (info) {
        /* XXX free info */
        return TRUE;
    }

 failed:

    if (op->expert) {
        ui_message(op, "No precompiled kernel interface was found to match "
                   "your kernel; this means that the installer will need to "
                   "compile a new kernel interface.");
    }

    return FALSE;
    
} /* find_precompiled_kernel_interface() */



/*
 * get_kernel_name() - get the kernel name: this is either what
 * the user specified via the --kernel-name option, or `uname -r`.
 */

char __kernel_name[256];

char *get_kernel_name(Options *op)
{
    struct utsname uname_buf;

    __kernel_name[0] = '\0';

    if (uname(&uname_buf) == -1) {
        ui_warn(op, "Unable to determine the version of the running kernel "
                "(%s).", strerror(errno));
    } else {
        strncpy(__kernel_name, uname_buf.release, sizeof(__kernel_name));
        __kernel_name[sizeof(__kernel_name) - 1] = '\0';
    }

    if (op->kernel_name) {
        if (strcmp(op->kernel_name, __kernel_name) != 0) {
            /* Don't load kernel modules built against a non-running kernel */
            op->skip_module_load = TRUE;
        }
        return op->kernel_name;
    }

    if (__kernel_name[0]) {
        return __kernel_name;
    }

    return NULL;
} /* get_kernel_name() */



/*
 ***************************************************************************
 * local static routines
 ***************************************************************************
 */



/*
 * default_kernel_module_installation_path() - do the equivalent of:
 *
 * SYSSRC = /lib/modules/$(shell uname -r)
 *
 * ifeq ($(shell if test -d $(SYSSRC)/kernel; then echo yes; fi),yes)
 *   INSTALLDIR = $(SYSSRC)/kernel/drivers/video
 * else
 *   INSTALLDIR = $(SYSSRC)/video
 * endif
 */

static char *default_kernel_module_installation_path(Options *op)
{
    char *str, *tmp;
    
    tmp = get_kernel_name(op);
    if (!tmp) return NULL;

    str = nvstrcat("/lib/modules/", tmp, "/kernel", NULL);
    
    if (directory_exists(op, str)) {
        free(str);
        str = nvstrcat("/lib/modules/", tmp, "/kernel/drivers/video", NULL);
        return str;
    }

    free(str);
    
    str = nvstrcat("/lib/modules/", tmp, "/video", NULL);

    return str;

} /* default_kernel_module_installation_path() */



/*
 * default_kernel_source_path() - determine the default kernel
 * source path, if possible.  Return NULL if no default kernel path
 * is found.
 *
 * Here is the logic:
 *
 * if --kernel-source-path was set, use that
 * 
 * if --kernel-include-path was set, use that (converting it to the
 * source path); also print a warning that --kernel-include-path is
 * deprecated.
 *
 * else if SYSSRC is set, use that
 *
 * else if /lib/modules/`uname -r`/build exists use that
 *
 * else if /usr/src/linux exists use that
 *
 * else return NULL
 *
 * One thing to note is that for the first two methods
 * (--kernel-source-path and $SYSSRC) we don't check for directory
 * existence before returning.  This is intentional: if the user set
 * one of these, then they're trying to set a particular path.  If
 * that directory doesn't exist, then better to abort installation with
 * an appropriate error message in determine_kernel_source_path().
 * Whereas, for the later two (/lib/modules/`uname -r`/build
 * and /usr/src/linux), these are not explicitly requested by
 * the user, so it makes sense to only use them if they exist.
 */ 

static char *default_kernel_source_path(Options *op)
{
    char *str, *tmp;
    
    str = tmp = NULL;
    
    /* check --kernel-source-path */

    if (op->kernel_source_path) {
        ui_log(op, "Using the kernel source path '%s' as specified by the "
               "'--kernel-source-path' commandline option.",
               op->kernel_source_path);
        return op->kernel_source_path;
    }

    /* check --kernel-include-path */

    if (op->kernel_include_path) {
        ui_warn(op, "The \"--kernel-include-path\" option is deprecated "
                "(as part of reorganization to support Linux 2.6); please use "
                "\"--kernel-source-path\" instead.");
        str = convert_include_path_to_source_path(op->kernel_include_path);
        ui_log(op, "Using the kernel source path '%s' (inferred from the "
               "'--kernel-include-path' commandline option '%s').",
               str, op->kernel_include_path);
        return str;
    }

    /* check SYSSRC */
    
    str = getenv("SYSSRC");
    if (str) {
        ui_log(op, "Using the kernel source path '%s', as specified by the "
               "SYSSRC environment variable.", str);
        return str;
    }
    
    /* check /lib/modules/`uname -r`/build and /usr/src/linux-`uname -r` */
    
    tmp = get_kernel_name(op);

    if (tmp) {
        str = nvstrcat("/lib/modules/", tmp, "/source", NULL);

        if (directory_exists(op, str)) {
            return str;
        }

        nvfree(str);

        str = nvstrcat("/lib/modules/", tmp, "/build", NULL);
    
        if (directory_exists(op, str)) {
            return str;
        }

        nvfree(str);

        /*
         * check "/usr/src/linux-`uname -r`", too; patch suggested by
         * Peter Berg Larsen <pebl@math.ku.dk>
         */

        str = nvstrcat("/usr/src/linux-", tmp, NULL);
        if (directory_exists(op, str)) {
            return str;
        }

        free(str);
    }

    /* finally, try /usr/src/linux */

    if (directory_exists(op, "/usr/src/linux")) {
        return "/usr/src/linux";
    }
    
    return NULL;
    
} /* default_kernel_source_path() */


/*
 * check_for_loaded_kernel_module() - check if the specified kernel
 * module is currently loaded using `lsmod`.  Returns TRUE if the
 * kernel module is loaded; FALSE if it is not.
 *
 * Be sure to check that the character following the kernel module
 * name is a space (to avoid getting false positivies when the given
 * kernel module name is contained within another kernel module name.
 */

static int check_for_loaded_kernel_module(Options *op, const char *module_name)
{
    char *ptr, *result = NULL;
    int ret;

    ret = run_command(op, op->utils[LSMOD], &result, FALSE, 0, TRUE);
    
    if ((ret == 0) && (result) && (result[0] != '\0')) {
        ptr = strstr(result, module_name);
        if (ptr) {
            ptr += strlen(module_name);
            if(!isspace(*ptr)) ret = 1;
        } else {
            ret = 1;
        }
    }
    
    if (result) free(result);
    
    return ret ? FALSE : TRUE;
    
} /* check_for_loaded_kernel_module() */


/*
 * rmmod_kernel_module() - run `rmmod $module_name`
 */

int rmmod_kernel_module(Options *op, const char *module_name)
{
    int ret, old_loglevel, loglevel_set;
    char *cmd;
    
    cmd = nvstrcat(op->utils[RMMOD], " ", module_name, NULL);

    loglevel_set = set_loglevel(PRINTK_LOGLEVEL_KERN_ALERT, &old_loglevel);
    
    ret = run_command(op, cmd, NULL, FALSE, 0, TRUE);

    if (loglevel_set) {
        set_loglevel(old_loglevel, NULL);
    }

    free(cmd);
    
    return ret ? FALSE : TRUE;
    
} /* rmmod_kernel_module() */



/*
 * check_cc_version() - check if the selected or default system
 * compiler is compatible with the one that was used to build the
 * currently running kernel.
 */

int check_cc_version(Options *op, Package *p)
{
    char *cmd, *CC, *result;
    char *arch;
    int ret;

    /* 
     * If we're building/installing for a different kernel, then we
     * can't do the gcc version check (we don't have a /proc/version
     * string from which to get the kernel's gcc version).
     * If the user passes the option no-cc-version-check, then we also
     * shouldn't perform the cc version check.
     */

    if (op->ignore_cc_version_check) {
        setenv("IGNORE_CC_MISMATCH", "1", 1);
        return TRUE;
    }

    arch = get_machine_arch(op);
    if (!arch)
        return FALSE;

    CC = getenv("CC");
    if (!CC) CC = "cc";
    
    ui_log(op, "Performing CC version check with CC=\"%s\".", CC);

    cmd = nvstrcat("sh ", p->kernel_module_build_directory,
                   "/conftest.sh ", CC, " ", CC, " ", arch, " ",
                   "DUMMY_SOURCE DUMMY_OUTPUT ",
                   "cc_version_check just_msg", NULL);

    ret = run_command(op, cmd, &result, FALSE, 0, TRUE);

    nvfree(cmd);

    if (ret == 0) return TRUE;

    ret = ui_yes_no(op, TRUE, "The CC version check failed:\n\n%s\n\n"
                    "If you know what you are doing and want to "
                    "ignore the gcc version check, select \"No\" to "
                    "continue installation.  Otherwise, select \"Yes\" to "
                    "abort installation, set the CC environment variable to "
                    "the name of the compiler used to compile your kernel, "
                    "and restart installation.  Abort now?", result);
    
    nvfree(result);
    
    if (!ret) setenv("IGNORE_CC_MISMATCH", "1", 1);
    
    return !ret;
             
} /* check_cc_version() */


/*
 * fbdev_check() - run the rivafb_sanity_check and the nvidiafb_sanity_check
 * conftests; if either test fails, print the error message from the test
 * and abort the driver installation.
 */

static int fbdev_check(Options *op, Package *p)
{
    char *CC, *cmd, *result;
    char *arch;
    int ret;
    
    arch = get_machine_arch(op);
    if (!arch)
        return FALSE;

    CC = getenv("CC");
    if (!CC) CC = "cc";
    
    ui_log(op, "Performing rivafb check.");
    
    cmd = nvstrcat("sh ", p->kernel_module_build_directory,
                   "/conftest.sh ", CC, " ", CC, " ", arch, " ",
                   op->kernel_source_path, " ",
                   op->kernel_output_path, " ",
                   "rivafb_sanity_check just_msg", NULL);
    
    ret = run_command(op, cmd, &result, FALSE, 0, TRUE);
    
    nvfree(cmd);
    
    if (ret != 0) {
        ui_error(op, "%s", result);
        nvfree(result);

        return FALSE;
    }

    ui_log(op, "Performing nvidiafb check.");
    
    cmd = nvstrcat("sh ", p->kernel_module_build_directory,
                   "/conftest.sh ", CC, " ", CC, " ", arch, " ",
                   op->kernel_source_path, " ",
                   op->kernel_output_path, " ",
                   "nvidiafb_sanity_check just_msg", NULL);
    
    ret = run_command(op, cmd, &result, FALSE, 0, TRUE);
    
    nvfree(cmd);

    if (ret != 0) {
        ui_error(op, "%s", result);
        nvfree(result);

        return FALSE;
    }

    return TRUE;
    
} /* fbdev_check() */



/*
 * xen_check() - run the xen_sanity_check conftest; if this test fails, print
 * the test's error message and abort the driver installation.
 */

static int xen_check(Options *op, Package *p)
{
    char *CC, *cmd, *result;
    char *arch;
    int ret;
    
    arch = get_machine_arch(op);
    if (!arch)
        return FALSE;

    CC = getenv("CC");
    if (!CC) CC = "cc";
    
    ui_log(op, "Performing Xen check.");
    
    cmd = nvstrcat("sh ", p->kernel_module_build_directory,
                   "/conftest.sh ", CC, " ", CC, " ", arch, " ",
                   op->kernel_source_path, " ",
                   op->kernel_output_path, " ",
                   "xen_sanity_check just_msg", NULL);
    
    ret = run_command(op, cmd, &result, FALSE, 0, TRUE);
    
    nvfree(cmd);
    
    if (ret != 0) {
        ui_error(op, "%s", result);
        nvfree(result);

        return FALSE;
    }

    return TRUE;
    
} /* xen_check() */



/*
 * scan_dir() - scan through the specified directory for a matching
 * precompiled kernel interface.
 */

static PrecompiledInfo *scan_dir(Options *op, Package *p,
                                 const char *directory_name,
                                 const char *output_filename,
                                 const char *proc_version_string)
{
    DIR *dir;
    struct dirent *ent;
    PrecompiledInfo *info = NULL;
    char *filename;
    
    if (!directory_name) return NULL;
    
    dir = opendir(directory_name);
    if (!dir) return NULL;

    /*
     * loop over all contents of the directory, looking for a
     * precompiled kernel interface that matches the running kernel
     */

    while ((ent = readdir(dir)) != NULL) {
            
        if (((strcmp(ent->d_name, ".")) == 0) ||
            ((strcmp(ent->d_name, "..")) == 0)) continue;
            
        filename = nvstrcat(directory_name, "/", ent->d_name, NULL);
        
        info = precompiled_unpack(op, filename, output_filename,
                                  proc_version_string,
                                  p->version);
            
        if (info) break;
            
        free(filename);
        filename = NULL;
    }
        
    if (closedir(dir) != 0) {
        ui_error(op, "Failure while closing directory '%s' (%s).",
                 directory_name,
                 strerror(errno));
    }
    
    return info;
    
} /* scan_dir() */



/*
 * build_distro_precompiled_kernel_interface_dir() - construct this
 * path:
 *
 *  /lib/modules/precompiled/`uname -r`/nvidia/gfx/
 */

static char *build_distro_precompiled_kernel_interface_dir(Options *op)
{
    struct utsname uname_buf;
    char *str;
    
    if (uname(&uname_buf) == -1) {
        ui_error(op, "Unable to determine kernel version (%s)",
                 strerror(errno));
        return NULL;
    }
    
    str = nvstrcat("/lib/modules/precompiled/", uname_buf.release,
                   "/nvidia/gfx/", NULL);
    
    return str;

} /* build_distro_precompiled_kernel_interface_dir() */



/*
 * convert_include_path_to_source_path() - given input to
 * "--kernel-include-path", convert it to "--kernel-source-path" by
 * scanning from the end to the previous "/".
 */

static char *convert_include_path_to_source_path(const char *inc)
{
    char *c, *str;

    str = nvstrdup(inc);

    /* go to the end of the string */

    for (c = str; *c; c++);
    
    /* move to the last printable character */

    c--;

    /* if the string ends in '/'; backup one more */

    if (*c == '/') c--;

    /* now back up to the next '/' */

    while ((c >= str) && (*c != '/')) c--;

    if (*c == '/') *c = '\0';

    return str;

} /* convert_include_path_to_source_path() */



/*
 * guess_kernel_module_filename() - parse uname to decide if the
 * kernel module filename is "nvidia.o" or "nvidia.ko".
 */

static char *guess_kernel_module_filename(Options *op)
{
    struct utsname uname_buf;
    char *tmp, *str, *dot0, *dot1;
    int major, minor;
    
    if (op->kernel_name) {
        str = op->kernel_name;
    } else {
        if (uname(&uname_buf) == -1) {
            ui_error (op, "Unable to determine kernel version (%s)",
                      strerror (errno));
            return NULL;
        }
        str = uname_buf.release;
    }
    
    tmp = nvstrdup(str);
    
    dot0 = strchr(tmp, '.');
    if (!dot0) goto fail;
    
    *dot0 = '\0';
    
    major = atoi(tmp);
    
    dot0++;
    dot1 = strchr(dot0, '.');
    if (!dot1) goto fail;
    
    *dot1 = '\0';
    
    minor = atoi(dot0);
    
    if ((major > 2) || ((major == 2) && (minor > 4))) {
        return nvstrdup("nvidia.ko");
    } else {
        return nvstrdup("nvidia.o");
    }

 fail:
    ui_error (op, "Unable to determine if kernel is 2.6.0 or greater from "
              "uname string '%s'; assuming the kernel module filename is "
              "'nvidia.o'.", str);
    return nvstrdup("nvidia.o");
    
} /* guess_kernel_module_filename() */



/*
 * get_machine_arch() - get the machine architecture, substituting
 * i386 for i586 and i686 or arm for arm7l.
 */

static char __machine_arch[16];

char *get_machine_arch(Options *op)
{
    struct utsname uname_buf;

    if (uname(&uname_buf) == -1) {
        ui_warn(op, "Unable to determine machine architecture (%s).",
                strerror(errno));
        return NULL;
    } else {
        if ((strncmp(uname_buf.machine, "i586", 4) == 0) ||
            (strncmp(uname_buf.machine, "i686", 4) == 0)) {
            strcpy(__machine_arch, "i386");
        } else if ((strncmp(uname_buf.machine, "armv", 4) == 0)) {
            strcpy(__machine_arch, "arm");
        } else {
            strncpy(__machine_arch, uname_buf.machine,
                    sizeof(__machine_arch));
        }
        return __machine_arch;
    }

} /* get_machine_arch() */
