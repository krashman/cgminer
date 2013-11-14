/*
 * Copyright 2013 Con Kolivas
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include "miner.h"
#include "driver-bitfury.h"
#include "sha2.h"

/* Wait longer 1/3 longer than it would take for a full nonce range */
#define BF1WAIT 1600
#define BF1MSGSIZE 7
#define BF1INFOSIZE 14

static void bf1_empty_buffer(struct cgpu_info *bitfury)
{
	char buf[512];
	int amount;

	do {
		usb_read_once(bitfury, buf, 512, &amount, C_BF1_FLUSH);
	} while (amount);
}

static bool bf1_open(struct cgpu_info *bitfury)
{
	uint32_t buf[2];
	int err;

	bf1_empty_buffer(bitfury);
	/* Magic sequence to reset device only really needed for windows but
	 * harmless on linux. */
	buf[0] = 0x80250000;
	buf[1] = 0x00000800;
	err = usb_transfer(bitfury, 0, 9, 1, 0, C_ATMEL_RESET);
	if (!err)
		err = usb_transfer(bitfury, 0x21, 0x22, 0, 0, C_ATMEL_OPEN);
	if (!err) {
		err = usb_transfer_data(bitfury, 0x21, 0x20, 0x0000, 0, buf,
					BF1MSGSIZE, C_ATMEL_INIT);
	}

	if (err < 0) {
		applog(LOG_INFO, "%s %d: Failed to open with error %s", bitfury->drv->name,
		       bitfury->device_id, libusb_error_name(err));
	}
	return (err == BF1MSGSIZE);
}

static void bf1_close(struct cgpu_info *bitfury)
{
	bf1_empty_buffer(bitfury);
}

static void bf1_identify(struct cgpu_info *bitfury)
{
	int amount;

	usb_write(bitfury, "L", 1, &amount, C_BF1_IDENTIFY);
}

static void bitfury_identify(struct cgpu_info *bitfury)
{
	struct bitfury_info *info = bitfury->device_data;

	switch(info->ident) {
		case IDENT_BF1:
			bf1_identify(bitfury);
			break;
		case IDENT_BXF:
		default:
			break;
	}
}

static bool bf1_getinfo(struct cgpu_info *bitfury, struct bitfury_info *info)
{
	int amount, err;
	char buf[16];

	err = usb_write(bitfury, "I", 1, &amount, C_BF1_REQINFO);
	if (err) {
		applog(LOG_INFO, "%s %d: Failed to write REQINFO",
		       bitfury->drv->name, bitfury->device_id);
		return false;
	}
	err = usb_read(bitfury, buf, BF1INFOSIZE, &amount, C_BF1_GETINFO);
	if (err) {
		applog(LOG_INFO, "%s %d: Failed to read GETINFO",
		       bitfury->drv->name, bitfury->device_id);
		return false;
	}
	if (amount != BF1INFOSIZE) {
		applog(LOG_INFO, "%s %d: Getinfo received %d bytes instead of %d",
		       bitfury->drv->name, bitfury->device_id, amount, BF1INFOSIZE);
		return false;
	}
	info->version = buf[1];
	memcpy(&info->product, buf + 2, 8);
	memcpy(&info->serial, buf + 10, 4);

	applog(LOG_INFO, "%s %d: Getinfo returned version %d, product %s serial %08x", bitfury->drv->name,
	       bitfury->device_id, info->version, info->product, info->serial);
	bf1_empty_buffer(bitfury);
	return true;
}

static bool bf1_reset(struct cgpu_info *bitfury)
{
	int amount, err;
	char buf[16];

	err = usb_write(bitfury, "R", 1, &amount, C_BF1_REQRESET);
	if (err) {
		applog(LOG_INFO, "%s %d: Failed to write REQRESET",
		       bitfury->drv->name, bitfury->device_id);
		return false;
	}
	err = usb_read_timeout(bitfury, buf, BF1MSGSIZE, &amount, BF1WAIT,
			       C_BF1_GETRESET);
	if (err) {
		applog(LOG_INFO, "%s %d: Failed to read GETRESET",
		       bitfury->drv->name, bitfury->device_id);
		return false;
	}
	if (amount != BF1MSGSIZE) {
		applog(LOG_INFO, "%s %d: Getreset received %d bytes instead of %d",
		       bitfury->drv->name, bitfury->device_id, amount, BF1MSGSIZE);
		return false;
	}
	applog(LOG_DEBUG, "%s %d: Getreset returned %s", bitfury->drv->name,
	       bitfury->device_id, buf);
	bf1_empty_buffer(bitfury);
	return true;
}

