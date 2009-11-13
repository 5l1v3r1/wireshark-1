/* capinfos.c
 * Reports capture file information including # of packets, duration, others
 *
 * Copyright 2004 Ian Schorr
 *
 * $Id$
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

/*
 * 2009-09-19: jyoung
 *
 * New capinfos features
 *
 * Continue processing additional files after 
 * a wiretap open failure.  The new -C option 
 * reverts to capinfos' original behavior which 
 * is to cancels any further file processing at 
 * first file open failure.
 *
 * Change the behavior of how the default display 
 * of all infos is initiated.  This gets rid of a 
 * special post getopt() argument count test.
 *
 * Add new table output format (with related options)
 * This feature allows outputting the various infos 
 * into a tab delimited text file, or to a comma 
 * separated variables file (*.csv) instead of the
 * original "long" format.
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <glib.h>

#include <epan/packet.h>
#include <epan/filesystem.h>
#include <epan/plugins.h>
#include <epan/report_err.h>
#include "wtap.h"
#include <wsutil/privileges.h>

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#else
#include "wsgetopt.h"
#endif

/*
 * By default capinfos now continues processing
 * the next filename if and when wiretap detects 
 * a problem opening a file.  
 * Use the '-C' option to revert back to original
 * capinfos behavior which is to abort any 
 * additional file processing at first open file 
 * failure.
 */

static gboolean continue_after_wtap_open_offline_failure = TRUE;

/*
 * table report variables
 */

static gboolean long_report = TRUE;         /* By default generate long report       */
static gchar table_report_header = TRUE;    /* Generate column header by default     */
static gchar field_separator = '\t';        /* Use TAB as field separator by default */
static gchar quote_char = '\0';             /* Do NOT quote fields by default        */

/*
 * capinfos has the ability to report on a number of 
 * various characteristics ("infos") for each input file.  
 * 
 * By default reporting of all info fields is enabled. 
 *
 * Optionally the reporting of any specific info field 
 * or combination of info fields can be enabled with 
 * individual options.
 */

static gboolean report_all_infos = TRUE;    /* Report all infos           */

static gboolean cap_file_type = TRUE;       /* Report capture type        */
static gboolean cap_file_encap = TRUE;      /* Report encapsulation       */
static gboolean cap_packet_count = TRUE;    /* Report packet count        */
static gboolean cap_file_size = TRUE;       /* Report file size           */

static gboolean cap_data_size = TRUE;       /* Report packet byte size    */
static gboolean cap_duration = TRUE;        /* Report capture duration    */
static gboolean cap_start_time = TRUE;      /* Report capture start time  */
static gboolean cap_end_time = TRUE;        /* Report capture end time    */

static gboolean cap_data_rate_byte = TRUE;  /* Report data rate bytes/sec */
static gboolean cap_data_rate_bit = TRUE;   /* Report data rate bites/sec */
static gboolean cap_packet_size = TRUE;     /* Report average packet size */
static gboolean cap_packet_rate = TRUE;     /* Report average packet rate */

typedef struct _capture_info {
	const char		*filename;
	guint16			file_type;
	int			file_encap;
	gint64			filesize;
	guint64			packet_bytes;
	double			start_time;
	double			stop_time;
	guint32			packet_count;
	gboolean		snap_set;
	guint32			snaplen;
	gboolean		drops_known;
	guint32			drop_count;

	double			duration;
	double			packet_rate;
	double			packet_size;
	double			data_rate;		/* in bytes */
} capture_info;

static void
enable_all_infos(void)
{
  report_all_infos = TRUE;

  cap_file_type = TRUE;
  cap_file_encap = TRUE;
  cap_packet_count = TRUE;
  cap_file_size = TRUE;

  cap_data_size = TRUE;
  cap_duration = TRUE;
  cap_start_time = TRUE;
  cap_end_time = TRUE;

  cap_data_rate_byte = TRUE;
  cap_data_rate_bit = TRUE;
  cap_packet_size = TRUE;
  cap_packet_rate = TRUE;
}

static void
disable_all_infos(void)
{
  report_all_infos = FALSE;

  cap_file_type = FALSE;
  cap_file_encap = FALSE;
  cap_packet_count = FALSE;
  cap_file_size = FALSE;

  cap_data_size = FALSE;
  cap_duration = FALSE;
  cap_start_time = FALSE;
  cap_end_time = FALSE;

  cap_data_rate_byte = FALSE;
  cap_data_rate_bit = FALSE;
  cap_packet_size = FALSE;
  cap_packet_rate = FALSE;
}

