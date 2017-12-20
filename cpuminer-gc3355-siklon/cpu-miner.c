/*
 * Copyright 2010 Jeff Garzik
 * Copyright 2012-2013 pooler
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "cpuminer-config.h"
#define _GNU_SOURCE

#include <curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#ifdef WIN32
#include <windows.h>
#else
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#if HAVE_SYS_SYSCTL_H
#include <sys/types.h>
#if HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <sys/sysctl.h>
#endif
#endif
#include <jansson.h>
#include <curl/curl.h>
#include "compat.h"
#include "miner.h"
#include "elist.h"

#define PROGRAM_NAME		"minerd"
#define DEF_RPC_URL		"http://127.0.0.1:9332/"

#define MINER_VERSION	"v1.0e"

enum workio_commands {
	WC_SUBMIT_WORK,
};

struct workio_cmd {
	enum workio_commands	cmd;
	struct thr_info		*thr;
	union {
		struct work	*work;
	} u;
};

enum sha256_algos {
	ALGO_SCRYPT,		/* scrypt(1024,1,1) */
	ALGO_SHA256D,		/* SHA-256d */
};

#define GC3355_DEFAULT_CHIPS 5
#define API_DEFAULT_PORT 4028
#define API_QUEUE 16
#define API_GET_STATS "stats"
#define API_START_TIME "start_time"
#define API_STOP_TIME "stop_time"
#define API_DEVICE_SERIAL "serial"
#define API_DEVICES "devices"
#define API_CHIPS "chips"
#define API_LAST_SHARE "last_share"
#define API_ACCEPTED "accepted"
#define API_REJECTED "rejected"
#define API_HW_ERRORS "hw_errors"
#define API_FREQUENCY "frequency"
#define API_HASHRATE "hashrate"
#define API_SHARES "shares"
#define API_AUTOTUNE "autotune"
#define API_POOL "pool"
#define API_POOLS "pools"
#define API_POOL_STATS "stats"
#define API_POOL_STATS_ID "stats_id"
#define API_POOL_URL "url"
#define API_POOL_USER "user"
#define API_POOL_PASS "pass"
#define API_POOL_PRIORITY "priority"
#define API_POOL_ACTIVE "active"
#define REFRESH_INTERVAL 2

struct gc3355_dev {
	int	id;
	int	dev_fd;
	char *serial;
	unsigned char type;
	unsigned char chips;
	bool resend;
	char *devname;
	unsigned short *freq;
	uint32_t *last_nonce;
	unsigned long long *hashes;
	double *time_now;
	double *time_spent;
	unsigned short *total_hwe;
	unsigned short *hwe;
	short *adjust;
	unsigned short *steps;
	unsigned int *autotune_accepted;
	unsigned int *accepted;
	unsigned int *rejected;
	double *hashrate;
	unsigned long long *shares;
	unsigned int *last_share;
	unsigned short errors;
	bool ready;
};

static char *gc3355_devname = NULL;
static unsigned short opt_frequency = 600;
static char *opt_gc3355_frequency = NULL;
static bool opt_gc3355_autotune = false;
static unsigned short opt_gc3355_chips = GC3355_DEFAULT_CHIPS;
static unsigned int opt_gc3355_timeout = 0;
static bool opt_gc3355_detect = false;
static struct gc3355_dev *gc3355_devs;
static struct gc3355_devices *device_list;
static unsigned int gc3355_time_start;
static json_t *opt_config;
char *log_path;
bool opt_refresh = false;
bool opt_log = false;
bool opt_curses = true;
bool opt_debug = false;
bool opt_protocol = false;
bool want_stratum = true;
bool have_stratum = false;
static bool opt_quiet = false;
static int opt_retries = 2;
static int opt_fail_pause = 5;
int opt_timeout = 270;
int opt_scantime = 5;
static json_t *opt_config;
static int opt_n_threads;
struct thr_info *thr_info;
static int work_thr_id;
int longpoll_thr_id = -1;
int stratum_thr_id = -1;
int api_thr_id = -1;
int tui_main_thr_id = -1;
int tui_user_thr_id = -1;
int check_pool_thr_id = -1;
unsigned short opt_api_port = API_DEFAULT_PORT;
int api_sock;
struct work_restart *work_restart = NULL;
static struct stratum_ctx *stratum;

struct display *display;
struct log_buffer *log_buffer = NULL;
time_t time_start;

pthread_mutex_t applog_lock;
pthread_mutex_t stats_lock;
pthread_mutex_t tui_lock;
pthread_mutex_t pool_lock;
pthread_mutex_t check_pool_lock;
pthread_cond_t check_pool_cond;
pthread_mutex_t switch_pool_lock;

#ifdef HAVE_GETOPT_LONG
#include <getopt.h>
#else
struct option {
	const char *name;
	int has_arg;
	int *flag;
	int val;
};
#endif

static char const usage[] = "\
Usage: " PROGRAM_NAME " [OPTIONS]\n\
Options:\n\
  -G, --gc3355=DEV0,DEV1,...,DEVn      					enable GC3355 chip mining mode (default: no)\n\
  -d  --gc3355-detect					      			automatically detect GC3355 miners (default: no)\n\
  -F, --freq=FREQUENCY  								set GC3355 core frequency in NONE dual mode (default: 600)\n\
  -f, --gc3355-freq=DEV0:F0,DEV1:F1,...,DEVn:Fn			individual frequency setting\n\
	  --gc3355-freq=DEV0:F0:CHIP0,...,DEVn:Fn:CHIPn		individual per chip frequency setting\n\
  -A, --gc3355-autotune  								auto overclocking each GC3355 chip (default: no)\n\
  -c, --gc3355-chips=N  								# of GC3355 chips (default: 5)\n\
  -x, --gc3355-timeout=N  								max. time after no share is submitted before restarting GC3355 chips (default: never)\n\
  -w, --no-refresh   									only send new work when a new block is detected\n\
  -a, --api-port=PORT  									set the JSON API port (default: 4028)\n\
  -t, --text											disable curses tui, output text\n\
  -L, --log=PATH										file logging\n\
  -o, --url=URL         								URL of mining server (default: " DEF_RPC_URL ")\n\
  -O, --userpass=U:P    								username:password pair for mining server\n\
  -u, --user=USERNAME   								username for mining server\n\
  -p, --pass=PASSWORD   								password for mining server\n\
  -r, --retries=N       								number of times to retry if a network call fails\n\
														(default: retry indefinitely)\n\
  -R, --retry-pause=N									time to pause between retries, in seconds (default: 30)\n\
  -T, --timeout=N       								network timeout, in seconds (default: 270)\n\
  -q, --quiet           								disable per-thread hashmeter output\n\
  -D, --debug           								enable debug output\n\
  -P, --protocol-dump   								verbose dump of protocol-level activities\n\
  -c, --config=FILE     								load a JSON-format configuration file\n\
  -V, --version         								display version information and exit\n\
  -h, --help            								display this help text and exit\n";

static char const short_options[] = 
	"G:F:f:A:c:a:t:L"
	"PDhp:qr:R:T:o:u:O:V";

static struct option const options[] = {
	{ "gc3355", 1, NULL, 'G' },
	{ "gc3355-detect", 0, NULL, 'd' },
	{ "config", 1, NULL, 'c' },
	{ "pools", 1, NULL, '\0' },
	{ "freq", 1, NULL, 'F' },
	{ "gc3355-freq", 1, NULL, 'f' },
	{ "gc3355-autotune", 0, NULL, 'A' },
	{ "gc3355-chips", 1, NULL, 'n' },
	{ "gc3355-timeout", 1, NULL, 'x' },
	{ "no-refresh", 0, NULL, 'w' },
	{ "api-port", 1, NULL, 'a' },
	{ "text", 0, NULL, 't' },
	{ "log", 1, NULL, 'L' },
	{ "debug", 0, NULL, 'D' },
	{ "pass", 1, NULL, 'p' },
	{ "quiet", 0, NULL, 'q' },
	{ "protocol-dump", 0, NULL, 'P' },
	{ "retries", 1, NULL, 'r' },
	{ "retry-pause", 1, NULL, 'R' },
	{ "timeout", 1, NULL, 'T' },
	{ "url", 1, NULL, 'o' },
	{ "user", 1, NULL, 'u' },
	{ "userpass", 1, NULL, 'O' },
	{ "version", 0, NULL, 'V' },
	{ "help", 0, NULL, 'h' },
	{ 0, 0, 0, 0 }
};

struct work {
	uint32_t data[32];
	uint32_t target[8];
	char job_id[128];
	uint32_t work_id;
	unsigned char xnonce2[8];
	unsigned short thr_id;
};

struct work_items
{
	struct list_head list;
	int thr_id;
	uint32_t nonce;
	uint16_t work_id;
	uint16_t id;
	double diff;
};

static bool can_work = false;

static struct work g_work;
static time_t g_work_time;
static time_t g_work_update_time;
static pthread_mutex_t g_work_lock;

static struct work_items *work_items;
static pthread_mutex_t work_items_lock;

struct pool_stats
{
	struct list_head list;
	unsigned int time_start;
	unsigned int time_stop;
	unsigned int accepted;
	unsigned int rejected;
	unsigned long long shares;
	unsigned int id;
};

struct pool_details
{
	struct list_head list;
	char *rpc_url;
	char *rpc_userpass;
	char *rpc_user, *rpc_pass;
	uint16_t prio;
	bool active;
	bool tried;
	bool usable;
	unsigned int id;
	struct pool_stats stats;
};

