/*
 * @file tcp_out.c
 * @date Feb 22, 2012
 * @author Jonathan Reed
 */

#include "tcp.h"

void *write_thread(void *local) {
	struct tcp_thread_data *thread_data = (struct tcp_thread_data *) local;
	uint32_t id = thread_data->id;
	struct tcp_connection *conn = thread_data->conn;
	uint8_t *called_data = thread_data->data_raw;
	uint32_t called_len = thread_data->data_len;
	uint32_t flags = thread_data->flags;
	uint32_t serial_num = thread_data->serial_num;
	free(thread_data);

	struct tcp_node *node;

	PRINT_DEBUG("Entered: id=%u", id);

	/*#*/PRINT_DEBUG("sem_wait: conn=%p", conn);
	secure_sem_wait(&conn->sem);
	if (conn->running_flag) {
		PRINT_DEBUG("state=%d", conn->state);
		if (conn->state == TS_SYN_SENT || conn->state == TS_SYN_RECV) { //equiv to non blocking
			PRINT_DEBUG("pre-connected non-blocking");

			int space = conn->write_queue->max - conn->write_queue->len;
			PRINT_DEBUG("space=%d, called_len=%u", space, called_len);
			if (space >= called_len) {
				node = tcp_node_create(called_data, called_len, 0, called_len - 1);
				tcp_queue_append(conn->write_queue, node);

				space -= called_len;

				conn_send_fcf(conn, serial_num, EXEC_TCP_SEND, 1, called_len);

				if (conn->main_wait_flag) {
					PRINT_DEBUG("posting to wait_sem");
					sem_post(&conn->main_wait_sem);
				}
			} else {
				conn_send_fcf(conn, serial_num, EXEC_TCP_SEND, 0, EAGAIN);
				free(called_data);
			}

			if (conn->poll_events & (POLLOUT | POLLWRNORM | POLLWRBAND)) {
				if (tcp_queue_is_empty(conn->request_queue)) {
					PRINT_DEBUG("conn=%p, space=%d", conn, space);
					if (space > 0) { //only possible if request_queue is empty
						conn_send_exec(conn, EXEC_TCP_POLL_POST, 1, POLLOUT | POLLWRNORM | POLLWRBAND);
						conn->poll_events &= ~(POLLOUT | POLLWRNORM | POLLWRBAND);
					}
				}
			}
		} else if (conn->state == TS_ESTABLISHED || conn->state == TS_CLOSE_WAIT) {
			struct tcp_request *request = (struct tcp_request *) secure_malloc(sizeof(struct tcp_request));
			request->data = called_data;
			request->len = called_len;
			request->flags = flags;
			request->serial_num = serial_num;

			if (flags & (MSG_DONTWAIT)) {
				PRINT_DEBUG("non-blocking");

				request->to_flag = 0;

				request->to_data = secure_malloc(sizeof(struct intsem_to_timer_data));
				request->to_data->handler = intsem_to_handler;
				request->to_data->flag = &request->to_flag;
				request->to_data->interrupt = &conn->request_interrupt;
				request->to_data->sem = &conn->main_wait_sem;
				timer_create_to((struct to_timer_data *) request->to_data);

				timer_once_start(request->to_data->tid, TCP_BLOCK_DEFAULT);
			} else {
				PRINT_DEBUG("blocking");

				request->to_flag = 0;
				request->to_data = NULL;
			}

			if (tcp_queue_has_space(conn->request_queue, 1)) {
				node = tcp_node_create((uint8_t *) request, 1, 0, 0);
				tcp_queue_append(conn->request_queue, node);

				handle_requests(conn);

				if (conn->main_wait_flag) {
					PRINT_DEBUG("posting to main_wait_sem");
					sem_post(&conn->main_wait_sem);
				}
			} else {
				PRINT_ERROR("request_list full, len=%u", conn->request_queue->len);
				//send NACK to send handler
				conn_send_fcf(conn, serial_num, EXEC_TCP_SEND, 0, 0);
				free(called_data);
			}
		} else {
			//TODO error, send/write'ing when conn sending is closed
			PRINT_ERROR("todo error");
			//send NACK to send handler
			conn_send_fcf(conn, serial_num, EXEC_TCP_SEND, 0, 0);

			free(called_data);
		}
	} else {
		PRINT_ERROR("todo error");
		//send NACK to send handler
		conn_send_fcf(conn, serial_num, EXEC_TCP_SEND, 0, 0);
		free(called_data);
	}

	/*#*/PRINT_DEBUG("");
	secure_sem_wait(&conn_list_sem);
	conn->threads--;
	PRINT_DEBUG("leaving thread: conn=%p, threads=%d", conn, conn->threads);
	sem_post(&conn_list_sem);

	/*#*/PRINT_DEBUG("sem_post: conn=%p", conn);
	sem_post(&conn->sem);

	PRINT_DEBUG("Exited: id=%u", id);
	//pthread_exit(NULL);
	return NULL;
}

void tcp_out_fdf(struct finsFrame *ff) {
	//receiving straight data from the APP layer, process/package into segment
	uint32_t src_ip;
	uint32_t dst_ip;
	uint32_t src_port;
	uint32_t dst_port;

	uint32_t flags;
	uint32_t serial_num;

	PRINT_DEBUG("Entered: ff=%p, meta=%p", ff, ff->metaData);

	metadata *params = ff->metaData;
	secure_metadata_readFromElement(params, "flags", &flags);
	secure_metadata_readFromElement(params, "serial_num", &serial_num);

	secure_metadata_readFromElement(params, "host_ip", &src_ip); //host
	secure_metadata_readFromElement(params, "host_port", &src_port);
	secure_metadata_readFromElement(params, "rem_ip", &dst_ip); //remote
	secure_metadata_readFromElement(params, "rem_port", &dst_port);

	//TODO if flags & MSG_DONTWAIT, read timeout

	/*#*/PRINT_DEBUG("");
	secure_sem_wait(&conn_list_sem);
	struct tcp_connection *conn = conn_list_find(src_ip, (uint16_t) src_port, dst_ip, (uint16_t) dst_port); //TODO check if right
	int start = (conn->threads < TCP_THREADS_MAX) ? ++conn->threads : 0;
	//if (start) {conn->write_threads++;}
	/*#*/PRINT_DEBUG("");
	sem_post(&conn_list_sem);

	if (conn) {
		if (start) {
			struct tcp_thread_data *thread_data = (struct tcp_thread_data *) secure_malloc(sizeof(struct tcp_thread_data));
			thread_data->id = tcp_gen_thread_id();
			thread_data->conn = conn;
			thread_data->data_len = ff->dataFrame.pduLength;
			thread_data->data_raw = ff->dataFrame.pdu;
			thread_data->flags = flags;
			thread_data->serial_num = serial_num;
			ff->dataFrame.pdu = NULL;

			PRINT_DEBUG("starting: id=%u, conn=%p, pdu=%p, len=%u, flags=0x%x, serial_num=%u",
					thread_data->id, thread_data->conn, thread_data->data_raw, thread_data->data_len, thread_data->flags, thread_data->serial_num);
			//pthread_t thread;
			//secure_pthread_create(&thread, NULL, write_thread, (void *) thread_data);
			//pthread_detach(thread);
			//pool_execute(conn->pool, write_thread, (void *) thread_data);
			write_thread((void *) thread_data);
		} else {
			PRINT_ERROR("Too many threads=%d. Dropping...", conn->threads);
		}
	} else {
		//TODO error
		PRINT_ERROR("todo error");

		//TODO LISTEN: if SEND, SYN, SYN_SENT
	}

	freeFinsFrame(ff);
}

