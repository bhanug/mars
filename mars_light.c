// (c) 2011 Thomas Schoebel-Theuer / 1&1 Internet AG

//#define BRICK_DEBUGGING
//#define MARS_DEBUGGING
//#define IO_DEBUGGING
//#define STAT_DEBUGGING // here means: display full statistics

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/debug_locks.h>

#include <linux/major.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>

#define _STRATEGY
#include "mars.h"

#include <linux/kthread.h>
#include <linux/wait.h>

// used brick types
#include "mars_server.h"
#include "mars_client.h"
#include "mars_copy.h"
#include "mars_bio.h"
#include "mars_aio.h"
#include "mars_trans_logger.h"
#include "mars_if.h"

#if 0
#define inline __attribute__((__noinline__))
#endif

static struct task_struct *main_thread = NULL;

typedef int (*light_worker_fn)(void *buf, struct mars_dent *dent);

struct light_class {
	char *cl_name;
	int    cl_len;
	char   cl_type;
	bool   cl_hostcontext;
	bool   cl_serial;
	int    cl_father;
	light_worker_fn cl_forward;
	light_worker_fn cl_backward;
};

///////////////////////////////////////////////////////////////////////

// TUNING

#define CONF_TRANS_CHUNKSIZE  (128 * 1024)
//#define CONF_TRANS_ALIGN      512
#define CONF_TRANS_ALIGN      0
//#define FLUSH_DELAY (HZ / 100 + 1)
#define FLUSH_DELAY 0

//#define TRANS_FAKE

#define CONF_TRANS_BATCHLEN 1024
//#define CONF_TRANS_FLYING 4
#define CONF_TRANS_FLYING 128
#define CONF_TRANS_PRIO   MARS_PRIO_HIGH
#define CONF_TRANS_LOG_READS false
//#define CONF_TRANS_LOG_READS true
#define CONF_TRANS_MINIMIZE_LATENCY false
//#define CONF_TRANS_MINIMIZE_LATENCY true
//#define CONF_TRANS_COMPLETION_SEMANTICS 2
#define CONF_TRANS_COMPLETION_SEMANTICS 0

//#define CONF_ALL_BATCHLEN 2
#define CONF_ALL_BATCHLEN 1
//#define CONF_ALL_FLYING 4
#define CONF_ALL_FLYING 1
#define CONF_ALL_CONTENTION 0
#define CONF_ALL_PRESSURE 0
#define CONF_ALL_PRIO   MARS_PRIO_NORMAL

#define CONF_ALL_MAX_QUEUE 10000
#define CONF_ALL_MAX_JIFFIES (180 * HZ)

#define IF_SKIP_SYNC true

#define IF_MAX_PLUGGED 10000
#define IF_READAHEAD 0
//#define IF_READAHEAD 1

#define BIO_READAHEAD 0
//#define BIO_READAHEAD 1
#define BIO_NOIDLE true
#define BIO_SYNC true
#define BIO_UNPLUG true

#define AIO_READAHEAD 1
#define AIO_WAIT_DURING_FDSYNC false

#define COPY_APPEND_MODE 0
//#define COPY_APPEND_MODE 1 // FIXME: does not work yet
#define COPY_PRIO MARS_PRIO_LOW

static
void _set_trans_params(struct mars_brick *_brick, void *private)
{
	struct trans_logger_brick *trans_brick = (void*)_brick;
	if (_brick->type != (void*)&trans_logger_brick_type) {
		MARS_ERR("bad brick type\n");
		return;
	}
	if (!trans_brick->q_phase2.q_ordering) {
		trans_brick->q_phase1.q_batchlen = CONF_TRANS_BATCHLEN;
		trans_brick->q_phase2.q_batchlen = CONF_ALL_BATCHLEN;
		trans_brick->q_phase3.q_batchlen = CONF_ALL_BATCHLEN;
		trans_brick->q_phase4.q_batchlen = CONF_ALL_BATCHLEN;

		trans_brick->q_phase1.q_max_flying = CONF_TRANS_FLYING;
		trans_brick->q_phase2.q_max_flying = CONF_ALL_FLYING;
		trans_brick->q_phase3.q_max_flying = CONF_ALL_FLYING;
		trans_brick->q_phase4.q_max_flying = CONF_ALL_FLYING;

		trans_brick->q_phase1.q_max_contention = CONF_ALL_CONTENTION;
		trans_brick->q_phase2.q_max_contention = CONF_ALL_CONTENTION;
		trans_brick->q_phase3.q_max_contention = CONF_ALL_CONTENTION;
		trans_brick->q_phase4.q_max_contention = CONF_ALL_CONTENTION;

		trans_brick->q_phase1.q_over_pressure = CONF_ALL_PRESSURE;
		trans_brick->q_phase2.q_over_pressure = CONF_ALL_PRESSURE;
		trans_brick->q_phase3.q_over_pressure = CONF_ALL_PRESSURE;
		trans_brick->q_phase4.q_over_pressure = CONF_ALL_PRESSURE;

		trans_brick->q_phase1.q_io_prio = CONF_TRANS_PRIO;
		trans_brick->q_phase2.q_io_prio = CONF_ALL_PRIO;
		trans_brick->q_phase3.q_io_prio = CONF_ALL_PRIO;
		trans_brick->q_phase4.q_io_prio = CONF_ALL_PRIO;

		trans_brick->q_phase2.q_max_queued = CONF_ALL_MAX_QUEUE;
		trans_brick->q_phase4.q_max_queued = CONF_ALL_MAX_QUEUE;

		trans_brick->q_phase2.q_max_jiffies = CONF_ALL_MAX_JIFFIES;
		trans_brick->q_phase4.q_max_jiffies = CONF_ALL_MAX_JIFFIES;

		trans_brick->q_phase2.q_ordering = true;
		trans_brick->q_phase4.q_ordering = true;

		trans_brick->log_reads = CONF_TRANS_LOG_READS;
		trans_brick->completion_semantics = CONF_TRANS_COMPLETION_SEMANTICS;
		trans_brick->minimize_latency = CONF_TRANS_MINIMIZE_LATENCY;
#ifdef TRANS_FAKE
		trans_brick->debug_shortcut = true;
#endif

		trans_brick->align_size = CONF_TRANS_ALIGN;
		trans_brick->chunk_size = CONF_TRANS_CHUNKSIZE;
		trans_brick->flush_delay = FLUSH_DELAY;

		if (!trans_brick->log_reads) {
			trans_brick->q_phase2.q_max_queued = 0;
			trans_brick->q_phase4.q_max_queued *= 2;
		}
	}
	MARS_INF("name = '%s' path = '%s'\n", _brick->brick_name, _brick->brick_path);
}

static
void _set_client_params(struct mars_brick *_brick, void *private)
{
	// currently no params
	MARS_INF("name = '%s' path = '%s'\n", _brick->brick_name, _brick->brick_path);
}

static
void _set_aio_params(struct mars_brick *_brick, void *private)
{
	struct aio_brick *aio_brick = (void*)_brick;
	if (_brick->type == (void*)&client_brick_type) {
		_set_client_params(_brick, private);
		return;
	}
	if (_brick->type != (void*)&aio_brick_type) {
		MARS_ERR("bad brick type\n");
		return;
	}
	aio_brick->readahead = AIO_READAHEAD;
	aio_brick->o_direct = false; // important!
	aio_brick->o_fdsync = true;
	aio_brick->wait_during_fdsync = AIO_WAIT_DURING_FDSYNC;
	MARS_INF("name = '%s' path = '%s'\n", _brick->brick_name, _brick->brick_path);
}

static
void _set_bio_params(struct mars_brick *_brick, void *private)
{
	struct bio_brick *bio_brick;
	if (_brick->type == (void*)&client_brick_type) {
		_set_client_params(_brick, private);
		return;
	}
	if (_brick->type == (void*)&aio_brick_type) {
		_set_aio_params(_brick, private);
		return;
	}
	if (_brick->type != (void*)&bio_brick_type) {
		MARS_ERR("bad brick type\n");
		return;
	}
	bio_brick = (void*)_brick;
	bio_brick->ra_pages = BIO_READAHEAD;
	bio_brick->do_noidle = BIO_NOIDLE;
	bio_brick->do_sync = BIO_SYNC;
	bio_brick->do_unplug = BIO_UNPLUG;
	MARS_INF("name = '%s' path = '%s'\n", _brick->brick_name, _brick->brick_path);
}


static
void _set_if_params(struct mars_brick *_brick, void *private)
{
	struct if_brick *if_brick = (void*)_brick;
	if (_brick->type != (void*)&if_brick_type) {
		MARS_ERR("bad brick type\n");
		return;
	}
	if_brick->max_plugged = IF_MAX_PLUGGED;
	if_brick->readahead = IF_READAHEAD;
	if_brick->skip_sync = IF_SKIP_SYNC;
	MARS_INF("name = '%s' path = '%s'\n", _brick->brick_name, _brick->brick_path);
}

static
void _set_copy_params(struct mars_brick *_brick, void *private)
{
	struct copy_brick *copy_brick = (void*)_brick;
	if (_brick->type != (void*)&copy_brick_type) {
		MARS_ERR("bad brick type\n");
		return;
	}
	copy_brick->append_mode = COPY_APPEND_MODE;
	copy_brick->io_prio = COPY_PRIO;
	MARS_INF("name = '%s' path = '%s'\n", _brick->brick_name, _brick->brick_path);
}

///////////////////////////////////////////////////////////////////////

// internal helpers

#define MARS_DELIM ','

static int _parse_args(struct mars_dent *dent, char *str, int count)
{
	int i;
	int status = -EINVAL;
	if (!str)
		goto done;
	if (!dent->d_args) {
		dent->d_args = kstrdup(str, GFP_MARS);
		if (!dent->d_args) {
			status = -ENOMEM;
			goto done;
		}
	}
	for (i = 0; i < count; i++) {
		char *tmp;
		int len;
		if (!*str)
			goto done;
		if (i == count-1) {
			len = strlen(str);
		} else {
			char *tmp = strchr(str, MARS_DELIM);
			if (!tmp)
				goto done;
			len = (tmp - str);
		}
		tmp = kzalloc(len+1, GFP_MARS);
		if (!tmp) {
			status = -ENOMEM;
			goto done;
		}
		if (dent->d_argv[i]) {
			kfree(dent->d_argv[i]);
		}
		dent->d_argv[i] = tmp;
		strncpy(dent->d_argv[i], str, len);
		dent->d_argv[i][len] = '\0';

		str += len;
		if (i != count-1)
			str++;
	}
	status = 0;
done:
	if (status < 0) {
		MARS_ERR("bad syntax '%s' (should have %d args), status = %d\n", dent->d_args ? dent->d_args : "", count, status);
	}
	return status;
}

