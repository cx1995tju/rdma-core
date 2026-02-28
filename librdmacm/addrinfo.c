/*
 * Copyright (c) 2010-2014 Intel Corporation.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * $Id: cm.c 3453 2005-09-15 21:43:21Z sean.hefty $
 */

#include <config.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#include "cma.h"
#include <rdma/rdma_cma.h>
#include <infiniband/ib.h>

static struct rdma_addrinfo nohints;

static void ucma_convert_to_ai(struct addrinfo *ai,
			       const struct rdma_addrinfo *rai)
{
	memset(ai, 0, sizeof(*ai));
	if (rai->ai_flags & RAI_PASSIVE)
		ai->ai_flags = AI_PASSIVE;
	if (rai->ai_flags & RAI_NUMERICHOST)
		ai->ai_flags |= AI_NUMERICHOST;
	if (rai->ai_family != AF_IB)
		ai->ai_family = rai->ai_family;

	switch (rai->ai_qp_type) {
	case IBV_QPT_RC:
	case IBV_QPT_UC:
	case IBV_QPT_XRC_SEND:
	case IBV_QPT_XRC_RECV:
		ai->ai_socktype = SOCK_STREAM;
		break;
	case IBV_QPT_UD:
		ai->ai_socktype = SOCK_DGRAM;
		break;
	}

	switch (rai->ai_port_space) {
	case RDMA_PS_TCP:
		ai->ai_protocol = IPPROTO_TCP;
		break;
	case RDMA_PS_IPOIB:
	case RDMA_PS_UDP:
		ai->ai_protocol = IPPROTO_UDP;
		break;
	case RDMA_PS_IB:
		if (ai->ai_socktype == SOCK_STREAM)
			ai->ai_protocol = IPPROTO_TCP;
		else if (ai->ai_socktype == SOCK_DGRAM)
			ai->ai_protocol = IPPROTO_UDP;
		break;
	}

	if (rai->ai_flags & RAI_PASSIVE) {
		ai->ai_addrlen = rai->ai_src_len;
		ai->ai_addr = rai->ai_src_addr;
	} else {
		ai->ai_addrlen = rai->ai_dst_len;
		ai->ai_addr = rai->ai_dst_addr;
	}
	ai->ai_canonname = rai->ai_dst_canonname;
	ai->ai_next = NULL;
}

static int ucma_copy_addr(struct sockaddr **dst, socklen_t *dst_len,
			  struct sockaddr *src, socklen_t src_len)
{
	*dst = malloc(src_len);
	if (!(*dst))
		return ERR(ENOMEM);

	memcpy(*dst, src, src_len);
	*dst_len = src_len;
	return 0;
}

void ucma_set_sid(enum rdma_port_space ps, struct sockaddr *addr,
		  struct sockaddr_ib *sib)
{
	__be16 port;

	port = addr ? ucma_get_port(addr) : 0;
	sib->sib_sid = htobe64(((uint64_t) ps << 16) + be16toh(port));

	if (ps)
		sib->sib_sid_mask = htobe64(RDMA_IB_IP_PS_MASK);
	if (port)
		sib->sib_sid_mask |= htobe64(RDMA_IB_IP_PORT_MASK);
}

static int ucma_convert_in6(int ps, struct sockaddr_ib **dst, socklen_t *dst_len,
			    struct sockaddr_in6 *src, socklen_t src_len)
{
	*dst = calloc(1, sizeof(struct sockaddr_ib));
	if (!(*dst))
		return ERR(ENOMEM);

	(*dst)->sib_family = AF_IB;
	(*dst)->sib_pkey = htobe16(0xFFFF);
	(*dst)->sib_flowinfo = src->sin6_flowinfo;
	ib_addr_set(&(*dst)->sib_addr, src->sin6_addr.s6_addr32[0],
		    src->sin6_addr.s6_addr32[1], src->sin6_addr.s6_addr32[2],
		    src->sin6_addr.s6_addr32[3]);
	ucma_set_sid(ps, (struct sockaddr *) src, *dst);
	(*dst)->sib_scope_id = src->sin6_scope_id;