void *close_thread(void *local) {
	struct tcp_thread_data *thread_data = (struct tcp_thread_data *) local;
	uint32_t id = thread_data->id;
	struct tcp_connection *conn = thread_data->conn;
	struct finsFrame *ff = thread_data->ff;
	free(thread_data);

	struct tcp_segment *seg;

	PRINT_DEBUG("Entered: id=%u", id);

	/*#*/PRINT_DEBUG("sem_wait: conn=%p", conn);
	secure_sem_wait(&conn->sem);
	if (conn->running_flag) {
		if (conn->state == TS_ESTABLISHED || conn->state == TS_SYN_RECV) {
			//if CLOSE, send FIN, FIN_WAIT_1
			PRINT_DEBUG("CLOSE, send FIN, FIN_WAIT_1: state=%d, conn=%p", conn->state, conn);
			conn->state = TS_FIN_WAIT_1;

			tcp_reply_fcf(ff, 1, 0);

			PRINT_DEBUG( "host: seqs=(%u, %u) (%u, %u), win=(%u/%u), rem: seqs=(%u, %u) (%u, %u), win=(%u/%u)",
					conn->send_seq_num-conn->issn, conn->send_seq_end-conn->issn, conn->send_seq_num, conn->send_seq_end, conn->recv_win, conn->recv_max_win, conn->recv_seq_num-conn->irsn, conn->recv_seq_end-conn->irsn, conn->recv_seq_num, conn->recv_seq_end, conn->send_win, conn->send_max_win);

			if (conn_is_finished(conn)) {
				//send FIN
				conn->fin_sent = 1;
				conn->fin_sep = 1;
				conn->fssn = conn->send_seq_end;
				conn->fsse = conn->fssn + 1;
				PRINT_DEBUG("setting: fin_sent=%u, fin_sep=%u, fssn=%u, fsse=%u", conn->fin_sent, conn->fin_sep, conn->fssn, conn->fsse);

				seg = seg_create(conn->host_ip, conn->host_port, conn->rem_ip, conn->rem_port, conn->send_seq_end, conn->send_seq_end);
				seg_update(seg, conn, FLAG_FIN | FLAG_ACK);
				seg_send(seg);
				seg_free(seg);

				//conn->send_seq_end++;

				//TODO add TO
			} //else piggy back it
		} else if (conn->state == TS_CLOSE_WAIT) {
			//if CLOSE_WAIT: CLOSE, send FIN, LAST_ACK
			PRINT_DEBUG("CLOSE_WAIT: CLOSE, send FIN, LAST_ACK: state=%d, conn=%p", conn->state, conn);
			conn->state = TS_LAST_ACK;

			tcp_reply_fcf(ff, 1, 0);

			PRINT_DEBUG( "host: seqs=(%u, %u) (%u, %u), win=(%u/%u), rem: seqs=(%u, %u) (%u, %u), win=(%u/%u)",
					conn->send_seq_num-conn->issn, conn->send_seq_end-conn->issn, conn->send_seq_num, conn->send_seq_end, conn->recv_win, conn->recv_max_win, conn->recv_seq_num-conn->irsn, conn->recv_seq_end-conn->irsn, conn->recv_seq_num, conn->recv_seq_end, conn->send_win, conn->send_max_win);

			PRINT_DEBUG("request_queue->len=%u, write_queue->len=%u", conn->request_queue->len, conn->write_queue->len);
			if (conn_is_finished(conn)) {
				//send FIN
				conn->fin_sent = 1;
				conn->fin_sep = 1;
				conn->fssn = conn->send_seq_end;
				conn->fsse = conn->fssn + 1;
				PRINT_DEBUG("setting: fin_sent=%u, fin_sep=%u, fssn=%u, fsse=%u", conn->fin_sent, conn->fin_sep, conn->fssn, conn->fsse);

				seg = seg_create(conn->host_ip, conn->host_port, conn->rem_ip, conn->rem_port, conn->send_seq_end, conn->send_seq_end);
				seg_update(seg, conn, FLAG_FIN | FLAG_ACK);
				seg_send(seg);
				seg_free(seg);

				//conn->send_seq_end++;

				//TODO add TO
			} //else piggy back it
		} else if (conn->state == TS_SYN_SENT) {
			//if CLOSE, send -, CLOSED
			PRINT_DEBUG("SYN_SENT: CLOSE, send -, CLOSED: state=%d, conn=%p", conn->state, conn);
			conn->state = TS_CLOSED;

			/*
			 if (conn->ff) {
			 tcp_reply_fcf(conn->ff, 0, 0); //send NACK about connect call
			 conn->ff = NULL;
			 } else {
			 PRINT_ERROR("todo error");
			 }
			 */

			tcp_reply_fcf(ff, 1, 0); //TODO check move to end of last_ack/start of time_wait?

			conn_shutdown(conn);
		} else if (conn->state == TS_CLOSED || conn->state == TS_TIME_WAIT) {
			if (conn->state == TS_CLOSED) {
				PRINT_DEBUG("CLOSED: CLOSE, -, CLOSED: state=%d, conn=%p", conn->state, conn);
			} else {
				PRINT_DEBUG("TIME_WAIT: CLOSE, -, TIME_WAIT: state=%d, conn=%p", conn->state, conn);
			}
			tcp_reply_fcf(ff, 1, 0);
		} else {
			PRINT_ERROR("todo error");

			PRINT_DEBUG("conn=%p, state=%u", conn, conn->state);
			//TODO figure out:
		}
	} else {
		//TODO figure out: conn shutting down already?
		PRINT_ERROR("todo error");
	}

	/*#*/PRINT_DEBUG("");
	secure_sem_wait(&conn_list_sem);
	conn->threads--;
	PRINT_DEBUG("leaving thread: conn=%p, threads=%d", conn, conn->threads);
	sem_post(&conn_list_sem);

	/*#*/PRINT_DEBUG("sem_post: conn=%p", conn);
	sem_post(&conn->sem);

	PRINT_DEBUG("Exited: id=%u", id);
	//pthread_exit(NULL);
	return NULL;
}

void tcp_exec_close(struct finsFrame *ff, uint32_t host_ip, uint16_t host_port, uint32_t rem_ip, uint16_t rem_port) {
	PRINT_DEBUG("Entered: host=%u/%u, rem=%u/%u", host_ip, host_port, rem_ip, rem_port);

	secure_sem_wait(&conn_list_sem);
	struct tcp_connection *conn = conn_list_find(host_ip, host_port, rem_ip, rem_port);
	if (conn) {
		if (conn->threads < TCP_THREADS_MAX) {
			conn->threads++;
			/*#*/PRINT_DEBUG("");
			sem_post(&conn_list_sem);

			struct tcp_thread_data *thread_data = (struct tcp_thread_data *) secure_malloc(sizeof(struct tcp_thread_data));
			thread_data->id = tcp_gen_thread_id();
			thread_data->conn = conn;
			thread_data->ff = ff;

			//pthread_t thread;
			//secure_pthread_create(&thread, NULL, close_thread, (void *) thread_data);
			//pthread_detach(thread);
			//pool_execute(conn->pool, close_thread, (void *) thread_data);
			close_thread((void *) thread_data);
		} else {
			/*#*/PRINT_DEBUG("");
			sem_post(&conn_list_sem);

			PRINT_ERROR("Too many threads=%d. Dropping...", conn->threads);
		}
	} else {
		PRINT_ERROR("todo error");
		sem_post(&conn_list_sem);
		//TODO error trying to close closed connection
	}
}

void *close_stub_thread(void *local) {
	struct tcp_thread_data *thread_data = (struct tcp_thread_data *) local;
	uint32_t id = thread_data->id;
	struct tcp_connection_stub *conn_stub = thread_data->conn_stub;
	//uint32_t send_ack = thread_data->flags;
	struct finsFrame *ff = thread_data->ff;
	free(thread_data);

	PRINT_DEBUG("Entered: id=%u", id);

	/*#*/PRINT_DEBUG("sem_wait: conn_stub=%p", conn_stub);
	secure_sem_wait(&conn_stub->sem);

	if (conn_stub->running_flag) {
		conn_stub_shutdown(conn_stub);

		//send ACK to close handler
		//conn_stub_send_daemon(conn_stub, EXEC_TCP_CLOSE_STUB, 1, 0);
		tcp_reply_fcf(ff, 1, 0);

		conn_stub_free(conn_stub);
	} else {
		PRINT_ERROR("todo error");
		//send NACK to close handler
		//conn_stub_send_daemon(conn_stub, EXEC_TCP_CLOSE_STUB, 0, 0);
		tcp_reply_fcf(ff, 0, 0);
	}

	//TODO add conn_stub->threads--?

	PRINT_DEBUG("Exited: id=%u", id);
	//pthread_exit(NULL);
	return NULL;
}

