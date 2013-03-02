/*
 * arp_in_out.c
 *
 *  Created on: September 5, 2012
 *      Author: Jonathan Reed
 */
#include "arp.h"

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include <finsdebug.h>
#include <finstypes.h>
#include <metadata.h>

void arp_exec_get_addr(struct finsFrame *ff, uint32_t src_ip, uint32_t dst_ip) {
	struct arp_interface *interface;
	struct arp_cache *cache;
	struct arp_cache *temp_cache;
	uint64_t dst_mac;
	uint64_t src_mac;

	PRINT_DEBUG("Entered: ff=%p, src_ip=%u, dst_ip=%u", ff, src_ip, dst_ip);

	metadata *meta = ff->metaData;

	interface = arp_interface_list_find(src_ip);
	if (interface) {
		src_mac = interface->addr_mac;
		PRINT_DEBUG("src: interface=%p, mac=0x%llx, ip=%u", interface, src_mac, src_ip);

		secure_metadata_writeToElement(meta, "src_mac", &src_mac, META_TYPE_INT64);

		interface = arp_interface_list_find(dst_ip);
		if (interface) { //Shouldn't occur since caught by IPv4
			dst_mac = interface->addr_mac;
			PRINT_DEBUG("dst: interface=%p, mac=0x%llx, ip=%u", interface, dst_mac, dst_ip);

			secure_metadata_writeToElement(meta, "dst_mac", &dst_mac, META_TYPE_INT64);

			ff->destinationID.id = ff->ctrlFrame.senderID;
			ff->ctrlFrame.senderID = ARP_ID;
			ff->ctrlFrame.opcode = CTRL_EXEC_REPLY;
			ff->ctrlFrame.ret_val = 1;

			arp_to_switch(ff);
		} else {
			cache = arp_cache_list_find(dst_ip);
			if (cache) {
				if (cache->seeking) {
					PRINT_DEBUG("cache seeking: cache=%p", cache);
					if (arp_request_list_has_space(cache->request_list)) {
						struct arp_request *request = arp_request_create(ff, src_mac, src_ip);
						arp_request_list_append(cache->request_list, request);
					} else {
						PRINT_ERROR("Error: request_list full, request_list->len=%d", cache->request_list->len);

						ff->destinationID.id = ff->ctrlFrame.senderID;
						ff->ctrlFrame.senderID = ARP_ID;
						ff->ctrlFrame.opcode = CTRL_EXEC_REPLY;
						ff->ctrlFrame.ret_val = 0;

						arp_to_switch(ff);
					}
				} else {
					dst_mac = cache->addr_mac;
					PRINT_DEBUG("dst: cache=%p, mac=0x%llx, ip=%u", cache, dst_mac, dst_ip);

					struct timeval current;
					gettimeofday(&current, 0);

					if (time_diff(&cache->updated_stamp, &current) <= ARP_CACHE_TO_DEFAULT) {
						PRINT_DEBUG("up to date cache: cache=%p", cache);

						secure_metadata_writeToElement(meta, "dst_mac", &dst_mac, META_TYPE_INT64);

						ff->destinationID.id = ff->ctrlFrame.senderID;
						ff->ctrlFrame.senderID = ARP_ID;
						ff->ctrlFrame.opcode = CTRL_EXEC_REPLY;
						ff->ctrlFrame.ret_val = 1;

						arp_to_switch(ff);
					} else {
						PRINT_DEBUG("cache expired: cache=%p", cache);

						struct arp_message msg;
						gen_requestARP(&msg, src_mac, src_ip, dst_mac, dst_ip);

						struct finsFrame *ff_req = arp_to_fdf(&msg);
						if (arp_to_switch(ff_req)) {
							cache->seeking = 1;
							cache->retries = 0;

							gettimeofday(&cache->updated_stamp, 0); //TODO use this value as start of seeking
							timer_once_start(cache->to_data->tid, ARP_RETRANS_TO_DEFAULT);

							if (arp_request_list_has_space(cache->request_list)) {
								struct arp_request *request = arp_request_create(ff, src_mac, src_ip);
								arp_request_list_append(cache->request_list, request);
							} else {
								PRINT_ERROR("Error: request_list full, request_list->len=%d", cache->request_list->len);

								ff->destinationID.id = ff->ctrlFrame.senderID;
								ff->ctrlFrame.senderID = ARP_ID;
								ff->ctrlFrame.opcode = CTRL_EXEC_REPLY;
								ff->ctrlFrame.ret_val = 0;

								arp_to_switch(ff);
							}
						} else {
							PRINT_ERROR("switch send failed");
							freeFinsFrame(ff_req);

							ff->destinationID.id = ff->ctrlFrame.senderID;
							ff->ctrlFrame.senderID = ARP_ID;
							ff->ctrlFrame.opcode = CTRL_EXEC_REPLY;
							ff->ctrlFrame.ret_val = 0;

							arp_to_switch(ff);
						}
					}
				}
			} else {
				PRINT_DEBUG("create cache: start seeking");

				dst_mac = ARP_MAC_BROADCAST;

				struct arp_message msg;
				gen_requestARP(&msg, src_mac, src_ip, dst_mac, dst_ip);

				struct finsFrame *ff_req = arp_to_fdf(&msg);
				if (arp_to_switch(ff_req)) {
					//TODO change this remove 1 cache by order of: nonseeking then seeking, most retries, oldest timestamp
					if (!arp_cache_list_has_space()) {
						PRINT_DEBUG("Making space in cache_list");

						temp_cache = arp_cache_list_remove_first_non_seeking();
						if (temp_cache) {
							struct arp_request *temp_request;
							struct finsFrame *temp_ff;

							while (!arp_request_list_is_empty(temp_cache->request_list)) {
								temp_request = arp_request_list_remove_front(temp_cache->request_list);
								temp_ff = temp_request->ff;

								temp_ff->destinationID.id = temp_ff->ctrlFrame.senderID;
								temp_ff->ctrlFrame.senderID = ARP_ID;
								temp_ff->ctrlFrame.opcode = CTRL_EXEC_REPLY;
								temp_ff->ctrlFrame.ret_val = 0;

								arp_to_switch(temp_ff);

								temp_request->ff = NULL;
								arp_request_free(temp_request);
							}

							arp_cache_shutdown(temp_cache);
							arp_cache_free(temp_cache);
						} else {
							PRINT_ERROR("Cache full");

							ff->destinationID.id = ff->ctrlFrame.senderID;
							ff->ctrlFrame.senderID = ARP_ID;
							ff->ctrlFrame.opcode = CTRL_EXEC_REPLY;
							ff->ctrlFrame.ret_val = 0;

							arp_to_switch(ff);
							return;
						}
					}

					cache = arp_cache_create(dst_ip);
					arp_cache_list_insert(cache);
					cache->seeking = 1;
					cache->retries = 0;

					gettimeofday(&cache->updated_stamp, 0);
					timer_once_start(cache->to_data->tid, ARP_RETRANS_TO_DEFAULT);

					struct arp_request *request = arp_request_create(ff, src_mac, src_ip);
					arp_request_list_append(cache->request_list, request);
				} else {
					PRINT_DEBUG("switch send failed");
					freeFinsFrame(ff_req);

					ff->destinationID.id = ff->ctrlFrame.senderID;
					ff->ctrlFrame.senderID = ARP_ID;
					ff->ctrlFrame.opcode = CTRL_EXEC_REPLY;
					ff->ctrlFrame.ret_val = 0;

					arp_to_switch(ff);
				}
			}
		}
	} else {
		PRINT_ERROR("No corresponding interface: ff=%p, src_ip=%u", ff, src_ip);

		ff->destinationID.id = ff->ctrlFrame.senderID;
		ff->ctrlFrame.senderID = ARP_ID;
		ff->ctrlFrame.opcode = CTRL_EXEC_REPLY;
		ff->ctrlFrame.ret_val = 0;

		arp_to_switch(ff);
	}
}