static bool bxf_detect_one(struct cgpu_info *bitfury, struct bitfury_info *info)
{
	if (!add_cgpu(bitfury))
		quit(1, "Failed to add_cgpu in bxf_detect_one");

	update_usb_stats(bitfury);
	applog(LOG_INFO, "%s %d: Successfully initialised %s",
	       bitfury->drv->name, bitfury->device_id, bitfury->device_path);

	/* FIXME Do some testing here */
	return true;
}

static bool bf1_detect_one(struct cgpu_info *bitfury, struct bitfury_info *info)
{
	if (!bf1_open(bitfury))
		goto out_close;

	/* Send getinfo request */
	if (!bf1_getinfo(bitfury, info))
		goto out_close;

	/* Send reset request */
	if (!bf1_reset(bitfury))
		goto out_close;

	bf1_identify(bitfury);
	bf1_empty_buffer(bitfury);

	if (!add_cgpu(bitfury))
		quit(1, "Failed to add_cgpu in bf1_detect_one");

	update_usb_stats(bitfury);
	applog(LOG_INFO, "%s %d: Successfully initialised %s",
	       bitfury->drv->name, bitfury->device_id, bitfury->device_path);

	/* This does not artificially raise hashrate, it simply allows the
	 * hashrate to adapt quickly on starting. */
	info->total_nonces = 1;

	return true;
out_close:
	bf1_close(bitfury);
	return false;
}

static bool bitfury_detect_one(struct libusb_device *dev, struct usb_find_devices *found)
{
	struct cgpu_info *bitfury;
	struct bitfury_info *info;
	enum sub_ident ident;
	bool ret = false;

	bitfury = usb_alloc_cgpu(&bitfury_drv, 1);

	if (!usb_init(bitfury, dev, found))
		goto out;
	applog(LOG_INFO, "%s %d: Found at %s", bitfury->drv->name,
	       bitfury->device_id, bitfury->device_path);

	info = calloc(sizeof(struct bitfury_info), 1);
	if (!info)
		quit(1, "Failed to calloc info in bitfury_detect_one");
	bitfury->device_data = info;
	info->ident = ident = usb_ident(bitfury);
	switch (ident) {
		case IDENT_BF1:
			ret = bf1_detect_one(bitfury, info);
			break;
		case IDENT_BXF:
			ret = bxf_detect_one(bitfury, info);
			break;
		default:
			applog(LOG_INFO, "%s %d: Unrecognised bitfury device",
			       bitfury->drv->name, bitfury->device_id);
			break;
	}

	if (!ret) {
		free(info);
		usb_uninit(bitfury);
out:
		bitfury = usb_free_cgpu(bitfury);
	}
	return ret;
}

static void bitfury_detect(bool __maybe_unused hotplug)
{
	usb_detect(&bitfury_drv, bitfury_detect_one);
}

static void parse_bxf_submit(struct cgpu_info *bitfury, struct bitfury_info *info, char *buf)
{
	uint32_t nonce, timestamp;
	unsigned int workid;

	if (!sscanf(&buf[7], "%08x %u %08x", &nonce, &workid, &timestamp)) {
		applog(LOG_WARNING, "%s %d: Failed to parse submit response",
		       bitfury->drv->name, bitfury->device_id);
		return;
	}
}

static void parse_bxf_temp(struct cgpu_info *bitfury, struct bitfury_info *info, char *buf)
{
	unsigned int temp;

	if (!sscanf(&buf[5], "%u", &temp)) {
		applog(LOG_INFO, "%s %d: Failed to parse temperature",
		       bitfury->drv->name, bitfury->device_id);
		return;
	}
	mutex_lock(&info->lock);
	info->temperature = (double)temp / 10;
	mutex_unlock(&info->lock);
}