void tcp_exec_close_stub(struct finsFrame *ff, uint32_t host_ip, uint16_t host_port) {
	PRINT_DEBUG("Entered: host=%u/%u", host_ip, host_port);

	secure_sem_wait(&conn_stub_list_sem);
	struct tcp_connection_stub *conn_stub = conn_stub_list_find(host_ip, host_port);
	if (conn_stub) {
		conn_stub_list_remove(conn_stub);
		if (conn_stub->threads < TCP_THREADS_MAX) {
			conn_stub->threads++;
			/*#*/PRINT_DEBUG("");
			sem_post(&conn_stub_list_sem);

			struct tcp_thread_data *thread_data = (struct tcp_thread_data *) secure_malloc(sizeof(struct tcp_thread_data));
			thread_data->id = tcp_gen_thread_id();
			thread_data->conn_stub = conn_stub;
			thread_data->ff = ff;
			thread_data->flags = 1;

			//pthread_t thread;
			//secure_pthread_create(&thread, NULL, close_stub_thread, (void *) thread_data);
			//pthread_detach(thread);
			//pool_execute(conn->pool, close_stub_thread, (void *) thread_data);
			close_stub_thread((void *) thread_data);
		} else {
			/*#*/PRINT_DEBUG("");
			sem_post(&conn_stub_list_sem);

			PRINT_ERROR("Too many threads=%d. Dropping...", conn_stub->threads);
		}
	} else {
		PRINT_ERROR("todo error");
		sem_post(&conn_stub_list_sem);
		//TODO error
	}
}

void tcp_exec_listen(struct finsFrame *ff, uint32_t host_ip, uint16_t host_port, uint32_t backlog) {
	struct tcp_connection_stub *conn_stub;

	PRINT_DEBUG("Entered: addr=%u/%u, backlog=%u", host_ip, host_port, backlog);
	secure_sem_wait(&conn_stub_list_sem); //TODO change from conn_stub to conn in listen
	conn_stub = conn_stub_list_find(host_ip, host_port);
	if (conn_stub == NULL) {
		if (conn_stub_list_has_space(1)) {
			conn_stub = conn_stub_create(host_ip, host_port, backlog);
			if (conn_stub_list_insert(conn_stub)) {
				/*#*/PRINT_DEBUG("");
				sem_post(&conn_stub_list_sem);
			} else {
				PRINT_DEBUG("");
				sem_post(&conn_stub_list_sem);

				//error - shouldn't happen
				PRINT_ERROR("conn_stub_insert fail");
				conn_stub_free(conn_stub);
			}
		} else {
			PRINT_ERROR("todo error");
			sem_post(&conn_stub_list_sem);
			//TODO throw minor error
		}
	} else {
		PRINT_ERROR("todo error");
		sem_post(&conn_stub_list_sem);
		//TODO error
	}

	//TODO send ACK to listen handler - don't? have nonblocking
	freeFinsFrame(ff);
}

void *accept_thread(void *local) { //this will need to be changed
	struct tcp_thread_data *thread_data = (struct tcp_thread_data *) local;
	uint32_t id = thread_data->id;
	struct tcp_connection_stub *conn_stub = thread_data->conn_stub;
	uint32_t flags = thread_data->flags;
	struct finsFrame *ff = thread_data->ff;
	free(thread_data);

	struct tcp_node *node;
	struct tcp_segment *seg;
	struct tcp_connection *conn;
	//int start;
	struct tcp_segment *temp_seg;

	PRINT_DEBUG("Entered: id=%u", id);

	/*#*/PRINT_DEBUG("sem_wait: conn_stub=%p", conn_stub);
	secure_sem_wait(&conn_stub->sem);
	while (conn_stub->running_flag) {
		if (!tcp_queue_is_empty(conn_stub->syn_queue)) {
			node = tcp_queue_remove_front(conn_stub->syn_queue);
			/*#*/PRINT_DEBUG("sem_post: conn_stub=%p", conn_stub);
			sem_post(&conn_stub->sem);

			seg = (struct tcp_segment *) node->data;

			/*#*/PRINT_DEBUG("");
			secure_sem_wait(&conn_list_sem);
			conn = conn_list_find(seg->dst_ip, seg->dst_port, seg->src_ip, seg->src_port);
			if (conn == NULL) {
				if (conn_list_has_space()) {
					conn = conn_create(seg->dst_ip, seg->dst_port, seg->src_ip, seg->src_port);
					if (conn_list_insert(conn)) {
						conn->threads++;
						/*#*/PRINT_DEBUG("");
						sem_post(&conn_list_sem);

						/*#*/PRINT_DEBUG("sem_wait: conn=%p", conn);
						secure_sem_wait(&conn->sem);
						if (conn->running_flag) { //LISTENING state
							//if SYN, send SYN ACK, SYN_RECV
							PRINT_DEBUG("SYN, send SYN ACK, SYN_RECV: state=%d", conn->state);
							conn->state = TS_SYN_RECV;
							conn->active_open = 0;
							conn->ff = ff;
							conn->poll_events = conn_stub->poll_events; //TODO specify more

							if (flags & (1)) {
								//TODO do specific flags/settings
							}

							conn->issn = tcp_rand();
							conn->send_seq_num = conn->issn;
							conn->send_seq_end = conn->send_seq_num;
							conn->send_win = (uint32_t) seg->win_size;
							conn->send_max_win = conn->send_win;

							conn->irsn = seg->seq_num;
							conn->recv_seq_num = seg->seq_num + 1;
							conn->recv_seq_end = conn->recv_seq_num + conn->recv_max_win;

							PRINT_DEBUG( "host: seqs=(%u, %u) (%u, %u), win=(%u/%u), rem: seqs=(%u, %u) (%u, %u), win=(%u/%u)",
									conn->send_seq_num-conn->issn, conn->send_seq_end-conn->issn, conn->send_seq_num, conn->send_seq_end, conn->recv_win, conn->recv_max_win, conn->recv_seq_num-conn->irsn, conn->recv_seq_end-conn->irsn, conn->recv_seq_num, conn->recv_seq_end, conn->send_win, conn->send_max_win);

							//TODO process options, decide: MSS, max window size!!
							//TODO MSS (2), Window scale (3), SACK (4), alt checksum (14)

							if (seg->opt_len) {
								process_options(conn, seg);
							}

							//conn_change_options(conn, tcp->options, SYN);

							//send SYN ACK
							temp_seg = seg_create(conn->host_ip, conn->host_port, conn->rem_ip, conn->rem_port, conn->send_seq_end, conn->send_seq_end);
							seg_update(temp_seg, conn, FLAG_SYN | FLAG_ACK);
							seg_send(temp_seg);
							seg_free(temp_seg);

							timer_once_start(conn->to_gbn_data->tid, TCP_MSL_TO_DEFAULT);
							conn->to_gbn_flag = 0;
						} else {
							PRINT_ERROR("todo error");
							//TODO error
						}

						/*#*/PRINT_DEBUG("");
						secure_sem_wait(&conn_list_sem);
						conn->threads--;
						PRINT_DEBUG("leaving thread: conn=%p, threads=%d", conn, conn->threads);
						sem_post(&conn_list_sem);

						/*#*/PRINT_DEBUG("sem_post: conn=%p", conn);
						sem_post(&conn->sem);

						seg_free(seg);
						free(node);
						break;
					} else {
						PRINT_DEBUG("");
						sem_post(&conn_list_sem);

						//error - shouldn't happen
						PRINT_ERROR("conn_insert fail");
						//conn->running_flag = 0;
						//sem_post(&conn->main_wait_sem);
						conn_shutdown(conn);
						//conn_free(conn);
					}
				} else {
					PRINT_ERROR("todo error");
					sem_post(&conn_list_sem);
					//TODO throw minor error
				}
			} else {
				PRINT_ERROR("todo error");
				sem_post(&conn_list_sem);
				//TODO error
			}

			seg_free(seg);
			free(node);
		} else {
			/*#*/PRINT_DEBUG("sem_post: conn_stub=%p", conn_stub);
			sem_post(&conn_stub->sem);

			/*#*/PRINT_DEBUG("");
			secure_sem_wait(&conn_stub->accept_wait_sem);
			sem_init(&conn_stub->accept_wait_sem, 0, 0);
			PRINT_DEBUG("left conn_stub->accept_wait_sem");
		}

		/*#*/PRINT_DEBUG("sem_wait: conn_stub=%p", conn_stub);
		secure_sem_wait(&conn_stub->sem);
	}

	if (!conn_stub->running_flag) {
		PRINT_ERROR("todo error");
		//conn_stub_send_daemon(conn_stub, EXEC_TCP_ACCEPT, 0, 0);
		tcp_reply_fcf(ff, 0, 0);
	}

	/*#*/PRINT_DEBUG("");
	secure_sem_wait(&conn_stub_list_sem);
	conn_stub->threads--;
	PRINT_DEBUG("leaving thread: conn_stub=%p, threads=%d", conn_stub, conn_stub->threads);
	sem_post(&conn_stub_list_sem);

	/*#*/PRINT_DEBUG("sem_post: conn_stub=%p", conn_stub);
	sem_post(&conn_stub->sem);

	PRINT_DEBUG("Exited: id=%u", id);
	//pthread_exit(NULL);
	return NULL;
}