static struct pool_details *gpool;
static struct pool_details *pools;
static bool must_switch = false;

static struct pool_details* init_pool_details();
static void add_pool(struct pool_details *pools, struct pool_details *pool);
static void set_active_pool(struct pool_details *pools, struct pool_details *active_pool, bool active);
static struct pool_details* get_active_pool(struct pool_details *pools);
static struct pool_details* get_main_pool(struct pool_details *pools);
static struct pool_details* get_next_pool(struct pool_details *pools);

static struct pool_details* init_pool_details()
{
	struct pool_details *pools = calloc(1, sizeof(struct pool_details));
	INIT_LIST_HEAD(&pools->list);
	INIT_LIST_HEAD(&pools->stats.list);
	return pools;
}

static struct pool_details* new_pool(bool empty)
{
	struct pool_details *pool = calloc(1, sizeof(struct pool_details));
	INIT_LIST_HEAD(&pool->stats.list);
	if(empty)
	{
		pool->rpc_url = DEF_RPC_URL;
		pool->rpc_user = strdup("");
		pool->rpc_pass = strdup("");
	}
	return pool;
}

static void add_pool_url(struct pool_details *pools, struct pool_details *pool, char *str)
{
	if(pool == NULL)
		pool = new_pool(false);
	if(pool != gpool)
		gpool = pool;
	if(pool->rpc_url != NULL)
		free(pool->rpc_url);
	pool->rpc_url = strdup(str);
	if(pool->rpc_url && pool->rpc_user && pool->rpc_pass)
		add_pool(pools, pool);
}

static void add_pool_user(struct pool_details *pools, struct pool_details *pool, char *str)
{
	if(pool == NULL)
		pool = new_pool(false);
	if(pool != gpool)
		gpool = pool;
	if(pool->rpc_user != NULL)
		free(pool->rpc_user);
	pool->rpc_user = strdup(str);
	if(pool->rpc_url && pool->rpc_user && pool->rpc_pass)
		add_pool(pools, pool);
}

static void add_pool_pass(struct pool_details *pools, struct pool_details *pool, char *str)
{
	if(pool == NULL)
		pool = new_pool(false);
	if(pool != gpool)
		gpool = pool;
	if(pool->rpc_pass != NULL)
		free(pool->rpc_pass);
	pool->rpc_pass = strdup(str);
	if(pool->rpc_url && pool->rpc_user && pool->rpc_pass)
		add_pool(pools, pool);
}

static bool check_pool_alive(struct pool_details *pool)
{
	bool alive = true;
	struct stratum_ctx *stratum = calloc(1, sizeof(struct stratum_ctx));
	pthread_mutex_init(&stratum->sock_lock, NULL);
	pthread_mutex_init(&stratum->work_lock, NULL);
	stratum->url = pool->rpc_url;
	if (!stratum_connect(stratum, stratum->url) ||
		!stratum_subscribe(stratum) ||
		!stratum_authorize(stratum, pool->rpc_user, pool->rpc_pass))
	{
		alive = false;
	}
	stratum_disconnect(stratum);
	free(stratum);
	return alive;
}

static void add_pool(struct pool_details *pools, struct pool_details *pool)
{
	pool->rpc_userpass = malloc(strlen(pool->rpc_user) + strlen(pool->rpc_pass) + 2);
	sprintf(pool->rpc_userpass, "%s:%s", pool->rpc_user, pool->rpc_pass);
	pool->usable = true;
	if(!list_empty(&pools->list))
	{
		pool->prio = ++(list_entry(&pools->list.prev, struct pool_details, list))->prio;
	}
	else
	{
		pool->active = true;
		pool->tried = true;
	}
	list_add_tail(&pool->list, &pools->list);
	gpool = NULL;
}

static struct pool_stats* new_pool_stats(struct pool_details *pool)
{
	struct pool_stats *pool_stats;
	pool_stats = calloc(1, sizeof(struct pool_stats));
	pool->id++;
	pool_stats->id = pool->id;
	list_add(&pool_stats->list, &pool->stats.list);
	return pool_stats;
}

static struct pool_stats* get_pool_stats(struct pool_details *pool)
{
	struct pool_stats *pool_stats, *ret = NULL;
	list_for_each_entry(pool_stats, &pool->stats.list, list)
	{
		if(pool->id == pool_stats->id)
		{
			ret = pool_stats;
			break;
		}
	}
	return ret;
}

static int get_pool_count(struct pool_details *pools)
{
	struct pool_details *pool;
	int count = 0;
	pool = list_entry(&pools->list.prev, struct pool_details, list);
	if(pool != NULL)
	{
		count = pool->prio + 1;
	}
	return count;
}

static void set_active_pool(struct pool_details *pools, struct pool_details *active_pool, bool active)
{
	struct pool_details *pool;
	list_for_each_entry(pool, &pools->list, list)
	{
		pool->active = false;
	}
	active_pool->active = active;
	active_pool->tried = true;
}

static struct pool_details* get_pool(struct pool_details *pools, int prio)
{
	struct pool_details *pool, *ret = NULL;
	list_for_each_entry(pool, &pools->list, list)
	{
		if(pool->prio == prio)
		{
			ret = pool;
			break;
		}
	}
	return ret;
}

static struct pool_details* get_active_pool(struct pool_details *pools)
{
	struct pool_details *pool, *ret = NULL;
	list_for_each_entry(pool, &pools->list, list)
	{
		if(pool->usable && pool->active)
		{
			ret = pool;
			break;
		}
	}
	return ret;
}

static struct pool_details* get_main_pool(struct pool_details *pools)
{
	struct pool_details *pool, *ret = NULL;
	pool = get_pool(pools, 0);
	if(pool != NULL && pool->usable)
		ret = pool;
	return ret;
}

static void clear_pool_tried(struct pool_details *pools)
{
	struct pool_details *pool;
	int tried = 0;
	list_for_each_entry(pool, &pools->list, list)
	{
		if(pool->tried)
			tried++;
	}
	if(tried == (list_entry(&pools->list.prev, struct pool_details, list))->prio + 1)
	{
		list_for_each_entry(pool, &pools->list, list)
		{
			pool->tried = false;
		}
	}
}

static struct pool_details* get_next_pool(struct pool_details *pools)
{
	struct pool_details *pool, *ret = NULL;
	clear_pool_tried(pools);
	list_for_each_entry(pool, &pools->list, list)
	{
		if(pool->usable && !pool->tried)
		{
			ret = pool;
			break;
		}
	}
	return ret;
}

static struct work_items* init_work_items()
{
	struct work_items *items = calloc(1, sizeof(struct work_items));
	items->id = 0xf00;
	INIT_LIST_HEAD(&items->list);
	return items;
}

static struct work_items* pop_work_item(struct work_items *items, uint32_t work_id)
{
	struct work_items *item, *tmp, *ret = NULL;
	list_for_each_entry_safe(item, tmp, &items->list, list)
	{
		if(item->work_id == work_id)
		{
			list_del(&item->list);
			ret = item;
			break;
		}
	}
	return ret;
}

static uint16_t push_work_item(struct work_items *items, struct work *work)
{
	struct work_items *item, *prev;
	item = calloc(1, sizeof(struct work_items));
	item->nonce = work->data[19];
	item->thr_id = work->thr_id;
	item->work_id = items->id;
	item->diff = stratum->job.diff;
	prev = pop_work_item(items, item->work_id);
	if(prev != NULL)
		free(prev);
	list_add(&item->list, &items->list);
	items->id = ((items->id + 1) & 0xfff) | 0xf00;
	return item->work_id;
}

static bool submit_work(struct thr_info *thr, const struct work *work_in);

static void stratum_gen_work(struct stratum_ctx *sctx, struct work *work);

/* added for GC3355 chip miner */
#include "gc3355.h"
/* end */

struct window_lines
{
	char ***str;
	int *width;
	int lines;
	int cols;
	int col;
};

struct window_lines* init_window_lines(int lines, int cols)
{
	int i;
	struct window_lines *wl = malloc(sizeof(struct window_lines));
	wl->str = calloc(lines, sizeof(char**));
	for(i = 0; i < lines; i++)
	{
		wl->str[i] = calloc(cols, sizeof(char*));
	}
	wl->width = calloc(cols, sizeof(int));
	wl->lines = lines;
	wl->cols = cols;
	wl->col = 0;
	return wl;
}

void window_lines_addstr(struct window_lines *wl, int line, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if(wl->str[line][wl->col] != NULL)
		free(wl->str[line][wl->col]);
	int len = vasprintf(&wl->str[line][wl->col], fmt, ap);
	if(len < 0)
	{
		wl->str[line][wl->col] = NULL;
		wl->width[wl->col] = 0;
		applog(LOG_ERR, "window_lines_addstr : vasprintf() failed");
	}
	else if(len > wl->width[wl->col]) wl->width[wl->col] = len;
	wl->col = (wl->col + 1) % wl->cols;
	va_end(ap);
}

void window_lines_print(struct window_lines *wl, WINDOW *win)
{
	int i, j, k;
	for(i = 0; i < wl->lines; i++)
	{
		for(j = 0; j < wl->cols; j++)
		{
			if(wl->str[i][j] != NULL)
			{
				int offset = 1;
				for(k = 0; k < j; k++)
				{
					offset += wl->width[k];
				}
				mvwprintw(win, i, offset, "%s", wl->str[i][j]);
			}
			else
			{
				applog(LOG_ERR, "window_lines_print : col (%d,%d) is NULL", i, j);
			}
		}
	}
}