///////////////////////////////////////////////////////////////////////

static
int __make_copy(
		struct mars_global *global,
		struct mars_dent *belongs,
		const char *switch_path,
		const char *copy_path,
		const char *parent,
		const char *argv[],
		loff_t start_pos, // -1 means at EOF
		struct copy_brick **__copy)
{
	struct mars_brick *copy;
	struct copy_brick *_copy;
 	const char *fullpath[2] = {};
	struct mars_output *output[2] = {};
	struct mars_info info[2] = {};
	int i;
	int status = -EINVAL;

	if (!switch_path) {
		goto done;
	}

	for (i = 0; i < 2; i++) {
		struct mars_brick *aio;

		if (parent) {
			fullpath[i] = path_make("%s/%s", parent, argv[i]);
			if (!fullpath[i]) {
				MARS_ERR("cannot make path '%s/%s'\n", parent, argv[i]);
				goto done;
			}
		} else {
			fullpath[i] = argv[i];
		}

		aio =
			make_brick_all(global,
				       NULL,
				       _set_bio_params,
				       NULL,
				       10 * HZ,
				       NULL,
				       (const struct generic_brick_type*)&bio_brick_type,
				       (const struct generic_brick_type*[]){},
				       NULL,
				       fullpath[i],
				       (const char *[]){},
				       0);
		if (!aio) {
			MARS_DBG("cannot instantiate '%s'\n", fullpath[i]);
			goto done;
		}
		output[i] = aio->outputs[0];
	}

	copy =
		make_brick_all(global,
			       belongs,
			       _set_copy_params,
			       NULL,
			       0,
			       fullpath[1],
			       (const struct generic_brick_type*)&copy_brick_type,
			       (const struct generic_brick_type*[]){NULL,NULL,NULL,NULL},
			       "%s",
			       "%s",
			       (const char *[]){"%s", "%s", "%s", "%s"},
			       4,
			       switch_path,
			       copy_path,
			       fullpath[0],
			       fullpath[0],
			       fullpath[1],
			       fullpath[1]);
	if (!copy) {
		MARS_DBG("creation of copy brick '%s' failed\n", copy_path);
		goto done;
	}
	copy->status_level = 2;
	_copy = (void*)copy;
	if (__copy)
		*__copy = _copy;

	/* Determine the copy area, switch on/off when necessary
	 */
	if (!_copy->power.button && _copy->power.led_off) {
		_copy->copy_last = 0;
		for (i = 0; i < 2; i++) {
			status = output[i]->ops->mars_get_info(output[i], &info[i]);
			if (status < 0) {
				MARS_ERR("cannot determine current size of '%s'\n", argv[i]);
				goto done;
			}
			MARS_DBG("%d '%s' current_size = %lld\n", i, fullpath[i], info[i].current_size);
		}
		_copy->copy_start = info[1].current_size;
		if (start_pos != -1) {
			_copy->copy_start = start_pos;
			if (unlikely(info[0].current_size != info[1].current_size)) {
				MARS_ERR("oops, devices have different size %lld != %lld at '%s'\n", info[0].current_size, info[1].current_size, copy_path);
				status = -EINVAL;
				goto done;
			}
			if (unlikely(start_pos > info[0].current_size)) {
				MARS_ERR("bad start position %lld is larger than actual size %lld on '%s'\n", start_pos, info[0].current_size, copy_path);
				status = -EINVAL;
				goto done;
			}
		}
		MARS_DBG("copy_start = %lld\n", _copy->copy_start);
		_copy->copy_end = info[0].current_size;
		MARS_DBG("copy_end = %lld\n", _copy->copy_end);
		if (_copy->copy_start < _copy->copy_end) {
			status = mars_power_button_recursive((void*)copy, true, false, 10 * HZ);
			MARS_DBG("copy switch on status = %d\n", status);
		}
	} else if (_copy->power.button && _copy->power.led_on && _copy->copy_last == _copy->copy_end && _copy->copy_end > 0) {
		status = mars_power_button((void*)copy, false, false);
		MARS_DBG("copy switch off status = %d\n", status);
	}
	status = 0;

done:
	MARS_DBG("status = %d\n", status);
	for (i = 0; i < 2; i++) {
		if (fullpath[i] && fullpath[i] != argv[i])
			kfree(fullpath[i]);
	}
	return status;
}

///////////////////////////////////////////////////////////////////////

// remote workers

struct mars_peerinfo {
	struct mars_global *global;
	char *peer;
	char *path;
	struct socket *socket;
	struct task_struct *thread;
	spinlock_t lock;
	struct list_head remote_dent_list;
	//wait_queue_head_t event;
	int maxdepth;
};

static
bool _is_usable_dir(const char *name)
{
	if (!strncmp(name, "resource-", 9)
	   || !strncmp(name, "switch", 6)
	   || !strncmp(name, "actual", 6)
	   || !strncmp(name, "defaults", 8)
	   ) {
		return true;
	}
	return false;
}

static
bool _is_peer_logfile(const char *name, const char *id)
{
	int len = strlen(name);
	int idlen = id ? strlen(id) : 4 + 9 + 1;

	if (len <= idlen ||
	   strncmp(name, "log-", 4) != 0) {
		MARS_DBG("not a logfile at all: '%s'\n", name);
		return false;
	}
	if (id &&
	   name[len - idlen - 1] == '-' &&
	   strncmp(name + len - idlen, id, idlen) == 0) {
		MARS_DBG("not a peer logfile: '%s'\n", name);
		return false;
	}
	MARS_DBG("found peer logfile: '%s'\n", name);
	return true;
}

static
int _update_file(struct mars_global *global, const char *switch_path, const char *copy_path, const char *file, const char *peer, loff_t end_pos)
{
	const char *tmp = path_make("%s@%s", file, peer);
	const char *argv[2] = { tmp, file };
	struct copy_brick *copy = NULL;
	int status = -ENOMEM;

	if (unlikely(!tmp))
		goto done;

	MARS_DBG("src = '%s' dst = '%s'\n", tmp, file);
	status = __make_copy(global, NULL, switch_path, copy_path, NULL, argv, -1, &copy);
	if (status >= 0 && copy && (!copy->append_mode || copy->power.led_off)) {
		if (end_pos > copy->copy_end) {
			MARS_DBG("appending to '%s' %lld => %lld\n", copy_path, copy->copy_end, end_pos);
			copy->copy_end = end_pos;
		}
	}

done:
	if (tmp)
		kfree(tmp);
	return status;
}

static
int check_logfile(struct mars_peerinfo *peer, struct mars_dent *dent, struct mars_dent *parent, loff_t dst_size)
{
	loff_t src_size = dent->new_stat.size;
	const char *switch_path = NULL;
	const char *copy_path = NULL;
	const char *alias_path = NULL;
	struct mars_dent *local_alias;
	struct copy_brick *copy_brick;
	int status = 0;

	// plausibility checks
	if (unlikely(dst_size > src_size)) {
		MARS_WRN("my local copy is larger than the remote one, ignoring\n");
		status = -EINVAL;
		goto done;
	}

	// check whether (some/another) copy is already running
	copy_path = path_make("%s/logfile-update", parent->d_path);
	if (unlikely(!copy_path)) {
		status = -ENOMEM;
		goto done;
	}
	copy_brick = (struct copy_brick*)mars_find_brick(peer->global, &copy_brick_type, copy_path);
	MARS_DBG("copy_path = '%s' copy_brick = %p dent = '%s'\n", copy_path, copy_brick, dent->d_path);
	if (copy_brick) {
		bool copy_is_done = (copy_brick->copy_last == copy_brick->copy_end);
		bool is_my_copy = !strcmp(copy_brick->brick_name, dent->d_path);
		bool is_next_copy = (dent->d_serial == parent->d_logfile_serial + 1);
		MARS_DBG("copy brick '%s' copy_last = %lld copy_end = %lld dent '%s' serial = %d/%d is_done = %d is_my_copy = %d is_next_copy = %d\n", copy_brick->brick_name, copy_brick->copy_last, copy_brick->copy_end, dent->d_path, dent->d_serial, parent->d_logfile_serial, copy_is_done, is_my_copy, is_next_copy);
		// ensure consecutiveness of logfiles
		if (copy_is_done && !is_my_copy && is_next_copy) {
			MARS_DBG("killing old copy brick '%s', now going to '%s'\n", copy_brick->brick_name, dent->d_path);
			status = mars_kill_brick((void*)copy_brick);
			if (status < 0)
				goto done;
		}
		if (!is_my_copy) {
			goto done;
		}
	}

	// create local alias symlink
	if (unlikely(dent->d_serial <= 0)) {
		MARS_ERR("path '%s' has invalid serial %d\n", dent->d_path, dent->d_serial);
		status = -EINVAL;
		goto done;
	}
	alias_path = path_make("%s/log-%09d-%s", parent->d_path, dent->d_serial, my_id());
	if (unlikely(!alias_path)) {
		status = -ENOMEM;
		goto done;
	}
	status = 0;
	MARS_DBG("local alias for '%s' is '%s'\n", dent->d_path, alias_path);
	local_alias = mars_find_dent((void*)peer->global, alias_path);
	if (!local_alias) {
		status = mars_symlink(dent->d_name, alias_path, &dent->new_stat.mtime, 0);
		MARS_DBG("create alias '%s' -> '%s' status = %d\n", alias_path, dent->d_name, status);
		//run_trigger = true;
	}

	// copy necessary?
	status = 0;
	if (dst_size >= src_size) { // nothing to do
		goto done;
	}

	// check whether connection is allowed
	switch_path = path_make("%s/switch-%s/connect", parent->d_path, my_id());
	
	// start / treat copy brick instance
	status = _update_file(peer->global, switch_path, copy_path, dent->d_path, peer->peer, src_size);
	MARS_DBG("update '%s' from peer '%s' status = %d\n", dent->d_path, peer->peer, status);
	if (status < 0) {
		goto done;
	}
	parent->d_logfile_serial = dent->d_serial;

done:
	if (copy_path)
		kfree(copy_path);
	if (alias_path)
		kfree(alias_path);
	if (switch_path)
		kfree(switch_path);
	return status;
}