void tcp_exec_accept(struct finsFrame *ff, uint32_t host_ip, uint16_t host_port, uint32_t flags) {
	PRINT_DEBUG("Entered: host=%u/%u, flags=0x%x", host_ip, host_port, flags);

	secure_sem_wait(&conn_stub_list_sem);
	struct tcp_connection_stub *conn_stub = conn_stub_list_find(host_ip, host_port);
	if (conn_stub) {
		if (conn_stub->threads < TCP_THREADS_MAX) {
			conn_stub->threads++;
			/*#*/PRINT_DEBUG("");
			sem_post(&conn_stub_list_sem);

			struct tcp_thread_data *thread_data = (struct tcp_thread_data *) secure_malloc(sizeof(struct tcp_thread_data));
			thread_data->id = tcp_gen_thread_id();
			thread_data->conn_stub = conn_stub;
			thread_data->ff = ff;
			thread_data->flags = flags;

			pthread_t thread;
			secure_pthread_create(&thread, NULL, accept_thread, (void *) thread_data);
			pthread_detach(thread);
		} else {
			/*#*/PRINT_DEBUG("");
			sem_post(&conn_stub_list_sem);

			PRINT_ERROR("Too many threads=%d. Dropping...", conn_stub->threads);
			//TODO send NACK
		}
	} else {
		PRINT_ERROR("todo error");
		sem_post(&conn_stub_list_sem);
		//TODO error, no listening stub

		//send NACK to accept handler
		tcp_reply_fcf(ff, 0, 1);
	}
}

void *connect_thread(void *local) {
	//this will need to be changed
	struct tcp_thread_data *thread_data = (struct tcp_thread_data *) local;
	uint32_t id = thread_data->id;
	struct tcp_connection *conn = thread_data->conn;
	uint32_t flags = thread_data->flags;
	struct finsFrame *ff = thread_data->ff;
	free(thread_data);

	struct tcp_segment *temp_seg;

	PRINT_DEBUG("Entered: id=%u", id);

	/*#*/PRINT_DEBUG("sem_wait: conn=%p", conn);
	secure_sem_wait(&conn->sem);
	if (conn->running_flag) {
		if (conn->state == TS_CLOSED || conn->state == TS_LISTEN) {
			//if CONNECT, send SYN, SYN_SENT
			if (conn->state == TS_CLOSED) {
				PRINT_DEBUG("CLOSED: CONNECT, send SYN, SYN_SENT: state=%d", conn->state);
			} else {
				PRINT_DEBUG("LISTEN: CONNECT, send SYN, SYN_SENT: state=%d", conn->state);
			}
			conn->state = TS_SYN_SENT;
			conn->active_open = 1;
			conn->ff = ff;

			if (flags & (1)) {
				//TODO do specific flags/settings
			}

			conn->issn = tcp_rand();
			conn->send_seq_num = conn->issn;
			conn->send_seq_end = conn->send_seq_num;

			PRINT_DEBUG( "host: seqs=(%u, %u) (%u, %u), win=(%u/%u), rem: seqs=(%u, %u) (%u, %u), win=(%u/%u)",
					conn->send_seq_num-conn->issn, conn->send_seq_end-conn->issn, conn->send_seq_num, conn->send_seq_end, conn->recv_win, conn->recv_max_win, conn->recv_seq_num-conn->irsn, conn->recv_seq_end-conn->irsn, conn->recv_seq_num, conn->recv_seq_end, conn->send_win, conn->send_max_win);

			//TODO add options, for: MSS, max window size!!
			//TODO MSS (2), Window scale (3), SACK (4), alt checksum (14)

			//conn_change_options(conn, tcp->options, SYN);

			//send SYN
			temp_seg = seg_create(conn->host_ip, conn->host_port, conn->rem_ip, conn->rem_port, conn->send_seq_end, conn->send_seq_end);
			seg_update(temp_seg, conn, FLAG_SYN);
			seg_send(temp_seg);
			seg_free(temp_seg);

			conn->timeout = TCP_GBN_TO_DEFAULT;
			//startTimer(conn->to_gbn_fd, conn->timeout); //TODO fix
		} else {
			//TODO error
			PRINT_ERROR("todo error");
			tcp_reply_fcf(ff, 0, 0);
		}
	} else {
		PRINT_ERROR("todo error");
		//send NACK to connect handler
		tcp_reply_fcf(ff, 0, 1);
	}

	/*#*/PRINT_DEBUG("");
	secure_sem_wait(&conn_list_sem);
	conn->threads--;
	PRINT_DEBUG("leaving thread: conn=%p, threads=%d", conn, conn->threads);
	sem_post(&conn_list_sem);

	/*#*/PRINT_DEBUG("sem_post: conn=%p", conn);
	sem_post(&conn->sem);

	PRINT_DEBUG("Exited: id=%u", id);
	//pthread_exit(NULL);
	return NULL;
}

void tcp_exec_connect(struct finsFrame *ff, uint32_t host_ip, uint16_t host_port, uint32_t rem_ip, uint16_t rem_port, uint32_t flags) {
	PRINT_DEBUG("Entered: host=%u/%u, rem=%u/%u", host_ip, host_port, rem_ip, rem_port);

	secure_sem_wait(&conn_list_sem);
	struct tcp_connection *conn = conn_list_find(host_ip, host_port, rem_ip, rem_port);
	if (conn == NULL) {
		if (conn_list_has_space()) {
			conn = conn_create(host_ip, host_port, rem_ip, rem_port);
			if (conn_list_insert(conn)) {
				conn->threads++;
				/*#*/PRINT_DEBUG("");
				sem_post(&conn_list_sem);

				//if listening stub remove
				/*#*/PRINT_DEBUG("");
				secure_sem_wait(&conn_stub_list_sem);
				struct tcp_connection_stub *conn_stub = conn_stub_list_find(host_ip, host_port);
				if (conn_stub) {
					conn_stub_list_remove(conn_stub);
					if (conn_stub->threads < TCP_THREADS_MAX) {
						conn_stub->threads++;
						/*#*/PRINT_DEBUG("");
						sem_post(&conn_stub_list_sem);

						struct tcp_thread_data *stub_thread_data = (struct tcp_thread_data *) secure_malloc(sizeof(struct tcp_thread_data));
						stub_thread_data->id = tcp_gen_thread_id();
						stub_thread_data->conn_stub = conn_stub;
						stub_thread_data->flags = 0;

						//pthread_t stub_thread;
						//secure_pthread_create(&stub_thread, NULL, close_stub_thread, (void *) stub_thread_data);
						//pthread_detach(stub_thread);
						//pool_execute(conn->pool, close_stub_thread, (void *) stub_thread_data);
						close_stub_thread((void *) stub_thread_data);
					} else {
						/*#*/PRINT_DEBUG("");
						sem_post(&conn_stub_list_sem);

						PRINT_ERROR("error");
					}
				} else {
					/*#*/PRINT_DEBUG("");
					sem_post(&conn_stub_list_sem);
				}

				struct tcp_thread_data *thread_data = (struct tcp_thread_data *) secure_malloc(sizeof(struct tcp_thread_data));
				thread_data->id = tcp_gen_thread_id();
				thread_data->conn = conn;
				thread_data->ff = ff;
				thread_data->flags = flags;

				//pthread_t thread;
				//secure_pthread_create(&thread, NULL, connect_thread, (void *) thread_data);
				//pthread_detach(thread);
				//pool_execute(conn->pool, connect_thread, (void *) thread_data);
				connect_thread((void *) thread_data);
			} else {
				/*#*/PRINT_DEBUG("");
				sem_post(&conn_list_sem);

				//error - shouldn't happen
				PRINT_ERROR("conn_insert fail");
				//conn->running_flag = 0;
				//sem_post(&conn->main_wait_sem);
				conn_shutdown(conn);
				//conn_free(conn);

				//TODO send NACK
			}
		} else {
			PRINT_ERROR("todo error");
			sem_post(&conn_list_sem);

			//TODO throw minor error, list full
			//TODO send NACK
		}
	} else {
		PRINT_ERROR("todo error");
		sem_post(&conn_list_sem);

		//TODO error, existing connection already connected there
		//TODO send NACK?
	}
}

