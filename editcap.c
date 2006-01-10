/* Edit capture files.  We can delete packets, adjust timestamps, or
 * simply convert from one format to another format.
 *
 * $Id$
 *
 * Originally written by Richard Sharpe.
 * Improved by Guy Harris.
 * Further improved by Richard Sharpe.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <glib.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <time.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <string.h>
#include "wtap.h"

#ifdef NEED_GETOPT_H
#include "getopt.h"
#endif

#ifdef _WIN32
#include <process.h>    /* getpid */
#endif

#include "svnversion.h"

/*
 * Some globals so we can pass things to various routines
 */

struct select_item {

  int inclusive;
  int first, second;

};

#define ONE_MILLION 1000000

/* Weights of different errors we can introduce */
/* We should probably make these command-line arguments */
/* XXX - Should we add a bit-level error? */
#define ERR_WT_BIT   5  /* Flip a random bit */
#define ERR_WT_BYTE  5  /* Substitute a random byte */
#define ERR_WT_ALNUM 5  /* Substitute a random character in [A-Za-z0-9] */
#define ERR_WT_FMT   2  /* Substitute "%s" */
#define ERR_WT_AA    1  /* Fill the remainder of the buffer with 0xAA */
#define ERR_WT_TOTAL (ERR_WT_BIT + ERR_WT_BYTE + ERR_WT_ALNUM + ERR_WT_FMT + ERR_WT_AA)

#define ALNUM_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
#define ALNUM_LEN (sizeof(ALNUM_CHARS) - 1)


struct time_adjustment {
  struct timeval tv;
  int is_negative;
};

static struct select_item selectfrm[100];
static int max_selected = -1;
static int keep_em = 0;
static int out_file_type = WTAP_FILE_PCAP;   /* default to "libpcap"   */
static int out_frame_type = -2;              /* Leave frame type alone */
static int verbose = 0;                      /* Not so verbose         */
static struct time_adjustment time_adj = {{0, 0}, 0}; /* no adjustment */
static double err_prob = 0.0;

/* Add a selection item, a simple parser for now */

static void add_selection(char *sel)
{
  char *locn;
  char *next;

  if (max_selected == (sizeof(selectfrm)/sizeof(struct select_item)) - 1)
    return;

  printf("Add_Selected: %s\n", sel);

  if ((locn = strchr(sel, '-')) == NULL) { /* No dash, so a single number? */

    printf("Not inclusive ...");

    max_selected++;
    selectfrm[max_selected].inclusive = 0;
    selectfrm[max_selected].first = atoi(sel);

    printf(" %i\n", selectfrm[max_selected].first);

  }
  else {

    printf("Inclusive ...");

    next = locn + 1;
    max_selected++;
    selectfrm[max_selected].inclusive = 1;
    selectfrm[max_selected].first = atoi(sel);
    selectfrm[max_selected].second = atoi(next);

    printf(" %i, %i\n", selectfrm[max_selected].first, selectfrm[max_selected].second);

  }


}

/* Was the packet selected? */

static int selected(int recno)
{
  int i = 0;

  for (i = 0; i<= max_selected; i++) {

    if (selectfrm[i].inclusive) {
      if (selectfrm[i].first <= recno && selectfrm[i].second >= recno)
	return 1;
    }
    else {
      if (recno == selectfrm[i].first)
	return 1;
    }
  }

  return 0;

}

static void
set_time_adjustment(char *optarg)
{
  char *frac, *end;
  long val;
  int frac_digits;

  if (!optarg)
    return;

  /* skip leading whitespace */
  while (*optarg == ' ' || *optarg == '\t') {
      optarg++;
  }

  /* check for a negative adjustment */
  if (*optarg == '-') {
      time_adj.is_negative = 1;
      optarg++;
  }

  /* collect whole number of seconds, if any */
  if (*optarg == '.') {         /* only fractional (i.e., .5 is ok) */
      val  = 0;
      frac = optarg;
  } else {
      val = strtol(optarg, &frac, 10);
      if (frac == NULL || frac == optarg || val == LONG_MIN || val == LONG_MAX) {
          fprintf(stderr, "editcap: \"%s\" isn't a valid time adjustment\n",
                  optarg);
          exit(1);
      }
      if (val < 0) {            /* implies '--' since we caught '-' above  */
          fprintf(stderr, "editcap: \"%s\" isn't a valid time adjustment\n",
                  optarg);
          exit(1);
      }
  }
  time_adj.tv.tv_sec = val;

  /* now collect the partial seconds, if any */
  if (*frac != '\0') {             /* chars left, so get fractional part */
    val = strtol(&(frac[1]), &end, 10);
    if (*frac != '.' || end == NULL || end == frac
        || val < 0 || val > ONE_MILLION || val == LONG_MIN || val == LONG_MAX) {
      fprintf(stderr, "editcap: \"%s\" isn't a valid time adjustment\n",
              optarg);
      exit(1);
    }
  }
  else {
    return;                     /* no fractional digits */
  }

  /* adjust fractional portion from fractional to numerator
   * e.g., in "1.5" from 5 to 500000 since .5*10^6 = 500000 */
  if (frac && end) {            /* both are valid */
    frac_digits = end - frac - 1;   /* fractional digit count (remember '.') */
    while(frac_digits < 6) {    /* this is frac of 10^6 */
      val *= 10;
      frac_digits++;
    }
  }
  time_adj.tv.tv_usec = val;
}