void window_lines_free(struct window_lines *wl)
{
	int i, j;
	for(i = 0; i < wl->lines; i++)
	{
		for(j = 0; j < wl->cols; j++)
		{
			if(wl->str[i][j] != NULL)
				free(wl->str[i][j]);
		}
		free(wl->str[i]);
	}
	free(wl->str);
	free(wl->width);
	free(wl);
}

static void clean_tui()
{
	del_win(display->top);
	del_win(display->summary);
	del_win(display->stats);
	del_win(display->log);
	free(display);
	endwin();
	clear();
}

static void init_tui()
{
	initscr();
	cbreak();
	keypad(stdscr, TRUE);
	noecho();
	curs_set(0);
	refresh();
}

static void start_tui()
{
	int i, log_height, stats_height;
	struct tm tm, *tm_p;
	char *p, *no_pool = "-";
	struct window_lines *wl;
	struct pool_details *pool;
	pthread_mutex_lock(&pool_lock);
	pool = get_active_pool(pools);
	pthread_mutex_unlock(&pool_lock);
	display = calloc(1, sizeof(struct display));
	bool has_scroll = false;
	tm_p = localtime(&time_start);
	memcpy(&tm, tm_p, sizeof(tm));
	display->top = new_win(2, COLS, 0, 0);
	display->summary = new_win(3, COLS, display->top->height, 0);
	stats_height = LINES - display->top->height - display->summary->height - TUI_MIN_LOG;
	if(stats_height >= opt_n_threads) stats_height = opt_n_threads;
	else
	{
		if(stats_height < 1) stats_height = 1;
		has_scroll = true;
	}
	display->stats = new_pad(opt_n_threads, COLS, stats_height, COLS, display->top->height + display->summary->height, 0);
	log_height = LINES - display->top->height - display->summary->height - display->stats->height - TUI_SCROLL;
	if(log_height < 1) log_height = 1;
	display->log = new_win(log_height, COLS - 2, display->top->height + display->summary->height + display->stats->height + TUI_SCROLL, 1);
	wmove(display->log->win, 0, 0);
	idlok(display->log->win, true);
	scrollok(display->log->win, true);
	leaveok(display->log->win, true);
	mvwprintw(display->top->win, 0, 1,  "cpuminer-gc3355 (%s) - Started: [%d-%02d-%02d %02d:%02d:%02d]",
		MINER_VERSION,
		tm.tm_year + 1900,
		tm.tm_mon + 1,
		tm.tm_mday,
		tm.tm_hour,
		tm.tm_min,
		tm.tm_sec
	);
	mvwprintw(display->summary->win, 0, 1, "(5s) | 0/0 MH/s | A:0 R:0 HW:0");
	if(pool != NULL)
	{
		p = strrchr(pool->rpc_url, '/');
		if(p == NULL) p = pool->rpc_url;
		else p++;
	}
	else
	{
		p = no_pool;
	}
	mvwprintw(display->summary->win, 1, 1,  "Connected to %s diff %d with stratum as user %s", p, (int) stratum->job.diff, pool != NULL ? (pool->rpc_user == NULL ? pool->rpc_userpass : pool->rpc_user) : no_pool);
	wl = init_window_lines(opt_n_threads, 4);
	for(i = 0; i < opt_n_threads; i++)
	{
		window_lines_addstr(wl, i, "GSD %d:", i);
		window_lines_addstr(wl, i, " | 0 MHz");
		window_lines_addstr(wl, i, " | 0/0 KH/s");
		window_lines_addstr(wl, i, " | A: 0 R: 0 HW: 0");
	}
	window_lines_print(wl, display->stats->win);
	window_lines_free(wl);
	mvwhline(display->top->win, display->top->height - 1, 0, '=', COLS);
	mvwhline(display->summary->win, display->summary->height - 1, 0, '=', COLS);
	mvhline(display->top->height + display->summary->height + display->stats->height + 1, 0, '=', COLS);
	if(has_scroll)
		mvprintw(display->top->height + display->summary->height + display->stats->height, 1, "Scroll with UP and DOWN keys");
	wrefresh(display->top->win);
	wrefresh(display->summary->win);
	prefresh(display->stats->win, 0, 0, display->stats->y, 0, display->stats->height + display->stats->y - 1, COLS);
	wrefresh(display->log->win);
	refresh();
}

static void resize_tui()
{
	pthread_mutex_lock(&tui_lock);
	clean_tui();
	refresh();
	start_tui();
	pthread_mutex_unlock(&tui_lock);
}

static void *tui_user_thread(void *userdata)
{
	int ch;
	while(opt_curses && (ch = getch()))
	{
		switch(ch)
		{
			case KEY_DOWN:
				pthread_mutex_lock(&tui_lock);
				if(display->stats->py < opt_n_threads - display->stats->height)
				prefresh(display->stats->win, ++display->stats->py, 0, display->stats->y, 0, display->stats->height + display->stats->y - 1, COLS);
				pthread_mutex_unlock(&tui_lock);
				break;
			case KEY_UP:
				pthread_mutex_lock(&tui_lock);
				if(display->stats->py > 0)
				prefresh(display->stats->win, --display->stats->py, 0, display->stats->y, 0, display->stats->height + display->stats->y - 1, COLS);
				pthread_mutex_unlock(&tui_lock);
				break;
			default:
				break;
		}
	}
	return NULL;
}

static void *tui_main_thread(void *userdata)
{
	char *p, *no_pool = "-";
	int i, j;
	struct timeval timestr;
	double hashrate, pool_hashrate, thread_hashrate, thread_pool_hashrate, pool_hashrate_width, hashrate_width;
	unsigned int accepted, rejected, hwe, thread_accepted, thread_rejected, thread_hwe, thread_freq, accepted_width, rejected_width, hwe_width, id_width, serial_width;
	struct window_lines *wl;
	struct pool_details *pool;
	while(opt_curses)
	{
		pthread_mutex_lock(&stats_lock);
		gettimeofday(&timestr, NULL);
		accepted_width = rejected_width = hwe_width = pool_hashrate_width = hashrate_width = serial_width = 0;
		for(i = 0; i < opt_n_threads; i++)
		{
			thread_hashrate = thread_pool_hashrate = thread_accepted = thread_rejected = thread_hwe = 0;
			if(gc3355_devs[i].ready)
			{
				for(j = 0; j < gc3355_devs[i].chips; j++)
				{
					thread_hashrate += gc3355_devs[i].hashrate[j];
					thread_pool_hashrate += gc3355_devs[i].shares[j];
					thread_accepted += gc3355_devs[i].accepted[j];
					thread_rejected += gc3355_devs[i].rejected[j];
					thread_hwe += gc3355_devs[i].total_hwe[j];
				}
				thread_pool_hashrate = (1 << 16) / ((timestr.tv_sec - gc3355_time_start) / thread_pool_hashrate);
				if(thread_accepted > accepted_width) accepted_width = thread_accepted;
				if(thread_rejected > rejected_width) rejected_width = thread_rejected;
				if(thread_hwe > hwe_width) hwe_width = thread_hwe;
				if(thread_hashrate > hashrate_width) hashrate_width = thread_hashrate;
				if(thread_pool_hashrate > pool_hashrate_width) pool_hashrate_width = thread_pool_hashrate;
				if(gc3355_devs[i].serial != NULL)
				{
					int tmp = snprintf(NULL, 0, "%s", gc3355_devs[i].serial);
					if(tmp > serial_width)
						serial_width = tmp;
				}
			}
		}
		accepted_width = snprintf(NULL, 0, "%d", accepted_width);
		rejected_width = snprintf(NULL, 0, "%d", rejected_width);
		hwe_width = snprintf(NULL, 0, "%d", hwe_width);
		hashrate_width = snprintf(NULL, 0, "%.1lf", hashrate_width / 1000);
		pool_hashrate_width = snprintf(NULL, 0, "%.1lf", pool_hashrate_width / 1000);
		id_width = snprintf(NULL, 0, "%d", opt_n_threads);
		hashrate = pool_hashrate = accepted = rejected = hwe = 0;
		wl = init_window_lines(opt_n_threads, 7);
		for(i = 0; i < opt_n_threads; i++)
		{
			thread_hashrate = thread_pool_hashrate = thread_accepted = thread_rejected = thread_hwe = thread_freq = 0;
			if(gc3355_devs[i].ready)
			{
				for(j = 0; j < gc3355_devs[i].chips; j++)
				{
					thread_hashrate += gc3355_devs[i].hashrate[j];
					thread_pool_hashrate += gc3355_devs[i].shares[j];
					thread_accepted += gc3355_devs[i].accepted[j];
					thread_rejected += gc3355_devs[i].rejected[j];
					thread_hwe += gc3355_devs[i].total_hwe[j];
					thread_freq += gc3355_devs[i].freq[j];
				}
				thread_freq /= gc3355_devs[i].chips;
				pool_hashrate += thread_pool_hashrate;
				thread_pool_hashrate = (1 << 16) / ((timestr.tv_sec - gc3355_time_start) / thread_pool_hashrate);
				hashrate += thread_hashrate;
				accepted += thread_accepted;
				rejected += thread_rejected;
				hwe += thread_hwe;
			}
			window_lines_addstr(wl, i, "GSD %*d:", id_width, i);
			if(gc3355_devs[i].serial != NULL)
				window_lines_addstr(wl, i, " %*s", serial_width, gc3355_devs[i].serial);
			else
				window_lines_addstr(wl, i, "");
			window_lines_addstr(wl, i, " | %d MHz", thread_freq);
			window_lines_addstr(wl, i, " | %*.1lf/%*.1lf KH/s", (int) pool_hashrate_width, thread_pool_hashrate / 1000, (int) hashrate_width, thread_hashrate / 1000);
			window_lines_addstr(wl, i, " | A: %*d", accepted_width, thread_accepted);
			window_lines_addstr(wl, i, " R: %*d", rejected_width, thread_rejected);
			window_lines_addstr(wl, i, " H: %*d", hwe_width, thread_hwe);
		}
		pool_hashrate = (1 << 16) / ((timestr.tv_sec - gc3355_time_start) / pool_hashrate);
		pthread_mutex_unlock(&stats_lock);
		pthread_mutex_lock(&tui_lock);
		werase(display->stats->win);
		werase(display->summary->win);
		window_lines_print(wl, display->stats->win);
		window_lines_free(wl);
		wl = init_window_lines(1, 1);
		window_lines_addstr(wl, 0, "(%ds) | %.2lf/%.2lf MH/s | A: %d R: %d HW: %d", REFRESH_INTERVAL, pool_hashrate / 1000000, hashrate / 1000000, accepted, rejected, hwe);
		window_lines_print(wl, display->summary->win);
		window_lines_free(wl);
		pthread_mutex_lock(&pool_lock);
		pool = get_active_pool(pools);
		pthread_mutex_unlock(&pool_lock);
		if(pool != NULL)
		{
			p = strrchr(pool->rpc_url, '/');
			if(p == NULL) p = pool->rpc_url;
			else p++;
		}
		else
		{
			p = no_pool;
		}
		mvwprintw(display->summary->win, 1, 1, "Connected to %s diff %d with stratum as user %s", p, (int) stratum->job.diff, pool != NULL ? (pool->rpc_user == NULL ? pool->rpc_userpass : pool->rpc_user) : no_pool);
		mvwhline(display->summary->win, display->summary->height - 1, 0, '=', COLS);
		wrefresh(display->summary->win);
		prefresh(display->stats->win, display->stats->py, 0, display->stats->y, 0, display->stats->height + display->stats->y - 1, COLS);
		pthread_mutex_unlock(&tui_lock);
		sleep(REFRESH_INTERVAL);
	}
	return NULL;
}