void *poll_thread(void *local) {
	struct tcp_thread_data *thread_data = (struct tcp_thread_data *) local;
	uint32_t id = thread_data->id;
	struct tcp_connection *conn = thread_data->conn;
	struct finsFrame *ff = thread_data->ff;
	uint32_t initial = thread_data->data_len;
	uint32_t events = thread_data->flags;
	free(thread_data);

	PRINT_DEBUG("Entered: id=%u, conn=%p, ff=%p,  initial=0x%x, events=0x%x", id, conn, ff, initial, events);

	uint32_t mask = 0;

	/*#*/PRINT_DEBUG("sem_wait: conn=%p", conn);
	secure_sem_wait(&conn->sem);
	if (conn->running_flag) {
		//TODO redo, for now mostly does POLLOUT

		if (events & (POLLERR)) {
			//TODO errors
		}

		if (events & (POLLIN | POLLRDNORM | POLLPRI | POLLRDBAND)) { //TODO remove - handled by daemon
			//mask |= POLLIN | POLLRDNORM; //TODO POLLPRI?

			//add a check to see if conn moves to CLOSE_WAIT, post: POLLHUP
		}

		if (events & (POLLOUT | POLLWRNORM | POLLWRBAND)) {
			if (tcp_queue_is_empty(conn->request_queue)) {
				int space = conn->write_queue->max - conn->write_queue->len;
				if (space > 0) {
					mask |= POLLOUT | POLLWRNORM | POLLWRBAND;
				} else {
					if (initial) {
						conn->poll_events |= (POLLOUT | POLLWRNORM | POLLWRBAND);
						PRINT_DEBUG("adding: poll_events=0x%x", conn->poll_events);
					} else {
						conn->poll_events &= ~(POLLOUT | POLLWRNORM | POLLWRBAND);
						PRINT_DEBUG("removing: poll_events=0x%x", conn->poll_events);
					}
				}
			} else {
				if (initial) {
					conn->poll_events |= (POLLOUT | POLLWRNORM | POLLWRBAND);
					PRINT_DEBUG("adding: poll_events=0x%x", conn->poll_events);
				} else {
					conn->poll_events &= ~(POLLOUT | POLLWRNORM | POLLWRBAND);
					PRINT_DEBUG("removing: poll_events=0x%x", conn->poll_events);
				}
			}
		}

		if (events & (POLLHUP)) {
			//TODO errors
		}

		tcp_reply_fcf(ff, 1, mask);
	} else {
		PRINT_ERROR("todo error");
		tcp_reply_fcf(ff, 1, POLLHUP); //TODO check on value?
	}

	/*#*/PRINT_DEBUG("");
	secure_sem_wait(&conn_list_sem);
	conn->threads--;
	PRINT_DEBUG("leaving thread: conn=%p, threads=%d", conn, conn->threads);
	sem_post(&conn_list_sem);

	/*#*/PRINT_DEBUG("sem_post: conn=%p", conn);
	sem_post(&conn->sem);

	PRINT_DEBUG("Exited: id=%u", id);
	//pthread_exit(NULL);
	return NULL;
}

void *poll_stub_thread(void *local) {
	struct tcp_thread_data *thread_data = (struct tcp_thread_data *) local;
	uint32_t id = thread_data->id;
	struct tcp_connection_stub *conn_stub = thread_data->conn_stub;
	struct finsFrame *ff = thread_data->ff;
	uint32_t initial = thread_data->data_len;
	uint32_t events = thread_data->flags;
	free(thread_data);

	PRINT_DEBUG("Entered: id=%u", id);

	uint32_t mask = 0;

	/*#*/PRINT_DEBUG("sem_wait: conn_stub=%p", conn_stub);
	secure_sem_wait(&conn_stub->sem);
	if (conn_stub->running_flag) {
		//TODO redo, for now mostly does POLLOUT

		if (events & (POLLERR)) {
			//TODO errors
		}

		if (events & (POLLIN | POLLRDNORM | POLLPRI | POLLRDBAND)) { //TODO remove - handled by daemon
			//mask |= POLLIN | POLLRDNORM; //TODO POLLPRI?

			//add a check to see if conn moves to CLOSE_WAIT, post: POLLHUP
		}

		if (events & (POLLOUT | POLLWRNORM | POLLWRBAND)) {
			int val = 0;
			//val = conn_stub->
			//sem_getvalue(&conn->write_sem, &val);
			//PRINT_DEBUG("conn_stub=%p, conn->write_sem=%d", conn_stub, val);

			if (val) {
				mask |= POLLOUT | POLLWRNORM | POLLWRBAND;
			} else {
				if (initial) {
					conn_stub->poll_events |= (POLLOUT | POLLWRNORM | POLLWRBAND);
				} else {
					conn_stub->poll_events &= ~(POLLOUT | POLLWRNORM | POLLWRBAND);
				}
			}
		}

		if (events & (POLLHUP)) {
			//TODO errors
		}

		tcp_reply_fcf(ff, 1, mask);
	} else {
		PRINT_ERROR("todo error");
		tcp_reply_fcf(ff, 1, POLLHUP); //TODO check on value?
	}

	/*#*/PRINT_DEBUG("");
	secure_sem_wait(&conn_stub_list_sem);
	conn_stub->threads--;
	PRINT_DEBUG("leaving thread: conn_stub=%p, threads=%d", conn_stub, conn_stub->threads);
	sem_post(&conn_stub_list_sem);

	/*#*/PRINT_DEBUG("sem_post: conn_stub=%p", conn_stub);
	sem_post(&conn_stub->sem);

	PRINT_DEBUG("Exited: id=%u", id);
	//pthread_exit(NULL);
	return NULL;
}