static void bxf_update_work(struct cgpu_info *bitfury, struct bitfury_info *info);

static void *bxf_get_results(void *userdata)
{
	struct cgpu_info *bitfury = userdata;
	struct bitfury_info *info = bitfury->device_data;
	char threadname[24], buf[512];
	int err, amount, len;

	snprintf(threadname, 24, "bxf_recv/%d", bitfury->device_id);

	/* We operate the device at lowest diff since it's not a lot of results
	 * to process and gives us a better indicator of the nonce return rate
	 * and hardware errors. */
	sprintf(buf, "target ffffffff\n");
	len = strlen(buf);
	err = usb_write(bitfury, buf, len, &amount, C_BXF_TARGET);
	if (!err || amount != len) {
		applog(LOG_WARNING, "%s %d: Error %d sending work sent %d of %d", bitfury->drv->name,
		       bitfury->device_id, err, amount, len);
		goto out;
	}
	/* Read thread sends the first work item to get the device started
	 * since it will roll ntime and make work itself from there on. */
	bxf_update_work(bitfury, info);

	while (likely(!bitfury->shutdown)) {
		if (unlikely(bitfury->usbinfo.nodev))
			break;

		err = usb_read_nl(bitfury, buf, 512, &amount, C_BXF_READ);
		if (err) {
			if (err != LIBUSB_ERROR_TIMEOUT)
				break;
			continue;
		}
		if (!strncmp(buf, "submit", 6))
			parse_bxf_submit(bitfury, info, buf);
		else if (!strncmp(buf, "temp", 4))
			parse_bxf_temp(bitfury, info, buf);
	}
out:
	return NULL;
}

static bool bxf_prepare(struct cgpu_info *bitfury, struct bitfury_info *info)
{
	mutex_init(&info->lock);
	if (pthread_create(&info->read_thr, NULL, bxf_get_results, (void *)bitfury))
		quit(1, "Failed to create bxf read_thr");
	return true;
}

static bool bitfury_prepare(struct thr_info *thr)
{
	struct cgpu_info *bitfury = thr->cgpu;
	struct bitfury_info *info = bitfury->device_data;

	info->thr = thr;

	switch(info->ident) {
		case IDENT_BXF:
			return bxf_prepare(bitfury, info);
			break;
		case IDENT_BF1:
		default:
			return true;
	}
}

static uint32_t decnonce(uint32_t in)
{
	uint32_t out;

	/* First part load */
	out = (in & 0xFF) << 24; in >>= 8;

	/* Byte reversal */
	in = (((in & 0xaaaaaaaa) >> 1) | ((in & 0x55555555) << 1));
	in = (((in & 0xcccccccc) >> 2) | ((in & 0x33333333) << 2));
	in = (((in & 0xf0f0f0f0) >> 4) | ((in & 0x0f0f0f0f) << 4));

	out |= (in >> 2)&0x3FFFFF;

	/* Extraction */
	if (in & 1) out |= (1 << 23);
	if (in & 2) out |= (1 << 22);

	out -= 0x800004;
	return out;
}

#define BT_OFFSETS 3
const uint32_t bf_offsets[] = {-0x800000, 0, -0x400000};

static bool bitfury_checkresults(struct thr_info *thr, struct work *work, uint32_t nonce)
{
	int i;

	for (i = 0; i < BT_OFFSETS; i++) {
		uint32_t noffset = nonce + bf_offsets[i];

		if (test_nonce(work, noffset)) {
			submit_tested_work(thr, work);
			return true;
		}
	}
	return false;
}

static int64_t bf1_scan(struct thr_info *thr, struct cgpu_info *bitfury,
			struct bitfury_info *info)
{
	int amount, i, aged = 0, total = 0, ms_diff;
	char readbuf[512], buf[45];
	struct work *work, *tmp;
	struct timeval tv_now;
	double nonce_rate;
	int64_t ret = 0;

