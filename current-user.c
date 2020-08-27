/*
 * Copyright (C) 2020 Jolla Ltd.
 * Copyright (C) 2020 Slava Monich <slava.monich@jolla.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as representing
 * any official policies, either expressed or implied.
 */

#define _GNU_SOURCE

#include <glib.h>

#include <dbusaccess_proc.h>

#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <grp.h>

#define RET_CMDLINE 1
#define RET_ERR 2
#define RET_EXEC 3

static
char*
cu_get_systemd_path(
    void)
{
    GError* error = NULL;
    const char* link = "/proc/1/exe";
    char* path = g_file_read_link(link, &error);

    if (path) {
        char* basename = g_path_get_basename(path);
        const gboolean is_systemd = !g_strcmp0(basename, "systemd");

        g_free(basename);
        if (is_systemd) {
            return path;
        } else {
            g_warning("PID 1 is %s rather than systemd", path);
            g_free(path);
        }
    } else {
        g_warning("%s", error->message);
        g_error_free(error);
    }
    return NULL;
}

static
DAProc*
cu_get_user_systemd_pid(
    void)
{
    DAProc* proc = NULL;
    char* systemd = cu_get_systemd_path();

    if (systemd) {
        GError* error = NULL;
        const char* root = "/proc";
        GDir* dir = g_dir_open(root, 0, &error);

        if (dir) {
            const char* name;

            while ((name = g_dir_read_name(dir)) != NULL) {
                /* Not interested in pid 1 */
                if (g_strcmp0(name, "1")) {
                    char* link = g_build_filename(root, name, "exe", NULL);
                    char* path = g_file_read_link(link, NULL);
                    const gboolean is_systemd = !g_strcmp0(path, systemd);

                    g_free(link);
                    g_free(path);
                    if (is_systemd) {
                        proc = da_proc_new(atoi(name));
                        if (proc) {
                            if (proc->cred.euid) {
                                /*
                                 * Not a root process - assume that's what
                                 * we've been looking for.
                                 */
                                break;
                            }
                            /* Continue searching */
                            da_proc_unref(proc);
                            proc = NULL;
                        }
                    }
                }
            }
            g_dir_close(dir);
        } else {
            g_warning("%s", error->message);
            g_error_free(error);
        }
        g_free(systemd);
    }
    return proc;
}

static
char**
cu_get_environ(
    pid_t pid)
{
    char* fname = g_strdup_printf("/proc/%u/environ", (guint)pid);
    GError* error = NULL;
    gchar* environ = NULL;
    gsize size = 0;
    char** envp = NULL;

    if (g_file_get_contents(fname, &environ, &size, &error)) {
        /* It better be non-empty and NULL terminated */
        if (size > 0 && !environ[size - 1]) {
            char* ptr = environ;
            char* end = ptr + size;
            GPtrArray* strv = g_ptr_array_new();

            while (ptr < end) {
                const size_t len = strlen(ptr);

                g_ptr_array_add(strv, g_memdup(ptr, len + 1));
                ptr += len + 1;
            }
            g_ptr_array_add(strv, NULL);
            envp = (char**)g_ptr_array_free(strv, FALSE);
        } else {
            g_warning("%s not NULL terminated?", fname);
        }
        g_free(environ);
    } else {
        g_warning("%s", error->message);
        g_error_free(error);
    }
    g_free(fname);
    return envp;
}

static
int
cu_exec(
    const DACred* cred,
    gid_t egid,
    char* const argv[],
    char* const envp[])
{
    if (setgroups(cred->ngroups, cred->groups)) {
        g_warning("setgroups error: %s\n", strerror(errno));
    } else if (!egid && setgid(cred->egid)) {
        g_warning("setgid(%u) error: %s\n", (guint)cred->egid,
            strerror(errno));
    } else if (egid && setgid(egid)) {
        g_warning("setgid(%u) error: %s\n", (guint)egid,
            strerror(errno));
    } else if (setuid(cred->euid)) {
        g_warning("setuid(%u) error: %s\n", (guint)cred->euid,
            strerror(errno));
    } else {
        const char* file = argv[0];

        execvpe(file, argv, envp);
        g_warning("exec(%s) error: %s\n", file, strerror(errno));
        return RET_EXEC;
    }
    return RET_ERR;
}

static
gboolean
cu_parse_group(
    const char* group,
    gid_t* gid)
{
    const struct group *gr = getgrnam(group);

    if (!gr) {
        /* Try numeric */
        int n = atoi(group);

        if (n > 0) {
            gr = getgrgid(n);
        }
    }

    if (gr) {
        *gid = gr->gr_gid;
        return TRUE;
    } else {
        g_warning("Invalid group '%s'", group);
        return FALSE;
    }
}

static
gboolean
cu_parse_cmdline(
    int* argc,
    char** argv,
    gid_t* egid)
{
    if (*argc > 1) {
        if (!strcmp("-g", argv[1])) {
            if ((*argc) > 2) {
                if (cu_parse_group(argv[2], egid)) {
                    memmove(argv + 1, argv + 3, sizeof(char*) * ((*argc) - 3));
                    (*argc) -= 2;
                } else {
                    return FALSE;
                }
            } else {
                return FALSE;
            }
        }
        return TRUE;
    }
    return FALSE;
}

int main(int argc, char* argv[])
{
    gid_t egid = 0;

    if (!cu_parse_cmdline(&argc, argv, &egid)) {
        g_print("Usage: current-user [-g GID] PROG [args]\n");
        return RET_CMDLINE;
    } else {
        DAProc* proc = cu_get_user_systemd_pid();

        if (proc) {
            char** envp = cu_get_environ(proc->pid);

            if (envp) {
                char** args = g_new(char*, argc);
                int i, ret;

                for (i = 0; i < argc - 1; i++) {
                    args[i] = argv[i + 1];
                }
                args[i] = NULL;
                ret = cu_exec(&proc->cred, egid, args, envp);
                da_proc_unref(proc);
                g_strfreev(envp);
                g_free(args);
                return ret;
            }
            da_proc_unref(proc);
        }
        return RET_ERR;
    }
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