static void usage(void)
{
  fprintf(stderr, "Editcap %s"
#ifdef SVNVERSION
	  " (" SVNVERSION ")"
#endif
	  "\n", VERSION);
  fprintf(stderr, "Edit and/or translate the format of capture files.\n");
  fprintf(stderr, "See http://www.ethereal.com for more information.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Usage: editcap [options] ... <infile> <outfile> [ <packet#>[-<packet#>] ... ]\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "A single packet or a range of packets can be selected.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Packets:\n");
  fprintf(stderr, "  -C <choplen>           chop each packet at the end by <choplen> bytes\n");
  fprintf(stderr, "  -E <error probability> set the probability (between 0.0 and 1.0 incl.)\n");
  fprintf(stderr, "                         that a particular packet byte will be randomly changed\n");
  fprintf(stderr, "  -r                     keep the selected packets, default is to delete them\n");
  fprintf(stderr, "  -s <snaplen>           truncate packets to max. <snaplen> bytes of data\n");
  fprintf(stderr, "  -t <time adjustment>   adjust the timestamp of selected packets,\n");
  fprintf(stderr, "                         <time adjustment> is in relative seconds (e.g. -0.5)\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Output File(s):\n");
  fprintf(stderr, "  -c <packets per file>  split the packet output to different files,\n");
  fprintf(stderr, "                         with a maximum of <packets per file> each\n");
  fprintf(stderr, "  -F <capture type>      set the output file type, default is libpcap\n");
  fprintf(stderr, "                         an empty \"-F\" option will list the file types\n");
  fprintf(stderr, "  -T <encap type>        set the output file encapsulation type,\n");
  fprintf(stderr, "                         default is the same as the input file\n");
  fprintf(stderr, "                         an empty \"-T\" option will list the encapsulation types\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "Miscellaneous:\n");
  fprintf(stderr, "  -h                     display this help and exit\n");
  fprintf(stderr, "  -v                     verbose output\n");
  fprintf(stderr, "\n");
}

static void list_capture_types(void) {
    int i;

    fprintf(stderr, "editcap: The available capture file types for \"F\":\n");
    for (i = 0; i < WTAP_NUM_FILE_TYPES; i++) {
      if (wtap_dump_can_open(i))
        fprintf(stderr, "    %s - %s\n",
          wtap_file_type_short_string(i), wtap_file_type_string(i));
    }
}

static void list_encap_types(void) {
    int i;
    const char *string;

    fprintf(stderr, "editcap: The available encapsulation types for \"T\":\n");
    for (i = 0; i < WTAP_NUM_ENCAP_TYPES; i++) {
        string = wtap_encap_short_string(i);
        if (string != NULL)
          fprintf(stderr, "    %s - %s\n",
            string, wtap_encap_string(i));
    }
}

int main(int argc, char *argv[])

{
  wtap *wth;
  int i, j, err;
  gchar *err_info;
  extern char *optarg;
  extern int optind;
  int opt;
  char *p;
  unsigned int snaplen = 0;             /* No limit               */
  unsigned int choplen = 0;             /* No chop                */
  wtap_dumper *pdh;
  int count = 1;
  long data_offset;
  struct wtap_pkthdr snap_phdr;
  const struct wtap_pkthdr *phdr;
  int err_type;
  guint8 *buf;
  int split_packet_count = 0;
  int written_count = 0;
  char *filename;

  /* Process the options first */

  while ((opt = getopt(argc, argv, "c:C:E:F:hrs:t:T:v")) !=-1) {

    switch (opt) {

    case 'E':
      err_prob = strtod(optarg, &p);
      if (p == optarg || err_prob < 0.0 || err_prob > 1.0) {
      	fprintf(stderr, "editcap: probability \"%s\" must be between 0.0 and 1.0\n",
      	    optarg);
      	exit(1);
      }
      srand(time(NULL) + getpid());
      break;

    case 'F':
      out_file_type = wtap_short_string_to_file_type(optarg);
      if (out_file_type < 0) {
      	fprintf(stderr, "editcap: \"%s\" isn't a valid capture file type\n\n",
      	    optarg);
        list_capture_types();
      	exit(1);
      }
      break;

    case 'c':
      split_packet_count = strtol(optarg, &p, 10);
      if (p == optarg || *p != '\0') {
      	fprintf(stderr, "editcap: \"%s\" isn't a valid packet count\n",
      	    optarg);
      	exit(1);
      }
      if (split_packet_count <= 0) {
      	fprintf(stderr, "editcap: \"%d\" packet count must be larger than zero\n",
		split_packet_count);
      	exit(1);
      }
      break;

    case 'C':
      choplen = strtol(optarg, &p, 10);
      if (p == optarg || *p != '\0') {
      	fprintf(stderr, "editcap: \"%s\" isn't a valid chop length\n",
      	    optarg);
      	exit(1);
      }
      break;

    case '?':              /* Bad options if GNU getopt */
      switch(optopt) {
      case'F':
        list_capture_types();
        break;
      case'T':
        list_encap_types();
        break;
      default:
        usage();
      }
      exit(1);
      break;

    case 'h':
      usage();
      exit(1);
      break;

    case 'r':
      keep_em = !keep_em;  /* Just invert */
      break;

    case 's':
      snaplen = strtol(optarg, &p, 10);
      if (p == optarg || *p != '\0') {
      	fprintf(stderr, "editcap: \"%s\" isn't a valid snapshot length\n",
      	    optarg);
      	exit(1);
      }
      break;

    case 't':
      set_time_adjustment(optarg);
      break;

    case 'T':
      out_frame_type = wtap_short_string_to_encap(optarg);
      if (out_frame_type < 0) {
      	fprintf(stderr, "editcap: \"%s\" isn't a valid encapsulation type\n\n",
      	    optarg);
        list_encap_types();
      	exit(1);
      }
      break;

    case 'v':
      verbose = !verbose;  /* Just invert */
      break;

    }

  }

#ifdef DEBUG
  printf("Optind = %i, argc = %i\n", optind, argc);
#endif

  if ((argc - optind) < 1) {

    usage();
    exit(1);

  }

  wth = wtap_open_offline(argv[optind], &err, &err_info, FALSE);

  if (!wth) {
    fprintf(stderr, "editcap: Can't open %s: %s\n", argv[optind],
        wtap_strerror(err));
    switch (err) {

    case WTAP_ERR_UNSUPPORTED:
    case WTAP_ERR_UNSUPPORTED_ENCAP:
    case WTAP_ERR_BAD_RECORD:
      fprintf(stderr, "(%s)\n", err_info);
      g_free(err_info);
      break;
    }
    exit(1);

  }

  if (verbose) {

    fprintf(stderr, "File %s is a %s capture file.\n", argv[optind],
	    wtap_file_type_string(wtap_file_type(wth)));

  }

  /*
   * Now, process the rest, if any ... we only write if there is an extra
   * argument or so ...
   */

  if ((argc - optind) >= 2) {

    if (out_frame_type == -2)
      out_frame_type = wtap_file_encap(wth);

    if (split_packet_count > 0) {
      filename = (char *) malloc(strlen(argv[optind+1]) + 20);
      if (!filename) {
	exit(5);
      }
      sprintf(filename, "%s-%05d", argv[optind+1], 0);
    } else {
      filename = argv[optind+1];
    }
  
    pdh = wtap_dump_open(filename, out_file_type,
			 out_frame_type, wtap_snapshot_length(wth), FALSE /* compressed */, &err);
    if (pdh == NULL) {

      fprintf(stderr, "editcap: Can't open or create %s: %s\n", filename,
	      wtap_strerror(err));
      exit(1);

    }

    for (i = optind + 2; i < argc; i++)
      add_selection(argv[i]);

    while (wtap_read(wth, &err, &err_info, &data_offset)) {

      if (split_packet_count > 0 && (written_count % split_packet_count == 0)) {
	if (!wtap_dump_close(pdh, &err)) {

	  fprintf(stderr, "editcap: Error writing to %s: %s\n", filename,
		  wtap_strerror(err));
	  exit(1);
	}

	sprintf(filename, "%s-%05d",argv[optind+1], count / split_packet_count);

	if (verbose) {
	  fprintf(stderr, "Continuing writing in file %s\n", filename);
	}

	pdh = wtap_dump_open(filename, out_file_type,
			     out_frame_type, wtap_snapshot_length(wth), FALSE /* compressed */, &err);
	if (pdh == NULL) {
	  
	  fprintf(stderr, "editcap: Can't open or create %s: %s\n", filename,
		  wtap_strerror(err));
	  exit(1);
	  
	}
      }

      if ((!selected(count) && !keep_em) ||
          (selected(count) && keep_em)) {

        if (verbose)
          printf("Packet: %u\n", count);

        /* We simply write it, perhaps after truncating it; we could do other
           things, like modify it. */

        phdr = wtap_phdr(wth);

        if (choplen != 0 && phdr->caplen > choplen) {
          snap_phdr = *phdr;
          snap_phdr.caplen -= choplen;
          phdr = &snap_phdr;
        }

        if (snaplen != 0 && phdr->caplen > snaplen) {
          snap_phdr = *phdr;
          snap_phdr.caplen = snaplen;
          phdr = &snap_phdr;
        }

        /* assume that if the frame's tv_sec is 0, then
         * the timestamp isn't supported */
        if (phdr->ts.secs > 0 && time_adj.tv.tv_sec != 0) {
          snap_phdr = *phdr;
          if (time_adj.is_negative)
            snap_phdr.ts.secs -= time_adj.tv.tv_sec;
          else
            snap_phdr.ts.secs += time_adj.tv.tv_sec;
          phdr = &snap_phdr;
        }

        /* assume that if the frame's tv_sec is 0, then
         * the timestamp isn't supported */
        if (phdr->ts.secs > 0 && time_adj.tv.tv_usec != 0) {
          snap_phdr = *phdr;
          if (time_adj.is_negative) { /* subtract */
            if (snap_phdr.ts.nsecs/1000 < time_adj.tv.tv_usec) { /* borrow */
              snap_phdr.ts.secs--;
              snap_phdr.ts.nsecs += ONE_MILLION * 1000;
            }
            snap_phdr.ts.nsecs -= time_adj.tv.tv_usec * 1000;
          } else {                  /* add */
            if (snap_phdr.ts.nsecs + time_adj.tv.tv_usec * 1000 > ONE_MILLION * 1000) {
              /* carry */
              snap_phdr.ts.secs++;
              snap_phdr.ts.nsecs += (time_adj.tv.tv_usec - ONE_MILLION) * 1000;
            } else {
              snap_phdr.ts.nsecs += time_adj.tv.tv_usec * 1000;
            }
          }
          phdr = &snap_phdr;
        }

        if (err_prob > 0.0) {
          buf = wtap_buf_ptr(wth);
          for (i = 0; i < (int) phdr->caplen; i++) {
            if (rand() <= err_prob * RAND_MAX) {
              err_type = rand() / (RAND_MAX / ERR_WT_TOTAL + 1);

              if (err_type < ERR_WT_BIT) {
                buf[i] ^= 1 << (rand() / (RAND_MAX / 8 + 1));
                err_type = ERR_WT_TOTAL;
              } else {
                err_type -= ERR_WT_BYTE;
              }

              if (err_type < ERR_WT_BYTE) {
                buf[i] = rand() / (RAND_MAX / 255 + 1);
                err_type = ERR_WT_TOTAL;
              } else {
                err_type -= ERR_WT_BYTE;
              }

              if (err_type < ERR_WT_ALNUM) {
                buf[i] = ALNUM_CHARS[rand() / (RAND_MAX / ALNUM_LEN + 1)];
                err_type = ERR_WT_TOTAL;
              } else {
                err_type -= ERR_WT_ALNUM;
              }

              if (err_type < ERR_WT_FMT) {
                if ((unsigned int)i < phdr->caplen - 2)
                  strcpy(&buf[i],  "%s");
                err_type = ERR_WT_TOTAL;
              } else {
                err_type -= ERR_WT_FMT;
              }

              if (err_type < ERR_WT_AA) {
                for (j = i; j < (int) phdr->caplen; j++) {
                  buf[j] = 0xAA;
                }
                i = phdr->caplen;
              }
            }
          }
        }

        if (!wtap_dump(pdh, phdr, wtap_pseudoheader(wth), wtap_buf_ptr(wth),
                       &err)) {

          fprintf(stderr, "editcap: Error writing to %s: %s\n",
                  filename, wtap_strerror(err));
          exit(1);

	}

	written_count++;

      }

      count++;

    }

    if (err != 0) {
      /* Print a message noting that the read failed somewhere along the line. */
      fprintf(stderr,
              "editcap: An error occurred while reading \"%s\": %s.\n",
	      argv[optind], wtap_strerror(err));
      switch (err) {

      case WTAP_ERR_UNSUPPORTED:
      case WTAP_ERR_UNSUPPORTED_ENCAP:
      case WTAP_ERR_BAD_RECORD:
	fprintf(stderr, "(%s)\n", err_info);
	break;
      }
    }

    if (!wtap_dump_close(pdh, &err)) {

      fprintf(stderr, "editcap: Error writing to %s: %s\n", filename,
	      wtap_strerror(err));
      exit(1);

    }
  }

  return 0;
}