	work = get_queue_work(thr, bitfury, thr->id);
	if (unlikely(thr->work_restart)) {
		work_completed(bitfury, work);
		goto out;
	}

	buf[0] = 'W';
	memcpy(buf + 1, work->midstate, 32);
	memcpy(buf + 33, work->data + 64, 12);

	/* New results may spill out from the latest work, making us drop out
	 * too early so read whatever we get for the first half nonce and then
	 * look for the results to prev work. */
	cgtime(&tv_now);
	ms_diff = 600 - ms_tdiff(&tv_now, &info->tv_start);
	if (ms_diff > 0) {
		usb_read_timeout_cancellable(bitfury, readbuf, 512, &amount, ms_diff,
					     C_BF1_GETRES);
		total += amount;
	}

	/* Now look for the bulk of the previous work results, they will come
	 * in a batch following the first data. */
	cgtime(&tv_now);
	ms_diff = BF1WAIT - ms_tdiff(&tv_now, &info->tv_start);
	/* If a work restart was sent, just empty the buffer. */
	if (unlikely(ms_diff < 10 || thr->work_restart))
		ms_diff = 10;
	usb_read_once_timeout_cancellable(bitfury, readbuf + total, BF1MSGSIZE,
					  &amount, ms_diff, C_BF1_GETRES);
	total += amount;
	while (amount) {
		usb_read_once_timeout(bitfury, readbuf + total, 512, &amount, 10,
				      C_BF1_GETRES);
		total += amount;
	};

	/* Don't send whatever work we've stored if we got a restart */
	if (unlikely(thr->work_restart))
		goto out;

	/* Send work */
	cgtime(&work->tv_work_start);
	usb_write(bitfury, buf, 45, &amount, C_BF1_REQWORK);
	cgtime(&info->tv_start);

	/* Get response acknowledging work */
	usb_read(bitfury, buf, BF1MSGSIZE, &amount, C_BF1_GETWORK);

out:
	/* Search for what work the nonce matches in order of likelihood. Last
	 * entry is end of result marker. */
	for (i = 0; i < total - BF1MSGSIZE; i += BF1MSGSIZE) {
		bool found = false;
		uint32_t nonce;

		/* Ignore state & switched data in results for now. */
		memcpy(&nonce, readbuf + i + 3, 4);
		nonce = decnonce(nonce);

		rd_lock(&bitfury->qlock);
		HASH_ITER(hh, bitfury->queued_work, work, tmp) {
			if (bitfury_checkresults(thr, work, nonce)) {
				info->nonces++;
				found = true;
				break;
			}
		}
		rd_unlock(&bitfury->qlock);

		if (!found)
			inc_hw_errors(thr);
	}

	cgtime(&tv_now);

	/* This iterates over the hashlist finding work started more than 6
	 * seconds ago which equates to leaving 5 past work items in the array
	 * to look for results. */
	wr_lock(&bitfury->qlock);
	HASH_ITER(hh, bitfury->queued_work, work, tmp) {
		if (tdiff(&tv_now, &work->tv_work_start) > 6.0) {
			__work_completed(bitfury, work);
			aged++;
		}
	}
	wr_unlock(&bitfury->qlock);

	if (aged) {
		applog(LOG_DEBUG, "%s %d: Aged %d work items", bitfury->drv->name,
		       bitfury->device_id, aged);
	}

	info->cycles++;
	info->total_nonces += info->nonces;
	info->saved_nonces += info->nonces;
	info->nonces = 0;
	nonce_rate = (double)info->total_nonces / (double)info->cycles;
	if (info->saved_nonces >= nonce_rate) {
		info->saved_nonces -= nonce_rate;
		ret = (double)0xffffffff * nonce_rate;
	}

	if (unlikely(bitfury->usbinfo.nodev)) {
		applog(LOG_WARNING, "%s %d: Device disappeared, disabling thread",
		       bitfury->drv->name, bitfury->device_id);
		ret = -1;
	}
	return ret;
}