static bool jobj_binary(const json_t *obj, const char *key,
			void *buf, size_t buflen)
{
	const char *hexstr;
	json_t *tmp;

	tmp = json_object_get(obj, key);
	if (unlikely(!tmp)) {
		applog(LOG_ERR, "JSON key '%s' not found", key);
		return false;
	}
	hexstr = json_string_value(tmp);
	if (unlikely(!hexstr)) {
		applog(LOG_ERR, "JSON key '%s' is not a string", key);
		return false;
	}
	if (!hex2bin(buf, hexstr, buflen))
		return false;

	return true;
}

static bool work_decode(const json_t *val, struct work *work)
{
	int i;
	
	if (unlikely(!jobj_binary(val, "data", work->data, sizeof(work->data)))) {
		applog(LOG_ERR, "JSON inval data");
		goto err_out;
	}
	if (unlikely(!jobj_binary(val, "target", work->target, sizeof(work->target)))) {
		applog(LOG_ERR, "JSON inval target");
		goto err_out;
	}

	for (i = 0; i < ARRAY_SIZE(work->data); i++)
		work->data[i] = le32dec(work->data + i);
	for (i = 0; i < ARRAY_SIZE(work->target); i++)
		work->target[i] = le32dec(work->target + i);

	return true;

err_out:
	return false;
}

static void share_result(int result, const char *reason, uint16_t work_id)
{
	int chip_id, thr_id;
	struct timeval timestr;
	struct work_items *work_item;
	struct pool_details *pool;
	struct pool_stats *pool_stats;
	pthread_mutex_lock(&work_items_lock);
	work_item = pop_work_item(work_items, work_id);
	pthread_mutex_unlock(&work_items_lock);
	pthread_mutex_lock(&pool_lock);
	pool = get_active_pool(pools);
	pool_stats = get_pool_stats(pool);
	pthread_mutex_unlock(&pool_lock);
	if(work_item == NULL)
	{
		applog(LOG_ERR, "Invalid work_id: %x", work_id);
		return;
	}
	thr_id = work_item->thr_id;
	chip_id = work_item->nonce / (0xffffffff / gc3355_devs[thr_id].chips);
	pthread_mutex_lock(&stats_lock);
	if(result)
	{
		gc3355_devs[thr_id].accepted[chip_id]++;
		if(opt_gc3355_autotune && gc3355_devs[thr_id].type == 1 && gc3355_devs[thr_id].adjust[chip_id] > 0)
		{
			gc3355_devs[thr_id].autotune_accepted[chip_id]++;
		}
		pool_stats->accepted++;
	}
	else
	{
		gc3355_devs[thr_id].rejected[chip_id]++;
		pool_stats->rejected++;
	}
	gettimeofday(&timestr, NULL);
	gc3355_devs[thr_id].last_share[chip_id] = timestr.tv_sec;
	gc3355_devs[thr_id].shares[chip_id] += (int) work_item->diff;
	pool_stats->shares += (int) work_item->diff;
	pthread_mutex_unlock(&stats_lock);
	applog(LOG_INFO, "%s %08x GSD %d@%d",
	   result ? "Accepted" : "Rejected",
	   work_item->nonce,
	   thr_id, chip_id
	);
	if (reason)
		applog(LOG_INFO, "DEBUG: reject reason: %s", reason);
	free(work_item);
}

static void restart_threads(void)
{
	int i;
	for (i = 0; i < opt_n_threads; i++)
		work_restart[i].restart = 1;
}

static bool submit_upstream_work(CURL *curl, struct work *work)
{
	char s[345];
	uint16_t id;
	bool rc = false;

	if (have_stratum) {
		uint32_t ntime, nonce;
		char *ntimestr, *noncestr, *xnonce2str;
		struct pool_details *pool;
		
		pthread_mutex_lock(&pool_lock);
		pool = get_active_pool(pools);
		pthread_mutex_unlock(&pool_lock);

		if (!work->job_id)
			return true;
		le32enc(&ntime, work->data[17]);
		le32enc(&nonce, work->data[19]);
		ntimestr = bin2hex((const unsigned char *)(&ntime), 4);
		noncestr = bin2hex((const unsigned char *)(&nonce), 4);
		xnonce2str = bin2hex(work->xnonce2, stratum->xnonce2_size);
		pthread_mutex_lock(&work_items_lock);
		id = push_work_item(work_items, work);
		pthread_mutex_unlock(&work_items_lock);
		sprintf(s,
			"{\"method\": \"mining.submit\", \"params\": [\"%s\", \"%s\", \"%s\", \"%s\", \"%s\"], \"id\":%d}",
			pool->rpc_user, work->job_id, xnonce2str, ntimestr, noncestr, id);
		free(ntimestr);
		free(noncestr);
		free(xnonce2str);
		
		if (unlikely(!stratum_send_line(stratum, s))) {
			applog(LOG_ERR, "submit_upstream_work stratum_send_line failed");
			can_work = false;
			goto out;
		}
		can_work = true;
	}

	rc = true;

out:
	return rc;
}

static const char *rpc_req =
	"{\"method\": \"getwork\", \"params\": [], \"id\":0}\r\n";

static bool get_upstream_work(CURL *curl, struct work *work)
{
	json_t *val;
	bool rc;
	struct timeval tv_start, tv_end, diff;
	struct pool_details *pool;
	
	pthread_mutex_lock(&pool_lock);
	pool = get_active_pool(pools);
	pthread_mutex_unlock(&pool_lock);

	gettimeofday(&tv_start, NULL);
	val = json_rpc_call(curl, pool->rpc_url, pool->rpc_userpass, rpc_req,
			    true, false, NULL);
	gettimeofday(&tv_end, NULL);

	if (have_stratum) {
		if (val)
			json_decref(val);
		return true;
	}

	if (!val)
		return false;

	rc = work_decode(json_object_get(val, "result"), work);

	if (opt_debug && rc) {
		timeval_subtract(&diff, &tv_end, &tv_start);
		applog(LOG_DEBUG, "DEBUG: got new work in %d ms",
		       diff.tv_sec * 1000 + diff.tv_usec / 1000);
	}

	json_decref(val);

	return rc;
}

static void workio_cmd_free(struct workio_cmd *wc)
{
	if (!wc)
		return;

	switch (wc->cmd) {
	case WC_SUBMIT_WORK:
		free(wc->u.work);
		break;
	default: /* do nothing */
		break;
	}

	memset(wc, 0, sizeof(*wc));	/* poison */
	free(wc);
}

static bool workio_submit_work(struct workio_cmd *wc, CURL *curl)
{
	int failures = 0;

	/* submit solution to bitcoin via JSON-RPC */
	while (!submit_upstream_work(curl, wc->u.work)) {
		if (unlikely((opt_retries >= 0) && (++failures > opt_retries))) {
			must_switch = true;
			return true;
		}

		/* pause, then restart work-request loop */
		applog(LOG_ERR, "...retry after %d seconds",
			opt_fail_pause);
		sleep(opt_fail_pause);
	}

	return true;
}

