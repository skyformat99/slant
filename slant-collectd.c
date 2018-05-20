#include <sys/queue.h>

#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ksql.h>

#include "slant-collectd.h"
#include "extern.h"
#include "db.h"

static	sig_atomic_t	doexit = 0;

static void
sig(int sig)
{

	doexit = 1;
}

static void
update_interval(struct kwbp *db, time_t span, 
	size_t have, size_t allowed,
	const struct record *first, const struct record *last, 
	enum interval ival, time_t now, const struct record *r)
{

	assert(allowed > 0);

	if (NULL != first && first->ctime + span > now) {
		/* Update the current entry. */
		assert(NULL != first);
		assert(NULL != last);
		db_record_update_current(db, 
			first->entries + 1, 
			first->cpu + r->cpu, first->id);
	} else if (have > allowed) {
		/* New entry: shift end of circular queue. */
		assert(NULL != first);
		assert(NULL != last);
		db_record_update_tail(db, now, 1, r->cpu, last->id);
	} else {
		/* New entry. */
		db_record_insert(db, now, 1, r->cpu, ival);
	}
}

/*
 * Update the database "db" given the current record "p" and all
 * existing database records "rq".
 */
static void
update(struct kwbp *db, const struct sysinfo *p, 
	const struct record_q *rq)
{
	size_t	 	 bymin = 0, byhour = 0, byqmin = 0,
			 byday = 0, byweek = 0, byyear = 0;
	time_t		 t = time(NULL);
	struct record	 rr;
	const struct record *r, 
	      		*first_bymin = NULL, *last_bymin = NULL,
			*first_byqmin = NULL, *last_byqmin = NULL,
			*first_byhour = NULL, *last_byhour = NULL,
			*first_byday = NULL, *last_byday = NULL,
			*first_byweek = NULL, *last_byweek = NULL,
			*first_byyear = NULL, *last_byyear = NULL;

	memset(&rr, 0, sizeof(struct record));
	rr.cpu = sysinfo_get_proc_avg(p);

	/* 
	 * First count what we have.
	 * We need this when determining how many "spare" entries to
	 * keep in any given interval, e.g., we keep 5 minutes of
	 * quarter-minute interval data, but only really need the last
	 * single minute for accumulation.
	 */

	TAILQ_FOREACH(r, rq, _entries)
		switch (r->interval) {
		case INTERVAL_byqmin:
			if (NULL == first_byqmin)
				first_byqmin = r;
			last_byqmin = r;
			byqmin++;
			break;
		case INTERVAL_bymin:
			if (NULL == first_bymin)
				first_bymin = r;
			last_bymin = r;
			bymin++;
			break;
		case INTERVAL_byhour:
			if (NULL == first_byhour)
				first_byhour = r;
			last_byhour = r;
			byhour++;
			break;
		case INTERVAL_byday:
			if (NULL == first_byday)
				first_byday = r;
			last_byday = r;
			byday++;
			break;
		case INTERVAL_byweek:
			if (NULL == first_byweek)
				first_byweek = r;
			last_byweek = r;
			byweek++;
			break;
		case INTERVAL_byyear:
			if (NULL == first_byyear)
				first_byyear = r;
			last_byyear = r;
			byyear++;
			break;
		}

	db_trans_open(db, 0, 0);

	/* 40 (10 minute) backlog of quarter-minute entries. */

	if (byqmin > (4 * 10)) {
		assert(NULL != last_byqmin);
		assert(NULL != first_byqmin);
		db_record_update_tail(db, t, 1, 
			rr.cpu, last_byqmin->id);
	} else
		db_record_insert(db, t, 1,
			rr.cpu, INTERVAL_byqmin);

	/* 300 (5 hours) backlog of by-minute entries. */

	update_interval(db, 60, bymin, 
		60 * 5, first_bymin, last_bymin, 
		INTERVAL_bymin, t, &rr);

	/* 96 (5 days) backlog of by-hour entries. */

	update_interval(db, 60 * 60, byhour, 
		24 * 5, first_byhour, last_byhour, 
		INTERVAL_byhour, t, &rr);

	/* 28 (4 weeks) backlog of by-day entries. */

	update_interval(db, 60 * 60 * 24, byday, 
		7 * 4, first_byday, last_byday, 
		INTERVAL_byday, t, &rr);

	/* 104 (two year) backlog of by-week entries. */

	update_interval(db, 60 * 60 * 24 * 7, byweek, 
		52 * 2, first_byday, last_byweek, 
		INTERVAL_byweek, t, &rr);

	/* Endless backlog of yearly entries. */

	update_interval(db, 60 * 60 * 24 * 365, byyear, 
		SIZE_MAX, first_byyear, last_byyear, 
		INTERVAL_byyear, t, &rr);

	db_trans_commit(db, 0);
}

int
main(int argc, char *argv[])
{
	struct kwbp	*db;
	struct record_q	*rq;
	struct sysinfo	*info;
	int		 c;
	const char	*dbfile = "/var/www/data/slant.db";

	/*
	 * Pre-pledge, establishing a reasonable baseline.
	 * Then open our database in a protected process.
	 * After that, drop us to minimum privilege/role.
	 */

	if (-1 == pledge
	    ("ps stdio rpath cpath wpath flock proc fattr", NULL))
		err(EXIT_FAILURE, "pledge");

	while (-1 != (c = getopt(argc, argv, "f:")))
		switch (c) {
		case 'f':
			dbfile = optarg;
			break;
		default:
			goto usage;
		}

	/* XXX: hack around ksql(3) exit when receives signal. */

	if (SIG_ERR == signal(SIGINT, SIG_IGN))
		err(EXIT_FAILURE, "signal");
	if (SIG_ERR == signal(SIGTERM, SIG_IGN))
		err(EXIT_FAILURE, "signal");

	db = db_open(dbfile);
	if (NULL == db)
		errx(EXIT_FAILURE, "%s", dbfile);

	if (-1 == pledge("ps stdio", NULL))
		err(EXIT_FAILURE, "pledge");

	db_role(db, ROLE_produce);

	/*
	 * From here on our, use the "out" label for bailing on errors,
	 * which will clean up behind us.
	 * Let SIGINT and SIGTERM trigger us into exiting safely.
	 */

	if (NULL == (info = sysinfo_alloc()))
		goto out;

	if (SIG_ERR == signal(SIGINT, sig) ||
	    SIG_ERR == signal(SIGTERM, sig)) {
		warn("signal");
		goto out;
	}

	/*
	 * Now enter our main loop.
	 * The body will run every 15 seconds.
	 * Start each iteration by grabbing the current system state
	 * using sysctl(3).
	 * Then grab what we have in the database.
	 * Lastly, modify the database state given our current.
	 */

	while ( ! doexit) {
		if (sleep(15 - ((time(NULL) + 1) % 15)))
			break;
		sysinfo_update(info);
		rq = db_record_list_lister(db);
		update(db, info, rq);
		db_record_freeq(rq);
	}

out:
	sysinfo_free(info);
	db_close(db);
	return EXIT_SUCCESS;
usage:
	fprintf(stderr, "usage: %s [-f dbfile]\n", getprogname());
	return EXIT_FAILURE;
}
