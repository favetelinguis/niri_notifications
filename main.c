#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#ifdef DEBUG
#define DO_LOG_INFO(fmt, ...) printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define DO_LOG_ERROR(fmt, ...)                                                 \
  fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define DO_LOG_ERRNO(fmt, ...)                                                 \
  fprintf(stderr, "[ERROR] " fmt ": %s\n", ##__VA_ARGS__, strerror(errno))
#else
#include <systemd/sd-journal.h>
#define DO_LOG_INFO(fmt, ...) sd_journal_print(LOG_INFO, fmt, ##__VA_ARGS__)
#define DO_LOG_ERROR(fmt, ...) sd_journal_print(LOG_ERR, fmt, ##__VA_ARGS__)
#define DO_LOG_ERRNO(fmt, ...)                                                 \
  sd_journal_print(LOG_ERR, fmt ": %s", ##__VA_ARGS__, strerror(errno))
#endif

#define ERROR -1

typedef enum { STATE_WAITING, STATE_LAYOUT_INIT, STATE_RECEIVING } state;

typedef struct {
  state s;
  char **layouts;
  int n;           // number of layouts
  int current_idx; // index of current layout
} program_state_t;

typedef struct {
  char *buf;
  size_t len;
  size_t capacity;
} line_buffer_t;

static void send_notification(char *message) {
  if (message == NULL) {
    DO_LOG_ERROR("Message can not be NULL");
    return;
  }
  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  sd_bus *bus = NULL;
  int ret = -1;

  if ((sd_bus_open_user(&bus) < 0)) {
    DO_LOG_ERROR("Failed to connect to bus: %s", strerror(-ret));
    goto finish;
  }
  ret = sd_bus_call_method(bus, "org.freedesktop.Notifications", /* service */
                           "/org/freedesktop/Notifications", /* object path */
                           "org.freedesktop.Notifications",  /* interface */
                           "Notify",                         /* method */
                           &error, &reply, "susssasa{sv}i",  /* signature */
                           "nirinotify",                     /* app_name */
                           0u,                               /* replaces_id */
                           "",                               /* app_icon */
                           "Layout Changed",                 /* summary */
                           message,                          /* body */
                           0,   /* actions (empty array) */
                           0,   /* hints (empty dict) */
                           5000 /* timeout (5 seconds) */
  );

  if (ret < 0) {
    DO_LOG_ERROR("Failed to send notification: %s", error.message);
    goto finish;
  }
finish:
  sd_bus_error_free(&error);
  sd_bus_message_unref(reply);
  sd_bus_unref(bus);
  return;
}

static void process_line(char *line, program_state_t *ps) {
  cJSON *root;
  if (!(root = cJSON_Parse(line))) {
    DO_LOG_ERROR("Invalid JSON format: %s", line);
    goto cleanup;
  }

  switch (ps->s) {
  case STATE_WAITING: {
    cJSON *ok_obj;
    if (!(ok_obj = cJSON_GetObjectItemCaseSensitive(root, "Ok"))) {
      goto cleanup;
    }
    ps->s = STATE_LAYOUT_INIT;
    break;
  }
  case STATE_LAYOUT_INIT: {
    cJSON *obj;
    if (!(obj = cJSON_GetObjectItemCaseSensitive(root,
                                                 "KeyboardLayoutsChanged"))) {
      goto cleanup;
    }
    ps->s = STATE_RECEIVING;
    cJSON *keyboard_layouts =
        cJSON_GetObjectItemCaseSensitive(obj, "keyboard_layouts");
    cJSON *current_idx =
        cJSON_GetObjectItemCaseSensitive(keyboard_layouts, "current_idx");
    if (cJSON_IsNumber(current_idx)) {
      ps->current_idx = current_idx->valueint;
    }
    cJSON *names = cJSON_GetObjectItemCaseSensitive(keyboard_layouts, "names");
    if (cJSON_IsArray(names)) {
      ps->layouts = calloc(cJSON_GetArraySize(names), sizeof(char *));
      if (!ps->layouts) {
        DO_LOG_ERRNO("calloc");
        goto cleanup;
      }

      cJSON *name = NULL;
      cJSON_ArrayForEach(name, names) {
        if (cJSON_IsString(name)) {
          char *layout = strdup(name->valuestring);
          if (!layout) {
            goto cleanup;
          }
          ps->layouts[ps->n] = layout;
          ps->n++;
        }
      }
    }
    break;
  }
  case STATE_RECEIVING: {
    cJSON *obj;
    if (!(obj = cJSON_GetObjectItemCaseSensitive(root,
                                                 "KeyboardLayoutSwitched"))) {
      goto cleanup;
    }
    cJSON *idx = cJSON_GetObjectItemCaseSensitive(obj, "idx");
    if (cJSON_IsNumber(idx)) {
      int new_idx = idx->valueint;
      if (new_idx >= 0 && new_idx < ps->n && ps->current_idx != new_idx) {
        ps->current_idx = idx->valueint;
        send_notification(ps->layouts[ps->current_idx]);
      }
    }
    break;
  }
  default:
    DO_LOG_ERROR("Unknown state");
    break;
  }
cleanup:
  cJSON_Delete(root);
  return;
}

static int read_socket(int sock) {
  int res = -1;
  program_state_t ps = {0};
  ps.s = STATE_WAITING;
  ps.n = 0;

  line_buffer_t lb = {0};
  lb.capacity = 4096;
  if (!(lb.buf = malloc(lb.capacity))) {
    DO_LOG_ERRNO("malloc");
    goto cleanup;
  }
  char temp[4096];
  ssize_t n;

  while ((n = read(sock, temp, sizeof(temp))) > 0) {
    for (ssize_t i = 0; i < n; i++) {
      // Grow buffer if needed
      if (lb.len + 1 >= lb.capacity) {
        lb.capacity *= 2;
        char *new_buf;
        if (!(realloc(lb.buf, lb.capacity))) {
          DO_LOG_ERRNO("realloc");
          goto cleanup;
        }
        lb.buf = new_buf;
      }
      lb.buf[lb.len++] = temp[i];

      // Found complete line
      if (temp[i] == '\n') {
        lb.buf[lb.len - 1] = '\0';
        process_line(lb.buf, &ps);
        lb.len = 0; // reset for next line
      }
    }
  }
  res = 0;
cleanup:
  // Free allocated layouts
  for (int i = 0; i < ps.n; i++) {
    free(ps.layouts[i]);
  }
  free(ps.layouts);

  free(lb.buf);
  return res;
}

int main() {
  DO_LOG_INFO("Starting Niri Notification Watcher");
  const char *path = getenv("NIRI_SOCKET");
  if (path == NULL) {
    DO_LOG_ERROR("Environment variable NIRI_SOCKET not found");
    exit(EXIT_FAILURE);
  }

  int sock;
  if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
    DO_LOG_ERRNO("socket");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    DO_LOG_ERRNO("connect");
    exit(EXIT_FAILURE);
  }

  const char *msg = "\"EventStream\"\n";
  if (write(sock, msg, strlen(msg)) < 0) {
    DO_LOG_ERRNO("write");
    close(sock);
    exit(EXIT_FAILURE);
  }
  int res = read_socket(sock);
  close(sock);

  DO_LOG_INFO("Shutting down Niri Notification Watcher");
  return res < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
