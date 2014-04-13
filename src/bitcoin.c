/*
 * Copyright 2014 Con Kolivas
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <string.h>

#include "ckpool.h"
#include "libckpool.h"

static const char *b58chars = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

bool validate_address(connsock_t *cs, const char *address)
{
	json_t *val, *res_val, *valid_val;
	char rpc_req[256];
	bool ret = false;
	int len, i, j;

	if (unlikely(!address)) {
		LOGWARNING("Null address passed to validate_address");
		return ret;
	}
	len = strlen(address);
	if (len < 27 || len > 34) {
		LOGWARNING("Invalid address length %d passed to validate_address", len);
		return ret;
	}
	for (i = 0; i < len; i++) {
		char c = address[i];
		bool found = false;

		for (j = 0; j < 58; j++) {
			if (c == b58chars[j]) {
				found = true;
				break;
			}
		}
		if (!found) {
			LOGWARNING("Invalid char %.1s passed to validate_address", &c);
			return ret;
		}
	}

	snprintf(rpc_req, 256, "{\"method\": \"validateaddress\", \"params\": [\"%s\"]}\n", address);
	val = json_rpc_call(cs, rpc_req);
	if (!val) {
		LOGERR("Failed to get valid json response to validate_address");
		return ret;
	}
	res_val = json_object_get(val, "result");
	if (!res_val) {
		LOGERR("Failed to get result json response to validate_address");
		goto out;
	}
	valid_val = json_object_get(res_val, "isvalid");
	if (!valid_val) {
		LOGERR("Failed to get isvalid json response to validate_address");
		goto out;
	}
	if (!json_is_true(valid_val))
		LOGWARNING("Bitcoin address %s is NOT valid", address);
	else {
		LOGDEBUG("Bitcoin address %s IS valid", address);
		ret = true;
	}
out:
	if (val)
		json_decref(val);
	return ret;
}

static bool gbt_merkle_bins(gbtbase_t *gbt, json_t *transaction_arr)
{
	int i, j, binleft, binlen;
	char hashhex[68];
	json_t *arr_val;
	uchar *hashbin;

	dealloc(gbt->txn_data);
	gbt->transactions = 0;
	gbt->merkles = 0;
	gbt->transactions = json_array_size(transaction_arr);
	binlen = gbt->transactions * 32 + 32;
	hashbin = alloca(binlen + 32);
	memset(hashbin, 0, 32);
	binleft = binlen / 32;
	if (gbt->transactions) {
		int len = 1, ofs = 0;
		const char *txn;

		for (i = 0; i < gbt->transactions; i++) {
			arr_val = json_array_get(transaction_arr, i);
			txn = json_string_value(json_object_get(arr_val, "data"));
			if (!txn) {
				LOGWARNING("json_string_value fail - cannot find transaction data");
				return false;
			}
			len += strlen(txn);
		}

		gbt->txn_data = ckalloc(len + 1);
		gbt->txn_data[len] = '\0';

		for (i = 0; i < gbt->transactions; i++) {
			char binswap[32];
			const char *hash;

			arr_val = json_array_get(transaction_arr, i);
			hash = json_string_value(json_object_get(arr_val, "hash"));
			txn = json_string_value(json_object_get(arr_val, "data"));
			len = strlen(txn);
			memcpy(gbt->txn_data + ofs, txn, len);
			ofs += len;
#if 0
			if (!hash) {
				char *txn_bin;
				int txn_len;

				txn_len = len / 2;
				txn_bin = ckalloc(txn_len);
				hex2bin(txn_bin, txn, txn_len);
				/* This is needed for pooled mining since only
				 * transaction data and not hashes are sent */
				gen_hash(txn_bin, hashbin + 32 + 32 * i, txn_len);
				continue;
			}
#endif
			if (!hex2bin(binswap, hash, 32)) {
				LOGERR("Failed to hex2bin hash in gbt_merkle_bins");
				return false;
			}
			bswap_256(hashbin + 32 + 32 * i, binswap);
		}
	}
	if (binleft > 1) {
		while (42) {
			if (binleft == 1)
				break;
			memcpy(gbt->merklebin + (gbt->merkles * 32), hashbin + 32, 32);
			gbt->merkles++;
			if (binleft % 2) {
				memcpy(hashbin + binlen, hashbin + binlen - 32, 32);
				binlen += 32;
				binleft++;
			}
			for (i = 32, j = 64; j < binlen; i += 32, j += 64)
				gen_hash(hashbin + j, hashbin + i, 64);
			binleft /= 2;
			binlen = binleft * 32;
		}
	}
	for (i = 0; i < gbt->merkles; i++) {
		__bin2hex(hashhex, gbt->merklebin + i * 32, 32);
		LOGDEBUG("MH%d %s",i, hashhex);
	}
	LOGINFO("Stored %d transactions", gbt->transactions);
	return true;
}

static const char *gbt_req = "{\"method\": \"getblocktemplate\", \"params\": [{\"capabilities\": [\"coinbasetxn\", \"workid\", \"coinbase/append\"]}]}\n";

bool gen_gbtbase(connsock_t *cs, gbtbase_t *gbt)
{
	json_t *transaction_arr, *coinbase_aux, *res_val, *val;
	const char *previousblockhash;
	uint64_t coinbasevalue;
	char hash_swap[32];
	const char *target;
	const char *flags;
	const char *bits;
	int version;
	int curtime;
	int height;
	bool ret = false;

	val = json_rpc_call(cs, gbt_req);
	if (!val) {
		LOGWARNING("Failed to get valid json response to getblocktemplate");
		return ret;
	}
	res_val = json_object_get(val, "result");
	if (!res_val) {
		LOGWARNING("Failed to get result in json response to getblocktemplate");
		return ret;
	}

	previousblockhash = json_string_value(json_object_get(res_val, "previousblockhash"));
	target = json_string_value(json_object_get(res_val, "target"));
	transaction_arr = json_object_get(res_val, "transactions");
	version = json_integer_value(json_object_get(res_val, "version"));
	curtime = json_integer_value(json_object_get(res_val, "curtime"));
	bits = json_string_value(json_object_get(res_val, "bits"));
	height = json_integer_value(json_object_get(res_val, "height"));
	coinbasevalue = json_integer_value(json_object_get(res_val, "coinbasevalue"));
	coinbase_aux = json_object_get(res_val, "coinbaseaux");
	flags = json_string_value(json_object_get(coinbase_aux, "flags"));

	if (unlikely(!previousblockhash || !target || !version || !curtime || !bits || !coinbase_aux || !flags)) {
		LOGERR("JSON failed to decode GBT %s %s %d %d %s %s", previousblockhash, target, version, curtime, bits, flags);
		goto out;
	}
	hex2bin(hash_swap, previousblockhash, 32);
	swap_256(gbt->previousblockhash, hash_swap);
	__bin2hex(gbt->prev_hash, gbt->previousblockhash, 32);
	hex2bin(hash_swap, target, 32);
	bswap_256(gbt->target, hash_swap);
	gbt->sdiff = diff_from_target(gbt->target);

	gbt->version = htobe32(version);
	gbt->curtime = htobe32(curtime);
	snprintf(gbt->ntime, 9, "%08x", curtime);
	snprintf(gbt->bbversion, 9, "%08x", version);
	snprintf(gbt->nbit, 9, "%s", bits);
	gbt->nValue = coinbasevalue;
	hex2bin(&gbt->bits, bits, 4);
	gbt_merkle_bins(gbt, transaction_arr);
	gbt->height = height;

out:
	if (res_val)
		json_decref(res_val);
	return ret;
}
