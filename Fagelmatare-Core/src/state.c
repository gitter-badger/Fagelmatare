/*
 *  state.c
 *    Watch for state change events in picam library to recognize if a recording
 *    has been started or stopped.
 *    Copyright (C) 2015 Linus Styrén
 *****************************************************************************
 *  This file is part of Fågelmataren:
 *    https://github.com/Linkaan/Fagelmatare/
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 *  USA
 *****************************************************************************
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <pthread.h>
#include <signal.h>
#include <dirent.h>
#include <log.h>
#include <state.h>

#define NUM_EVENT_BUF 10
#define EVENT_NAME_BUF_LEN 32

#define EVENT_SIZE  ( sizeof (struct inotify_event) )
#define EVENT_BUF_LEN  ( NUM_EVENT_BUF * ( EVENT_SIZE + EVENT_NAME_BUF_LEN ) )

typedef struct watch_target {
  char *dir;
  int (*callback)(char *, char *);
  int read_content;
} watch_target;

static int keep_watching = 1;
static pthread_t *watcher_thread;

void *watch_for_file_creation(watch_target *target) {
  int length, i;
  int fd;
  int wd;
  int status;
  int dir_strlen;
  char buffer[EVENT_BUF_LEN];
  char *dir = target->dir;
  int (*callback)(char *, char *) = target->callback;
  int read_content = target->read_content;
  free(target);
  dir_strlen = strlen(dir);

  fd = inotify_init();
  if (fd < 0) {
    log_fatal("inotify_init failed (%s)\n", strerror(errno));
    exit(1);
  }

  struct stat st;
  int err = stat(dir, &st);
  if (err == -1) {
    if (errno == ENOENT) {
      log_fatal("state directory (%s) does not exist\n", dir);
    } else {
      log_fatal("stat failed (%s)\n", strerror(errno));
    }
    exit(1);
  } else {
    if (!S_ISDIR(st.st_mode)) {
      log_fatal("state path (%s) is not a directory\n", dir);
      exit(1);
    }
  }

  if (access(dir, R_OK) != 0) {
    log_fatal("failed to access hook target directory (%s)\n", strerror(errno));
    exit(1);
  }

  uint32_t inotify_mask;
  if (read_content) {
    inotify_mask = IN_CLOSE_WRITE;
  } else {
    inotify_mask = IN_CREATE;
  }

  wd = inotify_add_watch(fd, dir, inotify_mask);

  while (keep_watching) {
    length = read(fd, buffer, EVENT_BUF_LEN);
    if (length < 0) {
      break;
    }

    i = 0;
    status = 0;
    while (i < length) {
      struct inotify_event *event = ( struct inotify_event * ) &buffer[ i ];
      if (event->len) {
        if (event->mask & inotify_mask) {
          if (!(event->mask & IN_ISDIR)) { // file
            int path_len = dir_strlen + strlen(event->name) + 2;
            char *path = malloc(path_len);
            if (path == NULL) {
              log_error("in watch_for_file_creation: memory allocation for file path failed: (%s)\n", strerror(errno));
            } else {
              snprintf(path, path_len, "%s/%s", dir, event->name);

              if (read_content) {
                // Read file contents
                FILE *fp = fopen(path, "rb");
                char *content = NULL;
                if (fp) {
                  fseek(fp, 0, SEEK_END);
                  long content_len = ftell(fp);
                  fseek(fp, 0, SEEK_SET);
                  content = malloc(content_len + 1);
                  if (content) {
                    fread(content, 1, content_len, fp);
                    content[content_len] = '\0';
                  } else {
                    log_error("in watch_for_file_creation: memory allocation for file content failed: (%s)\n", strerror(errno));
                  }
                  fclose(fp);
                } else {
                  log_error("fopen failed (%s)\n", strerror(errno));
                }
                status = callback(event->name, content);
                free(content);
              } else {
                status = callback(event->name, NULL);
              }

              // Delete that file
              if (status && unlink(path) != 0) {
                log_error("unlink failed (%s)\n", strerror(errno));
              }
              free(path);
            }
          }
        }
      }
      i += EVENT_SIZE + event->len;
    }
  }

  inotify_rm_watch(fd, wd);
  close(fd);
  pthread_exit(0);
}

void start_watching_state(pthread_t *thread, char *dir, int (*callback)(char *, char *), int read_content) {
  watch_target *target = malloc(sizeof(watch_target));
  target->dir = dir;
  target->callback = callback;
  target->read_content = read_content;
  pthread_create(thread, NULL, (void * (*)(void *))watch_for_file_creation, target);
  watcher_thread = thread;
}

void stop_watching_state() {
  keep_watching = 0;
  pthread_kill(*watcher_thread, SIGTERM);
}