void arp_in_fdf(struct finsFrame *ff) {
	struct arp_message *msg;

	PRINT_DEBUG("Entered: ff=%p, meta=%p", ff, ff->metaData);

	msg = fdf_to_arp(ff);
	if (msg) {
		print_msgARP(msg);

		if (check_valid_arp(msg)) {
			uint32_t dst_ip = msg->target_IP_addrs;

			struct arp_interface *interface = arp_interface_list_find(dst_ip);
			if (interface) {
				uint64_t dst_mac = interface->addr_mac;

				uint32_t src_ip = msg->sender_IP_addrs;
				uint64_t src_mac = msg->sender_MAC_addrs;

				if (msg->operation == ARP_OP_REQUEST) {
					PRINT_DEBUG("Request");

					struct arp_message arp_msg_reply;
					gen_replyARP(&arp_msg_reply, dst_mac, dst_ip, src_mac, src_ip);

					struct finsFrame *ff_reply = arp_to_fdf(&arp_msg_reply);
					if (!arp_to_switch(ff_reply)) {
						PRINT_ERROR("todo error");
						freeFinsFrame(ff_reply);
					}
				} else {
					PRINT_DEBUG("Reply");

					struct arp_cache *cache = arp_cache_list_find(src_ip);
					if (cache) {
						if (cache->seeking) {
							PRINT_DEBUG("Updating host: node=%p, mac=0x%llx, ip=%u", cache, src_mac, src_ip);
							timer_stop(cache->to_data->tid);
							cache->to_flag = 0;
							gettimeofday(&cache->updated_stamp, 0); //use this as time cache confirmed

							cache->seeking = 0;
							cache->addr_mac = src_mac;

							struct arp_request *request;
							struct finsFrame *ff_resp;

							while (!arp_request_list_is_empty(cache->request_list)) {
								request = arp_request_list_remove_front(cache->request_list);
								ff_resp = request->ff;

								secure_metadata_writeToElement(ff_resp->metaData, "dst_mac", &src_mac, META_TYPE_INT64);

								ff_resp->destinationID.id = ff_resp->ctrlFrame.senderID;
								ff_resp->ctrlFrame.senderID = ARP_ID;
								ff_resp->ctrlFrame.opcode = CTRL_EXEC_REPLY;
								ff_resp->ctrlFrame.ret_val = 1;

								if (!arp_to_switch(ff_resp)) {
									freeFinsFrame(ff_resp);
								}

								request->ff = NULL;
								arp_request_free(request);
							}
						} else {
							PRINT_ERROR("Not seeking addr. Dropping: ff=%p, src=0x%llx/%u, dst=0x%llx/%u, cache=%p",
									ff, src_mac, src_ip, dst_mac, dst_ip, cache);
						}
					} else {
						PRINT_ERROR("No corresponding request. Dropping: ff=%p, src=0x%llx/%u, dst=0x%llx/%u", ff, src_mac, src_ip, dst_mac, dst_ip);
					}
				}
			} else {
				PRINT_ERROR("No corresponding interface. Dropping: ff=%p, dst_ip=%u", ff, dst_ip); //TODO change to PRINT_ERROR
			}
		} else {
			PRINT_ERROR("Invalid Message. Dropping: ff=%p", ff);
		}

		free(msg);
	} else {
		PRINT_ERROR("Bad ARP message. Dropping: ff=%p", ff);
	}

	freeFinsFrame(ff);
}