static int64_t bitfury_scanwork(struct thr_info *thr)
{
	struct cgpu_info *bitfury = thr->cgpu;
	struct bitfury_info *info = bitfury->device_data;

	switch(info->ident) {
		case IDENT_BF1:
			return bf1_scan(thr, bitfury, info);
			break;
		case IDENT_BXF:
		default:
			return 0;
	}
}

static void bxf_send_work(struct cgpu_info *bitfury, struct work *work)
{
	char buf[512], hexwork[156];
	int err, amount, len;

	__bin2hex(hexwork, work->data, 76);
	sprintf(buf, "work %s %d\n", hexwork, work->subid);
	len = strlen(buf);
	err = usb_write(bitfury, buf, len, &amount, C_BXF_WORK);
	if (err || amount != len) {
		applog(LOG_WARNING, "%s %d: Error %d sending work sent %d of %d", bitfury->drv->name,
		       bitfury->device_id, err, amount, len);
	}
}

static void bxf_update_work(struct cgpu_info *bitfury, struct bitfury_info *info)
{
	struct thr_info *thr = info->thr;
	struct work *work;

	work = get_queue_work(thr, bitfury, thr->id);

	mutex_lock(&info->lock);
	work->subid = ++info->work_id;
	mutex_unlock(&info->lock);

	bxf_send_work(bitfury, work);
}

static void bitfury_flush_work(struct cgpu_info *bitfury)
{
	struct bitfury_info *info = bitfury->device_data;

	switch(info->ident) {
		case IDENT_BXF:
			bxf_update_work(bitfury, info);
		case IDENT_BF1:
		default:
			break;
	}
}

static void bitfury_update_work(struct cgpu_info *bitfury)
{
	struct bitfury_info *info = bitfury->device_data;

	switch(info->ident) {
		case IDENT_BXF:
			bxf_update_work(bitfury, info);
		case IDENT_BF1:
		default:
			break;
	}
}

static struct api_data *bitfury_api_stats(struct cgpu_info *cgpu)
{
	struct bitfury_info *info = cgpu->device_data;
	struct api_data *root = NULL;
	double nonce_rate;
	char serial[16];
	int version;

	version = info->version;
	root = api_add_int(root, "Version", &version, true);
	root = api_add_string(root, "Product", info->product, false);
	sprintf(serial, "%08x", info->serial);
	root = api_add_string(root, "Serial", serial, true);
	nonce_rate = (double)info->total_nonces / (double)info->cycles;
	root = api_add_double(root, "NonceRate", &nonce_rate, true);

	return root;
}

static void bf1_init(struct cgpu_info *bitfury)
{
	bf1_close(bitfury);
	bf1_open(bitfury);
	bf1_reset(bitfury);
}

static void bitfury_init(struct cgpu_info *bitfury)
{
	struct bitfury_info *info = bitfury->device_data;

	switch(info->ident) {
		case IDENT_BF1:
			bf1_init(bitfury);
			break;
		case IDENT_BXF:
		default:
			break;
	}
}

static void bxf_close(struct bitfury_info *info)
{
	pthread_join(info->read_thr, NULL);
	mutex_destroy(&info->lock);
}

static void bitfury_shutdown(struct thr_info *thr)
{
	struct cgpu_info *bitfury = thr->cgpu;
	struct bitfury_info *info = bitfury->device_data;

	switch(info->ident) {
		case IDENT_BF1:
			bf1_close(bitfury);
			break;
		case IDENT_BXF:
			bxf_close(info);
			break;
		default:
			break;
	}
}

/* Currently hardcoded to BF1 devices */
struct device_drv bitfury_drv = {
	.drv_id = DRIVER_bitfury,
	.dname = "bitfury",
	.name = "BF1",
	.drv_detect = bitfury_detect,
	.thread_prepare = bitfury_prepare,
	.hash_work = &hash_driver_work,
	.scanwork = bitfury_scanwork,
	.flush_work = bitfury_flush_work,
	.update_work = bitfury_update_work,
	.get_api_stats = bitfury_api_stats,
	.reinit_device = bitfury_init,
	.thread_shutdown = bitfury_shutdown,
	.identify_device = bitfury_identify
};
