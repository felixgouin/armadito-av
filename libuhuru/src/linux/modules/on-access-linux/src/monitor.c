#define _GNU_SOURCE

#include <libuhuru/core.h>

#include "monitor.h"

#include <assert.h>
#include <glib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/signalfd.h>
#include <fcntl.h>
#include <linux/fanotify.h>
#include <sys/types.h>
#include <unistd.h>

struct access_monitor {
  struct uhuru *uhuru;
  struct uhuru_scan_conf *scan_conf;

  int enable;
  int enable_permission;
  int enable_removable_media;
  GPtrArray *entries;

  pid_t my_pid;

  int start_pipe[2];

  int command_pipe[2];
  GIOChannel *command_channel;

  int fanotify_fd;
  GIOChannel *notify_channel;

  GThread *monitor_thread;
};

static gboolean access_monitor_start_cb(GIOChannel *source, GIOCondition condition, gpointer data);
static gboolean access_monitor_command_cb(GIOChannel *source, GIOCondition condition, gpointer data);
static gboolean access_monitor_fanotify_cb(GIOChannel *source, GIOCondition condition, gpointer data);

static gpointer access_monitor_thread_fun(gpointer data);

static void path_destroy_notify(gpointer data)
{
  free(data);
}

static void entry_destroy_notify(gpointer data)
{
  free(data);
}

struct access_monitor *access_monitor_new(struct uhuru *u)
{
  struct access_monitor *m = g_new(struct access_monitor, 1);
  GIOChannel *start_channel;

  m->uhuru = u;
  m->scan_conf = uhuru_scan_conf_on_access();

  m->enable = 0;
  m->enable_permission = 0;
  m->enable_removable_media = 0;

  m->entries = g_ptr_array_new_full(10, entry_destroy_notify);

  m->my_pid = getpid();
  
  /* this pipe will be used to trigger creation of the monitor thread when entering main thread loop, */
  /* so that the monitor thread does not start before all modules are initialized and the daemon main loop is entered */
  if (pipe(m->start_pipe) < 0) {
    uhuru_log(UHURU_LOG_MODULE, UHURU_LOG_LEVEL_ERROR, "fanotify: pipe failed (%s)", strerror(errno));
    g_free(m);
    return NULL;
  }

  /* this pipe will be used to send commands to the monitor thread */
  if (pipe(m->command_pipe) < 0) {
    uhuru_log(UHURU_LOG_MODULE, UHURU_LOG_LEVEL_ERROR, "fanotify: pipe failed (%s)", strerror(errno));
    g_free(m);
    return NULL;
  }

  start_channel = g_io_channel_unix_new(m->start_pipe[0]);	
  g_io_add_watch(start_channel, G_IO_IN, access_monitor_start_cb, m);

  uhuru_log(UHURU_LOG_MODULE, UHURU_LOG_LEVEL_DEBUG, "fanotify: init ok");

  return m;
}

int access_monitor_start(struct access_monitor *m)
{
  char c = 'A';

  if (m == NULL)
    return 0;

  if (write(m->start_pipe[1], &c, 1) < 0)
    return -1;

  return 0;
}

static gboolean access_monitor_start_cb(GIOChannel *source, GIOCondition condition, gpointer data)
{
  struct access_monitor *m = (struct access_monitor *)data;
  char c;

  if (read(m->start_pipe[0], &c, 1) < 0 || c != 'A') {
    uhuru_log(UHURU_LOG_MODULE, UHURU_LOG_LEVEL_ERROR, "fanotify: read() in activation callback failed (%s)", strerror(errno));

    return FALSE;
  }

  uhuru_log(UHURU_LOG_MODULE, UHURU_LOG_LEVEL_DEBUG, "fanotify: started");

  g_io_channel_shutdown(source, FALSE, NULL);

  m->monitor_thread = g_thread_new("access monitor thread", access_monitor_thread_fun, m);

  return TRUE;
}

static gpointer access_monitor_thread_fun(gpointer data)
{
  struct access_monitor *m = (struct access_monitor *)data;
  GMainLoop *loop;

  uhuru_log(UHURU_LOG_MODULE, UHURU_LOG_LEVEL_DEBUG, "fanotify: started thread");

  m->command_channel = g_io_channel_unix_new(m->command_pipe[0]);	
  g_io_add_watch(m->command_channel, G_IO_IN, access_monitor_command_cb, m);

  /* add the fanotify file desc to the thread loop */

  /* if configured, add the mount monitor */

  /* init all fanotify mark */

  loop = g_main_loop_new(NULL, FALSE);

  g_main_loop_run(loop);
}

static gboolean access_monitor_command_cb(GIOChannel *source, GIOCondition condition, gpointer data)
{
  struct access_monitor *m = (struct access_monitor *)data;
  char cmd;

  if (read(m->command_pipe[0], &cmd, 1) < 0) {
    uhuru_log(UHURU_LOG_MODULE, UHURU_LOG_LEVEL_ERROR, "fanotify: read() in command callback failed (%s)", strerror(errno));

    return FALSE;
  }

  switch(cmd) {
  case 'g':
    break;
  case 's':
    break;
  }

  return TRUE;
}