/*
 * ctime_no_lf()
 *
 * This function simply truncates the string returned 
 * from the ctime() function to remove the trailing 
 * '\n' character.
 *
 * The ctime() function returns a string formatted as:
 *   "Www Mmm dd hh:mm:ss yyyy\n"
 * The unwanted '\n' is the 24th character.
 */

static gchar *
ctime_no_lf(const time_t* timer)
{
  gchar *time_string;
  time_string = ctime(timer);
  time_string[24] = '\0';
  return(time_string);  
}

static double
secs_nsecs(const struct wtap_nstime * nstime)
{
  return (nstime->nsecs / 1000000000.0) + (double)nstime->secs;
}

static void print_value(gchar *text_p1, gint width, gchar *text_p2, double value) {
  if (value > 0.0)
    printf("%s%.*f%s\n", text_p1, width, value, text_p2);
  else
    printf("%sn/a\n", text_p1);
}

static void
print_stats(const gchar *filename, capture_info *cf_info)
{
  const gchar		*file_type_string, *file_encap_string;
  time_t		start_time_t;
  time_t		stop_time_t;

  /* Build printable strings for various stats */
  file_type_string = wtap_file_type_string(cf_info->file_type);
  file_encap_string = wtap_encap_string(cf_info->file_encap);
  start_time_t = (time_t)cf_info->start_time;
  stop_time_t = (time_t)cf_info->stop_time;

  if (filename)           printf     ("File name:           %s\n", filename);
  if (cap_file_type)      printf     ("File type:           %s\n", file_type_string);
  if (cap_file_encap)     printf     ("File encapsulation:  %s\n", file_encap_string);
  if (cap_packet_count)   printf     ("Number of packets:   %u\n", cf_info->packet_count);
  if (cap_file_size)      printf     ("File size:           %" G_GINT64_MODIFIER "d bytes\n", cf_info->filesize);
  if (cap_data_size)      printf     ("Data size:           %" G_GINT64_MODIFIER "u bytes\n", cf_info->packet_bytes);
  if (cap_duration)       print_value("Capture duration:    ", 0, " seconds",   cf_info->duration); 
  if (cap_start_time)     printf     ("Start time:          %s", (cf_info->packet_count>0) ? ctime (&start_time_t) : "n/a\n");
  if (cap_end_time)       printf     ("End time:            %s", (cf_info->packet_count>0) ? ctime (&stop_time_t)  : "n/a\n");
  if (cap_data_rate_byte) print_value("Data byte rate:      ", 2, " bytes/sec",   cf_info->data_rate);
  if (cap_data_rate_bit)  print_value("Data bit rate:       ", 2, " bits/sec",    cf_info->data_rate*8);
  if (cap_packet_size)    printf     ("Average packet size: %.2f bytes\n",        cf_info->packet_size);
  if (cap_packet_rate)    print_value("Average packet rate: ", 2, " packets/sec", cf_info->packet_rate);
}

static void
putsep(void)
{
  if (field_separator) putchar(field_separator);
}

static void
putquote(void)
{
  if (quote_char) putchar(quote_char);
}

static void
print_stats_table_header_label(gchar *label)
{
  putsep();
  putquote();
  printf("%s", label);
  putquote();
}

static void
print_stats_table_header(void)
{
  putquote();
  printf("File name");
  putquote();

  if (cap_file_type)      print_stats_table_header_label("File type");
  if (cap_file_encap)     print_stats_table_header_label("File encapsulation");
  if (cap_packet_count)   print_stats_table_header_label("Number of packets");
  if (cap_file_size)      print_stats_table_header_label("File size (bytes)");
  if (cap_data_size)      print_stats_table_header_label("Data size (bytes)");
  if (cap_duration)       print_stats_table_header_label("Capture duration (seconds)");
  if (cap_start_time)     print_stats_table_header_label("Start time");
  if (cap_end_time)       print_stats_table_header_label("End time");
  if (cap_data_rate_byte) print_stats_table_header_label("Data byte rate (bytes/sec)");
  if (cap_data_rate_bit)  print_stats_table_header_label("Data bit rate (bits/sec)");
  if (cap_packet_size)    print_stats_table_header_label("Average packet size (bytes)");
  if (cap_packet_rate)    print_stats_table_header_label("Average packet rate (packets/sec)");

  printf("\n");
}