static void *workio_thread(void *userdata)
{
	struct thr_info *mythr = userdata;
	CURL *curl;
	bool ok = true;

	curl = curl_easy_init();
	if (unlikely(!curl)) {
		applog(LOG_ERR, "CURL initialization failed");
		return NULL;
	}

	while (ok) {
		struct workio_cmd *wc;
		
		/* wait for workio_cmd sent to us, on our queue */
		wc = tq_pop(mythr->q, NULL);
		if (!wc) {
			ok = false;
			break;
		}

		/* process workio_cmd */
		switch (wc->cmd) {
		case WC_SUBMIT_WORK:
			ok = workio_submit_work(wc, curl);
			break;
		default:		/* should never happen */
			ok = false;
			break;
		}

		workio_cmd_free(wc);
	}

	tq_freeze(mythr->q);
	curl_easy_cleanup(curl);

	return NULL;
}

static bool submit_work(struct thr_info *thr, const struct work *work_in)
{
	struct workio_cmd *wc;
	
	/* fill out work request message */
	wc = calloc(1, sizeof(*wc));
	if (!wc)
		return false;

	wc->u.work = malloc(sizeof(*work_in));
	if (!wc->u.work)
		goto err_out;

	wc->cmd = WC_SUBMIT_WORK;
	wc->thr = thr;
	memcpy(wc->u.work, work_in, sizeof(*work_in));

	/* send solution to workio thread */
	if (!tq_push(thr_info[work_thr_id].q, wc))
		goto err_out;

	return true;

err_out:
	workio_cmd_free(wc);
	return false;
}

static void stratum_gen_work(struct stratum_ctx *sctx, struct work *work)
{
	unsigned char merkle_root[64];
	int i;
	unsigned char xnonce2s[sctx->xnonce2_size];
	unsigned char *coinbase;
	bool xclear = true;

	strcpy(work->job_id, g_work.job_id);
	
	uint32_t xbase = (16 << (8 * sctx->xnonce2_size - 4)) - 1;
	uint32_t xnonce2 = 0;
	for(i = 0; i < sctx->xnonce2_size; i++)
	{
		if(work->xnonce2[i])
		{
			xclear = false;
			break;
		}
	}
	if(xclear)
		xnonce2 = xbase / (work->thr_id + 2);
	else
	{
		for(i = 0; i < sctx->xnonce2_size; i++)
			xnonce2 |= work->xnonce2[i] << ((sctx->xnonce2_size - 1 - i) * 8);
		if(xnonce2 < (xbase / (work->thr_id + 1)) - 1)
			xnonce2++;
		else
			xnonce2 = xbase / (work->thr_id + 2);
	}
	for(i = 0; i < sctx->xnonce2_size; i++)
		xnonce2s[i] = xnonce2 >> ((sctx->xnonce2_size - 1 - i) * 8);
	coinbase = malloc(sctx->job.coinbase_size);
	memcpy(coinbase, sctx->job.coinbase, sctx->job.coinbase_size);
	memcpy(coinbase + (sctx->job.xnonce2 - sctx->job.coinbase), xnonce2s, sctx->xnonce2_size);
	memcpy(work->xnonce2, xnonce2s, sctx->xnonce2_size);
	
	/* Generate merkle root */
	sha256d(merkle_root, coinbase, sctx->job.coinbase_size);
	for (i = 0; i < sctx->job.merkle_count; i++)
	{
		memcpy(merkle_root + 32, sctx->job.merkle[i], 32);
		sha256d(merkle_root, merkle_root, 64);
	}

	/* Assemble block header */
	memset(work->data, 0, 128);
	work->data[0] = le32dec(sctx->job.version);
	for (i = 0; i < 8; i++)
		work->data[1 + i] = le32dec((uint32_t *)sctx->job.prevhash + i);
	for (i = 0; i < 8; i++)
		work->data[9 + i] = be32dec((uint32_t *)merkle_root + i);
	work->data[17] = le32dec(sctx->job.ntime);
	work->data[18] = le32dec(sctx->job.nbits);
	work->data[20] = 0x80000000;
	work->data[31] = 0x00000280;
	
	for(i = 0; i < 8; i++)
		work->target[i] = g_work.target[i];
	work->work_id = g_work.work_id;
	
	free(coinbase);
}

static bool stratum_handle_response(char *buf)
{
	json_t *val, *err_val, *res_val, *id_val;
	json_error_t err;
	bool ret = false;

	val = JSON_LOADS(buf, &err);
	if (!val) {
		applog(LOG_INFO, "JSON decode failed(%d): %s", err.line, err.text);
		goto out;
	}

	res_val = json_object_get(val, "result");
	err_val = json_object_get(val, "error");
	id_val = json_object_get(val, "id");
	
	if (!id_val || json_is_null(id_val))
	{
		applog(LOG_INFO, "Unrecognized JSON response: %s", buf);
		goto out;
	}

	uint32_t res_id = (uint32_t) json_integer_value(id_val);
	if(res_id)
	{
		share_result(json_is_true(res_val),
			err_val ? json_string_value(json_array_get(err_val, 1)) : NULL, res_id);
	}

	ret = true;
out:
	if (val)
		json_decref(val);

	return ret;
}

static void clean_stratum(struct stratum_ctx *sctx)
{
	if(sctx->curl)
		stratum_disconnect(sctx);
	memset(sctx, 0, sizeof(struct stratum_ctx));
	pthread_mutex_init(&sctx->sock_lock, NULL);
	pthread_mutex_init(&sctx->work_lock, NULL);
}

static void *stratum_thread(void *userdata)
{
	char *s;
	int i, failures, restarted;
	struct timeval timestr;
	struct pool_details *pool, *main_pool;
	struct pool_stats *pool_stats;
	bool switch_lock = false, switched = false, reconnect = false;
	uint32_t work_id;

	gettimeofday(&timestr, NULL);
	work_id = (timestr.tv_sec & 0xffff) << 16 | (timestr.tv_usec & 0xffff);
	g_work_time = 0;
	g_work_update_time = 0;
	
	while (1) {

login:
		switch_lock = true;
		pthread_mutex_lock(&switch_pool_lock);
		failures = 0;
		if(must_switch || switched)
		{
			must_switch = false;
			switched = false;
			pthread_mutex_lock(&g_work_lock);
			restart_threads();
			can_work = false;
			clean_stratum(stratum);
			g_work_time = 0;
			g_work_update_time = 0;
			pthread_mutex_unlock(&g_work_lock);
		}
		while (!stratum->curl)
		{
			pthread_mutex_lock(&pool_lock);
			pool = get_active_pool(pools);
			pthread_mutex_unlock(&pool_lock);
			if(pool == NULL)
			{
				applog(LOG_ERR, "Stratum pool info incomplete");
				goto out;
			}
			stratum->url = pool->rpc_url;
			if(!reconnect)
				applog(LOG_INFO, "Starting Stratum on %s", stratum->url);
			reconnect = false;
			if (!stratum_connect(stratum, stratum->url) ||
			    !stratum_subscribe(stratum) ||
			    !stratum_authorize(stratum, pool->rpc_user, pool->rpc_pass)) {
				stratum_disconnect(stratum);
				reconnect = true;
				if (opt_retries >= 0 && ++failures > opt_retries)
				{
					failures = 0;
					pthread_mutex_lock(&pool_lock);
					pool = get_next_pool(pools);
					set_active_pool(pools, pool, true);
					main_pool = get_main_pool(pools);
					pthread_mutex_unlock(&pool_lock);
					if(pool != main_pool)
					{
						pthread_cond_signal(&check_pool_cond);
					}
					if(switch_lock)
					{
						switch_lock = false;
						pthread_mutex_unlock(&switch_pool_lock);
					}
					applog(LOG_INFO, "Switching to pool: %s", pool->rpc_url);
					switched = true;
					goto login;
				}
				applog(LOG_ERR, "...retry after %d seconds", opt_fail_pause);
				sleep(opt_fail_pause);
			}
			memset(g_work.xnonce2, 0, 8);
			if(g_work_update_time)
				g_work_update_time = 0;
			gettimeofday(&timestr, NULL);
			pthread_mutex_lock(&pool_lock);
			pool_stats = get_pool_stats(pool);
			if(pool_stats != NULL)
			{
				pool_stats->time_stop = timestr.tv_sec;
				if(pool_stats->shares)
					pool_stats = new_pool_stats(pool);
			}
			else
				pool_stats = new_pool_stats(pool);
			pool_stats->time_start = timestr.tv_sec;
			pthread_mutex_unlock(&pool_lock);
		}
		can_work = true;
		restarted = 0;
		if (stratum->job.job_id &&
		    (strcmp(stratum->job.job_id, g_work.job_id) || !g_work_time || !g_work_update_time)) {
			pthread_mutex_lock(&g_work_lock);
			pthread_mutex_lock(&stratum->work_lock);
			if (stratum->job.clean || time(NULL) >= g_work_update_time + 60)
			{
				restart_threads();
				if(stratum->job.clean)
					applog(LOG_INFO, "Stratum detected new block");
				gettimeofday(&timestr, NULL);
				work_id = (timestr.tv_sec & 0xffff) << 16 | (timestr.tv_usec & 0xffff);
				restarted = 1;
				time(&g_work_update_time);
			}
			applog(LOG_INFO, "New Job_id: %s Diff: %d Work_id: %08x", stratum->job.job_id, (int) (stratum->job.diff), work_id);
			strcpy(g_work.job_id, stratum->job.job_id);
			diff_to_target(g_work.target, stratum->job.diff / 65536.0);
			g_work.work_id = work_id;
			time(&g_work_time);
			pthread_mutex_unlock(&stratum->work_lock);
			pthread_mutex_unlock(&g_work_lock);
		}
		
		if (!stratum_socket_full(stratum, 60)) {
			applog(LOG_ERR, "Stratum connection timed out");
			s = NULL;
		} else
			s = stratum_recv_line(stratum);
		if (!s) {
			stratum_disconnect(stratum);
			applog(LOG_ERR, "Stratum connection interrupted");
			if(switch_lock)
			{
				switch_lock = false;
				pthread_mutex_unlock(&switch_pool_lock);
			}
			continue;
		}
		if (!stratum_handle_method(stratum, s))
			stratum_handle_response(s);
		else if(!restarted)
		{
			if(stratum->job.diff != stratum->next_diff && stratum->next_diff > 0)
			{
				pthread_mutex_lock(&g_work_lock);
				pthread_mutex_lock(&stratum->work_lock);
				restart_threads();
				applog(LOG_INFO, "Stratum difficulty changed");
				gettimeofday(&timestr, NULL);
				work_id = (timestr.tv_sec & 0xffff) << 16 | (timestr.tv_usec & 0xffff);
				stratum->job.diff = stratum->next_diff;
				applog(LOG_INFO, "Diff: %d Work_id: %08x", (int) (stratum->job.diff), work_id);
				diff_to_target(g_work.target, stratum->job.diff / 65536.0);
				g_work.work_id = work_id;
				time(&g_work_update_time);
				time(&g_work_time);
				pthread_mutex_unlock(&stratum->work_lock);
				pthread_mutex_unlock(&g_work_lock);
			}
		}
		free(s);
		if(switch_lock)
		{
			switch_lock = false;
			pthread_mutex_unlock(&switch_pool_lock);
			usleep(1000);
		}
	}

out:
	return NULL;
}