static
int run_bone(struct mars_peerinfo *peer, struct mars_dent *dent)
{
	int status = 0;
	struct kstat local_stat = {};
	bool stat_ok;
	bool update_mtime = true;
	bool update_ctime = true;
	bool run_trigger = false;

	if (!strncmp(dent->d_name, ".tmp", 4)) {
		goto done;
	}
	if (!strncmp(dent->d_name, "ignore", 6)) {
		goto done;
	}

	status = mars_stat(dent->d_path, &local_stat, true);
	stat_ok = (status >= 0);

	if (stat_ok) {
		update_mtime = timespec_compare(&dent->new_stat.mtime, &local_stat.mtime) > 0;
		update_ctime = timespec_compare(&dent->new_stat.ctime, &local_stat.ctime) > 0;

		MARS_DBG("timestamps '%s' remote = %ld.%09ld local = %ld.%09ld\n", dent->d_path, dent->new_stat.mtime.tv_sec, dent->new_stat.mtime.tv_nsec, local_stat.mtime.tv_sec, local_stat.mtime.tv_nsec);

		if ((dent->new_stat.mode & S_IRWXU) !=
		   (local_stat.mode & S_IRWXU) &&
		   update_ctime) {
			mode_t newmode = local_stat.mode;
			MARS_DBG("chmod '%s' 0x%xd -> 0x%xd\n", dent->d_path, newmode & S_IRWXU, dent->new_stat.mode & S_IRWXU);
			newmode &= ~S_IRWXU;
			newmode |= (dent->new_stat.mode & S_IRWXU);
			mars_chmod(dent->d_path, newmode);
			run_trigger = true;
		}

		if (dent->new_stat.uid != local_stat.uid && update_ctime) {
			MARS_DBG("lchown '%s' %d -> %d\n", dent->d_path, local_stat.uid, dent->new_stat.uid);
			mars_lchown(dent->d_path, dent->new_stat.uid);
			run_trigger = true;
		}
	}

	if (S_ISDIR(dent->new_stat.mode)) {
		if (!_is_usable_dir(dent->d_name)) {
			MARS_DBG("ignoring directory '%s'\n", dent->d_path);
			goto done;
		}
		if (!stat_ok) {
			status = mars_mkdir(dent->d_path);
			MARS_DBG("create directory '%s' status = %d\n", dent->d_path, status);
			if (status >= 0) {
				mars_chmod(dent->d_path, dent->new_stat.mode);
				mars_lchown(dent->d_path, dent->new_stat.uid);
			}
		}
	} else if (S_ISLNK(dent->new_stat.mode) && dent->new_link) {
		if (!stat_ok || update_mtime) {
			status = mars_symlink(dent->new_link, dent->d_path, &dent->new_stat.mtime, dent->new_stat.uid);
			MARS_DBG("create symlink '%s' -> '%s' status = %d\n", dent->d_path, dent->new_link, status);
			run_trigger = true;
		}
	} else if (S_ISREG(dent->new_stat.mode) && _is_peer_logfile(dent->d_name, my_id())) {
		const char *parent_path = backskip_replace(dent->d_path, '/', false, "");
		if (likely(parent_path)) {
			struct mars_dent *parent = mars_find_dent(peer->global, parent_path);
			if (unlikely(!parent)) {
				MARS_DBG("ignoring non-existing local resource '%s'\n", parent_path);
			} else {
				status = check_logfile(peer, dent, parent, local_stat.size);
			}
			kfree(parent_path);
		}
	} else {
		MARS_DBG("ignoring '%s'\n", dent->d_path);
	}

 done:
	if (status >= 0) {
		status = run_trigger ? 1 : 0;
	}
	return status;
}

static
int run_bones(struct mars_peerinfo *peer)
{
	LIST_HEAD(tmp_list);
	struct list_head *tmp;
	unsigned long flags;
	bool run_trigger = false;
	int status = 0;

	traced_lock(&peer->lock, flags);

	list_replace_init(&peer->remote_dent_list, &tmp_list);
	
	traced_unlock(&peer->lock, flags);

	for (tmp = tmp_list.next; tmp != &tmp_list; tmp = tmp->next) {
		struct mars_dent *dent = container_of(tmp, struct mars_dent, dent_link);
		if (!dent->d_path) {
			MARS_DBG("NULL\n");
			continue;
		}
		MARS_DBG("path = '%s'\n", dent->d_path);
		status = run_bone(peer, dent);
		if (status > 0)
			run_trigger = true;
		//MARS_DBG("path = '%s' worker status = %d\n", dent->d_path, status);
	}
	mars_free_dent_all(&tmp_list);
#if 0
	if (run_trigger) {
		mars_trigger();
	}
#endif
	return status;
}

///////////////////////////////////////////////////////////////////////

// remote working infrastructure

static
void _peer_cleanup(struct mars_peerinfo *peer)
{
	if (peer->socket) {
		kernel_sock_shutdown(peer->socket, SHUT_WR);
		peer->socket = NULL;
	}
	//...
}

static
int remote_thread(void *data)
{
	struct mars_peerinfo *peer = data;
	char *real_peer;
	struct sockaddr_storage sockaddr = {};
	int status;

	if (!peer)
		return -1;

	real_peer = mars_translate_hostname(peer->global, peer->peer);
	MARS_INF("-------- remote thread starting on peer '%s' (%s)\n", peer->peer, real_peer);

	//fake_mm();

	status = mars_create_sockaddr(&sockaddr, real_peer);
	if (unlikely(status < 0)) {
		MARS_ERR("unusable remote address '%s' (%s)\n", real_peer, peer->peer);
		goto done;
	}

        while (!kthread_should_stop()) {
		LIST_HEAD(tmp_list);
		LIST_HEAD(old_list);
		unsigned long flags;
		struct mars_cmd cmd = {
			.cmd_code = CMD_GETENTS,
			.cmd_str1 = peer->path,
			.cmd_int1 = peer->maxdepth,
		};

		if (!peer->socket) {
			status = mars_create_socket(&peer->socket, &sockaddr, false);
			if (unlikely(status < 0)) {
				peer->socket = NULL;
				MARS_INF("no connection to '%s'\n", real_peer);
				msleep(5000);
				continue;
			}
			MARS_DBG("successfully opened socket to '%s'\n", real_peer);
			continue;
		}

		status = mars_send_struct(&peer->socket, &cmd, mars_cmd_meta);
		if (unlikely(status < 0)) {
			MARS_ERR("communication error on send, status = %d\n", status);
			_peer_cleanup(peer);
			msleep(5000);
			continue;
		}

		status = mars_recv_dent_list(&peer->socket, &tmp_list);
		if (unlikely(status < 0)) {
			MARS_ERR("communication error on receive, status = %d\n", status);
			_peer_cleanup(peer);
			msleep(5000);
			continue;
		}

		if (list_empty(&tmp_list)) {
			msleep(5000);
			continue;
		}
		//MARS_DBG("AHA!!!!!!!!!!!!!!!!!!!!\n");

		traced_lock(&peer->lock, flags);

		list_replace_init(&peer->remote_dent_list, &old_list);
		list_replace_init(&tmp_list, &peer->remote_dent_list);

		traced_unlock(&peer->lock, flags);

		mars_free_dent_all(&old_list);

		if (!kthread_should_stop())
			msleep(5 * 1000);
	}

	MARS_INF("-------- remote thread terminating\n");

	_peer_cleanup(peer);

done:
	//cleanup_mm();
	peer->thread = NULL;
	if (real_peer)
		kfree(real_peer);
	return 0;
}

///////////////////////////////////////////////////////////////////////

// helpers for worker functions

static int _kill_peer(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_peerinfo *peer = dent->d_private;

	if (global->global_power.button) {
		return 0;
	}
	if (!peer) {
		return 0;
	}
	if (!peer->thread) {
		MARS_ERR("oops, remote thread is not running - doing cleanup myself\n");
		_peer_cleanup(peer);
		dent->d_private = NULL;
		return -1;

	}
	kthread_stop(peer->thread);
	dent->d_private = NULL;
	return 0;
}

static int _make_peer(struct mars_global *global, struct mars_dent *dent, char *mypeer, char *path)
{
	static int serial = 0;
	struct mars_peerinfo *peer;
	int status = 0;

	if (!global->global_power.button || !dent->new_link) {
		return 0;
	}
	if (!mypeer) {
		status = _parse_args(dent, dent->new_link, 1);
		if (status < 0)
			goto done;
		mypeer = dent->d_argv[0];
	}

	MARS_DBG("peer '%s'\n", mypeer);
	if (!dent->d_private) {
		dent->d_private = kzalloc(sizeof(struct mars_peerinfo), GFP_MARS);
		if (!dent->d_private) {
			MARS_ERR("no memory for peer structure\n");
			return -1;
		}

		peer = dent->d_private;
		peer->global = global;
		peer->peer = mypeer;
		peer->path = path;
		peer->maxdepth = 2;
		spin_lock_init(&peer->lock);
		INIT_LIST_HEAD(&peer->remote_dent_list);
		//init_waitqueue_head(&peer->event);
	}

	peer = dent->d_private;
	if (!peer->thread) {
		peer->thread = kthread_create(remote_thread, peer, "mars_remote%d", serial++);
		if (unlikely(IS_ERR(peer->thread))) {
			MARS_ERR("cannot start peer thread, status = %d\n", (int)PTR_ERR(peer->thread));
			peer->thread = NULL;
			return -1;
		}
		MARS_DBG("starting peer thread\n");
		wake_up_process(peer->thread);
	}

	status = run_bones(peer);

done:
	return status;
}

static int kill_scan(void *buf, struct mars_dent *dent)
{
	return _kill_peer(buf, dent);
}

static int make_scan(void *buf, struct mars_dent *dent)
{
	MARS_DBG("path = '%s' peer = '%s'\n", dent->d_path, dent->d_rest);
	// don't connect to myself
	if (!strcmp(dent->d_rest, my_id())) {
		return 0;
	}
	return _make_peer(buf, dent, dent->d_rest, "/mars");
}


static
int kill_all(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;

	if (global->global_power.button) {
		return 0;
	}
	MARS_DBG("killing dent = '%s'\n", dent->d_path);
	mars_kill_dent(dent);
	return 0;
}

///////////////////////////////////////////////////////////////////////

// handlers / helpers for logfile rotation

struct mars_rotate {
	struct mars_global *global;
	struct mars_dent *replay_link;
	struct mars_dent *aio_dent;
	struct aio_brick *aio_brick;
	struct mars_info aio_info;
	struct trans_logger_brick *trans_brick;
	struct mars_dent *relevant_log;
	struct mars_brick *relevant_brick;
	struct mars_dent *next_relevant_log;
	struct mars_dent *current_log;
	struct mars_dent *prev_log;
	struct mars_dent *next_log;
	struct if_brick *if_brick;
	long long last_jiffies;
	loff_t start_pos;
	loff_t end_pos;
	int max_sequence;
	bool has_error;
	bool do_replay;
	bool is_primary;
};