static void
print_stats_table(const gchar *filename, capture_info *cf_info)
{
  const gchar		*file_type_string, *file_encap_string;
  time_t		start_time_t;
  time_t		stop_time_t;

  /* Build printable strings for various stats */
  file_type_string = wtap_file_type_string(cf_info->file_type);
  file_encap_string = wtap_encap_string(cf_info->file_encap);
  start_time_t = (time_t)cf_info->start_time;
  stop_time_t = (time_t)cf_info->stop_time;

  if (filename) {
    putquote();
    printf("%s", filename);
    putquote();
  }

  if (cap_file_type) {
    putsep();
    putquote();
    printf("%s", file_type_string);
    putquote();
  }

  if (cap_file_encap) {
    putsep();
    putquote();
    printf("%s", file_encap_string);
    putquote();
  }

  if (cap_packet_count) {
    putsep();
    putquote();
    printf("%u", cf_info->packet_count);
    putquote();
  }

  if (cap_file_size) {
    putsep();
    putquote();
    printf("%" G_GINT64_MODIFIER "d", cf_info->filesize);
    putquote();
  }

  if (cap_data_size) {
    putsep();
    putquote();
    printf("%" G_GINT64_MODIFIER "u", cf_info->packet_bytes);
    putquote();
  }

  if (cap_duration) {
    putsep();
    putquote();
    printf("%f", cf_info->duration);
    putquote();
  }

  if (cap_start_time) {
    putsep();
    putquote();
    printf("%s", (cf_info->packet_count>0) ? ctime_no_lf (&start_time_t) : "n/a");
    putquote();
  }

  if (cap_end_time) {
    putsep();
    putquote();
    printf("%s", (cf_info->packet_count>0) ? ctime_no_lf (&stop_time_t) : "n/a");
    putquote();
  }

  if (cap_data_rate_byte) {
    putsep();
    putquote();
    printf("%.2f", cf_info->data_rate);
    putquote();
  }

  if (cap_data_rate_bit) {
    putsep();
    putquote();
    printf("%.2f", cf_info->data_rate*8);
    putquote();
  }

  if (cap_packet_size) {
    putsep();
    putquote();
    printf("%.2f", cf_info->packet_size);
    putquote();
  }

  if (cap_packet_rate) {
    putsep();
    putquote();
    printf("%.2f", cf_info->packet_rate);
    putquote();
  }

  printf("\n");
}

static int
process_cap_file(wtap *wth, const char *filename)
{
  int			err;
  gchar			*err_info;
  gint64		size;
  gint64		data_offset;

  guint32		packet = 0;
  gint64		bytes = 0;
  const struct wtap_pkthdr *phdr;
  capture_info  	cf_info;
  double		start_time = 0;
  double		stop_time = 0;
  double		cur_time = 0;

  /* Tally up data that we need to parse through the file to find */
  while (wtap_read(wth, &err, &err_info, &data_offset))  {
    phdr = wtap_phdr(wth);
    cur_time = secs_nsecs(&phdr->ts);
    if(packet==0) {
      start_time = cur_time;
      stop_time = cur_time;
    }
    if (cur_time < start_time) {
      start_time = cur_time;
    }
    if (cur_time > stop_time) {
      stop_time = cur_time;
    }
    bytes+=phdr->len;
    packet++;
  }

  if (err != 0) {
    fprintf(stderr,
            "capinfos: An error occurred after reading %u packets from \"%s\": %s.\n",
	    packet, filename, wtap_strerror(err));
    switch (err) {

    case WTAP_ERR_UNSUPPORTED:
    case WTAP_ERR_UNSUPPORTED_ENCAP:
    case WTAP_ERR_BAD_RECORD:
      fprintf(stderr, "(%s)\n", err_info);
      g_free(err_info);
      break;
    }
    return 1;
  }

  /* File size */
  size = wtap_file_size(wth, &err);
  if (size == -1) {
    fprintf(stderr,
            "capinfos: Can't get size of \"%s\": %s.\n",
	    filename, strerror(err));
    return 1;
  }

  cf_info.filesize = size;

  /* File Type */
  cf_info.file_type = wtap_file_type(wth);

  /* File Encapsulation */
  cf_info.file_encap = wtap_file_encap(wth);

  /* # of packets */
  cf_info.packet_count = packet;

  /* File Times */
  cf_info.start_time = start_time;
  cf_info.stop_time = stop_time;
  cf_info.duration = stop_time-start_time;

  /* Number of packet bytes */
  cf_info.packet_bytes = bytes;

  cf_info.data_rate   = 0.0;
  cf_info.packet_rate = 0.0;
  cf_info.packet_size = 0.0;

  if (packet > 0) {
    if (cf_info.duration > 0.0) { 
      cf_info.data_rate   = (double)bytes  / (stop_time-start_time); /* Data rate per second */
      cf_info.packet_rate = (double)packet / (stop_time-start_time); /* packet rate per second */
    }
    cf_info.packet_size = (double)bytes / packet;                  /* Avg packet size      */
  }

  if(long_report) {
    print_stats(filename, &cf_info);
  } else {
    print_stats_table(filename, &cf_info);
  }

  return 0;
}

