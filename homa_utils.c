/* This file contains miscellaneous utility functions for the Homa protocol. */

#include "homa_impl.h"

/* Separate performance counters for each core. */
struct homa_metrics *homa_metrics[NR_CPUS];

/* Points to block of memory holding all homa_metrics; used to free it. */
char *metrics_memory;

/**
 * homa_rpc_new_client() - Allocate and construct a client RPC (one that is used
 * to issue an outgoing request).
 * @hsk:      Socket to which the RPC belongs.
 * @dest:     Address of host (ip and port) to which the RPC will be sent.
 * @length:   Size of the request message.
 * @iter:     Data for the message.
 * 
 * Return:    A printer to the newly allocated object, or a negative
 *            errno if an error occurred. 
 */
struct homa_rpc *homa_rpc_new_client(struct homa_sock *hsk,
		struct sockaddr_in *dest, size_t length, struct iov_iter *iter)
{
	int err;
	struct homa_rpc *crpc;
	crpc = (struct homa_rpc *) kmalloc(sizeof(*crpc), GFP_KERNEL);
	if (unlikely(!crpc))
		return ERR_PTR(-ENOMEM);
	crpc->hsk = hsk;
	crpc->peer = homa_peer_find(&hsk->homa->peers,
			dest->sin_addr.s_addr, &hsk->inet);
	if (unlikely(IS_ERR(crpc->peer))) {
		err = PTR_ERR(crpc->peer);
		goto error;
	}
	crpc->dport = ntohs(dest->sin_port);
	crpc->id = hsk->next_outgoing_id;
	hsk->next_outgoing_id++;
	crpc->state = RPC_OUTGOING;
	crpc->is_client = true;
	crpc->msgin.total_length = -1;
	err = homa_message_out_init(&crpc->msgout, hsk, iter,
			length, crpc->peer, crpc->dport, hsk->client_port,
			crpc->id);
        if (unlikely(err != 0))
		goto error;
	list_add(&crpc->rpc_links, &hsk->client_rpcs);
	INIT_LIST_HEAD(&crpc->grantable_links);
	return crpc;
	
    error:
	kfree(crpc);
	return ERR_PTR(err);
}

/**
 * homa_rpc_new_server() - Allocate and construct a server RPC (one that is
 * used to manage an incoming request).
 * @hsk:    Socket that owns this RPC.
 * @source: IP address (network byte order) of the RPC's client.
 * @h:      Header for the first data packet received for this RPC; used
 *          to initialize the RPC.
 * 
 * Return:  A pointer to the new object, or a negative errno if an error
 *          occurred.
 */
struct homa_rpc *homa_rpc_new_server(struct homa_sock *hsk,
		__be32 source, struct data_header *h)
{
	int err;
	struct homa_rpc *srpc;
	srpc = (struct homa_rpc *) kmalloc(sizeof(*srpc),
			GFP_KERNEL);
	if (!srpc)
		return ERR_PTR(-ENOMEM);
	srpc->hsk = hsk;
	srpc->peer = homa_peer_find(&hsk->homa->peers, source, &hsk->inet);
	if (unlikely(IS_ERR(srpc->peer))) {
		err = PTR_ERR(srpc->peer);
		kfree(srpc);
		return ERR_PTR(err);
	}
	srpc->dport = ntohs(h->common.sport);
	srpc->id = h->common.id;
	srpc->state = RPC_INCOMING;
	srpc->is_client = false;
	homa_message_in_init(&srpc->msgin, ntohl(h->message_length),
			ntohl(h->unscheduled));
	srpc->msgout.length = -1;
	list_add(&srpc->rpc_links, &hsk->server_rpcs);
	INIT_LIST_HEAD(&srpc->grantable_links);
	return srpc;
}

/**
 * homa_rpc_free() - Destructor for homa_rpc; also frees the memory for the
 * structure.
 * @rpc:  Structure to clean up.
 */
