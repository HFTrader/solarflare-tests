
#define _GNU_SOURCE 1

#include <etherfabric/vi.h>
#include <etherfabric/pd.h>
#include <etherfabric/memreg.h>
#include <ci/tools.h>
#include <ci/tools/ippacket.h>
#include <ci/net/ipv4.h>

#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <net/if.h>
#include <netdb.h>
#include <math.h>
#include <sys/signal.h>

void hexDump (char *desc, void *addr, int len);

#define CACHE_ALIGN           __attribute__((aligned(EF_VI_DMA_ALIGN)))

static int              cfg_iter = 10000000;
static int              cfg_phys_mode;
static int              cfg_rx_align;

#define EVENTS_PER_POLL 32
#define N_BUFS          256u
#define BUF_SIZE        2048


#define TEST(x)                                                 \
  do {                                                          \
    if (! (x)) {                                                \
      fprintf(stderr, "ERROR: '%s' failed\n", #x);              \
      fprintf(stderr, "ERROR: at %s:%d\n", __FILE__, __LINE__); \
      exit(1);                                                  \
    }                                                           \
  } while (0)

#define TRY(x)                                                  \
  do {                                                          \
    int __rc = (x);                                             \
    if (__rc < 0) {                                             \
      fprintf(stderr, "ERROR: '%s' failed\n", #x);              \
      fprintf(stderr, "ERROR: at %s:%d\n", __FILE__, __LINE__); \
      fprintf(stderr, "ERROR: rc=%d errno=%d (%s)\n",           \
              __rc, errno, strerror(errno));                    \
      exit(1);                                                  \
    }                                                           \
  } while (0)

#define MEMBER_OFFSET(c_type, mbr_name)  \
  ((uint32_t) (uintptr_t)(&((c_type*)0)->mbr_name))

struct pkt_buf {
  struct pkt_buf* next;
  ef_addr         dma_buf_addr;
  int             id;
  uint8_t         dma_buf[1] CACHE_ALIGN;
};


static ef_driver_handle  driver_handle;
static ef_vi		 vi;

struct pkt_buf*          pkt_bufs[N_BUFS];
static ef_pd             pd;
static ef_memreg         memreg;
static unsigned          rx_posted, rx_completed;

struct timeval start, end;

static int remain;

struct error {
  int start;
  int end;
  struct error* next;
};

#define MAX_ERROR_DETAILS 20
static int error_count;

struct error*
note_gap(struct error* error, int start, int end)
{
  if (++error_count < MAX_ERROR_DETAILS) {
    struct error* retval = malloc(sizeof(*error));
    retval->start = start;
    retval->end = end;
    retval->next = error;
    return retval;
  } else {
    return error;
  }
}

void
report_errors_internal(struct error* error)
{
  if (error) {
    report_errors_internal(error->next);
    if (error->next) {
      printf("+%d+ ", error->start - (error->next ? error->next->end : 0));
    }
    printf("-%d- ", error->end - error->start);
  }
}

void
report_errors(struct error* error)
{
  printf("Gaps:\n");
  report_errors_internal(error);
  putchar('\n');
  if (error_count > MAX_ERROR_DETAILS) {
    printf("%d more errors not shown\n", error_count - MAX_ERROR_DETAILS);
  }
}

void
show_status(int sig)
{
  static int prev_remain;
  if (remain == prev_remain) {
    printf("exiting\n");
    exit(0);
  }
  printf("remain: %d\n", remain);
  prev_remain = remain;
}

void show_status_and_exit(int sig)
{
  show_status(sig);
  exit(1);
}

static void rx_loop(void)
{
  int next_expected = 0;
  int in_gap = 0;
  int gap_start;
  struct error* error = 0;
  int n_recv_queued = 0;
  static long empty_polls;
  static long nonempty_polls;
  remain = cfg_iter;

  for (int buf_id = 0; buf_id < N_BUFS; buf_id++) {
    TRY(ef_vi_receive_init(&vi, pkt_bufs[buf_id]->dma_buf_addr, buf_id));
  }
  ef_vi_receive_push(&vi);

  while (1) {
    ef_event evs[EVENTS_PER_POLL];
    int n_ev = ef_eventq_poll(&vi, evs, EVENTS_PER_POLL);

    if (n_ev) {
      nonempty_polls++;
    } else {
      empty_polls++;
    }

    for (int i = 0; i < n_ev; ++i) {
      switch (EF_EVENT_TYPE(evs[i])) {
      case EF_EVENT_TYPE_RX:
        TEST(EF_EVENT_RX_SOP(evs[i]) == 1);
        TEST(EF_EVENT_RX_CONT(evs[i]) == 0);
        {
          int buf_id = EF_EVENT_RX_RQ_ID(evs[i]);
          struct pkt_buf* pb = pkt_bufs[buf_id];
          char* payload = (pb->dma_buf + cfg_rx_align + ETH_HLEN);
          unsigned int seq = *((int*)payload);
          if (seq >= cfg_iter) {
            fprintf(stderr, "received sequence number %d which is out of expected range (max %d) at iteration %d\n", seq, cfg_iter, i);
            hexDump("received", pb->dma_buf, 256);
            exit(1);
          }
          
          if (seq == next_expected) {
            if (in_gap) {
              in_gap = 0;
              error = note_gap(error, gap_start, seq - 1);
            }
          } else {
            if (!in_gap) {
              in_gap = 1;
              gap_start = next_expected;
            }
          }
          next_expected = seq + 1;

          if (remain == cfg_iter) {
            gettimeofday(&start, NULL);
          }
          remain--;
          if (remain <= 0) {
            printf("all packets received\n");
            goto done;
          }
          if (seq == cfg_iter - 1) {
            printf("receiver finished with errors, %d packets lost (%.3f%%)\n", remain, (double) remain / ((double) cfg_iter / 100.));
            report_errors(error);
            goto done;
          }
          TRY(ef_vi_receive_init(&vi, pb->dma_buf_addr, buf_id));
          n_recv_queued++;
        }
        break;
      case EF_EVENT_TYPE_RX_DISCARD:
        fprintf(stderr, "ERROR: RX_DISCARD type=%d, subtype=%d\n",
                EF_EVENT_RX_DISCARD_TYPE(evs[i]), evs[i].rx_discard.subtype);
        break;
      default:
        fprintf(stderr, "ERROR: unexpected event "EF_EVENT_FMT"\n",
                EF_EVENT_PRI_ARG(evs[i]));
        break;
      }
    }
    if (n_recv_queued >= 16) {
      ef_vi_receive_push(&vi);
      n_recv_queued = 0;
    }
  }
 done:
  printf("%.f%% receiver polls returned no data\n",
         (double) empty_polls / ((double) (empty_polls + nonempty_polls) / 100.));
}

/**********************************************************************/

static void recv_test(void)
{
  int i, usec;
  rx_loop();
  gettimeofday(&end, NULL);

  usec = (end.tv_sec - start.tv_sec) * 1000000;
  usec += end.tv_usec - start.tv_usec;
  printf("packet rate: %.1f Mpps\n", (double) cfg_iter / (double) usec);
}

/**********************************************************************/


static void do_init(int ifindex)
{
  enum ef_pd_flags pd_flags = EF_PD_DEFAULT;
  ef_filter_spec filter_spec;
  enum ef_vi_flags vi_flags = 0;
  unsigned char mac[6];

  if (cfg_phys_mode)
    pd_flags |= EF_PD_PHYS_MODE;

  /* Allocate virtual interface. */
  TRY(ef_driver_open(&driver_handle));
  TRY(ef_pd_alloc(&pd, driver_handle, ifindex, pd_flags));
  TRY(ef_vi_alloc_from_pd(&vi, driver_handle, &pd, driver_handle,
                          -1, -1, -1, NULL, -1, vi_flags));

  ef_vi_get_mac(&vi, driver_handle, mac);
#if 0
  printf("# Local MAC address %02x:%02x:%02x:%02x:%02x:%02x, MTU %d\n",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
         ef_vi_mtu(&vi, driver_handle));
#endif

  ef_filter_spec_init(&filter_spec, EF_FILTER_FLAG_NONE);
  TRY(ef_filter_spec_set_eth_local(&filter_spec, EF_FILTER_VLAN_ID_ANY, mac));
  TRY(ef_vi_filter_add(&vi, driver_handle, &filter_spec, NULL));

  {
    int bytes = N_BUFS * BUF_SIZE;
    void* p;
    TEST(posix_memalign(&p, CI_PAGE_SIZE, bytes) == 0);
    TRY(ef_memreg_alloc(&memreg, driver_handle, &pd, driver_handle, p, bytes));
    for (int i = 0; i < N_BUFS; ++i) {
      struct pkt_buf* pb = (void*) ((char*) p + i * BUF_SIZE);
      pb->id = i;
      pb->dma_buf_addr = ef_memreg_dma_addr(&memreg, i * BUF_SIZE);
      pb->dma_buf_addr += MEMBER_OFFSET(struct pkt_buf, dma_buf);
      pkt_bufs[i] = pb;
    }
  }

  for (int i = 0; i < N_BUFS; ++i) {
    pkt_bufs[i]->dma_buf_addr += cfg_rx_align;
  }
}


static int parse_interface(const char* s, int* ifindex_out)
{
  char dummy;
  if ((*ifindex_out = if_nametoindex(s)) == 0)
    if (sscanf(s, "%d%c", ifindex_out, &dummy) != 1)
      return 0;
  return 1;
}

static void usage(void)
{
  fprintf(stderr, "\nusage:\n");
  fprintf(stderr, "  efrecv [options] <interface>\n");
  fprintf(stderr, "  -n <iterations>         - set number of iterations\n");
  fprintf(stderr, "  -p                      - physical address mode\n");
  exit(1);
}


#define CL_CHK(x)                               \
  do{                                           \
    if (! (x))                                 \
      usage();                                  \
  }while(0)


int main(int argc, char* argv[])
{
  int c;

#if 0
  printf("# ef_vi_version_str: %s\n", ef_vi_version_str());
#endif

  while ((c = getopt (argc, argv, "n:pa:")) != -1)
    switch (c) {
    case 'n':
      cfg_iter = atoi(optarg);
      break;
    case 'p':
      cfg_phys_mode = 1;
      break;
    case 'a':
      cfg_rx_align = atoi(optarg);
      break;
    case '?':
      usage();
    default:
      TEST(0);
    }

  argc -= optind;
  argv += optind;

  if (argc != 1)
    usage();

  {
    int ifindex;
    CL_CHK(parse_interface(argv[0], &ifindex));

#if 0
    printf("# iterations: %d\n", cfg_iter);
    printf("# rx align: %d\n", cfg_rx_align);
#endif

    do_init(ifindex);

    signal(SIGINT, show_status);
    signal(SIGTERM, show_status_and_exit);

    recv_test();
  }

  return 0;
}