void tcp_exec_poll(struct finsFrame *ff, socket_state state, uint32_t host_ip, uint16_t host_port, uint32_t rem_ip, uint16_t rem_port, uint32_t initial,
		uint32_t flags) {
	//pthread_t thread;
	struct tcp_thread_data *thread_data;

	if (state > SS_UNCONNECTED) {
		PRINT_DEBUG("Entered: state=%u, host=%u/%u, rem=%u/%u, initial=%u, events=0x%x", state, host_ip, host_port, rem_ip, rem_port, initial, flags);
		secure_sem_wait(&conn_list_sem);
		struct tcp_connection *conn = conn_list_find(host_ip, host_port, rem_ip, rem_port);
		if (conn) {
			if (conn->threads < TCP_THREADS_MAX) {
				conn->threads++;
				/*#*/PRINT_DEBUG("");
				sem_post(&conn_list_sem);

				thread_data = (struct tcp_thread_data *) secure_malloc(sizeof(struct tcp_thread_data));
				thread_data->id = tcp_gen_thread_id();
				thread_data->conn = conn;
				thread_data->ff = ff;
				thread_data->data_len = initial;
				thread_data->flags = flags;

				//secure_pthread_create(&thread, NULL, poll_thread, (void *) thread_data);
				//pthread_detach(thread);
				//pool_execute(conn->pool, poll_thread, (void *) thread_data);
				poll_thread((void *) thread_data);
			} else {
				/*#*/PRINT_DEBUG("");
				sem_post(&conn_list_sem);

				PRINT_ERROR("Too many threads=%d. Dropping...", conn->threads);
				tcp_reply_fcf(ff, 1, POLLERR); //TODO check on value?
			}
		} else {
			PRINT_ERROR("todo error");
			sem_post(&conn_list_sem);
			//TODO error

			tcp_reply_fcf(ff, 1, POLLERR); //TODO check on value?
		}
	} else {
		PRINT_DEBUG("Entered: state=%u, host=%u/%u, initial=%u, flags=%u", state, host_ip, host_port, initial, flags);
		secure_sem_wait(&conn_stub_list_sem);
		struct tcp_connection_stub *conn_stub = conn_stub_list_find(host_ip, host_port);
		if (conn_stub) {
			if (conn_stub->threads < TCP_THREADS_MAX) {
				conn_stub->threads++;
				/*#*/PRINT_DEBUG("");
				sem_post(&conn_stub_list_sem);

				thread_data = (struct tcp_thread_data *) secure_malloc(sizeof(struct tcp_thread_data));
				thread_data->id = tcp_gen_thread_id();
				thread_data->conn_stub = conn_stub;
				thread_data->ff = ff;
				thread_data->data_len = initial;
				thread_data->flags = flags;

				//secure_pthread_create(&thread, NULL, poll_stub_thread, (void *) thread_data);
				//pthread_detach(thread);
				//pool_execute(conn_stub->pool, poll_stub_thread, (void *) thread_data);
				poll_stub_thread((void *) thread_data);
			} else {
				/*#*/PRINT_DEBUG("");
				sem_post(&conn_stub_list_sem);

				PRINT_ERROR("Too many threads=%d. Dropping...", conn_stub->threads);
				tcp_reply_fcf(ff, 1, POLLERR); //TODO check on value?
			}
		} else {
			PRINT_ERROR("todo error");
			sem_post(&conn_stub_list_sem);
			//TODO error

			tcp_reply_fcf(ff, 1, POLLERR); //TODO check on value?
		}
	}
}

void *read_param_conn_thread(void *local) {
	struct tcp_thread_data *thread_data = (struct tcp_thread_data *) local;
	uint32_t id = thread_data->id;
	struct tcp_connection *conn = thread_data->conn;
	struct finsFrame *ff = thread_data->ff;
	//socket_state state = thread_data->flags;
	free(thread_data);

	PRINT_DEBUG("Entered: ff=%p, conn=%p, id=%u", ff, conn, id);

	uint32_t value;

	metadata *params = ff->metaData;

	/*#*/PRINT_DEBUG("sem_wait: conn=%p", conn);
	secure_sem_wait(&conn->sem);
	if (conn->running_flag) {
		switch (ff->ctrlFrame.param_id) { //TODO optimize this code better when control format is fully fleshed out
		case READ_PARAM_TCP_HOST_WINDOW:
			PRINT_DEBUG("param_id=READ_PARAM_TCP_HOST_WINDOW (%d)", ff->ctrlFrame.param_id);
			value = conn->recv_win;
			secure_metadata_writeToElement(params, "ret_msg", &value, META_TYPE_INT32);

			ff->destinationID.id = ff->ctrlFrame.senderID;

			ff->ctrlFrame.senderID = TCP_ID;
			ff->ctrlFrame.opcode = CTRL_READ_PARAM_REPLY;
			ff->ctrlFrame.ret_val = 1;

			tcp_to_switch(ff);
			break;
		case READ_PARAM_TCP_SOCK_OPT:
			PRINT_DEBUG("param_id=READ_PARAM_TCP_SOCK_OPT (%d)", ff->ctrlFrame.param_id);
			//fill in with switch of opts? or have them separate?

			//TODO read sock opts

			ff->destinationID.id = ff->ctrlFrame.senderID;

			ff->ctrlFrame.senderID = TCP_ID;
			ff->ctrlFrame.opcode = CTRL_READ_PARAM_REPLY;
			ff->ctrlFrame.ret_val = 1;

			tcp_to_switch(ff);
			break;
		default:
			PRINT_ERROR("Error unknown param_id=%d", ff->ctrlFrame.param_id);
			//TODO implement?

			ff->destinationID.id = ff->ctrlFrame.senderID;

			ff->ctrlFrame.senderID = TCP_ID;
			ff->ctrlFrame.opcode = CTRL_READ_PARAM_REPLY;
			ff->ctrlFrame.ret_val = 0;

			tcp_to_switch(ff);
			break;
		}
	} else {
		PRINT_ERROR("todo error");

		ff->destinationID.id = ff->ctrlFrame.senderID;

		ff->ctrlFrame.senderID = TCP_ID;
		ff->ctrlFrame.opcode = CTRL_READ_PARAM_REPLY;
		ff->ctrlFrame.ret_val = 0;

		tcp_to_switch(ff);
	}

	/*#*/PRINT_DEBUG("");
	secure_sem_wait(&conn_list_sem);
	conn->threads--;
	PRINT_DEBUG("leaving thread: conn=%p, threads=%d", conn, conn->threads);
	sem_post(&conn_list_sem);

	/*#*/PRINT_DEBUG("sem_post: conn=%p", conn);
	sem_post(&conn->sem);

	PRINT_DEBUG("Exited: id=%u", id);
	//pthread_exit(NULL);
	return NULL;
}

void *read_param_conn_stub_thread(void *local) {
	struct tcp_thread_data *thread_data = (struct tcp_thread_data *) local;
	uint32_t id = thread_data->id;
	struct tcp_connection_stub *conn_stub = thread_data->conn_stub;
	struct finsFrame *ff = thread_data->ff;
	//socket_state state = thread_data->flags;
	free(thread_data);

	PRINT_DEBUG("Entered: ff=%p, conn_stub=%p, id=%u", ff, conn_stub, id);

	uint32_t value;

	metadata *params = ff->metaData;

	/*#*/PRINT_DEBUG("sem_wait: conn_stub=%p", conn_stub);
	secure_sem_wait(&conn_stub->sem);
	if (conn_stub->running_flag) {
		switch (ff->ctrlFrame.param_id) { //TODO optimize this code better when control format is fully fleshed out
		case READ_PARAM_TCP_HOST_WINDOW:
			PRINT_DEBUG("param_id=READ_PARAM_TCP_HOST_WINDOW (%d)", ff->ctrlFrame.param_id);
			secure_metadata_readFromElement(params, "value", &value);
			//TODO do something? error?

			//if (value > conn_stub->host_window) {
			//conn_stub->host_window -= value;
			//} else {
			//conn_stub->host_window = 0;
			//}

			ff->destinationID.id = ff->ctrlFrame.senderID;

			ff->ctrlFrame.senderID = TCP_ID;
			ff->ctrlFrame.opcode = CTRL_READ_PARAM_REPLY;
			ff->ctrlFrame.ret_val = 1;

			tcp_to_switch(ff);
			break;
		case READ_PARAM_TCP_SOCK_OPT:
			PRINT_DEBUG("param_id=READ_PARAM_TCP_SOCK_OPT (%d)", ff->ctrlFrame.param_id);
			secure_metadata_readFromElement(params, "value", &value);
			//fill in with switch of opts? or have them separate?

			//if (value > conn_stub->host_window) {
			//	conn_stub->host_window -= value;
			//} else {
			//	conn_stub->host_window = 0;
			//}

			ff->destinationID.id = ff->ctrlFrame.senderID;

			ff->ctrlFrame.senderID = TCP_ID;
			ff->ctrlFrame.opcode = CTRL_READ_PARAM_REPLY;
			ff->ctrlFrame.ret_val = 1;

			tcp_to_switch(ff);
			break;
		default:
			PRINT_ERROR("Error unknown param_id=%d", ff->ctrlFrame.param_id);
			//TODO implement?

			ff->destinationID.id = ff->ctrlFrame.senderID;

			ff->ctrlFrame.senderID = TCP_ID;
			ff->ctrlFrame.opcode = CTRL_READ_PARAM_REPLY;
			ff->ctrlFrame.ret_val = 0;

			tcp_to_switch(ff);
			break;
		}
	} else {
		PRINT_ERROR("todo error");

		ff->destinationID.id = ff->ctrlFrame.senderID;

		ff->ctrlFrame.senderID = TCP_ID;
		ff->ctrlFrame.opcode = CTRL_READ_PARAM_REPLY;
		ff->ctrlFrame.ret_val = 0;

		tcp_to_switch(ff);
	}

	/*#*/PRINT_DEBUG("");
	secure_sem_wait(&conn_stub_list_sem);
	conn_stub->threads--;
	PRINT_DEBUG("leaving thread: conn_stub=%p, threads=%d", conn_stub, conn_stub->threads);
	sem_post(&conn_stub_list_sem);

	/*#*/PRINT_DEBUG("sem_post: conn_stub=%p", conn_stub);
	sem_post(&conn_stub->sem);

	PRINT_DEBUG("Exited: id=%u", id);
	//pthread_exit(NULL);
	return NULL;
}