static void *switch_pool_handler(void *id)
{
	pthread_detach(pthread_self());
	struct pool_details *pool;
	int pool_id = *(int*)id;
	free(id);
	pthread_mutex_lock(&switch_pool_lock);
	pthread_mutex_lock(&pool_lock);
	pool = get_pool(pools, pool_id);
	if(pool != NULL)
	{
		applog(LOG_DEBUG, "API: Switching to pool %d", pool_id);
		clear_pool_tried(pools);
		set_active_pool(pools, pool, true);
		must_switch = true;
	}
	pthread_mutex_unlock(&pool_lock);
	pthread_mutex_unlock(&switch_pool_lock);
	return NULL;
}

static void *check_pool_thread()
{
	static struct pool_details *main_pool;
	static struct pool_details *active_pool;
	while(1)
	{
		pthread_mutex_lock(&pool_lock);
		main_pool = get_main_pool(pools);
		active_pool = get_active_pool(pools);
		pthread_mutex_unlock(&pool_lock);
		if(active_pool != main_pool)
		{
			applog(LOG_INFO, "Checking main pool: %s", main_pool->rpc_url);
			if(check_pool_alive(main_pool))
			{
				pthread_mutex_lock(&switch_pool_lock);
				applog(LOG_INFO, "Main pool is alive, attempting to switch");
				pthread_mutex_lock(&pool_lock);
				clear_pool_tried(pools);
				set_active_pool(pools, main_pool, true);
				pthread_mutex_unlock(&pool_lock);
				must_switch = true;
				pthread_mutex_unlock(&switch_pool_lock);
				goto wait;
			}
		}
		else
		{
wait:
			pthread_mutex_lock(&check_pool_lock);
			pthread_cond_wait(&check_pool_cond, &check_pool_lock);
			pthread_mutex_unlock(&check_pool_lock);
		}
		sleep(60);
	}
	return NULL;
}

#ifndef WIN32
static bool api_parse_get(const char *api_get, json_t *obj, json_t *err)
{
	json_t *dev, *devs, *chips, *chip, *jpools, *jpool, *jpool_stats, *stats;
	int i, j;
	struct pool_details *pool;
	struct pool_stats *pool_stats;
	if(!strcmp(api_get, API_GET_STATS))
	{
		jpools = json_array();
		pthread_mutex_lock(&pool_lock);
		list_for_each_entry(pool, &pools->list, list)
		{
			jpool = json_object();
			json_object_set_new(jpool, API_POOL_URL, json_string(pool->rpc_url));
			json_object_set_new(jpool, API_POOL_USER, json_string(pool->rpc_user));
			json_object_set_new(jpool, API_POOL_PASS, json_string(pool->rpc_pass));
			json_object_set_new(jpool, API_POOL_PRIORITY, json_integer(pool->prio));
			json_object_set_new(jpool, API_POOL_ACTIVE, json_integer(pool->active ? 1 : 0));
			jpool_stats = json_array();
			list_for_each_entry(pool_stats, &pool->stats.list, list)
			{
				stats = json_object();
				json_object_set_new(stats, API_START_TIME, json_integer(pool_stats->time_start));
				json_object_set_new(stats, API_STOP_TIME, json_integer(pool_stats->time_stop));
				json_object_set_new(stats, API_ACCEPTED, json_integer(pool_stats->accepted));
				json_object_set_new(stats, API_REJECTED, json_integer(pool_stats->rejected));
				json_object_set_new(stats, API_SHARES, json_integer(pool_stats->shares));
				json_object_set_new(stats, API_POOL_STATS_ID, json_integer(pool_stats->id));
				json_array_append_new(jpool_stats, stats);
			}
			json_object_set_new(jpool, API_POOL_STATS, jpool_stats);
			json_object_set_new(jpool, API_POOL_STATS_ID, json_integer(pool->id));
			json_array_append_new(jpools, jpool);
		}
		json_object_set_new(obj, API_POOLS, jpools);
		pthread_mutex_unlock(&pool_lock);
		json_object_set_new(obj, API_START_TIME, json_integer(gc3355_time_start));
		devs = json_object();
		pthread_mutex_lock(&stats_lock);
		for(i = 0; i < opt_n_threads; i++)
		{
			dev = json_object();
			chips = json_array();
			for(j = 0; j < gc3355_devs[i].chips; j++)
			{
				chip = json_object();
				json_object_set_new(chip, API_ACCEPTED, json_integer(gc3355_devs[i].accepted[j]));
				json_object_set_new(chip, API_REJECTED, json_integer(gc3355_devs[i].rejected[j]));
				json_object_set_new(chip, API_HW_ERRORS, json_integer(gc3355_devs[i].total_hwe[j]));
				json_object_set_new(chip, API_FREQUENCY, json_integer(gc3355_devs[i].freq[j]));
				json_object_set_new(chip, API_HASHRATE, json_integer(gc3355_devs[i].hashrate[j]));
				json_object_set_new(chip, API_SHARES, json_integer(gc3355_devs[i].shares[j]));
				json_object_set_new(chip, API_LAST_SHARE, json_integer(gc3355_devs[i].last_share[j]));
				json_object_set_new(chip, API_AUTOTUNE, json_integer(opt_gc3355_autotune ? (gc3355_devs[i].adjust[j] > 0 ? 1 : -1) : 0));
				json_array_append_new(chips, chip);
			}
			json_object_set_new(dev, API_CHIPS, chips);
			if(gc3355_devs[i].serial != NULL)
				json_object_set_new(dev, API_DEVICE_SERIAL, json_string(gc3355_devs[i].serial));
			char *path = gc3355_devs[i].devname;
			char *base = strrchr(path, '/');
			json_object_set_new(devs, base ? base + 1 : path, dev);
		}
		pthread_mutex_unlock(&stats_lock);
		json_object_set_new(obj, API_DEVICES, devs);
		return true;
	}
	return false;
}
#endif

#ifndef WIN32
static bool api_parse_set(const char *api_set, json_t *req, json_t *obj, json_t *err)
{
	json_t *dev, *devs, *chips, *chip, *jpool;
	const char *devname;
	char *path, *base;
	int i, j, pool_id, pool_count, *id;
	unsigned short freq;
	void *iter;
	struct pool_details *pool;
	pthread_t switch_pool_thread;
	if(!strcmp(api_set, API_FREQUENCY))
	{
		devs = json_object_get(req, API_DEVICES);
		if (!devs || !json_is_object(devs))
		{
			return false;
		}
		iter = json_object_iter(devs);
		while(iter)
		{
			devname = json_object_iter_key(iter);
			dev = json_object_iter_value(iter);
			for(i = 0; i < opt_n_threads; i++)
			{
				path = gc3355_devs[i].devname;
				base = strrchr(path, '/');
				if(!strcmp(devname, base + 1))
				{
					chips = json_object_get(dev, API_CHIPS);
					if (!chips || !json_is_array(chips))
					{
						return false;
					}
					applog(LOG_DEBUG, "API: %s: Change frequency", devname);
					for(j = 0; j < json_array_size(chips); j++)
					{
						chip = json_array_get(chips, j);
						freq = fix_freq(json_integer_value(chip));
						pthread_mutex_lock(&stats_lock);
						if(gc3355_devs[i].freq[j] != freq)
						{
							gc3355_set_core_freq(&gc3355_devs[i], j, freq);
						}
						pthread_mutex_unlock(&stats_lock);
					}
				}
			}
			iter = json_object_iter_next(devs, iter);
		}
		return true;
	}
	else if(!strcmp(api_set, API_POOL))
	{
		jpool = json_object_get(req, API_POOL);
		if(!jpool || !json_is_integer(jpool))
			return false;
		pool_id = json_integer_value(jpool);
		pthread_mutex_lock(&pool_lock);
		pool_count = get_pool_count(pools);
		pthread_mutex_unlock(&pool_lock);
		if(pool_id < 0 || pool_id >= pool_count)
		{
			applog(LOG_ERR, "API: Pool_id out of bounds: 0 <= Pool_id < %d", pool_count);
			return true;
		}
		id = malloc(sizeof(int));
		*id = pool_id;
        if(unlikely(pthread_create(&switch_pool_thread, NULL, switch_pool_handler, (void*)id)))
        {
            applog(LOG_ERR, "API: Could not create switch_pool thread");
			free(id);
        }
		return true;
	}
	return false;
}
#endif

