/****************************************************************************
 *
 *  File: yldisp.c
 *
 *  Copyright (C) 2006 - 2008  Thomas Reitmayr <treitmayr@devbase.at>
 *
 ****************************************************************************
 *
 *  This file is part of Yeaphone.
 *
 *  Yeaphone is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include "yldisp.h"
#include "ypmainloop.h"

#ifdef DMALLOC
#include <dmalloc.h>
#endif

/*****************************************************************/

#define NANOSEC 1000000000L

#define YLDISP_BLINK_ID     20
#define YLDISP_DATETIME_ID  21
#define YLDISP_MINRING_ID   22

const char *YLDISP_DRIVER_BASEDIR = "/sys/bus/usb/drivers/yealink/";
const char *YLDISP_INPUT_BASE = "/dev/input/event";

typedef struct yldisp_data_s {
  char *path_sysfs;
  char *path_event;
  char *path_buf;
  
  yl_models_t model;
  int led_inverted;
  
  unsigned int blink_on_time;
  unsigned int blink_off_time;
  int blink_off_reschedule;
  
  time_t counter_base;
  int wait_date_after_count;
  
  int ring_off_delayed;
} yldisp_data_t;


yldisp_data_t yldisp_data;

/*****************************************************************/
/* forward declarations */

static void yldisp_determine_model();

/*****************************************************************/

int exist_dir(const char *dirname)
{
  DIR *dir_handle;
  int result = 0;
  
  dir_handle = opendir(dirname);
  if (dir_handle) {
    closedir(dir_handle);
    result = 1;
  }
  return result;
}

/*****************************************************************/

typedef int (*cmp_dirent) (const char *dirname, void *priv);

char *find_dirent(const char *dirname, cmp_dirent compare, void *priv)
{
  DIR *dir_handle;
  struct dirent *dirent;
  char *result = NULL;
  
  dir_handle = opendir(dirname);
  if (!dir_handle) {
    return NULL;
  }
  while (!result && (dirent = readdir(dir_handle))) {
    if (compare(dirent->d_name, priv)) {
      result = strdup(dirent->d_name);
      if (!result) {
        perror("__FILE__/__LINE__: strdup");
        abort();
      }
    }
  }
  closedir(dir_handle);
  return result;
}

/*****************************************************************/

char *get_num_ptr(char *s)
{
  /* old link to input class directory found, now deprecated */
  char *cptr = s;
  while (*cptr && !isdigit(*cptr))
    cptr++;
  return (*cptr) ? cptr : NULL;
}

/*****************************************************************/

int cmp_devlink(const char *dirname, void *priv)
{
  (void) priv;
  return (dirname && dirname[0] >= '0' && dirname[0] <= '9');
}

int cmp_eventlink(const char *dirname, void *priv) {
  (void) priv;
  return (dirname &&
          ((!strncmp(dirname, "event", 5) && isdigit(dirname[5])) ||
           (!strncmp(dirname, "input:event", 11) && isdigit(dirname[11]))));
}

int cmp_inputdir(const char *dirname, void *priv) {
  char *s = (char *) priv;
  return (dirname && !strncmp(dirname, s, strlen(s)) &&
          isdigit(dirname[strlen(s)]));
}