	*dst_len = sizeof(struct sockaddr_ib);
	return 0;
}

/* XXX: ref ucam_getaddrinfo, 如果是 RoCEv2 场景的话, 这里传入的 ai 就是 ip 地址 + port
 * 此时这里的本质就是将 ip 地址信息转换为 rdma 地址信息.
 *
 * 我们需要特别关注这里, 理解 RoCEv2 场景下的地址转换逻辑.
 *
 * */
static int ucma_convert_to_rai(struct rdma_addrinfo *rai,
			       const struct rdma_addrinfo *hints,
			       const struct addrinfo *ai)
{
	int ret;

	if (hints->ai_qp_type) {
		rai->ai_qp_type = hints->ai_qp_type;
	} else {
		switch (ai->ai_socktype) {
		case SOCK_STREAM:
			rai->ai_qp_type = IBV_QPT_RC;
			break;
		case SOCK_DGRAM:
			rai->ai_qp_type = IBV_QPT_UD;
			break;
		}
	}

	if (hints->ai_port_space) {
		rai->ai_port_space = hints->ai_port_space;
	} else {
		switch (ai->ai_protocol) {
		case IPPROTO_TCP:
			rai->ai_port_space = RDMA_PS_TCP;
			break;
		case IPPROTO_UDP:
			rai->ai_port_space = RDMA_PS_UDP;
			break;
		}
	}

	// 有 AI_PASSIVE 的时候, 说明返回的地址适合 server 端使用, 比如 bind 操
	// 作. 其中的 ip 地址信息可能是通配地址 INADDR_ANY
	if (ai->ai_flags & AI_PASSIVE) { // server 端
		rai->ai_flags = RAI_PASSIVE;
		if (ai->ai_canonname) // 没啥好说的, 保存地址的 canonname
			rai->ai_src_canonname = strdup(ai->ai_canonname);

		if ((hints->ai_flags & RAI_FAMILY) && (hints->ai_family == AF_IB) &&
		    (hints->ai_flags & RAI_NUMERICHOST)) {
			rai->ai_family = AF_IB;
			ret = ucma_convert_in6(rai->ai_port_space,
					       (struct sockaddr_ib **) &rai->ai_src_addr,
					       &rai->ai_src_len,
					       (struct sockaddr_in6 *) ai->ai_addr,
					       ai->ai_addrlen);
		} else { // 一般走这里, server 端 copy src 地址, 服务端关心自己的地址
			rai->ai_family = ai->ai_family; // RoCEv2 里 AF_INET/AF_INET6
			ret = ucma_copy_addr(&rai->ai_src_addr, &rai->ai_src_len,
					     ai->ai_addr, ai->ai_addrlen);
		}
	} else { // client 端
		if (ai->ai_canonname)
			rai->ai_dst_canonname = strdup(ai->ai_canonname);

		if ((hints->ai_flags & RAI_FAMILY) && (hints->ai_family == AF_IB) &&
		    (hints->ai_flags & RAI_NUMERICHOST)) {
			rai->ai_family = AF_IB;
			ret = ucma_convert_in6(rai->ai_port_space,
					       (struct sockaddr_ib **) &rai->ai_dst_addr,
					       &rai->ai_dst_len,
					       (struct sockaddr_in6 *) ai->ai_addr,
					       ai->ai_addrlen);
		} else { // client 端 copy dst 地址, client 端关心对方的地址
			rai->ai_family = ai->ai_family;
			ret = ucma_copy_addr(&rai->ai_dst_addr, &rai->ai_dst_len,
					     ai->ai_addr, ai->ai_addrlen);
		}
	}
	return ret;
}

static int ucma_getaddrinfo(const char *node, const char *service,
			    const struct rdma_addrinfo *hints,
			    struct rdma_addrinfo *rai)
{
	struct addrinfo ai_hints;
	struct addrinfo *ai;
	int ret;