void homa_rpc_free(struct homa_rpc *rpc) {
	/* Before doing anything else, unlink the input message from
	 * homa->grantable_msgs. This will synchronize to ensure that
	 * homa_manage_grants doesn't access this RPC after destruction
	 * begins. The if statement below is tricky: we'd like to avoid
	 * calling homa_remove_from_grantable (because it requires global
	 * synchronization), but the if statement is not synchronized,
	 * so it must not use any information that homa_manage_grants
	 * might be changing concurrently.
	 */
	if ((rpc->state == RPC_INCOMING) && rpc->msgin.scheduled)
		homa_remove_from_grantable(rpc->hsk->homa, rpc);
	if (rpc->state == RPC_READY)
		__list_del_entry(&rpc->ready_links);
	__list_del_entry(&rpc->rpc_links);
	homa_message_out_destroy(&rpc->msgout);
	homa_message_in_destroy(&rpc->msgin);
	kfree(rpc);
}

/**
 * homa_find_client_rpc() - Locate client-side information about the RPC that
 * a packet belongs to, if there is any.
 * @hsk:      Socket via which packet was received.
 * @sport:    Port from which the packet was sent.
 * @id:       Unique identifier for the RPC.
 * 
 * Return:    A pointer to the homa_rpc for this id, or NULL if none.
 */
struct homa_rpc *homa_find_client_rpc(struct homa_sock *hsk,
		__u16 sport, __u64 id)
{
	struct list_head *pos;
	list_for_each(pos, &hsk->client_rpcs) {
		struct homa_rpc *crpc = list_entry(pos, struct homa_rpc,
				rpc_links);
		if (crpc->id == id) {
			return crpc;
		}
	}
	return NULL;
}

/**
 * homa_destroy() -  Destructor for homa objects.
 * @homa:      Object to destroy.
 */
void homa_destroy(struct homa *homa)
{
	int i;
	homa_socktab_destroy(&homa->port_map);
	homa_peertab_destroy(&homa->peers);
	if (metrics_memory) {
		kfree(metrics_memory);
		metrics_memory = NULL;
		for (i = 0; i < NR_CPUS; i++) {
			homa_metrics[i] = NULL;
		}
	}
	if (homa->metrics)
		kfree(homa->metrics);
}

/**
 * homa_find_server_rpc() - Locate server-side information about the RPC that
 * a packet belongs to, if there is any.
 * @hsk:      Socket via which packet was received.
 * @saddr:    Address from which the packet was sent.
 * @sport:    Port at @saddr from which the packet was sent.
 * @id:       Unique identifier for the RPC.
 * 
 * Return:    A pointer to the homa_rpc for this saddr-id combination,
 *            or NULL if none.
 */
struct homa_rpc *homa_find_server_rpc(struct homa_sock *hsk,
		__be32 saddr, __u16 sport, __u64 id)
{
	struct list_head *pos;
	list_for_each(pos, &hsk->server_rpcs) {
		struct homa_rpc *srpc = list_entry(pos, struct homa_rpc,
				rpc_links);
		if ((srpc->id == id) &&
				(srpc->dport == sport) &&
				(srpc->peer->addr == saddr)) {
			return srpc;
		}
	}
	return NULL;
}

/**
 * homa_init() - Constructor for homa objects.
 * @homa:   Object to initialize.
 * 
 * Return:  0 on success, or a negative errno if there was an error.
 */