void arp_out_fdf(struct finsFrame *ff) {

}

void arp_handle_to(struct arp_cache *cache) {
	PRINT_DEBUG("Entered: cache=%p", cache);

	if (cache->seeking) {
		if (cache->retries < ARP_RETRIES) {
			uint64_t dst_mac = ARP_MAC_BROADCAST;
			uint32_t dst_ip = cache->addr_ip;

			if (arp_request_list_is_empty(cache->request_list)) {
				PRINT_ERROR("todo error");
				//TODO retrans from default interface?
				//TODO send error FCF ?
			} else {
				struct arp_request *request = cache->request_list->front;

				uint64_t src_mac = request->src_mac;
				uint32_t src_ip = request->src_ip;

				struct arp_message msg;
				gen_requestARP(&msg, src_mac, src_ip, dst_mac, dst_ip);

				struct finsFrame *ff_req = arp_to_fdf(&msg);
				if (arp_to_switch(ff_req)) {
					cache->retries++;

					//gettimeofday(&cache->updated_stamp, 0);
					timer_once_start(cache->to_data->tid, ARP_RETRANS_TO_DEFAULT);
				} else {
					PRINT_ERROR("todo error");
					freeFinsFrame(ff_req);

					//TODO send error FCF
				}
			}
		} else {
			PRINT_DEBUG("Unreachable address, sending error FCF");

			struct arp_request *request;
			struct finsFrame *ff;

			//TODO figure out if it rejects all requests or re-seeks per request
			while (!arp_request_list_is_empty(cache->request_list)) {
				request = arp_request_list_remove_front(cache->request_list);
				ff = request->ff;

				ff->destinationID.id = ff->ctrlFrame.senderID;
				ff->ctrlFrame.senderID = ARP_ID;
				ff->ctrlFrame.opcode = CTRL_EXEC_REPLY;
				ff->ctrlFrame.ret_val = 0;

				arp_to_switch(ff);

				request->ff = NULL;
				arp_request_free(request);
			}

			arp_cache_list_remove(cache);
			arp_cache_shutdown(cache);
			arp_cache_free(cache);
		}
	} else {
		PRINT_DEBUG("Dropping TO: cache=%p", cache);
	}
}
