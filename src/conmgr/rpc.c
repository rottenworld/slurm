/*****************************************************************************\
 *  rpc.c - definitions for SLURM rpc connection in connection manager
 *****************************************************************************
 *  Copyright (C) SchedMD LLC.
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "src/common/slurm_protocol_api.h"
#include "src/common/pack.h"
#include "src/common/read_config.h"
#include "src/common/xmalloc.h"

/*
 * Handlers specific to CON_TYPE_RPC
 */

#include "src/conmgr/conmgr.h"
#include "src/conmgr/mgr.h"

extern int on_rpc_connection_data(conmgr_fd_t *con, void *arg)
{
	int rc = SLURM_ERROR;
	uint32_t need;
	slurm_msg_t *msg = NULL;
	buf_t *rpc = NULL;

	xassert(con->magic == MAGIC_CON_MGR_FD);

	/* based on slurm_msg_recvfrom_timeout() */
	if (!con->msglen) {
		log_flag(NET, "%s: [%s] got %d bytes pending for RPC connection",
			 __func__, con->name, size_buf(con->in));

		xassert(sizeof(con->msglen) == sizeof(uint32_t));
		if (size_buf(con->in) >= sizeof(con->msglen)) {
			con->msglen = ntohl(
				*(uint32_t *) get_buf_data(con->in));
			log_flag(NET, "%s: [%s] got message length %u for RPC connection with %d bytes pending",
				 __func__, con->name, con->msglen, size_buf(con->in));
		} else {
			log_flag(NET, "%s: [%s] waiting for message length for RPC connection",
				 __func__, con->name);
			return SLURM_SUCCESS;
		}

		if (con->msglen > MAX_MSG_SIZE) {
			log_flag(NET, "%s: [%s] rejecting RPC message length: %u",
				 __func__, con->name, con->msglen);
			return SLURM_PROTOCOL_INSANE_MSG_LENGTH;
		}
	}

	need = sizeof(con->msglen) + con->msglen;
	if ((rc = try_grow_buf_remaining(con->in, need))) {
		log_flag(NET, "%s: [%s] unable to increase buffer %u bytes for RPC message: %s",
			 __func__, con->name, need, slurm_strerror(rc));
		return rc;
	}

	if (size_buf(con->in) < need) {
		log_flag(NET, "%s: [%s] waiting for message length %u/%u for RPC message",
			 __func__, con->name, size_buf(con->in), need);
		return SLURM_SUCCESS;
	}

	/* there is enough data to unpack now */
	rpc = create_shadow_buf((get_buf_data(con->in) +
				 sizeof(con->msglen)),
				con->msglen);

	msg = xmalloc(sizeof(*msg));
	slurm_msg_t_init(msg);

	log_flag_hex(NET_RAW, get_buf_data(rpc), size_buf(rpc),
		     "%s: [%s] unpacking RPC", __func__, con->name);

	if ((rc = slurm_unpack_received_msg(msg, con->input_fd, rpc))) {
		rc = errno;
		error("%s: [%s] slurm_unpack_received_msg() failed: %s",
		      __func__, con->name, slurm_strerror(rc));
		slurm_free_msg(msg);
		msg = NULL;
	} else {
		log_flag(NET, "%s: [%s] unpacked %u bytes containing %s RPC",
			 __func__, con->name, need,
			 rpc_num2string(msg->msg_type));
	}

	/* notify conmgr we processed some data */
	set_buf_offset(con->in, need);

	/* reset message length to start all over again */
	con->msglen = 0;

	FREE_NULL_BUFFER(rpc);

	if (!rc) {
		log_flag(PROTOCOL, "%s: [%s] received RPC %s",
			 __func__, con->name, rpc_num2string(msg->msg_type));
		log_flag(CONMGR, "%s: [%s] RPC BEGIN func=0x%"PRIxPTR" arg=0x%"PRIxPTR,
			 __func__, con->name, (uintptr_t) con->events.on_msg,
			 (uintptr_t) con->arg);
		rc = con->events.on_msg(con, msg, con->arg);
		log_flag(CONMGR, "%s: [%s] RPC END func=0x%"PRIxPTR" arg=0x%"PRIxPTR" rc=%s",
			 __func__, con->name, (uintptr_t) con->events.on_msg,
			 (uintptr_t) con->arg, slurm_strerror(rc));
	}

	return rc;
}

/*
 * based on _pack_msg() and slurm_send_node_msg() in slurm_protocol_api.c
 */
extern int conmgr_queue_write_msg(conmgr_fd_t *con, slurm_msg_t *msg)
{
	int rc;
	msg_bufs_t buffers = {0};
	uint32_t msglen = 0;

	xassert(con->magic == MAGIC_CON_MGR_FD);
	xassert(msg->protocol_version <= SLURM_PROTOCOL_VERSION);
	xassert(msg->protocol_version >= SLURM_MIN_PROTOCOL_VERSION);

	if ((rc = slurm_buffers_pack_msg(msg, &buffers, false)))
		goto cleanup;

	msglen = get_buf_offset(buffers.body) + get_buf_offset(buffers.header);

	if (buffers.auth)
		msglen += get_buf_offset(buffers.auth);

	if (msglen > MAX_MSG_SIZE) {
		log_flag(NET, "%s: [%s] invalid RPC message length: %u",
			 __func__, con->name, msglen);
		rc = SLURM_PROTOCOL_INSANE_MSG_LENGTH;
		goto cleanup;
	}

	/* switch to network order */
	msglen = htonl(msglen);

	//TODO: handing over the buffers would be better than copying

	if ((rc = conmgr_queue_write_fd(con, &msglen, sizeof(msglen))))
		goto cleanup;

	if ((rc = conmgr_queue_write_fd(con, get_buf_data(buffers.header),
					get_buf_offset(buffers.header))))
		goto cleanup;

	if (buffers.auth &&
	    (rc = conmgr_queue_write_fd(con, get_buf_data(buffers.auth),
					get_buf_offset(buffers.auth))))
		goto cleanup;

	rc = conmgr_queue_write_fd(con, get_buf_data(buffers.body),
				   get_buf_offset(buffers.body));
cleanup:
	if (!rc) {
		log_flag(PROTOCOL, "%s: [%s] sending RPC %s",
			 __func__, con->name, rpc_num2string(msg->msg_type));
		log_flag(NET, "%s: [%s] sending RPC %s packed into %u bytes",
			 __func__, con->name, rpc_num2string(msg->msg_type),
			 ntohl(msglen));
	} else {
		log_flag(NET, "%s: [%s] error packing RPC %s: %s",
			 __func__, con->name, rpc_num2string(msg->msg_type),
			 slurm_strerror(rc));
	}

	FREE_NULL_BUFFER(buffers.auth);
	FREE_NULL_BUFFER(buffers.body);
	FREE_NULL_BUFFER(buffers.header);

	return rc;
}