int homa_init(struct homa *homa)
{
	size_t aligned_size;
	char *first;
	int i, err;
	_Static_assert(HOMA_NUM_PRIORITIES >= 8,
			"homa_init assumes at least 8 priority levels");
	
	/* Initialize Homa metrics (if no-one else has already done it),
	 * making sure that each core has private cache lines for its metrics.
	 */
	if (!metrics_memory) {
		aligned_size = (sizeof(homa_metrics) + 0x3f) & ~0x3f;
		metrics_memory = kmalloc(0x3f + (NR_CPUS*aligned_size),
				GFP_KERNEL);
		first = (char *) (((__u64) metrics_memory + 0x3f) & ~0x3f);
		for (i = 0; i < NR_CPUS; i++) {
			homa_metrics[i] = (struct homa_metrics *)
					(first + i*aligned_size);
			memset(homa_metrics[i], 0, aligned_size);
		}
	}
	
	homa->next_client_port = HOMA_MIN_CLIENT_PORT;
	homa_socktab_init(&homa->port_map);
	err = homa_peertab_init(&homa->peers);
	if (err) {
		printk(KERN_ERR "Couldn't initialize peer table (errno %d)\n",
			-err);
		return err;
	}
	
	/* Wild guesses to initialize configuration values... */
	homa->rtt_bytes = 10000;
	homa->max_prio = HOMA_NUM_PRIORITIES - 1;
	homa->min_prio = 0;
	homa->max_sched_prio = HOMA_NUM_PRIORITIES - 5;
	homa->unsched_cutoffs[HOMA_NUM_PRIORITIES-1] = 200;
	homa->unsched_cutoffs[HOMA_NUM_PRIORITIES-2] =
			2*HOMA_MAX_DATA_PER_PACKET;
	homa->unsched_cutoffs[HOMA_NUM_PRIORITIES-3] =
			10*HOMA_MAX_DATA_PER_PACKET;
	homa->unsched_cutoffs[HOMA_NUM_PRIORITIES-4] = HOMA_MAX_MESSAGE_SIZE;
#ifdef __UNIT_TEST__
	/* Unit tests won't send CUTOFFS messages unless the test changes
	 * this variable.
	 */
	homa->cutoff_version = 0;
#else
	homa->cutoff_version = 1;
#endif
	homa->max_overcommit = 8;
	spin_lock_init(&homa->grantable_lock);
	INIT_LIST_HEAD(&homa->grantable_rpcs);
	homa->num_grantable = 0;
	spin_lock_init(&homa->metrics_lock);
	homa->metrics = NULL;
	homa->metrics_capacity = 0;
	homa->metrics_length = 0;
	homa->metrics_active_opens = 0;
	return 0;
}

/**
 * homa_print_ipv4_addr() - Convert an IPV4 address to the standard string
 * representation.
 * @addr:    Address to convert, in network byte order.
 * @buffer:  Where to store the converted value; must have room for
 *           "255.255.255.255" plus a terminating NULL character.
 * 
 * Return:   The converted value (@buffer).
 * 
 * Note: Homa uses this function, rather than the %pI4 format specifier
 * for snprintf et al., because the kernel's version of snprintf isn't
 * available in Homa's unit test environment.
 */
char *homa_print_ipv4_addr(__be32 addr, char *buffer)
{
	__u32 a2 = ntohl(addr);
	sprintf(buffer, "%u.%u.%u.%u", (a2 >> 24) & 0xff, (a2 >> 16) & 0xff,
			(a2 >> 8) & 0xff, a2 & 0xff);
	return buffer;
}

/**
 * homa_print_packet() - Print a human-readable string describing the
 * information in a Homa packet.
 * @skb:     Packet whose information should be printed.
 * @buffer:  Buffer in which to generate the string.
 * @length:  Number of bytes available at @buffer.
 * 
 * Return:   @buffer
 */