#ifndef WIN32
static void *api_request_handler(void *socket)
{
	pthread_detach(pthread_self());
	int sock = *(int*)socket;
	free(socket);
    int read_size, read_pos, buffer_size = 256, err_size = 256;
    char request[buffer_size], *message, *pos, err_msg[err_size];
	const char *api_get, *api_set;
	json_t *obj, *req, *get, *set, *err;
	json_error_t json_err;
read:
	memset(err_msg, 0, err_size);
	memset(request, 0, buffer_size);
	read_pos = 0;
    while((read_size = recv(sock, request + read_pos, buffer_size - read_pos, 0)) > 0)
    {
		read_pos += read_size;
		if(read_pos >= buffer_size) goto read;
		if((pos = strchr(request, '\n')) == NULL) continue;
		while((pos = strchr(request, '\r')) != NULL || (pos = strchr(request, '\n')) != NULL)
			*pos = '\0';
		obj = json_object();
		req = JSON_LOADS(request, &json_err);
		if (!req)
		{
			snprintf(err_msg, err_size, "API: JSON decode failed(%d): %s (%s)", json_err.line, json_err.text, request);
			goto err;
		}
		get = json_object_get(req, "get");
		if (!get || !json_is_string(get))
		{
			set = json_object_get(req, "set");
			if (!set || !json_is_string(set))
			{
				snprintf(err_msg, err_size, "API: Unrecognized JSON query: %s", request);
				goto err;
			}
			else
			{
				api_set = json_string_value(set);
				if(!api_parse_set(api_set, req, obj, err))
				{
					snprintf(err_msg, err_size, "API: Unrecognized SET command: %s", request);
					goto err;
				}
				else
				{
					applog(LOG_DEBUG, "API: SET: %s", api_set);
					err = json_integer(0);
				}
			}
		}
		else
		{
			api_get = json_string_value(get);
			if(!api_parse_get(api_get, obj, err))
			{
				snprintf(err_msg, err_size, "API: Unrecognized GET command: %s", request);
				goto err;
			}
			else
			{
				applog(LOG_DEBUG, "API: GET: %s", api_get);
				err = json_integer(0);
			}
		}
		goto write;
    }
	close(sock);
    return NULL;
err:
	applog(LOG_ERR, "%s", err_msg);
	err = json_integer(1);
	json_object_set_new(obj, "errstr", json_string(err_msg));
write:
	if(req)
		json_decref(req);
	json_object_set_new(obj, "err", err);
	message = json_dumps(obj, JSON_COMPACT);
	json_decref(obj);
	write(sock, message, strlen(message));
	free(message);
	goto read;
}
#endif

#ifndef WIN32
static void *api_thread(void *userdata)
{
    int new_socket, *psocket, c, yes;
    struct sockaddr_in server, client;
	char client_ip[INET_ADDRSTRLEN];
	pthread_t api_request_thread;
    api_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (api_sock == -1)
    {
        applog(LOG_ERR, "Could not create socket");
		goto out;
    }
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(opt_api_port);
	yes = 1;
	if(setsockopt(api_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) < 0)
	{
        applog(LOG_ERR, "API: Sockopt failed");
		goto out;
	}
    if(bind(api_sock, (struct sockaddr *)&server, sizeof(server)) < 0)
    {
        applog(LOG_ERR, "API: Bind failed");
		goto out;
    }
    listen(api_sock, API_QUEUE);
    c = sizeof(struct sockaddr_in);
    while((new_socket = accept(api_sock, (struct sockaddr *)&client, (socklen_t*)&c)))
    {
		inet_ntop(AF_INET, &(client.sin_addr.s_addr), client_ip, INET_ADDRSTRLEN);
		applog(LOG_DEBUG, "API: Client %s connected", client_ip);
		psocket = malloc(sizeof(int));
		*psocket = new_socket;
        if(unlikely(pthread_create(&api_request_thread, NULL, api_request_handler, (void*)psocket)))
        {
            applog(LOG_ERR, "API: Could not create request thread");
			free(psocket);
        }
		usleep(100000);
    }
    if (new_socket < 0)
    {
		applog(LOG_ERR, "API: Accept failed");
		goto out;
    }
out:
	if(api_sock != -1)
	{
		close(api_sock);
	}
	return NULL;
}
#endif

static void show_version_and_exit(void)
{
	printf("%s\n%s\n%s\n", PACKAGE_STRING, curl_version(), MINER_VERSION);
	exit(0);
}

static void show_usage_and_exit(int status)
{
	if (status)
		fprintf(stderr, "Try `" PROGRAM_NAME " --help' for more information.\n");
	else
		printf(usage);
	exit(status);
}