static
void _create_new_logfile(const char *path)
{
	struct file *f;
	const int flags = O_RDWR | O_CREAT | O_EXCL;
	const int prot = 0600;
	mm_segment_t oldfs;

	oldfs = get_fs();
	set_fs(get_ds());
	f = filp_open(path, flags, prot);
	set_fs(oldfs);
	if (IS_ERR(f)) {
		int err = PTR_ERR(f);
		if (err == -EEXIST) {
			MARS_INF("logfile '%s' already exists\n", path);
		} else {
			MARS_ERR("could not create logfile '%s' status = %d\n", path, err);
		}
	} else {
		MARS_DBG("created empty logfile '%s'\n", path);
		filp_close(f, NULL);
		mars_trigger();
	}
}

static
int _update_replaylink(struct mars_dent *parent, int sequence, loff_t start_pos, loff_t end_pos, bool check_exist)
{
	struct timespec now = {};
	char *old;
	char *new;
	int status = -ENOMEM;

	if (check_exist) {
		struct kstat kstat;
		char *test = path_make("%s/log-%09d-%s", parent->d_path, sequence, my_id());
		if (!test) {
			goto out_old;
		}
		status = mars_stat(test, &kstat, true);
		kfree(test);
		if (status < 0) {
			MARS_DBG("could not update replay link to nonexisting logfile %09d\n", sequence);
			goto out_old;
		}
		status = -ENOMEM;
	}

	old = path_make("log-%09d-%s,%lld,%lld", sequence, my_id(), start_pos, end_pos - start_pos);
	if (!old) {
		goto out_old;
	}
	new = path_make("%s/replay-%s", parent->d_path, my_id());
	if (!new) {
		goto out_new;
	}

	get_lamport(&now);
	status = mars_symlink(old, new, &now, 0);
	if (status < 0) {
		MARS_ERR("cannot create symlink '%s' -> '%s' status = %d\n", old, new, status);
	} else {
		MARS_DBG("make replay symlink '%s' -> '%s' status = %d\n", old, new, status);
	}

	kfree(new);
out_new:
	kfree(old);
out_old:
	return status;
}

static
int _update_versionlink(struct mars_global *global, struct mars_dent *parent, int sequence, loff_t start_pos, loff_t end_pos)
{
	char *prev = NULL;
	struct mars_dent *prev_link = NULL;
	char *prev_digest = NULL;
	struct timespec now = {};
	char *new = NULL;
	int i;
	int status = -EINVAL;
	int len = 0;
	char data[mars_digest_size + 96];
	char digest[mars_digest_size];
	char old[mars_digest_size * 2 + 2];

	if (sequence > 1) {
		prev = path_make("%s/version-%09d-%s", parent->d_path, sequence-1, my_id());
		if (unlikely(!prev)) {
			goto out;
		}
		prev_link = mars_find_dent(global, prev);
		if (unlikely(!prev_link)) {
			MARS_ERR("cannot find previous version symlink '%s'\n", prev);
			goto out;
		}
		prev_digest = prev_link->new_link;
	}

	len = snprintf(data, sizeof(data), "%d,%lld,%lld,%s", sequence, start_pos, end_pos, prev_digest ? prev_digest : "");

	MARS_DBG("data = '%s' len = %d\n", data, len);

	mars_digest(digest, data, len);

	for (i = 0; i < mars_digest_size; i++) {
		sprintf(old + i * 2, "%02x", digest[i]);
	}
	new = path_make("%s/version-%09d-%s", parent->d_path, sequence, my_id());
	if (!new) {
		goto out;
	}

	get_lamport(&now);
	status = mars_symlink(old, new, &now, 0);
	if (status < 0) {
		MARS_ERR("cannot create symlink '%s' -> '%s' status = %d\n", old, new, status);
	} else {
		MARS_DBG("make version symlink '%s' -> '%s' status = %d\n", old, new, status);
	}

out:
	if (new) {
		kfree(new);
	}
	if (prev) {
		kfree(prev);
	}
	return status;
}

/* This must be called once at every round of logfile checking.
 */
static
int make_log_init(void *buf, struct mars_dent *parent)
{
	struct mars_global *global = buf;
	struct mars_brick *aio_brick;
	struct mars_brick *trans_brick;
	struct mars_rotate *rot = parent->d_private;
	struct mars_dent *replay_link;
	struct mars_dent *aio_dent;
	struct mars_output *output;
	const char *replay_path = NULL;
	const char *aio_path = NULL;
	const char *switch_path = NULL;
	int status;

	if (!rot) {
		rot = kzalloc(sizeof(struct mars_rotate), GFP_MARS);
		parent->d_private = rot;
		if (!rot) {
			MARS_ERR("cannot allocate rot structure\n");
			status = -ENOMEM;
			goto done;
		}
		rot->global = global;
	}

	rot->replay_link = NULL;
	rot->aio_dent = NULL;
	rot->aio_brick = NULL;
	rot->relevant_log = NULL;
	rot->relevant_brick = NULL;
	rot->next_relevant_log = NULL;
	rot->prev_log = NULL;
	rot->next_log = NULL;
	rot->max_sequence = 0;
	rot->has_error = false;

	/* Fetch the replay status symlink.
	 * It must exist, and its value will control everything.
	 */
	replay_path = path_make("%s/replay-%s", parent->d_path, my_id());
	if (unlikely(!replay_path)) {
		MARS_ERR("cannot make path\n");
		status = -ENOMEM;
		goto done;
	}

	replay_link = (void*)mars_find_dent(global, replay_path);
	if (unlikely(!replay_link || !replay_link->new_link)) {
		MARS_DBG("replay status symlink '%s' does not exist (%p)\n", replay_path, replay_link);
		status = -ENOENT;
		goto done;
	}

	status = _parse_args(replay_link, replay_link->new_link, 3);
	if (unlikely(status < 0)) {
		goto done;
	}
	rot->replay_link = replay_link;

	/* Fetch the referenced AIO dentry.
	 */
	aio_path = path_make("%s/%s", parent->d_path, replay_link->d_argv[0]);
	if (unlikely(!aio_path)) {
		MARS_ERR("cannot make path\n");
		status = -ENOMEM;
		goto done;
	}

	aio_dent = (void*)mars_find_dent(global, aio_path);
	if (unlikely(!aio_dent)) {
		MARS_DBG("logfile '%s' does not exist\n", aio_path);
		status = -ENOENT;
		if (rot->is_primary) { // try to create an empty logfile
			_create_new_logfile(aio_path);
		}
		goto done;
	}
	rot->aio_dent = aio_dent;

	/* Fetch / make the AIO brick instance
	 */
	aio_brick =
		make_brick_all(global,
			       aio_dent,
			       _set_aio_params,
			       NULL,
			       10 * HZ,
			       aio_path,
			       (const struct generic_brick_type*)&aio_brick_type,
			       (const struct generic_brick_type*[]){},
			       NULL,
			       "%s/%s",
			       (const char *[]){},
			       0,
			       parent->d_path,
			       replay_link->d_argv[0]);
	if (!aio_brick) {
		MARS_ERR("cannot access '%s'\n", aio_path);
		status = -EIO;
		goto done;
	}
	rot->aio_brick = (void*)aio_brick;

	/* Fetch the actual logfile size
	 */
	output = aio_brick->outputs[0];
	status = output->ops->mars_get_info(output, &rot->aio_info);
	if (status < 0) {
		MARS_ERR("cannot get info on '%s'\n", aio_path);
		goto done;
	}
	MARS_DBG("logfile '%s' size = %lld\n", aio_path, rot->aio_info.current_size);

	// check whether attach is allowed
	switch_path = path_make("%s/switch-%s/attach", parent->d_path, my_id());

	/* Fetch / make the transaction logger.
	 * We deliberately "forget" to connect the log input here.
	 * Will be carried out later in make_log().
	 * The final switch-on will be started in make_log_finalize().
	 */
	trans_brick =
		make_brick_all(global,
			       parent,
			       _set_trans_params,
			       NULL,
			       0,
			       aio_path,
			       (const struct generic_brick_type*)&trans_logger_brick_type,
			       (const struct generic_brick_type*[]){NULL},
			       switch_path,
			       "%s/logger", 
			       (const char *[]){"%s/data-%s"},
			       1,
			       parent->d_path,
			       parent->d_path,
			       my_id());
	status = -ENOENT;
	if (!trans_brick) {
		goto done;
	}
	rot->trans_brick = (void*)trans_brick;
	/* For safety, default is to try an (unnecessary) replay in case
	 * something goes wrong later.
	 */
	rot->do_replay = true;

	status = 0;

done:
	if (aio_path)
		kfree(aio_path);
	if (replay_path)
		kfree(replay_path);
	if (switch_path)
		kfree(switch_path);
	return status;
}

/* Note: this is strictly called in d_serial order.
 * This is important!
 */
static
int make_log(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_dent *parent = dent->d_parent;
	struct mars_rotate *rot = parent->d_private;
	struct trans_logger_brick *trans_brick;
	struct mars_dent *prev_log;
	int status = -EINVAL;

	CHECK_PTR(rot, err);

	status = 0;
	trans_brick = rot->trans_brick;
	if (!global->global_power.button || !dent->d_parent || !trans_brick || rot->has_error) {
		MARS_DBG("nothing to do rot_error = %d\n", rot->has_error);
		goto done;
	}

	/* Check for consecutiveness of logfiles
	 */
	prev_log = rot->next_log;
	if (prev_log && prev_log->d_serial + 1 != dent->d_serial) {
		MARS_ERR("transaction logs are not consecutive at '%s' (%d ~> %d)\n", dent->d_path, prev_log->d_serial, dent->d_serial);
		status = -EINVAL;
		goto done;
	}

	if (dent->d_serial > rot->max_sequence) {
		rot->max_sequence = dent->d_serial;
	}

	/* Skip any logfiles after the relevant one.
	 * This should happen only when replaying multiple logfiles
	 * in sequence, or when starting a new logfile for writing.
	 */
	status = 0;
	if (rot->relevant_log) {
		if (!rot->next_relevant_log) {
			rot->next_relevant_log = dent;
		}
		goto ok;
	}

	/* Preconditions
	 */
	if (!rot->replay_link || !rot->aio_dent || !rot->aio_brick) {
		//MARS_DBG("nothing to do on '%s'\n", dent->d_path);
		goto ok;
	}

	/* Remember the relevant logs.
	 */
	if (rot->aio_dent->d_serial == dent->d_serial) {
		rot->relevant_log = dent;
	}

ok:
	/* All ok: switch over the indicators.
	 */
	rot->prev_log = rot->next_log;
	rot->next_log = dent;

done:
	if (status < 0) {
		MARS_DBG("rot_error status = %d\n", status);
		rot->has_error = true;
	}
err:
	return status;
}


