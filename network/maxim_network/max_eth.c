#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/timeouts.h"
#include "lwip/stats.h"
#include "lwip/init.h"
#include "lwip/tcpbase.h"
#include "lwip/tcpip.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include "lwip/api.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "netif/ethernet.h"
#include "lwip/inet.h"

#include "lwipcfg.h"
#include "lwipopts.h"
#include "arch/cc.h"
#include "max_eth.h"

#include "mxc_device.h"
#include "maxim_irq.h"
#include "maxim_gpio_irq.h"
#include "no_os_timer.h"
#include "no_os_delay.h"
#include "maxim_timer.h"
#include "tcp_socket.h"

#include "adin1110.h"

static uint8_t lwip_buff[ADIN1110_LWIP_BUFF_SIZE];

static void max_eth_config_noos_if(struct max_eth_desc *desc);

/* Get socket pointer from socket id */
static struct socket_desc *_get_sock(struct max_eth_desc *desc, uint32_t id)
{
	if (id >= MAX_SOCKETS)
		return NULL;

	return &desc->sockets[id];
}

/* Get first socket with state SOCKET_UNUSED. Then, set id with index */
static int32_t _get_unused_socket(struct max_eth_desc *desc, uint32_t *id)
{
	uint32_t i;

	for (i = 0; i < MAX_SOCKETS; i++)
		if (desc->sockets[i].state == SOCKET_UNUSED) {
			*id = i;
			desc->sockets[i].state = SOCKET_DISCONNECTED;

			return 0;
		}

	/* All the available connections are used */
	return -ENOMEM;
}

/* Mark socket as SOCKET_UNUSED. */
static void _release_socket(struct max_eth_desc *desc, uint32_t id)
{
	struct socket_desc *sock = _get_sock(desc, id);

	sock->state = SOCKET_UNUSED;
}

static err_t mxc_eth_netif_output(struct netif *netif, struct pbuf *p)
{
	struct max_eth_desc *eth_desc;
	struct adin1110_desc *mac_desc;
	struct adin1110_eth_buff buff;
	uint32_t frame_len;
	int ret;

	eth_desc = netif->state;
	mac_desc = eth_desc->mac_desc;

	LINK_STATS_INC(link.xmit);
	frame_len = pbuf_copy_partial(p, lwip_buff, p->tot_len, 0);

	memcpy(&buff.mac_dest, lwip_buff, ADIN1110_ETH_HDR_LEN);

	buff.len = frame_len;
	buff.payload = &lwip_buff[ADIN1110_ETH_HDR_LEN];

	/* The TX FIFO might be full, so retry. */
	do {
		__disable_irq();
		ret = adin1110_write_fifo(mac_desc, 0, &buff);
		__enable_irq();
	} while (ret == -EAGAIN);

	return ret;
}

static err_t max_eth_netif_init(struct netif *netif)
{
	unsigned char hwaddr[MAC_LEN] = {MAC_BYTE1, MAC_BYTE2, MAC_BYTE3,
					 MAC_BYTE4, MAC_BYTE5, MAC_BYTE6
					};

	netif->linkoutput = mxc_eth_netif_output;
	netif->output = etharp_output;
	netif->mtu = MXC_NETIF_MTU_SIZE;
	netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;

	memcpy(netif->hwaddr, hwaddr, MAC_LEN);
	netif->hwaddr_len = MAC_LEN;

	return ERR_OK;
}

static struct pbuf *get_recvd_frames(struct max_eth_desc *eth_desc)
{
	uint32_t frame_cnt;
	size_t offset = 0;
	struct adin1110_desc *mac_desc;
	struct adin1110_eth_buff mac_buff = {0};
	struct pbuf *p = NULL;
	int ret;

	mac_desc = eth_desc->mac_desc;
	mac_buff.payload = &lwip_buff[ADIN1110_ETH_HDR_LEN];

	// do {
	// ret = adin1110_reg_read(mac_desc, ADIN1110_RX_FSIZE_REG, &frame_cnt);
	// if (ret)
	// 	goto out;
	// } while (frame_cnt > 0xFFFFF);

	// if (!frame_cnt)
	// 	goto out;

	ret = adin1110_read_fifo(mac_desc, 0, &mac_buff);
	if (ret || !mac_buff.len)
		goto out;

	memcpy(lwip_buff, mac_buff.mac_dest, ADIN1110_ETH_HDR_LEN);
	p = pbuf_alloc(PBUF_RAW, mac_buff.len, PBUF_POOL);
	if (p != NULL)
		pbuf_take(p, lwip_buff, mac_buff.len);

out:
	return p;
}