char *homa_print_packet(struct sk_buff *skb, char *buffer, int length)
{
	char *pos = buffer;
	int space_left = length;
	char addr_buf[20];
	struct common_header *common = (struct common_header *) skb->data;
	
	int result = snprintf(pos, space_left,
		"%s from %s:%u, dport %d, id %llu, length %u",
		homa_symbol_for_type(common->type),
		homa_print_ipv4_addr(ip_hdr(skb)->saddr, addr_buf),
		ntohs(common->sport), ntohs(common->dport), common->id,
		skb->len);
	if ((result == length) || (result < 0)) {
		buffer[length-1] = 0;
		return buffer;
	}
	pos += result;
	space_left -= result;
	if (skb->vlan_tci & VLAN_TAG_PRESENT) {
		int priority = (skb->vlan_tci & VLAN_PRIO_MASK)
				>> VLAN_PRIO_SHIFT;
		if (priority == 0) {
			priority = 1;
		} else if (priority == 1) {
			priority = 0;
		}
		result = snprintf(pos, space_left, " prio %d", priority);
		if ((result == length) || (result < 0)) {
			buffer[length-1] = 0;
			return buffer;
		}
		pos += result;
		space_left -= result;
	}		
	switch (common->type) {
	case DATA: {
		struct data_header *h = (struct data_header *)
				skb->data;
		snprintf(pos, space_left,
				", message_length %d, offset %d, unscheduled %d%s",
				ntohl(h->message_length), ntohl(h->offset),
				ntohl(h->unscheduled),
				h->retransmit ? " RETRANSMIT" : "");
		break;
	}
	case GRANT: {
		struct grant_header *h = (struct grant_header *) skb->data;
		snprintf(pos, space_left, ", offset %d, grant_prio %u",
				ntohl(h->offset), h->priority);
		break;
	}
	case RESEND: {
		struct resend_header *h = (struct resend_header *) skb->data;
		snprintf(pos, space_left,
				", offset %d, length %d, resend_prio %u%s",
				ntohl(h->offset), ntohl(h->length),
				h->priority, h->restart ? ", RESTART" : "");
		break;
	}
	case BUSY:
		/* Nothing to add here. */
		break;
	case CUTOFFS: {
		struct cutoffs_header *h = (struct cutoffs_header *) skb->data;
		snprintf(pos, space_left,
				", cutoffs %d %d %d %d %d %d %d %d, version %u",
				ntohl(h->unsched_cutoffs[0]),
				ntohl(h->unsched_cutoffs[1]),
				ntohl(h->unsched_cutoffs[2]),
				ntohl(h->unsched_cutoffs[3]),
				ntohl(h->unsched_cutoffs[4]),
				ntohl(h->unsched_cutoffs[5]),
				ntohl(h->unsched_cutoffs[6]),
				ntohl(h->unsched_cutoffs[7]),
				ntohs(h->cutoff_version));
		break;
	}}
	buffer[length-1] = 0;
	return buffer;
}

/**
 * homa_print_packet() - Print a human-readable string describing the
 * information in a Homa packet. This function generates a more
 * abbreviated description than homa_print_packet.
 * @skb:     Packet whose information should be printed.
 * @buffer:  Buffer in which to generate the string.
 * @length:  Number of bytes available at @buffer.
 * 
 * Return:   @buffer
 */
char *homa_print_packet_short(struct sk_buff *skb, char *buffer, int length)
{
	struct common_header *common = (struct common_header *) skb->data;
	switch (common->type) {
	case DATA: {
		struct data_header *h = (struct data_header *) skb->data;
		snprintf(buffer, length, "DATA%s %d/%d",
				h->retransmit ? " retrans" : "",
				ntohl(h->offset), ntohl(h->message_length));
		break;
	}
	case GRANT: {
		struct grant_header *h = (struct grant_header *) skb->data;
		snprintf(buffer, length, "GRANT %d@%d", ntohl(h->offset),
				h->priority);
		break;
	}
	case RESEND: {
		struct resend_header *h = (struct resend_header *) skb->data;
		snprintf(buffer, length, "RESEND %d-%d@%d", ntohl(h->offset),
				ntohl(h->offset) + ntohl(h->length) - 1,
				h->priority);
		break;
	}
	case BUSY:
		snprintf(buffer, length, "BUSY");
		break;
	case CUTOFFS: 
		snprintf(buffer, length, "CUTOFFS");
		break;
	default:
		snprintf(buffer, length, "unknown packet type %d",
				common->type);
		break;
	}
	return buffer;
}

/**
 * homa_symbol_for_type() - Returns a printable string describing a packet type.
 * @type:  A value from those defined by &homa_packet_type.
 * 
 * Return: A static string holding the packet type corresponding to @type.
 */
char *homa_symbol_for_type(uint8_t type)
{
	static char buffer[20];
	switch (type) {
	case DATA:
		return "DATA";
	case GRANT:
		return "GRANT";
	case RESEND:
		return "RESEND";
	case BUSY:
		return "BUSY";
	case CUTOFFS:
		return "CUTOFFS";
	}
	
	/* Using a static buffer can produce garbled text under concurrency,
	 * but (a) it's unlikely (this code only executes if the opcode is
	 * bogus), (b) this is mostly for testing and debugging, and (c) the
	 * code below ensures that the string cannot run past the end of the
	 * buffer, so the code is safe. */
	snprintf(buffer, sizeof(buffer)-1, "UNKNOWN(%u)", type);
	buffer[sizeof(buffer)-1] = 0;
	return buffer;
}