/* Internal helper. Return codes:
 * ret < 0 : error
 * ret == 0 : not relevant
 * ret == 1 : relevant, no transaction replay, switch to the next
 * ret == 2 : relevant for transaction replay
 * ret == 3 : relevant for appending
 */
static
int _check_logging_status(struct mars_rotate *rot, long long *oldpos_start, long long *oldpos_end, long long *newpos)
{
	struct mars_dent *dent = rot->relevant_log;
	struct mars_dent *parent;
	struct mars_global *global;
	int status = 0;

	if (!dent)
		goto done;
	
	status = -EINVAL;
	parent = dent->d_parent;
	CHECK_PTR(parent, done);
	global = rot->global;
	CHECK_PTR(global, done);
	CHECK_PTR(rot->replay_link, done);
	CHECK_PTR(rot->aio_brick, done);

	if (sscanf(rot->replay_link->d_argv[1], "%lld", oldpos_start) != 1) {
		MARS_ERR("bad start position argument '%s'\n", rot->replay_link->d_argv[1]);
		goto done;
	}
	if (sscanf(rot->replay_link->d_argv[2], "%lld", oldpos_end) != 1) {
		MARS_ERR("bad end position argument '%s'\n", rot->replay_link->d_argv[2]);
		goto done;
	}
	*oldpos_end += *oldpos_start;
	if (unlikely(*oldpos_end < *oldpos_start)) {
		MARS_ERR("end_pos %lld < start_pos %lld\n", *oldpos_end, *oldpos_start);
	}

	if (unlikely(rot->aio_info.current_size < *oldpos_start)) {
		MARS_ERR("oops, bad replay position attempted at logfile '%s' (file length %lld should never be smaller than requested position %lld, is your filesystem corrupted?) => please repair this by hand\n", rot->aio_dent->d_path, rot->aio_info.current_size, *oldpos_start);
		status = -EBADF;
		goto done;
	}

	status = 0;
	if (rot->aio_info.current_size > *oldpos_start || rot->aio_info.current_size < *oldpos_end) {
		MARS_DBG("transaction log replay is necessary on '%s' from %lld to %lld (dirty region ends at %lld)\n", rot->aio_dent->d_path, *oldpos_start, rot->aio_info.current_size, *oldpos_end);
		*newpos = rot->aio_info.current_size;
		status = 2;
	} else if (rot->next_relevant_log) {
		MARS_DBG("transaction log '%s' is already applied, and the next one is available for switching\n", rot->aio_dent->d_path);
		*newpos = rot->aio_info.current_size;
		status = 1;
	} else if (rot->is_primary) {
		if (!S_ISREG(dent->new_stat.mode)) {
			MARS_DBG("transaction log '%s' is a symlink, therefore a fresh local logfile must be created\n", dent->d_path);
			*newpos = rot->aio_info.current_size;
			status = 1;
		} else if (rot->aio_info.current_size > 0) {
			MARS_DBG("transaction log '%s' is already applied (would be usable for appending at position %lld, but a fresh logfile will be used for safety reasons)\n", rot->aio_dent->d_path, *oldpos_end);
			*newpos = rot->aio_info.current_size;
			status = 1;
		} else {
			MARS_DBG("empty transaction log '%s' is usable for me as a primary node\n", rot->aio_dent->d_path);
			status = 3;
		}
	} else {
		MARS_DBG("transaction log '%s' is the last one, currently fully applied\n", rot->aio_dent->d_path);
		status = 0;
	}

done:
	return status;
}


static
int _make_logging_status(struct mars_rotate *rot)
{
	struct mars_dent *dent = rot->relevant_log;
	struct mars_dent *parent;
	struct mars_global *global;
	struct trans_logger_brick *trans_brick;
	loff_t start_pos = 0;
	loff_t dirty_pos = 0;
	loff_t end_pos = 0;
	int status = 0;

	if (!dent)
		goto done;

	status = -EINVAL;
	parent = dent->d_parent;
	CHECK_PTR(parent, done);
	global = rot->global;
	CHECK_PTR(global, done);

	status = 0;
	trans_brick = rot->trans_brick;
	if (!global->global_power.button || !trans_brick || rot->has_error) {
		MARS_DBG("nothing to do rot_error = %d\n", rot->has_error);
		goto done;
	}

	/* Find current logging status.
	 */
	status = _check_logging_status(rot, &start_pos, &dirty_pos, &end_pos);
	if (status < 0) {
		goto done;
	}
	/* Relevant or not?
	 */
	switch (status) {
	case 0: // not relevant
		goto ok;
	case 1: /* Relevant, and transaction replay already finished.
		 * Allow switching over to a new logfile.
		 */
		if (!trans_brick->power.button && !trans_brick->power.led_on && trans_brick->power.led_off) {
			_update_replaylink(dent->d_parent, dent->d_serial + 1, 0, 0, !rot->is_primary);
			_update_versionlink(global, dent->d_parent, dent->d_serial + 1, 0, 0);
			trans_brick->current_pos = 0;
			rot->last_jiffies = jiffies;
			//mars_trigger();
		}
		status = -EAGAIN;
		goto done;
	case 2: // relevant for transaction replay
		MARS_DBG("replaying transaction log '%s' from %lld to %lld\n", dent->d_path, start_pos, end_pos);
		rot->do_replay = true;
		rot->start_pos = start_pos;
		rot->end_pos = end_pos;
		rot->relevant_log = dent;
		break;
	case 3: // relevant for appending
		MARS_DBG("appending to transaction log '%s'\n", dent->d_path);
		rot->do_replay = false;
		rot->start_pos = 0;
		rot->end_pos = 0;
		rot->relevant_log = dent;
		break;
	default:
		MARS_ERR("bad internal status %d\n", status);
		status = -EINVAL;
		goto done;
	}

ok:
	/* All ok: switch over the indicators.
	 */
	rot->prev_log = rot->next_log;
	rot->next_log = dent;

done:
	if (status < 0) {
		MARS_DBG("rot_error status = %d\n", status);
		rot->has_error = true;
	}
	return status;
}

static
void _change_trans(struct mars_rotate *rot)
{
	struct trans_logger_brick *trans_brick = rot->trans_brick;

	if ((trans_brick->do_replay)) {
		trans_brick->replay_start_pos = rot->start_pos;
		trans_brick->replay_end_pos = rot->end_pos;
	} else {
		trans_brick->log_start_pos = rot->start_pos;
	}
}

static
int _start_trans(struct mars_rotate *rot)
{
	struct trans_logger_brick *trans_brick = rot->trans_brick;
	struct trans_logger_input *trans_input;
	int status;

	/* Internal safety checks
	 */
	status = -EINVAL;
	if (unlikely(!trans_brick)) {
		MARS_ERR("logger instance does not exist\n");
		goto done;
	}
	trans_input = trans_brick->inputs[TL_INPUT_FW_LOG1];
	if (unlikely(!trans_input)) {
		MARS_ERR("log input does not exist\n");
		goto done;
	}
	if (unlikely(!rot->aio_brick || !rot->relevant_log)) {
		MARS_ERR("something is missing, this should not happen\n");
		goto done;
	}

	/* Update status when already working
	 */
	if (trans_brick->power.button || !trans_brick->power.led_off) {
		_change_trans(rot);
		status = 0;
		goto done;
	}

	/* Really start transaction logging now.
	 * Check some preconditions.
	 */
	if (unlikely(rot->relevant_brick)) {
		MARS_ERR("log aio brick already present, this should not happen\n");
		goto done;
	}

	/* For safety, disconnect old connection first
	 */
	if (trans_input->connect) {
		(void)generic_disconnect((void*)trans_input);
	}

	/* Open new transaction log
	 */
	rot->relevant_brick =
		make_brick_all(rot->global,
			       rot->relevant_log,
			       NULL,
			       NULL,
			       10 * HZ,
			       rot->relevant_log->d_path,
			       (const struct generic_brick_type*)&aio_brick_type,
			       (const struct generic_brick_type*[]){},
			       NULL,
			       rot->relevant_log->d_path,
			       (const char *[]){},
			       0);
	if (!rot->relevant_brick) {
		MARS_ERR("log aio brick not open\n");
		goto done;
	}

	/* Connect to new transaction log
	 */
	status = generic_connect((void*)trans_input, (void*)rot->relevant_brick->outputs[0]);
	if (status < 0) {
		goto done;
	}

	/* Supply all relevant parameters
	 */
	trans_input->sequence = rot->relevant_log->d_serial;
	trans_brick->do_replay = rot->do_replay;
	_change_trans(rot);

	/* Switch on....
	 */
	status = mars_power_button((void*)trans_brick, true, false);
	MARS_DBG("status = %d\n", status);

done:
	return status;
}

static
int _stop_trans(struct mars_rotate *rot)
{
	struct trans_logger_brick *trans_brick = rot->trans_brick;
	int status = 0;

	if (!trans_brick) {
		goto done;
	}

	/* Switch off temporarily....
	 */
	status = mars_power_button((void*)trans_brick, false, false);
	MARS_DBG("status = %d\n", status);
	if (status < 0) {
		goto done;
	}

	/* Disconnect old connection(s)
	 */
	if (trans_brick->power.led_off) {
		int i;
		for (i = TL_INPUT_FW_LOG1; i <= TL_INPUT_BW_LOG2; i++) {
			struct trans_logger_input *trans_input;
			trans_input = trans_brick->inputs[i];
			if (trans_input && trans_input->connect) {
				(void)generic_disconnect((void*)trans_input);
			}
		}
	}

done:
	return status;
}