int max_lwip_tick(void *data)
{
	struct max_eth_desc *eth_desc = data;
	struct adin1110_desc *mac_desc;
	struct netif *netif_desc;
	struct pbuf *p;
	int ret = 0;

	netif_desc = eth_desc->lwip_netif;
	mac_desc = eth_desc->mac_desc;

	do {
		__disable_irq();
		p = get_recvd_frames(eth_desc);
		__enable_irq();
		if (p != NULL) {
			LINK_STATS_INC(link.recv);
			ret = netif_desc->input(p, netif_desc);
			if (ret) {
				if (p->ref)
					pbuf_free(p);
			}
		}
	} while(p);

	sys_check_timeouts();

	return ret;
}

int max_eth_init(struct netif **netif_desc, struct max_eth_param *param)
{
	struct network_interface *network_descriptor;
	struct max_eth_desc *descriptor;
	struct netif *netif_descriptor;
	ip4_addr_t ipaddr, netmask, gw;
	uint32_t reg_val;
	char *addr;
	int ret;

	if (!param)
		return -EINVAL;

	network_descriptor = calloc(1, sizeof(*network_descriptor));
	if (!network_descriptor)
		return -ENOMEM;

	netif_descriptor = calloc(1, sizeof(*netif_descriptor));
	if (!netif_descriptor) {
		ret = -ENOMEM;
		goto free_network_descriptor;
	}

	descriptor = calloc(1, sizeof(*descriptor));
	if (!descriptor) {
		ret = -ENOMEM;
		goto free_netif;
	}

	ret = adin1110_init(&descriptor->mac_desc, &param->adin1110_ip);
	if (ret)
		goto free_descriptor;

	lwip_init();

	/* This doesn't support static IP assignment and requires a DHCP server. */
	ip4_addr_set_zero(&ipaddr);
	ip4_addr_set_zero(&netmask);
	ip4_addr_set_zero(&gw);

	netif_add(netif_descriptor, &ipaddr, &netmask, &gw, NULL, max_eth_netif_init,
		  ethernet_input);
	descriptor->lwip_netif = netif_descriptor;
	netif_descriptor->state = descriptor;

	ret = adin1110_set_mac_addr(descriptor->mac_desc, netif_descriptor->hwaddr);
	if (ret)
		goto free_descriptor;

	ret = adin1110_broadcast_filter(descriptor->mac_desc, true);
	if (ret)
		goto free_descriptor;

	descriptor->name[0] = param->name[0];
	descriptor->name[1] = param->name[1];

	netif_set_default(netif_descriptor);
	netif_set_up(netif_descriptor);

	netif_set_link_up(netif_descriptor);
	ret = dhcp_start(netif_descriptor);
	if (ret)
		return ret;

	max_eth_config_noos_if(descriptor);

	*netif_desc = netif_descriptor;

	return 0;

free_descriptor:
	free(descriptor);
free_netif:
	free(netif_descriptor);
free_network_descriptor:
	free(network_descriptor);

	return ret;
}

void max_eth_err_callback(void *arg, err_t err)
{
	printf("Error :? %d\n", err);
}

err_t max_eth_recv_callback(void *arg, struct tcp_pcb *tpcb, struct pbuf *p,
			    err_t err)
{
	struct socket_desc *sock = arg;

	if (!p) {
		tcp_recv(sock->pcb, NULL);
		sock->state = SOCKET_DISCONNECTED;

		return ERR_OK;
	}

	if (err != ERR_OK) {
		pbuf_free(p);
		return err;
	}

	if (!sock->p) {
		sock->p = p;
		sock->p_idx = 0;
	} else {
		if (p != sock->p)
			pbuf_chain(sock->p, p);
	}

	return ERR_OK;
}

static void max_eth_config_socket(struct socket_desc *socket)
{
	tcp_arg(socket->pcb, socket);
	tcp_recv(socket->pcb, max_eth_recv_callback);
	tcp_err(socket->pcb, max_eth_err_callback);
}

static int32_t max_socket_open(void *net, uint32_t sock_id,
			       enum socket_protocol proto,
			       uint32_t buff_size)
{
	struct max_eth_desc *desc = net;
	struct tcp_pcb *pcb;
	int32_t ret;

	ret = _get_unused_socket(desc, &sock_id);
	if (ret)
		return ret;

	pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
	if (!pcb) {
		_release_socket(desc, sock_id);
		return -ENOMEM;
	}

	ip_set_option(pcb, SOF_REUSEADDR);

	desc->sockets[sock_id].pcb = pcb;
	desc->sockets[sock_id].desc = desc;
	desc->sockets[sock_id].id = sock_id;
	desc->sockets[sock_id].p = NULL;

	max_eth_config_socket(&desc->sockets[sock_id]);

	return 0;
}