/**
 * homa_compile_metrics() - Combine all of the core-specific metrics into
 * a single collection.
 * @m:    Will be updated so that each entry in this structure contains
 *        the sum of all of the values from the core-specific metrics
 *        structures.
 */
void homa_compile_metrics(struct homa_metrics *m)
{
	int i, j;
	memset(m, 0, sizeof(*m));
	for (i = 0; i < NR_CPUS; i++) {
		struct homa_metrics *cm = homa_metrics[i];
		for (j = 0; j < HOMA_NUM_SMALL_COUNTS; j++)
			m->small_msg_bytes[j] += cm->small_msg_bytes[j];
		for (j = 0; j < HOMA_NUM_MEDIUM_COUNTS; j++)
			m->medium_msg_bytes[j] += cm->medium_msg_bytes[j];
		m->large_msg_bytes += cm->large_msg_bytes;
		for (j = DATA; j < BOGUS;  j++) {
			m->packets_sent[j-DATA] += cm->packets_sent[j-DATA];
			m->packets_received[j-DATA] +=
					cm->packets_received[j-DATA];
		}
		m->peer_hash_links += cm->peer_hash_links;
		m->peer_new_entries += cm->peer_new_entries;
		m->peer_kmalloc_errors += cm->peer_kmalloc_errors;
		m->peer_route_errors += cm->peer_route_errors;
		m->control_xmit_errors += cm->control_xmit_errors;
		m->data_xmit_errors += cm->data_xmit_errors;
		m->unknown_rpc += cm->unknown_rpc;
		m->server_cant_create_rpc += cm->server_cant_create_rpc;
		m->unknown_packet_type += cm->unknown_packet_type;
	}
}


/**
 * homa_append_metric() - Formats a new metric and appends it to homa->metrics.
 * @homa:        The new data will appended to the @metrics field of
 *               this structure.
 * @format:      Standard printf-style format string describing the
 *               new metric. Arguments after this provide the usual
 *               values expected for printf-like functions.
 */
void homa_append_metric(struct homa *homa, const char* format, ...)
{
	char *new_buffer;
	size_t new_chars;
	va_list ap;
	
	if (!homa->metrics) {
#ifdef __UNIT_TEST__
		homa->metrics_capacity =  30;
#else
		homa->metrics_capacity =  4096;
#endif
		homa->metrics =  kmalloc(homa->metrics_capacity, GFP_KERNEL);
		homa->metrics_length = 0;
	}
	
	/* May have to execute this loop multiple times if we run out
	 * of space in homa->metrics; each iteration expands the storage,
	 * until eventually it is large enough.
	 */
	while (true) {
		va_start(ap, format);
		new_chars = vsnprintf(homa->metrics + homa->metrics_length,
				homa->metrics_capacity - homa->metrics_length,
				format, ap);
		va_end(ap);
		if ((homa->metrics_length + new_chars) < homa->metrics_capacity)
			break;
		
		/* Not enough room; expand buffer capacity. */
		homa->metrics_capacity *= 2;
		new_buffer = kmalloc(homa->metrics_capacity, GFP_KERNEL);
		memcpy(new_buffer, homa->metrics, homa->metrics_length);
		kfree(homa->metrics);
		homa->metrics = new_buffer;
	}
	homa->metrics_length += new_chars;
}

/**
 * homa_print_metrics() - Sample all of the Homa performance metrics and
 * generate a human-readable string describing all of them.
 * @homa:    Overall data about the Homa protocol implementation;
 *           the formatted string will be stored in homa->metrics.
 * 
 * Return:   The formatted string. 
 */