static void parse_arg (int key, char *arg, char *pname)
{
	char *p;
	int v;
	struct pool_details *pool;

	switch(key) {
	case 'd':
		opt_gc3355_detect = true;
		break;
	case 'G':
		gc3355_devname = strdup(arg);
		break;
	case 'F':
		opt_frequency = atoi(arg);
		break;
	case 'f':
		opt_gc3355_frequency = strdup(arg);
		break;
	case 'A':
		opt_gc3355_autotune = true;
		break;
	case 'n':
		opt_gc3355_chips = atoi(arg);
		break;
	case 'x':
		opt_gc3355_timeout = atoi(arg);
		break;
	case 'a':
		opt_api_port = atoi(arg);
		break;
	case 'c': {
		json_error_t err;
		if (opt_config)
			json_decref(opt_config);
#if JANSSON_VERSION_HEX >= 0x020000
		opt_config = json_load_file(arg, 0, &err);
#else
		opt_config = json_load_file(arg, &err);
#endif
		if (!json_is_object(opt_config)) {
			if (err.line < 0)
				fprintf(stderr, "%s: %s\n", pname, err.text);
			else
				fprintf(stderr, "%s: %s:%d: %s\n",
					pname, arg, err.line, err.text);
			exit(1);
		}
		break;
	}
	case 'w':
		opt_refresh = false;
		break;
	case 't':
		opt_curses = false;
		break;
	case 'L':
		opt_log = true;
		log_path = strdup(arg);
		FILE* fp = fopen(log_path, "w+");
		fclose(fp);
		break;
	case 'q':
		opt_quiet = true;
		break;
	case 'D':
		opt_debug = true;
		break;
	case 'p':
		add_pool_pass(pools, gpool, arg);
		break;
	case 'P':
		opt_protocol = true;
		break;
	case 'r':
		v = atoi(arg);
		if (v < -1 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_retries = v;
		break;
	case 'R':
		v = atoi(arg);
		if (v < 1 || v > 9999)	/* sanity check */
			show_usage_and_exit(1);
		opt_fail_pause = v;
		break;
	case 'T':
		v = atoi(arg);
		if (v < 1 || v > 99999)	/* sanity check */
			show_usage_and_exit(1);
		opt_timeout = v;
		break;
	case 'u':
		add_pool_user(pools, gpool, arg);
		break;
	case 'o':			/* --url */
		pool = gpool;
		p = strstr(arg, "://");
		if (p) {
			if (strncasecmp(arg, "http://", 7) && strncasecmp(arg, "https://", 8) &&
					strncasecmp(arg, "stratum+tcp://", 14))
				show_usage_and_exit(1);
			add_pool_url(pools, gpool, arg);
		} else {
			if (!strlen(arg) || *arg == '/')
				show_usage_and_exit(1);
			char *rpc_url = malloc(strlen(arg) + 8);
			sprintf(rpc_url, "http://%s", arg);
			add_pool_url(pools, gpool, rpc_url);
			free(rpc_url);
		}
		if(pool == NULL)
			pool = gpool;
		have_stratum = !strncasecmp(pool->rpc_url, "stratum", 7);
		break;
	case 'O':			/* --userpass */
		p = strchr(arg, ':');
		if (!p)
			show_usage_and_exit(1);
		char *rpc_user = calloc(p - arg + 1, 1);
		strncpy(rpc_user, arg, p - arg);
		char *rpc_pass = strdup(p + 1);
		add_pool_user(pools, gpool, rpc_user);
		add_pool_pass(pools, gpool, rpc_pass);
		free(rpc_user);
		free(rpc_pass);
		break;
	case 'V':
		show_version_and_exit();
	case 'h':
		show_usage_and_exit(0);
	default:
		show_usage_and_exit(1);
	}
}

static void parse_config(char *pname)
{
	int i, j, k;
	json_t *val;

	if (!json_is_object(opt_config))
		return;

	for (i = 0; i < ARRAY_SIZE(options); i++) {
		if (!options[i].name)
			break;
		if (!strcmp(options[i].name, "config"))
			continue;

		val = json_object_get(opt_config, options[i].name);
		if (!val)
			continue;

		if (options[i].has_arg && json_is_string(val)) {
			char *s = strdup(json_string_value(val));
			if (!s)
				break;
			parse_arg(options[i].val, s, pname);
			free(s);
		} else if (!options[i].has_arg && json_is_true(val))
			parse_arg(options[i].val, "", pname);
		else if(json_is_array(val))
		{
			if(options[i].val == '\0')
			{
				for(j = 0; j < json_array_size(val); j++)
				{
					json_t *obj, *value;
					obj = json_array_get(val, j);
					for (k = 0; k < ARRAY_SIZE(options); k++)
					{
						if (!options[k].name)
							break;
						value = json_object_get(obj, options[k].name);
						if(!value || !json_is_string(value))
							continue;
						char *s = strdup(json_string_value(value));
						if (!s)
							continue;
						parse_arg(options[k].val, s, pname);
						free(s);
					}
				}
			}
			else
			{
				char *s;
				const char *bit;
				int len;
				json_t *value = json_array_get(val, 0);
				if(!value || !json_is_string(value))
					continue;
				bit = json_string_value(value);
				s = strdup(bit);
				len = strlen(bit) + 1;
				for(j = 1; j < json_array_size(val); j++)
				{
					value = json_array_get(val, j);
					if(!value || !json_is_string(value))
						continue;
					bit = json_string_value(value);
					len += strlen(bit) + 1;
					s = realloc(s, len);
					strncat(strncat(s, ",", len), bit, len);
				}
				parse_arg(options[i].val, s, pname);
				free(s);
			}
		}
		else
		{
			fprintf(stderr, "%s: invalid argument for option '%s'\n",
				pname, options[i].name);
			exit(1);
		}
	}
}

static void parse_cmdline(int argc, char *argv[])
{
	int key;

	while (1) {
#if HAVE_GETOPT_LONG
		key = getopt_long(argc, argv, short_options, options, NULL);
#else
		key = getopt(argc, argv, short_options);
#endif
		if (key < 0)
			break;

		parse_arg(key, optarg, argv[0]);
	}
	if (optind < argc) {
		fprintf(stderr, "%s: unsupported non-option argument '%s'\n",
			argv[0], argv[optind]);
		show_usage_and_exit(1);
	}
	
	parse_config(argv[0]);
}

static void clean_up()
{
	can_work = false;
	if(opt_curses)
	{
		applog(LOG_INFO, "Clean up");
		pthread_mutex_lock(&tui_lock);
		opt_curses = false;
		clean_tui();
		curs_set(1);
		pthread_mutex_unlock(&tui_lock);
	}
	close(api_sock);
	int i;
	for(i = 0; i < opt_n_threads; i++)
	{
		gc3355_close(gc3355_devs[i].dev_fd);	
	}
}

#ifndef WIN32
void signal_handler(int sig)
{
	switch (sig) {
	case SIGHUP:
		applog(LOG_DEBUG, "SIGHUP received");
		break;
	case SIGINT:
		applog(LOG_DEBUG, "SIGINT received, exiting");
		clean_up();
		exit(0);
		break;
	case SIGTERM:
		applog(LOG_DEBUG, "SIGTERM received, exiting");
		clean_up();
		exit(0);
		break;
	case SIGSEGV:
		applog(LOG_DEBUG, "SIGSEGV received, exiting");
		clean_up();
		exit(0);
		break;
	case SIGWINCH:
		applog(LOG_DEBUG, "SIGWINCH received");
		resize_tui();
		break;
	}
}
#endif

int main(int argc, char *argv[])
{
	struct thr_info *thr;
	long flags;
	
	pthread_mutex_init(&applog_lock, NULL);
	pthread_mutex_init(&stats_lock, NULL);
	pthread_mutex_init(&tui_lock, NULL);
	pthread_mutex_init(&g_work_lock, NULL);
	pthread_mutex_init(&work_items_lock, NULL);
	pthread_mutex_init(&pool_lock, NULL);
	pthread_mutex_init(&check_pool_lock, NULL);
	pthread_mutex_init(&switch_pool_lock, NULL);
	stratum = calloc(1, sizeof(struct stratum_ctx));
	pthread_mutex_init(&stratum->sock_lock, NULL);
	pthread_mutex_init(&stratum->work_lock, NULL);
	pthread_cond_init(&check_pool_cond, NULL);
	
	time(&time_start);

	pools = init_pool_details();
	
	/* parse command line */
	parse_cmdline(argc, argv);

#ifndef WIN32
	signal(SIGHUP, signal_handler);
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGSEGV, signal_handler);
	signal(SIGWINCH, signal_handler);
#endif

	struct pool_details *pool = get_main_pool(pools);
	if(pool == NULL)
	{
		pool = new_pool(true);
		set_active_pool(pools, pool, true);
	}
	flags = strncmp(pool->rpc_url, "https:", 6)
	      ? (CURL_GLOBAL_ALL & ~CURL_GLOBAL_SSL)
	      : CURL_GLOBAL_ALL;
	if (curl_global_init(flags)) {
		applog(LOG_ERR, "CURL initialization failed");
		return 1;
	}
	
	opt_n_threads = 0;

	device_list = gc3355_get_device_list();
	if(opt_gc3355_detect)
	{
		opt_n_threads = gc3355_get_device_count(device_list);
	}
	else if (gc3355_devname != NULL)
	{
		char *p = gc3355_devname;
		int nn=0;
		do {
			p = strchr(p+1, ',');
			nn++;
		} while(p!=NULL);
		opt_n_threads = nn;
	}
	
	if(!opt_n_threads)
	{
		applog(LOG_ERR, "No GC3355 devices specified, please use --gc3355-detect for auto-detection, or manually specify with --gc3355=DEV0,DEV1,...,DEVn");
		exit(1);
	}

	struct gc3355_dev devs[opt_n_threads];
	memset(&devs, 0, sizeof(devs));
	gc3355_devs = devs;
	
	if(opt_curses)
	{
		pthread_mutex_lock(&tui_lock);
		init_tui();
		start_tui();
		pthread_mutex_unlock(&tui_lock);
	}
	
	work_items = init_work_items();

	work_restart = calloc(opt_n_threads, sizeof(*work_restart));
	if (!work_restart)
		return 1;

	thr_info = calloc(opt_n_threads + 7, sizeof(*thr));
	if (!thr_info)
		return 1;

	/* init workio thread info */
	work_thr_id = opt_n_threads;
	thr = &thr_info[work_thr_id];
	thr->id = work_thr_id;
	thr->q = tq_new();
	if (!thr->q)
		return 1;
		
	/* start work I/O thread */
	if (pthread_create(&thr->pth, NULL, workio_thread, thr)) {
		applog(LOG_ERR, "workio thread create failed");
		return 1;
	}
	
	check_pool_thr_id = opt_n_threads + 6;
	thr = &thr_info[check_pool_thr_id];
	thr->id = check_pool_thr_id;
	/* start check_pool thread */
	if (unlikely(pthread_create(&thr->pth, NULL, check_pool_thread, thr))) {
		applog(LOG_ERR, "check_pool thread create failed");
		return 1;
	}
	
	if (want_stratum) {
		/* init stratum thread info */
		stratum_thr_id = opt_n_threads + 2;
		thr = &thr_info[stratum_thr_id];
		thr->id = stratum_thr_id;
		thr->q = tq_new();
		if (!thr->q)
			return 1;

		/* start stratum thread */
		if (unlikely(pthread_create(&thr->pth, NULL, stratum_thread, thr))) {
			applog(LOG_ERR, "stratum thread create failed");
			return 1;
		}
	}
	/* start mining threads */

	if (create_gc3355_miner_threads(thr_info, opt_n_threads) != 0)
		return 1;

#ifndef WIN32
	/* init api thread info */
	api_thr_id = opt_n_threads + 3;
	thr = &thr_info[api_thr_id];
	thr->id = api_thr_id;
	/* start api thread */
	if (unlikely(pthread_create(&thr->pth, NULL, api_thread, thr))) {
		applog(LOG_ERR, "api thread create failed");
		return 1;
	}
#endif

	if(opt_curses)
	{
		/* init tui thread info */
		tui_main_thr_id = opt_n_threads + 4;
		thr = &thr_info[tui_main_thr_id];
		thr->id = tui_main_thr_id;
		/* start api thread */
		if (unlikely(pthread_create(&thr->pth, NULL, tui_main_thread, thr))) {
			applog(LOG_ERR, "tui main thread create failed");
			return 1;
		}
		/* init tui thread info */
		tui_user_thr_id = opt_n_threads + 5;
		thr = &thr_info[tui_user_thr_id];
		thr->id = tui_user_thr_id;
		/* start api thread */
		if (unlikely(pthread_create(&thr->pth, NULL, tui_user_thread, thr))) {
			applog(LOG_ERR, "tui thread create failed");
			return 1;
		}
	}

	/* main loop - simply wait for workio thread to exit */
	pthread_join(thr_info[work_thr_id].pth, NULL);
	applog(LOG_INFO, "workio thread dead, exiting.");
	
	clean_up();
	
	return 0;
}