static
int make_log_finalize(struct mars_global *global, struct mars_dent *parent)
{
	struct mars_rotate *rot = parent->d_private;
	struct trans_logger_brick *trans_brick;
	int status = -EINVAL;

	CHECK_PTR(rot, done);
	trans_brick = rot->trans_brick;
	status = 0;
	if (!trans_brick) {
		MARS_DBG("nothing to do\n");
		goto done;
	}

	/* Stopping is also possible in case of errors
	 */
	if (trans_brick->power.button && trans_brick->power.led_on && !trans_brick->power.led_off) {
		bool do_stop = true;
		if (trans_brick->do_replay) {
			if (trans_brick->replay_code >= 0) {
				int i;
				for (i = TL_INPUT_FW_LOG1; i <= TL_INPUT_FW_LOG2; i++) {
					struct trans_logger_input *trans_input;
					trans_input = trans_brick->inputs[i];
					if (!trans_input) {
						continue;
					}
					if (trans_input->replay_min_pos != trans_brick->replay_end_pos) {
						do_stop = false;
						break;
					}
				}
			}
		} else {
			do_stop =
				(rot->relevant_log && rot->relevant_log != rot->current_log) ||
				(rot->current_log && !S_ISREG(rot->current_log->new_stat.mode));
		}

		MARS_DBG("do_stop = %d\n", (int)do_stop);

		if (do_stop || (long long)jiffies > rot->last_jiffies + 5 * HZ) {
			struct trans_logger_input *old_input = NULL; // FIXME: kludge, do it right(tm)
			int i;
			for (i = TL_INPUT_FW_LOG1; i <= TL_INPUT_FW_LOG2; i++) {
				struct trans_logger_input *trans_input;
				trans_input = trans_brick->inputs[i];
				if (!trans_input || trans_input == old_input) {
					continue;
				}
				status = _update_replaylink(parent, trans_input->sequence, trans_input->replay_min_pos, trans_input->replay_max_pos, true);
				status = _update_versionlink(global, parent, trans_input->sequence, trans_input->replay_min_pos, trans_input->replay_max_pos);
				old_input = trans_input;
			}
			rot->last_jiffies = jiffies;
		}
		if (do_stop) {
			status = _stop_trans(rot);
		}
		goto done;
	}

	/* Starting is only possible when no error occurred.
	 */
	if (!rot->relevant_log || rot->has_error) {
		MARS_DBG("nothing to do\n");
		goto done;
	}

	/* Start when necessary
	 */
	if (!trans_brick->power.button && !trans_brick->power.led_on && trans_brick->power.led_off) {
		bool do_start;

		status = _make_logging_status(rot);
		if (status <= 0) {
			goto done;
		}

		do_start = (!rot->do_replay || rot->start_pos != rot->end_pos);
		MARS_DBG("do_start = %d\n", (int)do_start);

		if (do_start) {
			status = _start_trans(rot);
			rot->current_log = rot->relevant_log;
		}
	} else {
		MARS_DBG("trans_brick %d %d %d\n", trans_brick->power.button, trans_brick->power.led_on, trans_brick->power.led_off);
	}

done:
	return status;
}

///////////////////////////////////////////////////////////////////////

// specific handlers

static
int make_primary(void *buf, struct mars_dent *dent)
{
	struct mars_dent *parent;
	struct mars_rotate *rot;
	int status = -EINVAL;

	parent = dent->d_parent;
	CHECK_PTR(parent, done);
	rot = parent->d_private;
	CHECK_PTR(rot, done);

	rot->is_primary = (dent->new_link && !strcmp(dent->new_link, my_id()));
	status = 0;

done:
	return status;
}

static
int make_bio(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_brick *brick;
	int status = 0;

	if (!global->global_power.button) {
		goto done;
	}
	if (mars_find_brick(global, NULL, dent->d_path)) {
		goto done;
	}
	brick =
		make_brick_all(global,
			       dent,
			       _set_bio_params,
			       NULL,
			       10 * HZ,
			       dent->d_path,
			       (const struct generic_brick_type*)&bio_brick_type,
			       (const struct generic_brick_type*[]){},
			       NULL,
			       dent->d_path,
			       (const char *[]){},
			       0);
	if (unlikely(!brick)) {
		status = -ENXIO;
		goto done;
	}
	brick->outputs[0]->output_name = dent->d_path;
	status = mars_power_button((void*)brick, true, false);
	if (status < 0) {
		kill_all(buf, dent);
	}
 done:
	return status;
}

static int make_replay(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_dent *parent = dent->d_parent;
	int status = 0;

	if (!global->global_power.button || !parent || !dent->new_link) {
		MARS_DBG("nothing to do\n");
		goto done;
	}

	status = make_log_finalize(global, parent);
	if (status < 0) {
		MARS_DBG("logger not initialized\n");
		goto done;
	}

done:
	return status;
}

static
void _show_primary(struct mars_rotate *rot, struct mars_dent *parent)
{
	char *src;
	char *dst;
	bool ok;
	int status;

	if (!rot || !parent) {
		return;
	}

	ok = rot->is_primary;
	if (rot->if_brick && !rot->if_brick->power.led_off) {
		ok = true;
	}

	src = ok ? "1" : "0";
	dst = path_make("%s/actual-%s/is-primary", parent->d_path, my_id());
	if (!dst)
		return;
	MARS_DBG("symlink '%s' -> '%s'\n", dst, src);
	status = mars_symlink(src, dst, NULL, 0);
	if (dst)
		kfree(dst);
}


static
int make_dev(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_dent *parent = dent->d_parent;
	struct mars_rotate *rot = NULL;
	struct mars_brick *dev_brick;
	struct if_brick *_dev_brick;
	int status = 0;

	if (!parent || !dent->new_link) {
		MARS_ERR("nothing to do\n");
		return -EINVAL;
	}
	if (!global->global_power.button) {
		MARS_DBG("nothing to do\n");
		goto done;
	}
	rot = parent->d_private;
	if (!rot) {
		MARS_DBG("nothing to do\n");
		goto done;
	}
	if (!rot->trans_brick || rot->trans_brick->do_replay || !rot->trans_brick->power.led_on || rot->trans_brick->power.led_off) {
		MARS_DBG("transaction logger not ready for writing\n");
		goto done;
	}

	status = _parse_args(dent, dent->new_link, 1);
	if (status < 0) {
		MARS_DBG("fail\n");
		goto done;
	}

	dev_brick =
		make_brick_all(global,
			       dent,
			       _set_if_params,
			       NULL,
			       10 * HZ,
			       dent->d_argv[0],
			       (const struct generic_brick_type*)&if_brick_type,
			       (const struct generic_brick_type*[]){(const struct generic_brick_type*)&trans_logger_brick_type},
			       rot->is_primary ? NULL : "", // KLUDGE
			       "%s/linuxdev-%s", 
			       (const char *[]){"%s/logger"},
			       1,
			       parent->d_path,
			       dent->d_argv[0],
			       parent->d_path);
	rot->if_brick = (void*)dev_brick;
	if (!dev_brick) {
		MARS_DBG("device not shown\n");
		goto done;
	}
	dev_brick->status_level = 1;
	_dev_brick = (void*)dev_brick;
#if 0
	if (_dev_brick->has_closed) {
		_dev_brick->has_closed = false;
		MARS_INF("rotating logfile for '%s'\n", parent->d_name);
		status = mars_power_button((void*)rot->trans_brick, false);
		rot->relevant_log = NULL;
	}
#endif

done:
	_show_primary(rot, parent);
	return status;
}

static int _make_direct(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	struct mars_brick *brick;
	char *src_path = NULL;
	int status;
	bool do_dealloc = false;

	if (!global->global_power.button || !dent->d_parent || !dent->new_link) {
		return 0;
	}
	status = _parse_args(dent, dent->new_link, 2);
	if (status < 0) {
		MARS_DBG("parse status = %d\n", status);
		goto done;
	}
	src_path = dent->d_argv[0];
	if (src_path[0] != '/') {
		src_path = path_make("%s/%s", dent->d_parent->d_path, dent->d_argv[0]);
		do_dealloc = true;
	}
	brick = 
		make_brick_all(global,
			       dent,
			       _set_bio_params,
			       NULL,
			       10 * HZ,
			       src_path,
			       (const struct generic_brick_type*)&bio_brick_type,
			       (const struct generic_brick_type*[]){},
			       NULL,
			       "%s",
			       (const char *[]){},
			       0,
			       src_path);
	status = -1;
	if (!brick) {
		MARS_DBG("fail\n");
		goto done;
	}

	brick = 
		make_brick_all(global,
			       dent,
			       _set_if_params,
			       NULL,
			       10 * HZ,
			       dent->d_argv[1],
			       (const struct generic_brick_type*)&if_brick_type,
			       (const struct generic_brick_type*[]){NULL},
			       NULL,
			       "%s/linuxdev-%s",
			       (const char *[]){ "%s" },
			       1,
			       dent->d_parent->d_path,
			       dent->d_argv[1],
			       src_path);
	status = -1;
	if (!brick) {
		MARS_DBG("fail\n");
		goto done;
	}

	status = 0;
done:
	MARS_DBG("status = %d\n", status);
	if (do_dealloc)
		kfree(src_path);
	return status;
}

static int _make_copy(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	const char *switch_path = NULL;
	const char *copy_path = NULL;
	int status;

	if (!global->global_power.button || !dent->d_parent || !dent->new_link) {
		return 0;
	}
	status = _parse_args(dent, dent->new_link, 2);
	if (status < 0) {
		goto done;
	}
	copy_path = backskip_replace(dent->d_path, '/', true, "/copy-");
	if (unlikely(!copy_path)) {
		status = -ENOMEM;
		goto done;
	}
	// check whether connection is allowed
	switch_path = path_make("%s/switch-%s/connect", dent->d_parent->d_path, my_id());

	status = __make_copy(global, dent, switch_path, copy_path, dent->d_parent->d_path, (const char**)dent->d_argv, -1, NULL);

done:
	MARS_DBG("status = %d\n", status);
	if (copy_path)
		kfree(copy_path);
	if (switch_path)
		kfree(switch_path);
	return status;
}