static int32_t max_socket_close(void *net, uint32_t sock_id)
{
	struct max_eth_desc *desc = net;
	struct socket_desc *sock;
	struct pbuf *p, *old_p;
	err_t ret;

	sock = _get_sock(desc, sock_id);
	if (!sock)
		return -EINVAL;

	if (sock->state == SOCKET_UNUSED)
		return -ENOENT;

	tcp_recv(sock->pcb, NULL);
	tcp_err(sock->pcb, NULL);

	if (sock->p) {
		tcp_recved(sock->pcb, sock->p->tot_len);
		pbuf_free(sock->p);
	}

	/*
	 * This may fail if there is not enough memory for the RST pbuf.
	 * In such case retry.
	 */
	do {
		ret = tcp_close(sock->pcb);
	} while(ret != ERR_OK);

	sock->p_idx = 0;
	sock->p = NULL;
	_release_socket(desc, sock_id);

	return 0;
}

static int32_t max_socket_send(void *net, uint32_t sock_id, const void *data,
			       uint32_t size)
{
	struct max_eth_desc *desc = net;
	struct socket_desc *sock;
	uint32_t segment_size;
	uint32_t avail;
	uint32_t flags;
	err_t err;
	int ret;

	MXC_GPIO_OutPut(MXC_GPIO_GET_GPIO(2), 1 << 1, 0);
	MXC_GPIO_OutPut(MXC_GPIO_GET_GPIO(2), 1 << 1, 1 << 1);
	sock = _get_sock(desc, sock_id);
	if (!sock)
		return -EINVAL;

	if (sock->state != SOCKET_CONNECTED)
		return -ENOTCONN;

	avail = tcp_sndbuf(sock->pcb);
	flags = TCP_WRITE_FLAG_COPY;
	if (avail < size)
		/* Partial write */
		flags |= TCP_WRITE_FLAG_MORE;

	size = no_os_min(avail, size);
	err = tcp_write(sock->pcb, data, size, flags);
	if (err != ERR_OK) {
		ret = err;
		printf("TCP write err: %d\n", ret);

		if (err == ERR_MEM)
			return -EAGAIN;

		return ret;
	}

	if (!(flags & TCP_WRITE_FLAG_MORE)) {
		/* Mark data as ready to be sent */
		err = tcp_output(sock->pcb);
		if (err != ERR_OK) {
			ret = err;
			printf("TCP output err: %d\n", ret);

			if (err == ERR_MEM)
				return -EAGAIN;

			return ret;
		}
	} else {
		printf("More\n");
	}

	MXC_GPIO_OutPut(MXC_GPIO_GET_GPIO(2), 1 << 1, 0);

	return size;
}

static int32_t max_socket_recv(void *net, uint32_t sock_id, void *data,
			       uint32_t size)
{
	struct max_eth_desc *desc = net;
	struct socket_desc *sock;
	struct pbuf *p, *old_p;
	uint8_t *buf, *pdata;
	static bool refed = 0;
	uint32_t i, len;

	sock = _get_sock(desc, sock_id);
	if (!sock)
		return -EINVAL;

	if (sock->state != SOCKET_CONNECTED)
		return -ENOTCONN;

	i = 0;
	p = sock->p;
	pdata = data;

	/* Iterate over payloads until requested data has been read */
	while (p && i < size) {
		len = no_os_min(size - i, p->len - sock->p_idx);
		buf = p->payload;
		buf += sock->p_idx;
		memcpy(pdata + i, buf, len);
		i += len;
		sock->p_idx += len;
		if (sock->p_idx == p->len) {
			/* Done with current p. Cleanup and mark as read */
			old_p = p;
			p = p->next;
			if (p)
				pbuf_ref(p);

			if (old_p->ref > 0)
				pbuf_free(old_p);
			tcp_recved(sock->pcb, sock->p_idx);
			sock->p_idx = 0;
		}
	}
	sock->p = p;

	return i;
}

static int32_t max_socket_bind(void *net, uint32_t sock_id, uint16_t port)
{
	struct max_eth_desc *desc = net;
	struct socket_desc *sock;
	err_t err;

	sock = _get_sock(desc, sock_id);
	if (!sock)
		return -EINVAL;

	err = tcp_bind(sock->pcb, IP_ANY_TYPE, port);
	if (err != ERR_OK) {
		printf("Unable to bind port %"PRIu16"\n", port);
		return -EINVAL;
	}

	return 0;
}