void yldisp_init() {
  char *symlink;
  char *dirname;
  int plen;
  char *evnum;
  struct stat event_stat;
  
  yldisp_data.path_sysfs = NULL;
  yldisp_data.path_event = NULL;
  yldisp_data.path_buf   = NULL;
  
  dirname = find_dirent(YLDISP_DRIVER_BASEDIR, cmp_devlink, NULL);
  if (!dirname) {
    fprintf(stderr, "Please connect your handset first!\n");
    abort();
  }
  plen = strlen(YLDISP_DRIVER_BASEDIR) + strlen(dirname) + 10;
  yldisp_data.path_sysfs = malloc(plen);
  if (!yldisp_data.path_sysfs) {
    perror("__FILE__/__LINE__: malloc");
    abort();
  }
  strcpy(yldisp_data.path_sysfs, YLDISP_DRIVER_BASEDIR);
  strcat(yldisp_data.path_sysfs, dirname);
  strcat(yldisp_data.path_sysfs, "/");
  free(dirname);
  printf("path_sysfs = %s\n", yldisp_data.path_sysfs);
  
  /* allocate buffer for sysfs interface path */
  yldisp_data.path_buf = malloc(plen + 20);
  if (!yldisp_data.path_buf) {
    perror("__FILE__/__LINE__: malloc");
    abort();
  }
  strcpy(yldisp_data.path_buf, yldisp_data.path_sysfs);
  
  evnum = NULL;
  symlink = malloc(plen + 50);
  if (!symlink) {
    perror("__FILE__/__LINE__: malloc");
    abort();
  }
  strcpy(symlink, yldisp_data.path_sysfs);
  dirname = find_dirent(symlink, cmp_eventlink, NULL);
  if (dirname) {
    evnum = get_num_ptr(dirname);
  }
  if (!evnum) {
    dirname = find_dirent(symlink, cmp_inputdir, "input:input");
    if (dirname) {
      strcat(symlink, dirname);
      free(dirname);
      dirname = find_dirent(symlink, cmp_eventlink, NULL);
      if (dirname) {
        evnum = get_num_ptr(dirname);
      }
    }
  }
  if (!evnum) {
    strcat(symlink, "input/");
    dirname = find_dirent(symlink, cmp_inputdir, "input");
    if (dirname) {
      strcat(symlink, dirname);
      free(dirname);
      dirname = find_dirent(symlink, cmp_eventlink, NULL);
      if (dirname) {
        evnum = get_num_ptr(dirname);
      }
    }
  }
  if (evnum) {
    yldisp_data.path_event = malloc(strlen(YLDISP_INPUT_BASE) +
                                    strlen(evnum) + 4);
    strcpy(yldisp_data.path_event, YLDISP_INPUT_BASE);
    strcat(yldisp_data.path_event, evnum);
    free(dirname);
    printf("path_event = %s\n", yldisp_data.path_event);
  }
  else {
    fprintf(stderr, "Could not find the input event interface via %s!\n",
            yldisp_data.path_sysfs);
    abort();
  }

  if (stat(yldisp_data.path_event, &event_stat)) {
    perror(yldisp_data.path_event);
    abort();
  }
  if (!S_ISCHR(event_stat.st_mode)) {
    fprintf(stderr, "Error: %s is no character device\n",
            yldisp_data.path_event);
    abort();
  }
  
  yldisp_determine_model();
  
  yldisp_hide_all();
}

/*****************************************************************/

void yldisp_uninit()
{
  yp_ml_remove_event(-1, YLDISP_BLINK_ID);
  yp_ml_remove_event(-1, YLDISP_DATETIME_ID);
  
  /* more to come, eg. free */
  
  yldisp_hide_all();
}

/*****************************************************************/

static int yld_write_control_file_buf(yldisp_data_t *yld_ptr,
                                      const char *control,
                                      const char *buf,
                                      int size)
{
  FILE *fp;
  int res;
  
  strcpy(yld_ptr->path_buf, yld_ptr->path_sysfs);
  strcat(yld_ptr->path_buf, control);
  
  fp = fopen(yld_ptr->path_buf, "wb");
  if (fp) {
    res = fwrite(buf, 1, size, fp);
    if (res < size)
      perror(yld_ptr->path_buf);
    fclose(fp);
  }
  else {
    perror(yld_ptr->path_buf);
    res = -1;
  }
  
  return res;
}

/*****************************************************************/

static inline int yld_write_control_file(yldisp_data_t *yld_ptr,
                                         const char *control,
                                         const char *line)
{
  return yld_write_control_file_buf(yld_ptr, control, line, strlen(line));
}

/*****************************************************************/

static int yld_read_control_file_buf(yldisp_data_t *yld_ptr,
                                     const char *control,
                                     char *buf,
                                     int size)
{
  FILE *fp;
  int res;
  
  strcpy(yld_ptr->path_buf, yld_ptr->path_sysfs);
  strcat(yld_ptr->path_buf, control);
  
  fp = fopen(yld_ptr->path_buf, "rb");
  if (fp) {
    res = fread(buf, 1, size, fp);
    if (res < 0)
      perror(yld_ptr->path_buf);
    fclose(fp);
  }
  else {
    perror(yld_ptr->path_buf);
    res = -1;
  }
  
  return res;
}