static void
usage(gboolean is_error)
{
  FILE *output;

  if (!is_error) {
    output = stdout;
    /* XXX - add capinfos header info here */
  }
  else {
    output = stderr;
  }

  fprintf(output, "Capinfos %s"
#ifdef SVNVERSION
	  " (" SVNVERSION " from " SVNPATH ")"
#endif
	  "\n", VERSION);
  fprintf(output, "Prints various information (infos) about capture files.\n");
  fprintf(output, "See http://www.wireshark.org for more information.\n");
  fprintf(output, "\n");
  fprintf(output, "Usage: capinfos [options] <infile> ...\n");
  fprintf(output, "\n");
  fprintf(output, "General infos:\n");
  fprintf(output, "  -t display the capture file type\n");
  fprintf(output, "  -E display the capture file encapsulation\n");
  fprintf(output, "\n");
  fprintf(output, "Size infos:\n");
  fprintf(output, "  -c display the number of packets\n");
  fprintf(output, "  -s display the size of the file (in bytes)\n");
  fprintf(output, "  -d display the total length of all packets (in bytes)\n");
  fprintf(output, "\n");
  fprintf(output, "Time infos:\n");
  fprintf(output, "  -u display the capture duration (in seconds)\n");
  fprintf(output, "  -a display the capture start time\n");
  fprintf(output, "  -e display the capture end time\n");
  fprintf(output, "\n");
  fprintf(output, "Statistic infos:\n");
  fprintf(output, "  -y display average data rate (in bytes/sec)\n");
  fprintf(output, "  -i display average data rate (in bits/sec)\n");
  fprintf(output, "  -z display average packet size (in bytes)\n");
  fprintf(output, "  -x display average packet rate (in packets/sec)\n");
  fprintf(output, "\n");
  fprintf(output, "Output format:\n");
  fprintf(output, "  -L generate long report (default)\n");
  fprintf(output, "  -T generate table report\n");
  fprintf(output, "\n");
  fprintf(output, "Table report options:\n");
  fprintf(output, "  -R generate header record (default)\n");
  fprintf(output, "  -r do not generate header record\n");
  fprintf(output, "\n");
  fprintf(output, "  -B separate infos with TAB character (default)\n");
  fprintf(output, "  -m separate infos with comma (,) character\n");
  fprintf(output, "  -b separate infos with SPACE character\n");
  fprintf(output, "\n");
  fprintf(output, "  -N do not quote infos (default)\n");
  fprintf(output, "  -q quote infos with single quotes (')\n");
  fprintf(output, "  -Q quote infos with double quotes (\")\n");
  fprintf(output, "\n");
  fprintf(output, "Miscellaneous:\n");
  fprintf(output, "  -h display this help and exit\n");
  fprintf(output, "  -C cancel processing if file open fails (default is to continue)\n");
  fprintf(output, "  -A generate all infos (default)\n");
  fprintf(output, "\n");
  fprintf(output, "Options are processed from left to right order with later options superceeding\n");
  fprintf(output, "or adding to earlier options.\n");
  fprintf(output, "\n");
  fprintf(output, "If no options are given the default is to display all infos in long report\n");
  fprintf(output, "output format.\n");
}

#ifdef HAVE_PLUGINS
/*
 *  Don't report failures to load plugins because most (non-wiretap) plugins
 *  *should* fail to load (because we're not linked against libwireshark and
 *  dissector plugins need libwireshark).
 */
