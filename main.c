#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

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

void process_response(const char *json_string) {
  cJSON *root = cJSON_Parse(json_string);
  if (!root) {
    fprintf(stderr, "Invalid JSON format\n");
    return;
  }

  // 1. Check for "Err" first
  cJSON *err_obj = cJSON_GetObjectItemCaseSensitive(root, "Err");
  if (err_obj != NULL) {
    if (cJSON_IsString(err_obj)) {
      printf("Application Error: %s\n", err_obj->valuestring);
    } else {
      printf("Application Error: (Unknown error format)\n");
    }
    goto cleanup; // Exit early but free memory
  }

  // 2. Check for "Ok"
  cJSON *ok_obj = cJSON_GetObjectItemCaseSensitive(root, "Ok");
  if (ok_obj == NULL) {
    fprintf(stderr, "Unexpected JSON structure: Missing 'Ok' and 'Err'\n");
    goto cleanup;
  }

  // 3. Process the "Ok" payload
  cJSON *layouts = cJSON_GetObjectItemCaseSensitive(ok_obj, "KeyboardLayouts");
  cJSON *names = cJSON_GetObjectItemCaseSensitive(layouts, "names");

  if (cJSON_IsArray(names)) {
    cJSON *name = NULL;
    cJSON_ArrayForEach(name, names) {
      if (cJSON_IsString(name))
        printf("Layout: %s\n", name->valuestring);
    }
  }

cleanup:
  cJSON_Delete(root);
}

int send_notification(char *message) {
  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  sd_bus *bus = NULL;
  int ret;

  ret = sd_bus_open_user(&bus);
  if (ret < 0) {
    fprintf(stderr, "Failed to connect to bus: %s\n", strerror(-ret));
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
    fprintf(stderr, "Failed to send notification: %s\n", error.message);
    goto finish;
  }

finish:
  sd_bus_error_free(&error);
  sd_bus_message_unref(reply);
  sd_bus_unref(bus);

  return ret;
}

int process_line(char *line, program_state_t *ps) {
  int res = 0;
  cJSON *root = cJSON_Parse(line);
  if (!root) {
    fprintf(stderr, "Invalid JSON format\n");
    res = -1;
    goto cleanup;
  }

  switch (ps->s) {
  case STATE_WAITING: {
    cJSON *ok_obj = cJSON_GetObjectItemCaseSensitive(root, "Ok");
    if (ok_obj == NULL) {
      goto cleanup;
    }
    ps->s = STATE_LAYOUT_INIT;
    break;
  }
  case STATE_LAYOUT_INIT: {
    cJSON *obj =
        cJSON_GetObjectItemCaseSensitive(root, "KeyboardLayoutsChanged");
    if (obj == NULL) {
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
        fprintf(stderr, "Memory allocation failed\n");
        res = -1;
        goto cleanup;
      }

      cJSON *name = NULL;
      cJSON_ArrayForEach(name, names) {
        if (cJSON_IsString(name)) {
          ps->layouts[ps->n] = strdup(name->valuestring);
          ps->n++;
        }
      }
    }
    break;
  }
  case STATE_RECEIVING: {
    cJSON *obj =
        cJSON_GetObjectItemCaseSensitive(root, "KeyboardLayoutSwitched");
    if (obj == NULL) {
      goto cleanup;
    }
    cJSON *idx = cJSON_GetObjectItemCaseSensitive(obj, "idx");
    if (cJSON_IsNumber(idx)) {
      if (ps->current_idx != idx->valueint) {
        ps->current_idx = idx->valueint;
        res = send_notification(ps->layouts[ps->current_idx]);
        if (res < 0) {
          res = -1;
          goto cleanup;
        }
      }
    }
    break;
  default:
    fprintf(stderr, "Unknown state\n");
    break;
  }
  }
cleanup:
  cJSON_Delete(root);
  return res;
}

int read_socket(int sock) {
  program_state_t ps;
  ps.s = STATE_WAITING;
  ps.n = 0;

  line_buffer_t lb = {0};
  lb.capacity = 4096;
  lb.buf = malloc(lb.capacity);

  char temp[4096];
  ssize_t n;

  int res = 0;

  while ((n = read(sock, temp, sizeof(temp))) > 0) {
    for (ssize_t i = 0; i < n; i++) {
      // Grow buffer if needed
      if (lb.len + 1 >= lb.capacity) {
        lb.capacity *= 2;
        lb.buf = realloc(lb.buf, lb.capacity);
      }
      lb.buf[lb.len++] = temp[i];

      // Found complete line
      if (temp[i] == '\n') {
        lb.buf[lb.len - 1] = '\0';
        res = process_line(lb.buf, &ps);
        if (res < 0) {
          fprintf(stderr, "Reading failed\n");
          res = -1;
          goto cleanup;
        }
        lb.len = 0; // reset for next line
      }
    }
  }
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
  const char *path = getenv("NIRI_SOCKET");
  if (path == NULL) {
    fputs("Environment variable NIRI_SOCKET not found\n", stderr);
    exit(EXIT_FAILURE);
  }

  int sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("connect");
    exit(EXIT_FAILURE);
  }

  const char *msg = "\"EventStream\"\n";
  if (write(sock, msg, strlen(msg)) < 0) {
    perror("write");
    close(sock);
    exit(EXIT_FAILURE);
  }
  int res = read_socket(sock);
  close(sock);

  return res < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
