#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <locale.h>

#include <rte_memzone.h>
#include <rte_memcpy.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>

#define RTE_LOGTYPE_APP         RTE_LOGTYPE_USER1

#define PKT_SIZE 64
#define MBUF_SIZE (PKT_SIZE + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)

#define PRINT_INTERVAL 1

#define BURST_SIZE 32

#define CALC_RX_STATS

#define RUN_TIME 30

/* function prototypes */
void send_receive_loop(void);
void init(char * dev_name);
void crtl_c_handler(int s);
void ALARMhandler(int sig);
inline int send_packets(struct rte_mbuf ** packets);
void print_stats(void);
void print_final_stats(void);

static uint64_t rx_vec[RUN_TIME];

unsigned int counter = 0;

volatile sig_atomic_t stop;
volatile sig_atomic_t pause_;
void crtl_c_handler(int s);

/* Per-port statistics struct */
struct port_statistics {
	uint32_t tx;
	uint32_t rx;
	uint32_t tx_retries;
	uint32_t alloc_fails;
} __rte_cache_aligned;

//Allocation methods
#define ALLOC 		1	/* allocate and deallocate packets */
#define NO_ALLOC 	2	/* send the same packets always*/

#define ALLOC_METHOD NO_ALLOC

struct rte_mempool * packets_pool = NULL;

struct port_statistics stats;

uint8_t portid;