/*****************************************************************/

static inline int yld_read_control_file(yldisp_data_t *yld_ptr,
                                        const char *control,
                                        char *line,
                                        int size)
{
  int len = yld_read_control_file_buf(yld_ptr, control, line, size - 1);
  if (len >= 0)
    line[len] = '\0';
  return len;
}

/*****************************************************************/

const static char *model_strings[] = { "Unknown", "P1K", "P4K", "B2K", "P1KH" };

static void yldisp_determine_model()
{
  char model_str[50];
  int len;
  
  len = yld_read_control_file(&yldisp_data, "model",
                              model_str, sizeof(model_str));
  yldisp_data.led_inverted = ((len < 0) || (model_str[0] == ' ') ||
                                           (model_str[0] == '*'));
  if ((len < 0) || !strcmp(model_str, "P1K") || strstr(model_str, "*P1K"))
    yldisp_data.model = YL_MODEL_P1K;
  else
  if (!strcmp(model_str, "P1KH"))
    yldisp_data.model = YL_MODEL_P1KH;
  else
  if (!strcmp(model_str, "P4K") || strstr(model_str, "*P4K"))
    yldisp_data.model = YL_MODEL_P4K;
  else
  if (!strcmp(model_str, "B2K") || strstr(model_str, "*B2K"))
    yldisp_data.model = YL_MODEL_B2K;
  else
    yldisp_data.model = YL_MODEL_UNKNOWN;
  
  if (yldisp_data.model != YL_MODEL_UNKNOWN)
    printf("Detected handset Yealink USB-%s\n", model_strings[yldisp_data.model]);
  else
    printf("Unable to detect type of handset\n");
}

yl_models_t get_yldisp_model()
{
  return yldisp_data.model;
}

/*****************************************************************/

static void led_off_callback(int id, int group, void *private_data) {
  yldisp_data_t *yld_ptr = private_data;
  
  if (yld_ptr->blink_off_reschedule) {
    yld_ptr->blink_off_reschedule = 0;
    yp_ml_schedule_periodic_timer(YLDISP_BLINK_ID,
                                  yld_ptr->blink_on_time + yld_ptr->blink_off_time,
                                  led_off_callback, private_data);
  }
  yld_write_control_file(yld_ptr,
                         (yldisp_data.led_inverted) ? "show_icon" : "hide_icon",
                         "LED");
}

static void led_on_callback(int id, int group, void *private_data) {
  yldisp_data_t *yld_ptr = private_data;
  
  yld_write_control_file(yld_ptr,
                         (yldisp_data.led_inverted) ? "hide_icon" : "show_icon",
                         "LED");
}

void yldisp_led_blink(unsigned int on_time, unsigned int off_time) {
  yp_ml_remove_event(-1, YLDISP_BLINK_ID);
  
  yldisp_data.blink_on_time = on_time;
  yldisp_data.blink_off_time = off_time;
  
  if (on_time > 0) {
    /* turn on LED */
    led_on_callback(0, 0, &yldisp_data);
    
    if (off_time > 0) {
      yldisp_data.blink_off_reschedule = 1;
      yp_ml_schedule_timer(YLDISP_BLINK_ID, off_time,
                           led_off_callback, &yldisp_data);
      yp_ml_schedule_periodic_timer(YLDISP_BLINK_ID, (on_time + off_time),
                                    led_on_callback, &yldisp_data);
    }
  }
  else {
    /* turn off LED */
    yldisp_data.blink_off_reschedule = 0;
    led_off_callback(0, 0, &yldisp_data);
  }
}

void yldisp_led_off() {
  yldisp_led_blink(0, 1);
}

void yldisp_led_on() {
  yldisp_led_blink(1, 0);
}

/*****************************************************************/