static int make_sync(void *buf, struct mars_dent *dent)
{
	struct mars_global *global = buf;
	loff_t start_pos = 0;
	struct mars_dent *connect_dent;
	char *peer;
	struct copy_brick *copy = NULL;
	char *tmp = NULL;
	const char *switch_path = NULL;
	const char *copy_path = NULL;
	const char *src = NULL;
	const char *dst = NULL;
	int status;

	if (!global->global_power.button || !dent->d_parent || !dent->new_link) {
		return 0;
	}

	/* Analyze replay position
	 */
	status = sscanf(dent->new_link, "%lld", &start_pos);
	if (status != 1) {
		MARS_ERR("bad syncstatus symlink syntax '%s' (%s)\n", dent->new_link, dent->d_path);
		status = -EINVAL;
		goto done;
	}

	/* Determine peer
	 */
	tmp = path_make("%s/connect-%s", dent->d_parent->d_path, my_id());
	status = -ENOMEM;
	if (unlikely(!tmp))
		goto done;
	connect_dent = (void*)mars_find_dent(global, tmp);
	if (!connect_dent || !connect_dent->new_link) {
		MARS_ERR("cannot determine peer, symlink '%s' is missing\n", tmp);
		status = -ENOENT;
		goto done;
	}
	peer = connect_dent->new_link;

	/* Start copy
	 */
	src = path_make("data-%s@%s", peer, peer);
	dst = path_make("data-%s", my_id());
	copy_path = backskip_replace(dent->d_path, '/', true, "/copy-");

	// check whether connection is allowed
	switch_path = path_make("%s/switch-%s/sync", dent->d_parent->d_path, my_id());

	status = -ENOMEM;
	if (unlikely(!src || !dst || !copy_path || !switch_path))
		goto done;

	MARS_DBG("starting initial sync '%s' => '%s'\n", src, dst);

	{
		const char *argv[2] = { src, dst };
		status = __make_copy(global, dent, switch_path, copy_path, dent->d_parent->d_path, argv, start_pos, &copy);
	}

	/* Update syncstatus symlink
	 */
	if (status >= 0 && copy &&
	   ((copy->power.button && copy->power.led_on) ||
	    (copy->copy_last == copy->copy_end && copy->copy_end > 0))) {
		kfree(src);
		kfree(dst);
		src = path_make("%lld", copy->copy_last);
		dst = path_make("%s/syncstatus-%s", dent->d_parent->d_path, my_id());
		status = -ENOMEM;
		if (unlikely(!src || !dst))
			goto done;
		status = mars_symlink(src, dst, NULL, 0);
	}

done:
	MARS_DBG("status = %d\n", status);
	if (tmp)
		kfree(tmp);
	if (src)
		kfree(src);
	if (dst)
		kfree(dst);
	if (copy_path)
		kfree(copy_path);
	if (switch_path)
		kfree(switch_path);
	return status;
}

///////////////////////////////////////////////////////////////////////

// the order is important!
enum {
	// root element: this must have index 0
	CL_ROOT,
	// replacement for DNS in kernelspace
	CL_IPS,
	CL_PEERS,
	// resource definitions
	CL_RESOURCE,
	CL_DEFAULTS0,
	CL_DEFAULTS,
	CL_DEFAULTS_ITEMS0,
	CL_DEFAULTS_ITEMS,
	CL_SWITCH,
	CL_SWITCH_ITEMS,
	CL_ACTUAL,
	CL_ACTUAL_ITEMS,
	CL_CONNECT,
	CL_SIZE,
	CL_DATA,
	CL_PRIMARY,
	CL__FILE,
	CL_SYNC,
	CL__COPY,
	CL__DIRECT,
	CL_VERSION,
	CL_LOG,
	CL_REPLAYSTATUS,
	CL_DEVICE,
};

/* Please keep the order the same as in the enum.
 */
static const struct light_class light_classes[] = {
	/* Placeholder for root node /mars/
	 */
	[CL_ROOT] = {
	},

	/* Directory containing the addresses of all peers
	 */
	[CL_IPS] = {
		.cl_name = "ips",
		.cl_len = 3,
		.cl_type = 'd',
		.cl_father = CL_ROOT,
	},
	/* Anyone participating in a MARS cluster must
	 * be named here (symlink pointing to the IP address).
	 * We have no DNS in kernel space.
	 */
	[CL_PEERS] = {
		.cl_name = "ip-",
		.cl_len = 3,
		.cl_type = 'l',
		.cl_father = CL_IPS,
#if 1
		.cl_forward = make_scan,
		.cl_backward = kill_scan,
#endif
	},

	/* Directory containing all items of a resource
	 */
	[CL_RESOURCE] = {
		.cl_name = "resource-",
		.cl_len = 9,
		.cl_type = 'd',
		.cl_father = CL_ROOT,
		.cl_forward = make_log_init,
		.cl_backward = NULL,
	},

	/* Subdirectory for defaults...
	 */
	[CL_DEFAULTS0] = {
		.cl_name = "defaults",
		.cl_len = 8,
		.cl_type = 'd',
		.cl_hostcontext = false,
		.cl_father = CL_RESOURCE,
		.cl_forward = NULL,
		.cl_backward = NULL,
	},
	[CL_DEFAULTS] = {
		.cl_name = "defaults-",
		.cl_len = 9,
		.cl_type = 'd',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = NULL,
		.cl_backward = NULL,
	},
	/* ... and its contents
	 */
	[CL_DEFAULTS_ITEMS0] = {
		.cl_name = "",
		.cl_len = 0, // catch any
		.cl_type = 'l',
		.cl_father = CL_DEFAULTS0,
		.cl_forward = NULL,
		.cl_backward = NULL,
	},
	[CL_DEFAULTS_ITEMS] = {
		.cl_name = "",
		.cl_len = 0, // catch any
		.cl_type = 'l',
		.cl_father = CL_DEFAULTS,
		.cl_forward = NULL,
		.cl_backward = NULL,
	},

	/* Subdirectory for controlling items...
	 */
	[CL_SWITCH] = {
		.cl_name = "switch-",
		.cl_len = 7,
		.cl_type = 'd',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = NULL,
		.cl_backward = NULL,
	},
	/* ... and its contents
	 */
	[CL_SWITCH_ITEMS] = {
		.cl_name = "",
		.cl_len = 0, // catch any
		.cl_type = 'l',
		.cl_father = CL_SWITCH,
		.cl_forward = NULL,
		.cl_backward = NULL,
	},

	/* Subdirectory for actual state
	 */
	[CL_ACTUAL] = {
		.cl_name = "actual-",
		.cl_len = 7,
		.cl_type = 'd',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = NULL,
		.cl_backward = NULL,
	},
	/* ... and its contents
	 */
	[CL_ACTUAL_ITEMS] = {
		.cl_name = "",
		.cl_len = 0, // catch any
		.cl_type = 'l',
		.cl_father = CL_ACTUAL,
		.cl_forward = NULL,
		.cl_backward = NULL,
	},


	/* Symlink indicating the current peer
	 */
	[CL_CONNECT] = {
		.cl_name = "connect-",
		.cl_len = 8,
		.cl_type = 'l',
		.cl_hostcontext = false, // not used here
		.cl_father = CL_RESOURCE,
		.cl_forward = NULL,
		.cl_backward = NULL,
	},
	/* Symlink indiating the (common) size of the resource
	 */
	[CL_SIZE] = {
		.cl_name = "size",
		.cl_len = 4,
		.cl_type = 'l',
		.cl_hostcontext = false,
		.cl_father = CL_RESOURCE,
		.cl_forward = NULL,
		.cl_backward = NULL,
	},
	/* File or symlink to the real device / real (sparse) file
	 * when hostcontext is missing, the corresponding peer will
	 * not participate in that resource.
	 */
	[CL_DATA] = {
		.cl_name = "data-",
		.cl_len = 5,
		.cl_type = 'F',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_bio,
		.cl_backward = kill_all,
	},
	/* Symlink pointing to the name of the primary node
	 */
	[CL_PRIMARY] = {
		.cl_name = "primary",
		.cl_len = 7,
		.cl_type = 'l',
		.cl_hostcontext = false,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_primary,
		.cl_backward = NULL,
	},
	/* Only for testing: open local file
	 */
	[CL__FILE] = {
		.cl_name = "_file-",
		.cl_len = 6,
		.cl_type = 'F',
		.cl_serial = true,
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_bio,
		.cl_backward = kill_all,
	},
	/* symlink indicating the current status / end
	 * of initial data sync.
	 */
	[CL_SYNC] = {
		.cl_name = "syncstatus-",
		.cl_len = 11,
		.cl_type = 'l',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_sync,
		.cl_backward = kill_all,
	},
	/* Only for testing: make a copy instance
	 */
	[CL__COPY] = {
		.cl_name = "_copy-",
		.cl_len = 6,
		.cl_type = 'l',
		.cl_serial = true,
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = _make_copy,
		.cl_backward = kill_all,
	},
	/* Only for testing: access local data
	 */
	[CL__DIRECT] = {
		.cl_name = "_direct-",
		.cl_len = 8,
		.cl_type = 'l',
		.cl_serial = true,
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = _make_direct,
		.cl_backward = kill_all,
	},

	/* Passive symlink indicating the split-brain crypto hash
	 */
	[CL_VERSION] = {
		.cl_name = "version-",
		.cl_len = 8,
		.cl_type = 'l',
		.cl_serial = true,
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = NULL,
		.cl_backward = NULL,
	},
	/* Logfiles for transaction logger
	 */
	[CL_LOG] = {
		.cl_name = "log-",
		.cl_len = 4,
		.cl_type = 'F',
		.cl_serial = true,
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
#if 1
		.cl_forward = make_log,
		.cl_backward = kill_all,
#endif
	},
	/* Symlink indicating the last state of
	 * transaction log replay.
	 */
	[CL_REPLAYSTATUS] = {
		.cl_name = "replay-",
		.cl_len = 7,
		.cl_type = 'l',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_replay,
		.cl_backward = NULL,
	},

	/* Name of the device appearing at the primary
	 */
	[CL_DEVICE] = {
		.cl_name = "device-",
		.cl_len = 7,
		.cl_type = 'l',
		.cl_hostcontext = true,
		.cl_father = CL_RESOURCE,
		.cl_forward = make_dev,
		.cl_backward = kill_all,
	},
	{}
};

/* Helper routine to pre-determine the relevance of a name from the filesystem.
 */
static int light_checker(struct mars_dent *parent, const char *_name, int namlen, unsigned int d_type, int *prefix, int *serial)
{
	int class;
	int status = -2;
#ifdef MARS_DEBUGGING
	const char *name = kstrdup(_name, GFP_MARS);
	if (!name)
		return -ENOMEM;
#else
	const char *name = _name;
#endif

	//MARS_DBG("trying '%s' '%s'\n", path, name);
	for (class = CL_ROOT + 1; ; class++) {
		const struct light_class *test = &light_classes[class];
		int len = test->cl_len;
		if (!test->cl_name) { // end of table
			break;
		}

		//MARS_DBG("   testing class '%s'\n", test->cl_name);

#ifdef MARS_DEBUGGING
		if (len != strlen(test->cl_name)) {
			MARS_ERR("internal table '%s' mismatch: %d != %d\n", test->cl_name, len, (int)strlen(test->cl_name));
			len = strlen(test->cl_name);
		}
#endif

		if (test->cl_father &&
		   (!parent || parent->d_class != test->cl_father)) {
			continue;
		}

		if (len > 0 &&
		   (namlen < len || memcmp(name, test->cl_name, len))) {
			continue;
		}

		//MARS_DBG("path '%s/%s' matches class %d '%s'\n", path, name, class, test->cl_name);

		// check special contexts
		if (test->cl_serial) {
			int plus = 0;
			int count;
			count = sscanf(name+len, "%d%n", serial, &plus);
			if (count < 1) {
				//MARS_DBG("'%s' serial number mismatch at '%s'\n", name, name+len);
				status = -1;
				goto done;
			}
			//MARS_DBG("'%s' serial number = %d\n", name, *serial);
			len += plus;
			if (name[len] == '-')
				len++;
		}
		if (prefix)
			*prefix = len;
		if (test->cl_hostcontext) {
			if (memcmp(name+len, my_id(), namlen-len)) {
				//MARS_DBG("context mismatch '%s' at '%s'\n", name, name+len);
				status = -1;
				goto done;
			}
		}

		// all ok
		status = class;
		goto done;
	}

	//MARS_DBG("no match for '%s' '%s'\n", path, name);

done:
#ifdef MARS_DEBUGGING
	if (name)
		kfree(name);
#endif
	return status;
}