static void
failure_message(const char *msg_format _U_, va_list ap _U_)
{
	return;
}
#endif


int
main(int argc, char *argv[])
{
  wtap *wth;
  int err;
  gchar *err_info;
  int opt;
  int status = 0;
#ifdef HAVE_PLUGINS
  char* init_progfile_dir_error;
#endif

  /*
   * Get credential information for later use.
   */
  get_credential_info();

#ifdef HAVE_PLUGINS
  /* Register wiretap plugins */

    if ((init_progfile_dir_error = init_progfile_dir(argv[0], main))) {
		g_warning("capinfos: init_progfile_dir(): %s", init_progfile_dir_error);
		g_free(init_progfile_dir_error);
    } else {
		init_report_err(failure_message,NULL,NULL,NULL);
		init_plugins();
    }
#endif

  /* Process the options */

  while ((opt = getopt(argc, argv, "tEcsduaeyizvhxCALTRrNqQBmb")) !=-1) {

    switch (opt) {

    case 't':
      if (report_all_infos) disable_all_infos();
      cap_file_type = TRUE;
      break;

    case 'E':
      if (report_all_infos) disable_all_infos();
      cap_file_encap = TRUE;
      break;

    case 'c':
      if (report_all_infos) disable_all_infos();
      cap_packet_count = TRUE;
      break;

    case 's':
      if (report_all_infos) disable_all_infos();
      cap_file_size = TRUE;
      break;

    case 'd':
      if (report_all_infos) disable_all_infos();
      cap_data_size = TRUE;
      break;

    case 'u':
      if (report_all_infos) disable_all_infos();
      cap_duration = TRUE;
      break;

    case 'a':
      if (report_all_infos) disable_all_infos();
      cap_start_time = TRUE;
      break;

    case 'e':
      if (report_all_infos) disable_all_infos();
      cap_end_time = TRUE;
      break;

    case 'y':
      if (report_all_infos) disable_all_infos();
      cap_data_rate_byte = TRUE;
      break;

    case 'i':
      if (report_all_infos) disable_all_infos();
      cap_data_rate_bit = TRUE;
      break;

    case 'z':
      if (report_all_infos) disable_all_infos();
      cap_packet_size = TRUE;
      break;

    case 'x':
      if (report_all_infos) disable_all_infos();
      cap_packet_rate = TRUE;
      break;

    case 'C':
      continue_after_wtap_open_offline_failure = FALSE;
      break;

    case 'A':
      enable_all_infos();
      break;

    case 'L':
      long_report = TRUE;
      break;

    case 'T':
      long_report = FALSE;
      break;

    case 'R':
      table_report_header = TRUE;
      break;

    case 'r':
      table_report_header = FALSE;
      break;

    case 'N':
      quote_char = '\0';
      break;

    case 'q':
      quote_char = '\'';
      break;

    case 'Q':
      quote_char = '"';
      break;

    case 'B':
      field_separator = '\t';
      break;

    case 'm':
      field_separator = ',';
      break;

    case 'b':
      field_separator = ' ';
      break;

    case 'h':
      usage(FALSE);
      exit(1);
      break;

    case '?':              /* Bad flag - print usage message */
      usage(TRUE);
      exit(1);
      break;
    }
  }

  if ((argc - optind) < 1) {
    usage(TRUE);
    exit(1);
  }

  if(!long_report && table_report_header) {
    print_stats_table_header();
  }

  for (opt = optind; opt < argc; opt++) {

    wth = wtap_open_offline(argv[opt], &err, &err_info, FALSE);

    if (!wth) {
      fprintf(stderr, "capinfos: Can't open %s: %s\n", argv[opt],
	wtap_strerror(err));
      switch (err) {

      case WTAP_ERR_UNSUPPORTED:
      case WTAP_ERR_UNSUPPORTED_ENCAP:
      case WTAP_ERR_BAD_RECORD:
        fprintf(stderr, "(%s)\n", err_info);
        g_free(err_info);
        break;
      }
      if(!continue_after_wtap_open_offline_failure)
        exit(1);
    }

    if(wth) {
      if ((opt > optind) && (long_report))
        printf("\n");
      status = process_cap_file(wth, argv[opt]);

      wtap_close(wth);
      if (status)
        exit(status);
    }
  }
  return 0;
}