struct rte_eth_dev_info dev_info;
/* TODO: verify this setup */
static const struct rte_eth_conf port_conf = {
	.rxmode = {
		.split_hdr_size = 0,
		.header_split = 0,
		.hw_ip_checksum = 0,
		.hw_vlan_filter = 0,
		.jumbo_frame = 0,
		.hw_strip_crc = 0,
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};

static uint16_t nb_rxd = 128;
static uint16_t nb_txd = 512;

int main(int argc, char *argv[])
{
	setlocale(LC_NUMERIC, "en_US.utf-8");

	int retval = 0;

	if ((retval = rte_eal_init(argc, argv)) < 0)
		return -1;

	argc -= retval;
	argv +=  retval;

	if(argc < 2)
	{
		RTE_LOG(INFO, APP, "usage: -- port\n");
		return 0;
	}

	init(argv[1]);

	RTE_LOG(INFO, APP, "Finished Process Init.\n");

/* Print information about all the flags! */

	send_receive_loop();	//Forward packets...

	print_final_stats();

	RTE_LOG(INFO, APP, "Done\n");
	return 0;
}

void init(char * dev_name)
{
	int ret;

	/* XXX: is there a better way to get the port id based on the name? */
	portid = atoi(dev_name);

	/* TODO: verify memory pool creation options */
	//packets_pool = rte_pktmbuf_pool_create("packets", 256*1024, 32,
	//	0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	packets_pool = rte_mempool_lookup("ovs_mp_2030_0_262144");
	if(packets_pool == NULL)
	{
		rte_exit(EXIT_FAILURE, "Cannot find memory pool\n");
	}

	rte_eth_dev_info_get(portid, &dev_info);

	ret = rte_eth_dev_configure(portid, 1, 1, &port_conf);
	if(ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot configure device\n");

	ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
								SOCKET_ID_ANY/*rte_eth_dev_socket_id(portid1)*/, NULL);
	if(ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot configure device tx queue\n");

	ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
			rte_eth_dev_socket_id(portid), NULL, packets_pool);
	if(ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot configure device rx queue\n");

	ret = rte_eth_dev_start(portid);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot start device\n");

	rte_eth_promiscuous_enable(portid);
}

/* send packets */
inline int send_packets(struct rte_mbuf ** packets)
{
	int i = 0;
	#ifdef SEND_FULL_BURST
	int ntosend = BURST_SIZE;
	do
	{
		#ifdef CALC_TX_TRIES
		stats.tx_retries++;
		#endif

		i += rte_eth_tx_burst(portid, 0, &packets[i], ntosend - i);
		if(unlikely(stop))
			break;
	} while(unlikely(i < ntosend));
	return BURST_SIZE;
	#else
	int sent = i = rte_eth_tx_burst(portid, 0, &packets[0], BURST_SIZE);
	if (unlikely(i < BURST_SIZE)) {
		do {
			rte_pktmbuf_free(packets[i]);
		} while (++i < BURST_SIZE);
	}
	return sent;
	#endif
}

void send_receive_loop(void)
{
	struct rte_mbuf * packets_array[BURST_SIZE] = {0};

	char pkt[PKT_SIZE] = {0};

	int i;
	int nreceived;
	int retval = 0;
	(void) retval;
	srand(time(NULL));

	//Initializate packet contents
	for(i = 0; i < PKT_SIZE; i++)
		pkt[i] = 0xCC;

/* prealloc packets */
#if ALLOC_METHOD == NO_ALLOC
	int n;
	do
	{
		n = rte_mempool_get_bulk(packets_pool, (void **) packets_array, BURST_SIZE);
	} while(n != 0 && !stop);

	for(i = 0; i < BURST_SIZE; i++)
	{
		rte_memcpy(rte_pktmbuf_mtod(packets_array[i], void *), pkt, PKT_SIZE);
		packets_array[i]->next = NULL;
		packets_array[i]->pkt_len = PKT_SIZE;
		packets_array[i]->data_len = PKT_SIZE;
	}
#endif

	signal (SIGINT,crtl_c_handler);

	signal(SIGALRM, ALARMhandler);
	alarm(PRINT_INTERVAL);

	while(likely(!stop))
	{
		while(pause_);

		/* send packets */
#if ALLOC_METHOD == ALLOC
		int n;
		/* get BURST_SIZE free slots */
		do {
			n = rte_mempool_get_bulk(packets_pool, (void **) packets_array, BURST_SIZE);
			if(unlikely(n != 0))
				stats.alloc_fails++;
		} while(n != 0 && !stop);

		//Copy data to the buffers
		for(i = 0; i < BURST_SIZE; i++)
		{
			/* XXX: is this a valid aprroach? */
			rte_mbuf_refcnt_set(packets_array[i], 1);

			rte_memcpy(rte_pktmbuf_mtod(packets_array[i], void *), pkt, PKT_SIZE);
			packets_array[i]->next = NULL;
			packets_array[i]->pkt_len = PKT_SIZE;
			packets_array[i]->data_len = PKT_SIZE;

	#ifdef CALC_CHECKSUM
				for(kk = 0; kk < 8; kk++) /** XXX: HARDCODED value**/
					checksum += ((uint64_t *)packets_array[i]->buf_addr)[kk];
	#endif
		}
#endif
		stats.tx += send_packets(packets_array);

		/* receive packets */
		nreceived = rte_eth_rx_burst(portid, 0, packets_array, BURST_SIZE);

#ifdef CALC_CHECKSUM
		for(i = 0; i < nreceived; i++)
			for(kk = 0; kk < PKT_LEN/8; kk++)
				checksum += ((uint64_t *)packets_array[i]->buf_addr)[kk];
#endif

#if ALLOC_METHOD == ALLOC
		if(likely(nreceived > 0))
			rte_mempool_mp_put_bulk(packets_pool, (void **) packets_array, nreceived);
#endif

		stats.rx += nreceived;
	}
}

void crtl_c_handler(int s)
{
	(void) s; /* Avoid compile warning */
	printf("Requesting stop.\n");
	stop = 1;
}

void print_final_stats(void)
{
	uint64_t totalrx = 0;
	int i;
	for(i = 5 ; i < RUN_TIME - 5; i++)
		totalrx += rx_vec[i];

	totalrx = totalrx/(RUN_TIME - 10);

	printf("%" PRIu64 "\n", totalrx);
}


void print_stats(void)
{

#ifdef CALC_RX_STATS
	static int i = 0;
	static uint32_t p = 0;
	printf("RX Packets:\t%'" PRIu32 "\n", stats.rx);

	rx_vec[i++] = p;
	p = stats.rx;
	stats.rx = 0;
#endif

#ifdef CALC_TX_STATS
	printf("TX Packets:\t%'" PRIu32 "\n", stats.tx);
	stats.tx = 0;
#endif

#ifdef CALC_TX_TRIES
	printf("TX retries:\t%'" PRIu32 "\n", stats.tx_retries);
	stats.tx_retries = 0;
#endif

#ifdef CALC_ALLOC_STATS
	printf("Alloc fails:\t%'" PRIu32 "\n", stats.alloc_fails);
	stats.alloc_fails = 0;
#endif

	printf("\n");
}

void ALARMhandler(int sig)
{
	(void) sig;

	static unsigned int seconds = 0;

	seconds++;

	if (seconds == RUN_TIME) {
		stop = 1;
		return;
	}

	signal(SIGALRM, SIG_IGN);          /* ignore this signal       */

	if(!pause_)
		print_stats();
	signal(SIGALRM, ALARMhandler);     /* reinstall the handler    */
	alarm(PRINT_INTERVAL);


	switch(counter)
	{
		case 0:
		break;

		case 1:
			pause_ = !pause_;
			if(pause_)
				printf("Pausing...\n");
			else
				printf("Resumming...\n");
		break;

		default:
			stop = 1;
			pause_ = 0;
		break;
	}

	counter = 0;
}