static char *get_file_path_from_fd(int fd, char *buffer, size_t buffer_size)
{
  ssize_t len;

  if (fd <= 0)
    return NULL;

  sprintf (buffer, "/proc/self/fd/%d", fd);
  if ((len = readlink(buffer, buffer, buffer_size - 1)) < 0)
    return NULL;

  buffer[len] = '\0';

  return buffer;
}

static int write_response(struct access_monitor *m, int fd, __u32 r, const char *path, const char *reason)
{
  struct fanotify_response response;
  GLogLevelFlags log_level = UHURU_LOG_LEVEL_INFO;
  const char *msg = "ALLOW";

  response.fd = fd;
  response.response = r;

  write(m->fanotify_fd, &response, sizeof(struct fanotify_response));
  
  if (r == FAN_DENY) {
    log_level = UHURU_LOG_LEVEL_WARNING;
    msg = "DENY";
  }

  uhuru_log(UHURU_LOG_MODULE, log_level, "fanotify:  path '%s' -> %s", path ? path : "unknown", msg);

  close(fd);

  return r;
}

static __u32 file_status_2_response(enum uhuru_file_status status)
{
  switch(status) {
  case UHURU_SUSPICIOUS:
  case UHURU_MALWARE:
    return FAN_DENY;
  }

  return FAN_ALLOW;
}

void scan_file_thread_fun(gpointer data, gpointer user_data)
{
  struct uhuru_file_context *file_context = (struct uhuru_file_context *)data;
  struct access_monitor *m = (struct access_monitor *)user_data;
  struct uhuru_scan *scan = uhuru_scan_new(m->uhuru, -1);
  enum uhuru_file_status status;
	
  status = uhuru_scan_context(scan, file_context);

  write_response(m, file_context->fd, file_status_2_response(status), file_context->path, NULL);

  /* send notification if malware */
  
  uhuru_file_context_free(file_context);

  uhuru_scan_free(scan);
}

static int perm_event_process(struct access_monitor *m, struct fanotify_event_metadata *event)
{
  struct stat buf;
  struct uhuru_file_context file_context;
  enum uhuru_file_context_status context_status;
  char file_path[PATH_MAX + 1];
  char *p;

  if (m->enable_permission == 0)  /* permission check is disabled, always allow */
    return write_response(m, event->fd, FAN_ALLOW, NULL, "permission is not activated");

  if (m->my_pid == event->pid)   /* file was opened by myself, always allow */
    return write_response(m, event->fd, FAN_ALLOW, NULL, "event PID is myself");


  /* the 2 following tests could be removed: */
  /* if file descriptor does not refer to a file, read() will fail inside os_mime_type_guess_fd() */
  /* in this case, mime_type will be null, context_status will be error and */
  /* response will be ALLOW */
  if (fstat(event->fd, &buf) < 0)
    return write_response(m, event->fd, FAN_ALLOW, NULL, "stat failed");

  if (!S_ISREG(buf.st_mode))
    return write_response(m, event->fd, FAN_ALLOW, NULL, "fd is not a file");

  p = get_file_path_from_fd(event->fd, file_path, PATH_MAX);

  /* get file scan context */
  context_status = uhuru_file_context_get(&file_context, event->fd, p, m->scan_conf);

  if (context_status) {   /* means file must not be scanned */
    uhuru_file_context_close(&file_context);
    return write_response(m, event->fd, FAN_ALLOW, p, "file type is not scanned");
  }

  /* scan in this thread */
  /* g_thread_pool_push(m->thread_pool, uhuru_file_context_clone(&file_context), NULL); */

  return 0;
}

static void notify_event_process(struct access_monitor *m, struct fanotify_event_metadata *event)
{
  char file_path[PATH_MAX + 1];
  char *p;

  p = get_file_path_from_fd(event->fd, file_path, PATH_MAX);

  /* if (m->flags & MONITOR_LOG_EVENT) */
  /*   fprintf(stderr, "fanotify: path %s\n", p); */
}

/* Size of buffer to use when reading fanotify events */
/* 8192 is recommended by fanotify man page */
#define FANOTIFY_BUFFER_SIZE 8192

static gboolean access_monitor_fanotify_cb(GIOChannel *source, GIOCondition condition, gpointer data)
{
  struct access_monitor *m = (struct access_monitor *)data;
  char buf[FANOTIFY_BUFFER_SIZE];
  ssize_t len;

  if ((len = read (m->fanotify_fd, buf, FANOTIFY_BUFFER_SIZE)) > 0)  {
    struct fanotify_event_metadata *event;

    for(event = (struct fanotify_event_metadata *)buf; FAN_EVENT_OK(event, len); event = FAN_EVENT_NEXT(event, len)) {
      char file_path[PATH_MAX + 1];
      char *p;

      p = get_file_path_from_fd(event->fd, file_path, PATH_MAX);

      if ((event->mask & FAN_OPEN_PERM))
	perm_event_process(m, event);
      else
	notify_event_process(m, event);
    }
  }

  return TRUE;
}