	// HERE IT IS, 还是走到了标准的 getaddrinfo 了
	// 如果是 RoCEv2 由于传入的 node 和 service 一般就是 IP 和端口号, 所以这里并没有什么特殊的
	if (hints != &nohints) {
		ucma_convert_to_ai(&ai_hints, hints);
		ret = getaddrinfo(node, service, &ai_hints, &ai);
	} else {
		ret = getaddrinfo(node, service, NULL, &ai);
	}
	if (ret)
		return ret;

	// 这里需要注意下, 如果是 RoCEv2, ai 里返回的地址类型一般是 AF_INET 或
	// AF_INET6 的(getaddinfo 返回的), 这里要转换下
	// 关注这里的转换逻辑
	ret = ucma_convert_to_rai(rai, hints, ai);
	freeaddrinfo(ai);
	return ret;
}

/* 这个函数支持 IB 和 RoCE
 * - IB 里:
 *   - node 一般是一个设备名字, 比如: mlx5_0, 或者 GID, 或者为空, 表示本地;
 *   - service 是一个数值字符串, 用来做多路分解的
 *
 * - RoCEv2 里, node 一般是 IP 或主机名, service 就是端口号, 也是做多路分解的.
 *   但注意不会和 TCP/UDP 连接冲突的, 因为 RoCEv2 外层都是用 UDP port 4791. 这
 *   里仅仅是用来多路分解 rdma 流量的.
 *
 * 提供 node, service, hints 等信息, 让内核将其解析为合法可用的 rdma_addrinfo 结构 res
 *
 *
 * summary: 做一些地址转换, 最终还是调用到标准的 getaddrinfo 函数去解析地址信息.
 *
 *
 * librdmacm 同时支持多种底层 rdma 网络(ib, rocev2...). 地址结构是不同的, 需要
 * 的信息也不同的, 所以提供一个 helper, 用户传入地址(e.g. ip+port), 其将其转换
 * 为标准的 addr 结构, 供后续使用.
 * */
int rdma_getaddrinfo(const char *node, const char *service,
		     const struct rdma_addrinfo *hints,
		     struct rdma_addrinfo **res)
{
	struct rdma_addrinfo *rai;
	int ret;

	// 都为空, 显然不行.
	if (!service && !node && !hints)
		return ERR(EINVAL);

	// 例行调用 ucma_init 函数
	ret = ucma_init();
	if (ret)
		return ret;

	rai = calloc(1, sizeof(*rai));
	if (!rai)
		return ERR(ENOMEM);

	if (!hints)
		hints = &nohints;

	if (node || service) {
		// 一般走这里, 让内核解析并将结果通过 rai 返回
		ret = ucma_getaddrinfo(node, service, hints, rai);
	} else {
		rai->ai_flags = hints->ai_flags;
		rai->ai_family = hints->ai_family;
		rai->ai_qp_type = hints->ai_qp_type;
		rai->ai_port_space = hints->ai_port_space;
		if (hints->ai_dst_len) {
			ret = ucma_copy_addr(&rai->ai_dst_addr, &rai->ai_dst_len,
					     hints->ai_dst_addr, hints->ai_dst_len);
		}
	}
	if (ret)
		goto err;

	if (!rai->ai_src_len && hints->ai_src_len) {
		ret = ucma_copy_addr(&rai->ai_src_addr, &rai->ai_src_len,
				     hints->ai_src_addr, hints->ai_src_len);
		if (ret)
			goto err;
	}

	if (!(rai->ai_flags & RAI_PASSIVE))
		ucma_ib_resolve(&rai, hints);

	*res = rai;
	return 0;

err:
	rdma_freeaddrinfo(rai);
	return ret;
}

void rdma_freeaddrinfo(struct rdma_addrinfo *res)
{
	struct rdma_addrinfo *rai;

	while (res) {
		rai = res;
		res = res->ai_next;

		if (rai->ai_connect)
			free(rai->ai_connect);

		if (rai->ai_route)
			free(rai->ai_route);

		if (rai->ai_src_canonname)
			free(rai->ai_src_canonname);

		if (rai->ai_dst_canonname)
			free(rai->ai_dst_canonname);

		if (rai->ai_src_addr)
			free(rai->ai_src_addr);

		if (rai->ai_dst_addr)
			free(rai->ai_dst_addr);

		free(rai);
	}
}