static void show_date_callback(int id, int group, void *private_data) {
  yldisp_data_t *yld_ptr = private_data;
  time_t t;
  struct tm *tms;
  char line1[18];
  char line2[10];

  t = time(NULL);
  tms = localtime(&t);
  
  strcpy(line2, "\t\t       ");
  line2[tms->tm_wday + 2] = '.';
  yld_write_control_file(yld_ptr, "line2", line2);

  sprintf(line1, "%2d.%2d.%2d.%02d\t\t\t %02d",
          tms->tm_mon + 1, tms->tm_mday,
          tms->tm_hour, tms->tm_min, tms->tm_sec);
  yld_write_control_file(yld_ptr, "line1", line1);
}

static void delayed_date_callback(int id, int group, void *private_data) {
  yldisp_data.wait_date_after_count = 0;
  yldisp_show_date();
}

void yldisp_show_date() {
  yp_ml_remove_event(-1, YLDISP_DATETIME_ID);
  
  if (yldisp_data.wait_date_after_count) {
    yp_ml_schedule_timer(YLDISP_DATETIME_ID, 5000,
                         delayed_date_callback, &yldisp_data);
  }
  else {
    show_date_callback(0, 0, &yldisp_data);
    yp_ml_schedule_periodic_timer(YLDISP_DATETIME_ID, 1000,
                                  show_date_callback, &yldisp_data);
  }
}


static void show_counter_callback(int id, int group, void *private_data) {
  yldisp_data_t *yld_ptr = private_data;
  time_t diff;
  char line1[18];
  int h,m,s;

  diff = time(NULL) - yld_ptr->counter_base;
  h = m = 0;
  s = diff % 60;
  if (diff >= 60) {
    m = ((diff - s) / 60);
    if (m >= 60) {
      h = m / 60;
      m -= h * 60;
    }
  }
  sprintf(line1, "      %2d.%02d\t\t\t %02d", h, m, s);
  yld_write_control_file(yld_ptr, "line1", line1);
  yld_write_control_file(yld_ptr, "line2", "\t\t       ");
}

void yldisp_start_counter() {
  yp_ml_remove_event(-1, YLDISP_DATETIME_ID);
  yldisp_data.wait_date_after_count = 1;
  yldisp_data.counter_base = time(NULL);
  show_date_callback(0, 0, &yldisp_data);
  yp_ml_schedule_periodic_timer(YLDISP_DATETIME_ID, 1000,
                                show_counter_callback, &yldisp_data);
}


void yldisp_stop_counter() {
  yp_ml_remove_event(-1, YLDISP_DATETIME_ID);
}

/*****************************************************************/

void set_yldisp_call_type(yl_call_type_t ct) {
  char line1[14];
  
  strcpy(line1, "\t\t\t\t\t\t\t\t\t\t\t  ");
  if (ct == YL_CALL_IN) {
    line1[11] = '.';
  }
  else if (ct == YL_CALL_OUT) {
    line1[12] = '.';
  }
  
  yld_write_control_file(&yldisp_data, "line1", line1);
}


yl_call_type_t get_yldisp_call_type() {
  return(0);
}


void set_yldisp_store_type(yl_store_type_t st) {
  char line1[15];
  
  strcpy(line1, "\t\t\t\t\t\t\t\t\t\t\t\t\t ");
  if (st == YL_STORE_ON) {
    line1[13] = '.';
  }
  yld_write_control_file(&yldisp_data, "line1", line1);
}


yl_store_type_t get_yldisp_store_type() {
  return(0);
}

/*****************************************************************/

