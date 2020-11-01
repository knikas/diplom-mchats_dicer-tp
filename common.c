/*
 * BSD LICENSE
 *
 * Copyright(c) 2019 Intel Corporation. All rights reserved.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
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
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.O
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pqos.h"

#include "common.h"
#include "dicer.h"

/**
 * @brief Checks if a path to a file contains any symbolic links.
 *
 * @param [in] name a path to a file
 *
 * @return Operation status
 * @retval PQOS_RETVAL_OK if the path exists and does not contain any
 * symbolic links.
 */
static int
check_symlink(const char *name)
{
        int fd;
        int oflag;
        char *dir = strdup(name);
        char *path = dir;

        if (dir == NULL)
                return PQOS_RETVAL_ERROR;

        oflag = O_RDONLY;

        do {
                fd = open(path, oflag | O_NOFOLLOW);
                if (fd == -1) {
                        if (errno == ELOOP)
                                printf("File %s is a symlink\n", path);

                        free(dir);
                        return PQOS_RETVAL_ERROR;
                }

                oflag = O_RDONLY | O_DIRECTORY;
                path = dirname(path);

                if (fd != -1)
                        close(fd);
        } while ((strcmp(path, ".") != 0) && (strcmp(path, "/") != 0));

        free(dir);
        return PQOS_RETVAL_OK;
}

FILE *
fopen_check_symlink(const char *name, const char *mode)
{
        int fd;
        int ret;
        char *dir;
        char *file_name;

        /* If the file will be created, check a parent directory for symlinks */
        if (mode != NULL && (mode[0] == 'w' || mode[0] == 'a')) {
                file_name = strdup(name);

                if (file_name == NULL)
                        return NULL;

                fd = open(file_name, O_RDONLY);

                if (fd == -1) {
                        dir = dirname(file_name);
                        ret = check_symlink(dir);
                } else {
                        ret = check_symlink(name);
                        close(fd);
                }

                free(file_name);
        } else
                ret = check_symlink(name);

        if (ret != PQOS_RETVAL_OK)
                return NULL;

        return fopen(name, mode);
}

int
open_check_symlink(const char *pathname, int flags, mode_t mode)
{
        int ret;

        ret = check_symlink(pathname);
        if (ret != PQOS_RETVAL_OK) {
                errno = EACCES;
                return -1;
        }

        return open(pathname, flags, mode);
}