/* Do some syntactic checks, then delegate work to the real worker functions
 * from the light_classes[] table.
 */
static int light_worker(struct mars_global *global, struct mars_dent *dent, bool direction)
{
	light_worker_fn worker;
	int class = dent->d_class;

	if (class < 0 || class >= sizeof(light_classes)/sizeof(struct light_class)) {
		MARS_ERR_ONCE(dent, "bad internal class %d of '%s'\n", class, dent->d_path);
		return -EINVAL;
	}
	switch (light_classes[class].cl_type) {
	case 'd':
		if (!S_ISDIR(dent->new_stat.mode)) {
			MARS_ERR_ONCE(dent, "'%s' should be a directory, but is something else\n", dent->d_path);
			return -EINVAL;
		}
		break;
	case 'f':
		if (!S_ISREG(dent->new_stat.mode)) {
			MARS_ERR_ONCE(dent, "'%s' should be a regular file, but is something else\n", dent->d_path);
			return -EINVAL;
		}
		break;
	case 'F':
		if (!S_ISREG(dent->new_stat.mode) && !S_ISLNK(dent->new_stat.mode)) {
			MARS_ERR_ONCE(dent, "'%s' should be a regular file or a symlink, but is something else\n", dent->d_path);
			return -EINVAL;
		}
		break;
	case 'l':
		if (!S_ISLNK(dent->new_stat.mode)) {
			MARS_ERR_ONCE(dent, "'%s' should be a symlink, but is something else\n", dent->d_path);
			return -EINVAL;
		}
		break;
	}
	if (likely(class > CL_ROOT)) {
		int father = light_classes[class].cl_father;
		if (father == CL_ROOT) {
			if (unlikely(dent->d_parent)) {
				MARS_ERR_ONCE(dent, "'%s' is not at the root of the hierarchy\n", dent->d_path);
				return -EINVAL;
			}
		} else if (unlikely(!dent->d_parent || dent->d_parent->d_class != father)) {
			MARS_ERR_ONCE(dent, "last component '%s' from '%s' is at the wrong position in the hierarchy (class = %d, parent_class = %d, parent = '%s')\n", dent->d_name, dent->d_path, father, dent->d_parent ? dent->d_parent->d_class : -9999, dent->d_parent ? dent->d_parent->d_path : "");
			return -EINVAL;
		}
	}
	if (direction) {
		worker = light_classes[class].cl_backward;
	} else {
		worker = light_classes[class].cl_forward;
	}
	if (worker) {
		int status;
		//MARS_DBG("working %s on '%s' rest='%s'\n", direction ? "backward" : "forward", dent->d_path, dent->d_rest);
		status = worker(global, (void*)dent);
		MARS_DBG("worked %s on '%s', status = %d\n", direction ? "backward" : "forward", dent->d_path, status);
		return status;
	}
	return 0;
}

static
void _show_status(struct mars_global *global)
{
	struct list_head *tmp;
	
	down_read(&global->brick_mutex);
	for (tmp = global->brick_anchor.next; tmp != &global->brick_anchor; tmp = tmp->next) {
		struct mars_brick *test;
		const char *path;
		char *src;
		char *dst;
		int status;
		
		test = container_of(tmp, struct mars_brick, global_brick_link);
		if (test->status_level <= 0)
			continue;
		
		path = test->brick_path;
		if (!path) {
			MARS_DBG("bad path\n");
			continue;
		}
		if (*path != '/') {
			MARS_DBG("bogus path '%s'\n", path);
			continue;
		}

		src = test->power.led_on ? "1" : "0";
		dst = backskip_replace(path, '/', true, "/actual-%s/", my_id());
		if (!dst)
			continue;

		status = mars_symlink(src, dst, NULL, 0);
		MARS_DBG("status symlink '%s' -> '%s' status = %d\n", dst, src, status);
		if (test->status_level > 1) {
			char perc[8];
			char *dst2 = path_make("%s.percent", dst);
			if (likely(dst2)) {
				snprintf(perc, sizeof(perc), "%d", test->power.percent_done);
				status = mars_symlink(perc, dst2, NULL, 0);
				MARS_DBG("percent symlink '%s' -> '%s' status = %d\n", dst2, src, status);
				kfree(dst2);
			}
		}
		kfree(dst);
	}
	up_read(&global->brick_mutex);
}

#ifdef STAT_DEBUGGING
static
void _show_statist(struct mars_global *global)
{
	struct list_head *tmp;
	int dent_count = 0;
	int brick_count = 0;
	
	MARS_STAT("================================== dents:\n");
	down_read(&global->dent_mutex);
	for (tmp = global->dent_anchor.next; tmp != &global->dent_anchor; tmp = tmp->next) {
		struct mars_dent *dent;
		dent = container_of(tmp, struct mars_dent, dent_link);
		MARS_STAT("dent %d '%s' '%s' stamp=%ld.%09ld\n", dent->d_class, dent->d_path, dent->new_link ? dent->new_link : "", dent->new_stat.mtime.tv_sec, dent->new_stat.mtime.tv_nsec);
		dent_count++;
	}
	up_read(&global->dent_mutex);
	MARS_STAT("================================== bricks:\n");
	down_read(&global->brick_mutex);
	for (tmp = global->brick_anchor.next; tmp != &global->brick_anchor; tmp = tmp->next) {
		struct mars_brick *test;
		int i;
		test = container_of(tmp, struct mars_brick, global_brick_link);
		if (brick_count) {
			MARS_STAT("---------\n");
		}
		MARS_STAT("brick type = %s path = '%s' name = '%s' level = %d button = %d off = %d on = %d\n", test->type->type_name, test->brick_path, test->brick_name, test->status_level, test->power.button, test->power.led_off, test->power.led_on);
		brick_count++;
		if (test->ops && test->ops->brick_statistics) {
			char *info = test->ops->brick_statistics(test, 0);
			if (info) {
				MARS_STAT("%s", info);
				kfree(info);
			}
		}
		for (i = 0; i < test->nr_inputs; i++) {
			struct mars_input *input = test->inputs[i];
			struct mars_output *output = input ? input->connect : NULL;
			if (output) {
				MARS_STAT("   input %d connected with %s path = '%s' name = '%s'\n", i, output->brick->type->type_name, output->brick->brick_path, output->brick->brick_name);
			} else {
				MARS_STAT("   input %d not connected\n", i);
			}
		}
	}
	up_read(&global->brick_mutex);
	
	MARS_INF("==================== STATISTICS: %d dents, %d bricks\n", dent_count, brick_count);
}
#endif

static int light_thread(void *data)
{
	char *id = my_id();
	int status = 0;
	struct mars_global global = {
		.dent_anchor = LIST_HEAD_INIT(global.dent_anchor),
		.brick_anchor = LIST_HEAD_INIT(global.brick_anchor),
		.global_power = {
			.button = true,
		},
		.dent_mutex = __RWSEM_INITIALIZER(global.dent_mutex),
		.brick_mutex = __RWSEM_INITIALIZER(global.brick_mutex),
		.main_event = __WAIT_QUEUE_HEAD_INITIALIZER(global.main_event),
	};
	mars_global = &global; // TODO: cleanup, avoid stack

	if (!id || strlen(id) < 2) {
		MARS_ERR("invalid hostname\n");
		status = -EFAULT;
		goto done;
	}	

	//fake_mm();

	MARS_INF("-------- starting as host '%s' ----------\n", id);

        while (global.global_power.button || !list_empty(&global.brick_anchor)) {
		int status;
		global.global_power.button = !kthread_should_stop();

		status = mars_dent_work(&global, "/mars", sizeof(struct mars_dent), light_checker, light_worker, &global, 3);
		MARS_DBG("worker status = %d\n", status);

		_show_status(&global);
#ifdef STAT_DEBUGGING
		_show_statist(&global);
#endif

		msleep(500);

		wait_event_interruptible_timeout(global.main_event, global.main_trigger, 10 * HZ);
		global.main_trigger = false;
	}

done:
	MARS_INF("-------- cleaning up ----------\n");

	mars_free_dent_all(&global.dent_anchor);

	mars_global = NULL;
	main_thread = NULL;

	MARS_INF("-------- done status = %d ----------\n", status);
	//cleanup_mm();
	return status;
}

static void __exit exit_light(void)
{
	// TODO: make this thread-safe.
	struct task_struct *thread = main_thread;
	if (thread) {
		main_thread = NULL;
		MARS_DBG("====================== stopping everything...\n");
		kthread_stop_nowait(thread);
		mars_trigger();
		kthread_stop(thread);
		put_task_struct(thread);
		MARS_DBG("====================== stopped everything.\n");
	}
}

static int __init init_light(void)
{
	struct task_struct *thread;
	thread = kthread_create(light_thread, NULL, "mars_light");
	if (IS_ERR(thread)) {
		return PTR_ERR(thread);
	}
	get_task_struct(thread);
	main_thread = thread;
	wake_up_process(thread);
#if 1 // quirk: bump the memory reserve limits. TODO: determine right values.
	{
		extern int min_free_kbytes;
		min_free_kbytes *= 4;
		setup_per_zone_wmarks();
	}
#endif
	return 0;
}

// force module loading
const void *dummy1 = &client_brick_type;
const void *dummy2 = &server_brick_type;

MODULE_DESCRIPTION("MARS Light");
MODULE_AUTHOR("Thomas Schoebel-Theuer <tst@1und1.de>");
MODULE_LICENSE("GPL");

module_init(init_light);
module_exit(exit_light);