char *homa_print_metrics(struct homa *homa)
{
	struct homa_metrics m;
	int i;
	__u64 total_bytes = 0;
	
	homa_compile_metrics(&m);
	homa->metrics_length = 0;
	for (i = 0; i < HOMA_NUM_SMALL_COUNTS; i++) {
		total_bytes += m.small_msg_bytes[i];
		homa_append_metric(homa,
			"msg_bytes_%-9d    %15llu  "
			"Bytes in messages containing < %d bytes\n",
			(i+1)*64, total_bytes, (i+1)*64);
	}
	for (i = (HOMA_NUM_SMALL_COUNTS*64)/1024; i < HOMA_NUM_MEDIUM_COUNTS;
			i++) {
		total_bytes += m.medium_msg_bytes[i];
		homa_append_metric(homa,
			"msg_bytes_%-9d    %15llu  "
			"Bytes in messages containing < %d bytes\n",
			(i+1)*1024, total_bytes, (i+1)*1024);
	}
	total_bytes += m.large_msg_bytes;
	homa_append_metric(homa,
			"total_msg_bytes        %15llu  "
			"Bytes in all messages\n",
			total_bytes);
	for (i = DATA; i < BOGUS;  i++) {
		char *symbol = homa_symbol_for_type(i);
		homa_append_metric(homa,
				"packets_sent_%-6s    %15llu  "
				"%s packets sent\n",
				symbol, m.packets_sent[i-DATA], symbol);
	}
	for (i = DATA; i < BOGUS;  i++) {
		char *symbol = homa_symbol_for_type(i);
		homa_append_metric(homa,
				"packets_rcvd_%-6s    %15llu  "
				"%s packets received\n",
				symbol, m.packets_received[i-DATA], symbol);
	}
	homa_append_metric(homa,
			"peer_hash_links        %15llu  "
			"hash chain link traversals in peer table\n",
			m.peer_hash_links);
	homa_append_metric(homa,
			"peer_new_entries       %15llu  "
			"new entries created in peer table\n",
			m.peer_new_entries);
	homa_append_metric(homa,
			"peer_kmalloc_errors    %15llu  "
			"kmalloc failures creating peer table entries\n",
			m.peer_kmalloc_errors);
	homa_append_metric(homa,
			"peer_route_errors      %15llu  "
			"routing failures creating peer table entries\n",
			m.peer_route_errors);
	homa_append_metric(homa,
			"control_xmit_errors    %15llu  "
			"errors sending control packets\n",
			m.peer_route_errors);
	homa_append_metric(homa,
			"data_xmit_errors       %15llu  "
			"errors sending data packets\n",
			m.peer_route_errors);
	homa_append_metric(homa,
			"unknown_rpc            %15llu  "
			"packets discarded because RPC is unknown\n",
			m.peer_route_errors);
	homa_append_metric(homa,
			"server_cant_create_rpc %15llu  "
			"packets discarded because server couldn't create RPC\n",
			m.peer_route_errors);
	homa_append_metric(homa,
			"unknown_packet_type    %15llu  "
			"packets discarded because of unsupported type\n",
			m.peer_route_errors);
	
	return homa->metrics;
}

/**
 * homa_prios_changed() - This function is called whenever configuration
 * information related to priorities, such as @homa->unsched_cutoffs or
 * @homa->min_prio, is modified. It adjust the cutoffs if needed to maintain
 * consistency, and it updates other values that depend on this information.
 * @homa: Contains the priority info to be checked and updated.
 */
void homa_prios_changed(struct homa *homa)
{
	int i;
	
	/* This guarantees that we will choose priority 0 if nothing else
	 * in the cutoff array matches.
	 */
	homa->unsched_cutoffs[0] = INT_MAX;
	
	for (i = HOMA_NUM_PRIORITIES-1; ; i--) {
		if (i > homa->max_prio) {
			homa->unsched_cutoffs[i] = 0;
			continue;
		}
		if (i == homa->min_prio) {
			homa->unsched_cutoffs[i] = INT_MAX;
			homa->max_sched_prio = i-1;
			break;
		}
		if ((homa->unsched_cutoffs[i] >= HOMA_MAX_MESSAGE_SIZE)) {
			homa->max_sched_prio = i-1;
			break;
		}
	}
	if (homa->max_sched_prio < homa->min_prio) {
		/* Must have at least one priority level for scheduled
		 * packets; will end up with min_prio shared between
		 * scheduled and unscheduled packets.
		 */
		homa->max_sched_prio = homa->min_prio;
	}
	homa->cutoff_version++;
}