static int32_t max_socket_listen(void *net, uint32_t sock_id, uint32_t back_log)
{
	struct max_eth_desc *desc = net;
	struct socket_desc *sock;
	struct tcp_pcb *pcb;

	sock = _get_sock(desc, sock_id);
	if (!sock)
		return -EINVAL;

	pcb = tcp_listen_with_backlog(sock->pcb, back_log);
	if (!pcb) {
		printf("Unable to listen on socket\n");
		return -ENOMEM;
	}
	sock->pcb = pcb;
	sock->state = LISTEN;

	max_eth_config_socket(sock);

	return 0;
}

static err_t max_eth_accept_callback(void *arg, struct tcp_pcb *new_pcb,
				     err_t err)
{
	int32_t ret;
	uint32_t id;
	struct socket_desc *sock;
	struct socket_desc *serv_sock = arg;
	struct max_eth_desc *desc = serv_sock->desc;

	if (err != ERR_OK) {
		printf("Accept callback err %d\n", err);
		return ERR_OK;
	}

	ret = _get_unused_socket(desc, &id);
	if (ret)
		return ret;

	sock = _get_sock(desc, id);
	sock->pcb = new_pcb;
	sock->state = SOCKET_WAITING_ACCEPT;

	tcp_setprio(sock->pcb, 0);

	max_eth_config_socket(sock);

	tcp_nagle_disable(sock->pcb);

	return 0;
}

static int32_t max_socket_accept(void *net, uint32_t sock_id,
				 uint32_t *client_socket_id)
{
	struct max_eth_desc *desc = net;
	struct socket_desc *serv_sock;
	struct socket_desc *cli_sock;
	uint32_t i;

	serv_sock = _get_sock(desc, sock_id);
	if (!serv_sock)
		return -EINVAL;

	if (serv_sock->state != SOCKET_ACCEPTING) {
		if (serv_sock->state != SOCKET_LISTENING)
			return -EINVAL;
		tcp_accept(serv_sock->pcb, max_eth_accept_callback);
		serv_sock->state = SOCKET_ACCEPTING;
	}

	for (i = 0; i < MAX_SOCKETS; ++i) {
		cli_sock = &desc->sockets[i];
		if (cli_sock->state == SOCKET_WAITING_ACCEPT) {
			/* New client connection for server */
			*client_socket_id = i;
			cli_sock->state = SOCKET_CONNECTED;
			return 0;
		}
	}

	return -EAGAIN;
}

static int32_t max_socket_sendto(void *net, uint32_t sock_id, const void *data,
				 uint32_t size, const struct socket_address *to)
{
	return -ENOENT;
}

static int32_t max_socket_recvfrom(void *net, uint32_t sock_id, void *data,
				   uint32_t size,
				   struct socket_address *from)
{
	return -ENOENT;
}

static int32_t max_socket_connect(void *net, uint32_t sock_id,
				  struct socket_address *addr)
{
	return -ENOENT;

}

static int32_t max_socket_disconnect(void *net, uint32_t sock_id)
{
	return -ENOENT;
}

u32_t sys_now(void)
{
	struct no_os_time time = no_os_get_time();

	return time.s * 1000 + time.us / 1000;
}

static void max_eth_config_noos_if(struct max_eth_desc *desc)
{
	struct network_interface *net = &desc->noos_net;

	net->socket_open = max_socket_open;
	net->socket_close = max_socket_close;
	net->socket_connect = max_socket_connect;
	net->socket_disconnect = max_socket_disconnect;
	net->socket_send = max_socket_send;
	net->socket_recv = max_socket_recv;
	net->socket_sendto = max_socket_sendto;
	net->socket_recvfrom = max_socket_recvfrom;
	net->socket_bind = max_socket_bind;
	net->socket_listen = max_socket_listen;
	net->socket_accept = max_socket_accept;

	net->net = desc;
}

struct network_interface maxim_net = {
	.socket_open = max_socket_open,
	.socket_bind = max_socket_bind,
	.socket_listen = max_socket_listen,
	.socket_accept = max_socket_accept,
	.socket_connect = max_socket_connect,
	.socket_recv = max_socket_recv,
	.socket_send = max_socket_send,
	.socket_recvfrom = max_socket_recvfrom,
	.socket_sendto = max_socket_sendto,
	.socket_disconnect = max_socket_disconnect,
	.socket_close = max_socket_close,
};