void tcp_read_param(struct finsFrame *ff) {
	socket_state state;
	uint32_t host_ip;
	uint16_t host_port;
	uint32_t rem_ip;
	uint16_t rem_port;

	struct tcp_connection *conn;
	struct tcp_connection_stub *conn_stub;
	//pthread_t thread;
	struct tcp_thread_data *thread_data;

	metadata *params = ff->metaData;
	metadata_read_conn(params, &state, &host_ip, &host_port, &rem_ip, &rem_port);

	if (state > SS_UNCONNECTED) {
		PRINT_DEBUG("searching: host=%u/%u, rem=%u/%u", host_ip, host_port, rem_ip, rem_port);
		secure_sem_wait(&conn_list_sem);
		conn = conn_list_find(host_ip, host_port, rem_ip, rem_port);
		if (conn) {
			if (conn->threads < TCP_THREADS_MAX) {
				conn->threads++;
				/*#*/PRINT_DEBUG("");
				sem_post(&conn_list_sem);

				thread_data = (struct tcp_thread_data *) secure_malloc(sizeof(struct tcp_thread_data));
				thread_data->id = tcp_gen_thread_id();
				thread_data->conn = conn;
				thread_data->ff = ff;

				//secure_pthread_create(&thread, NULL, read_param_conn_thread, (void *) thread_data);
				//pthread_detach(thread);
				//pool_execute(conn->pool, read_param_conn_thread, (void *) thread_data);
				read_param_conn_thread((void *) thread_data);
			} else {
				/*#*/PRINT_DEBUG("");
				sem_post(&conn_list_sem);

				PRINT_ERROR("Too many threads=%d. Dropping...", conn->threads);
			}
		} else {
			PRINT_ERROR("todo error");
			sem_post(&conn_list_sem);

			//TODO error
		}
	} else {
		PRINT_DEBUG("searching: host=%u/%u", host_ip, host_port);
		secure_sem_wait(&conn_stub_list_sem);
		conn_stub = conn_stub_list_find(host_ip, host_port);
		if (conn_stub) {
			if (conn_stub->threads < TCP_THREADS_MAX) {
				conn_stub->threads++;
				/*#*/PRINT_DEBUG("");
				sem_post(&conn_stub_list_sem);

				thread_data = (struct tcp_thread_data *) secure_malloc(sizeof(struct tcp_thread_data));
				thread_data->id = tcp_gen_thread_id();
				thread_data->conn_stub = conn_stub;
				thread_data->ff = ff;

				//secure_pthread_create(&thread, NULL, read_param_conn_stub_thread, (void *) thread_data);
				//pthread_detach(thread);
				//pool_execute(conn->pool, read_param_conn_stub_thread, (void *) thread_data);
				read_param_conn_stub_thread((void *) thread_data);
			} else {
				/*#*/PRINT_DEBUG("");
				sem_post(&conn_stub_list_sem);

				PRINT_ERROR("Too many threads=%d. Dropping...", conn_stub->threads);
			}
		} else {
			PRINT_ERROR("todo error");
			sem_post(&conn_stub_list_sem);

			//TODO error
		}
	}
}

void *set_param_conn_thread(void *local) {
	struct tcp_thread_data *thread_data = (struct tcp_thread_data *) local;
	uint32_t id = thread_data->id;
	struct tcp_connection *conn = thread_data->conn;
	struct finsFrame *ff = thread_data->ff;
	//socket_state state = thread_data->flags;
	free(thread_data);

	PRINT_DEBUG("Entered: ff=%p, conn=%p, id=%u", ff, conn, id);

	uint32_t value;

	metadata *params = ff->metaData;

	/*#*/PRINT_DEBUG("sem_wait: conn=%p", conn);
	secure_sem_wait(&conn->sem);
	if (conn->running_flag) {
		switch (ff->ctrlFrame.param_id) { //TODO optimize this code better when control format is fully fleshed out
		case SET_PARAM_TCP_HOST_WINDOW:
			PRINT_DEBUG("param_id=SET_PARAM_TCP_HOST_WINDOW (%d)", ff->ctrlFrame.param_id);
			secure_metadata_readFromElement(params, "value", &value);
			PRINT_DEBUG( "host: seqs=(%u, %u) (%u, %u), win=(%u/%u), rem: seqs=(%u, %u) (%u, %u), win=(%u/%u), before",
					conn->send_seq_num-conn->issn, conn->send_seq_end-conn->issn, conn->send_seq_num, conn->send_seq_end, conn->recv_win, conn->recv_max_win, conn->recv_seq_num-conn->irsn, conn->recv_seq_end-conn->irsn, conn->recv_seq_num, conn->recv_seq_end, conn->send_win, conn->send_max_win);
			uint32_increase(&conn->recv_win, value, conn->recv_max_win);
			PRINT_DEBUG( "host: seqs=(%u, %u) (%u, %u), win=(%u/%u), rem: seqs=(%u, %u) (%u, %u), win=(%u/%u), after",
					conn->send_seq_num-conn->issn, conn->send_seq_end-conn->issn, conn->send_seq_num, conn->send_seq_end, conn->recv_win, conn->recv_max_win, conn->recv_seq_num-conn->irsn, conn->recv_seq_end-conn->irsn, conn->recv_seq_num, conn->recv_seq_end, conn->send_win, conn->send_max_win);

			if (0) {
				ff->destinationID.id = ff->ctrlFrame.senderID;

				ff->ctrlFrame.senderID = TCP_ID;
				ff->ctrlFrame.opcode = CTRL_SET_PARAM_REPLY;
				ff->ctrlFrame.ret_val = 1;

				tcp_to_switch(ff);
			} else {
				freeFinsFrame(ff);
			}
			break;
		case SET_PARAM_TCP_SOCK_OPT:
			PRINT_DEBUG("param_id=SET_PARAM_TCP_SOCK_OPT (%d)", ff->ctrlFrame.param_id);
			secure_metadata_readFromElement(params, "value", &value);
			//fill in with switch of opts? or have them separate?

			PRINT_ERROR("todo");

			if (0) {
				ff->destinationID.id = ff->ctrlFrame.senderID;

				ff->ctrlFrame.senderID = TCP_ID;
				ff->ctrlFrame.opcode = CTRL_SET_PARAM_REPLY;
				ff->ctrlFrame.ret_val = 1;

				tcp_to_switch(ff);
			} else {
				freeFinsFrame(ff);
			}
			break;
		default:
			PRINT_ERROR("Error unknown param_id=%d", ff->ctrlFrame.param_id);
			//TODO implement?

			ff->destinationID.id = ff->ctrlFrame.senderID;

			ff->ctrlFrame.senderID = TCP_ID;
			ff->ctrlFrame.opcode = CTRL_SET_PARAM_REPLY;
			ff->ctrlFrame.ret_val = 0;

			tcp_to_switch(ff);
			break;
		}
	} else {
		PRINT_ERROR("todo error");

		ff->destinationID.id = ff->ctrlFrame.senderID;

		ff->ctrlFrame.senderID = TCP_ID;
		ff->ctrlFrame.opcode = CTRL_SET_PARAM_REPLY;
		ff->ctrlFrame.ret_val = 0;

		tcp_to_switch(ff);
	}

	/*#*/PRINT_DEBUG("");
	secure_sem_wait(&conn_list_sem);
	conn->threads--;
	PRINT_DEBUG("leaving thread: conn=%p, threads=%d", conn, conn->threads);
	sem_post(&conn_list_sem);

	/*#*/PRINT_DEBUG("sem_post: conn=%p", conn);
	sem_post(&conn->sem);

	PRINT_DEBUG("Exited: id=%u", id);
	//pthread_exit(NULL);
	return NULL;
}