#define RINGTONE_MAXLEN 256
#define RING_DIR ".yeaphone/ringtone"
void set_yldisp_ringtone(char *ringname, unsigned char volume)
{
  int fd_in;
  char ringtone[RINGTONE_MAXLEN];
  int len = 0;
  char *ringfile;
  char *home;

  if ((yldisp_data.model == YL_MODEL_P4K) || (yldisp_data.model == YL_MODEL_B2K))
    return;

  /* make sure the buzzer is turned off! */
  if (yp_ml_remove_event(-1, YLDISP_MINRING_ID) > 0) {
    yld_write_control_file(&yldisp_data, "hide_icon", "RINGTONE");
    usleep(10000);   /* urgh! TODO: Get rid of the delay! */
  }
  /* ringname may be either a path relative to RINGDIR or an absolute path */
  home = getenv("HOME");
  if (home && (ringname[0] != '/')) {
    len = strlen(home) + strlen(RING_DIR) + strlen(ringname) + 3;
    ringfile = malloc(len);
    strcpy(ringfile, home);
    strcat(ringfile, "/"RING_DIR"/");
    strcat(ringfile, ringname);
  } else {
    ringfile = strdup(ringname);
  }

  /* read binary file (replacing first byte with volume)
  ** and write to ringtone control file
  ** TODO: track changes - if unchanged, don't set it again
  ** (write to current.ring file)
  */
  fd_in = open(ringfile, O_RDONLY);
  if (fd_in >= 0)
  {
    len = read(fd_in, ringtone, RINGTONE_MAXLEN);
    if (len > 4)
    {
      /* write volume (replace first byte) */
      ringtone[0] = volume;
      yld_write_control_file_buf(&yldisp_data, "ringtone", ringtone, len);
    }
    else
    {
      fprintf(stderr, "too short ringfile %s (len=%d)\n", ringfile, len);
    }
    close(fd_in);
  }
  else
  {
    fprintf(stderr, "can't open ringfile %s\n", ringfile);
  }
  
  free(ringfile);
}


void yldisp_minring_callback(int id, int group, void *private_data) {
  yldisp_data_t *yld_ptr = private_data;
  if (yld_ptr->ring_off_delayed) {
    yld_write_control_file(yld_ptr, "hide_icon", "RINGTONE");
    yld_ptr->ring_off_delayed = 0;
  }
}

void set_yldisp_ringer(yl_ringer_state_t rs, int minring) {
  switch (rs) {
    case YL_RINGER_ON:
      if (yp_ml_remove_event(-1, YLDISP_MINRING_ID) > 0) {
        yld_write_control_file(&yldisp_data, "hide_icon",
                 (yldisp_data.model == YL_MODEL_P4K) ? "SPEAKER" : "RINGTONE");
        usleep(10000);   /* urgh! TODO: Get rid of the delay! */
      }
      yldisp_data.ring_off_delayed = 0;
      yp_ml_schedule_timer(YLDISP_MINRING_ID, minring,
                           yldisp_minring_callback, &yldisp_data);
      yld_write_control_file(&yldisp_data, "show_icon",
               (yldisp_data.model == YL_MODEL_P4K) ? "SPEAKER" : "RINGTONE");
      break;
    case YL_RINGER_OFF_DELAYED:
      if (yp_ml_count_events(-1, YLDISP_MINRING_ID) > 0)
        yldisp_data.ring_off_delayed = 1;
      else
        yld_write_control_file(&yldisp_data, "hide_icon",
                 (yldisp_data.model == YL_MODEL_P4K) ? "SPEAKER" : "RINGTONE");
      break;
    case YL_RINGER_OFF:
      yld_write_control_file(&yldisp_data, "hide_icon",
               (yldisp_data.model == YL_MODEL_P4K) ? "SPEAKER" : "RINGTONE");
      yp_ml_remove_event(-1, YLDISP_MINRING_ID);
      yldisp_data.ring_off_delayed = 0;
      break;
  }
}

yl_ringer_state_t get_yldisp_ringer() {
  return(0);
}

void yldisp_ringer_vol_up() {
}

void yldisp_ringer_vol_down() {
}

/*****************************************************************/

void set_yldisp_text(char *text) {
  yld_write_control_file(&yldisp_data, "line3", text);
}

char *get_yldisp_text() {
  return(NULL);
}


void yldisp_hide_all() {
  set_yldisp_ringer(YL_RINGER_OFF, 0);
  yldisp_led_off();
  yldisp_stop_counter();
  yld_write_control_file(&yldisp_data, "line1", "                 ");
  yld_write_control_file(&yldisp_data, "line2", "         ");
  yld_write_control_file(&yldisp_data, "line3", "            ");
}

/*****************************************************************/

char *get_yldisp_sysfs_path() {
  return(yldisp_data.path_sysfs);
}


char *get_yldisp_event_path() {
  return(yldisp_data.path_event);
}