void *set_param_conn_stub_thread(void *local) {
	struct tcp_thread_data *thread_data = (struct tcp_thread_data *) local;
	uint32_t id = thread_data->id;
	struct tcp_connection_stub *conn_stub = thread_data->conn_stub;
	struct finsFrame *ff = thread_data->ff;
	//socket_state state = thread_data->flags;
	free(thread_data);

	PRINT_DEBUG("Entered: ff=%p, conn_stub=%p, id=%u", ff, conn_stub, id);

	uint32_t value;

	metadata *params = ff->metaData;

	/*#*/PRINT_DEBUG("sem_wait: conn_stub=%p", conn_stub);
	secure_sem_wait(&conn_stub->sem);
	if (conn_stub->running_flag) {
		switch (ff->ctrlFrame.param_id) { //TODO optimize this code better when control format is fully fleshed out
		case SET_PARAM_TCP_HOST_WINDOW:
			PRINT_DEBUG("param_id=READ_PARAM_TCP_HOST_WINDOW (%d)", ff->ctrlFrame.param_id);
			secure_metadata_readFromElement(params, "value", &value);
			//TODO do something? error?

			//if (value > conn_stub->host_window) {
			//conn_stub->host_window -= value;
			//} else {
			//conn_stub->host_window = 0;
			//}

			ff->ctrlFrame.opcode = CTRL_SET_PARAM_REPLY;
			ff->ctrlFrame.ret_val = 1;

			tcp_to_switch(ff);
			break;
		case SET_PARAM_TCP_SOCK_OPT:
			PRINT_DEBUG("param_id=READ_PARAM_TCP_SOCK_OPT (%d)", ff->ctrlFrame.param_id);
			secure_metadata_readFromElement(params, "value", &value);
			//fill in with switch of opts? or have them separate?

			//if (value > conn_stub->host_window) {
			//	conn_stub->host_window -= value;
			//} else {
			//	conn_stub->host_window = 0;
			//}

			ff->ctrlFrame.opcode = CTRL_SET_PARAM_REPLY;
			ff->ctrlFrame.ret_val = 1;

			tcp_to_switch(ff);
			break;
		default:
			PRINT_ERROR("Error unknown param_id=%d", ff->ctrlFrame.param_id);
			//TODO implement?

			ff->ctrlFrame.opcode = CTRL_SET_PARAM_REPLY;
			ff->ctrlFrame.ret_val = 0;

			tcp_to_switch(ff);
			break;
		}
	} else {
		PRINT_ERROR("todo error");

		ff->ctrlFrame.opcode = CTRL_SET_PARAM_REPLY;
		ff->ctrlFrame.ret_val = 0;

		tcp_to_switch(ff);
	}

	/*#*/PRINT_DEBUG("");
	secure_sem_wait(&conn_stub_list_sem);
	conn_stub->threads--;
	PRINT_DEBUG("leaving thread: conn_stub=%p, threads=%d", conn_stub, conn_stub->threads);
	sem_post(&conn_stub_list_sem);

	/*#*/PRINT_DEBUG("sem_post: conn_stub=%p", conn_stub);
	sem_post(&conn_stub->sem);

	PRINT_DEBUG("Exited: id=%u", id);
	//pthread_exit(NULL);
	return NULL;
}

void tcp_set_param(struct finsFrame *ff) {
	socket_state state = 0;
	uint32_t host_ip = 0;
	uint16_t host_port = 0;
	uint32_t rem_ip = 0;
	uint16_t rem_port = 0;

	//pthread_t thread;
	struct tcp_thread_data *thread_data;

	metadata *params = ff->metaData;
	metadata_read_conn(params, &state, &host_ip, &host_port, &rem_ip, &rem_port);

	if (state > SS_UNCONNECTED) {
		PRINT_DEBUG("searching: host=%u/%u, rem=%u/%u", host_ip, host_port, rem_ip, rem_port);
		secure_sem_wait(&conn_list_sem);
		struct tcp_connection *conn = conn_list_find(host_ip, host_port, rem_ip, rem_port);
		if (conn) {
			if (conn->threads < TCP_THREADS_MAX) {
				conn->threads++;
				/*#*/PRINT_DEBUG("");
				sem_post(&conn_list_sem);

				thread_data = (struct tcp_thread_data *) secure_malloc(sizeof(struct tcp_thread_data));
				thread_data->id = tcp_gen_thread_id();
				thread_data->conn = conn;
				thread_data->ff = ff;
				thread_data->flags = state;

				//secure_pthread_create(&thread, NULL, set_param_conn_thread, (void *) thread_data);
				//pthread_detach(thread);
				//pool_execute(conn->pool, set_param_conn_thread, (void *) thread_data);
				set_param_conn_thread((void *) thread_data);
			} else {
				/*#*/PRINT_DEBUG("");
				sem_post(&conn_list_sem);

				PRINT_ERROR("Too many threads=%d. Dropping...", conn->threads);
			}
		} else {
			PRINT_ERROR("todo error");
			sem_post(&conn_list_sem);

			//TODO error

			ff->destinationID.id = ff->ctrlFrame.senderID;

			ff->ctrlFrame.senderID = TCP_ID;
			ff->ctrlFrame.opcode = CTRL_SET_PARAM_REPLY;
			ff->ctrlFrame.ret_val = 0;

			tcp_to_switch(ff);
		}
	} else {
		PRINT_DEBUG("searching: host=%u/%u", host_ip, host_port);
		secure_sem_wait(&conn_stub_list_sem);
		struct tcp_connection_stub *conn_stub = conn_stub_list_find(host_ip, host_port);
		if (conn_stub) {
			if (conn_stub->threads < TCP_THREADS_MAX) {
				conn_stub->threads++;
				/*#*/PRINT_DEBUG("");
				sem_post(&conn_stub_list_sem);

				thread_data = (struct tcp_thread_data *) secure_malloc(sizeof(struct tcp_thread_data));
				thread_data->id = tcp_gen_thread_id();
				thread_data->conn_stub = conn_stub;
				thread_data->ff = ff;
				thread_data->flags = state;

				//secure_pthread_create(&thread, NULL, set_param_conn_stub_thread, (void *) thread_data);
				//pthread_detach(thread);
				//pool_execute(conn->pool, set_param_conn_stub_thread, (void *) thread_data);
				set_param_conn_stub_thread((void *) thread_data);
			} else {
				/*#*/PRINT_DEBUG("");
				sem_post(&conn_stub_list_sem);

				PRINT_ERROR("Too many threads=%d. Dropping...", conn_stub->threads);
			}
		} else {
			PRINT_ERROR("todo error");
			sem_post(&conn_stub_list_sem);

			//TODO error

			ff->destinationID.id = ff->ctrlFrame.senderID;

			ff->ctrlFrame.senderID = TCP_ID;
			ff->ctrlFrame.opcode = CTRL_SET_PARAM_REPLY;
			ff->ctrlFrame.ret_val = 0;

			tcp_to_switch(ff);
		}
	}
}
