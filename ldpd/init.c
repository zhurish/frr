// SPDX-License-Identifier: ISC
/*	$OpenBSD$ */

/*
 * Copyright (c) 2009 Michele Marchetto <michele@openbsd.org>
 */

#include <zebra.h>

#include "ldpd.h"
#include "ldpe.h"
#include "log.h"
#include "ldp_debug.h"

static int	gen_init_prms_tlv(struct ibuf *, struct nbr *);
static int	gen_cap_dynamic_tlv(struct ibuf *);
static int	gen_cap_twcard_tlv(struct ibuf *, int);
static int	gen_cap_unotif_tlv(struct ibuf *, int);

void
send_init(struct nbr *nbr)
{
	struct ibuf		*buf;
	uint16_t		 size;
	int			 err = 0;

	debug_msg_send("initialization: lsr-id %pI4", &nbr->id);

	size = LDP_HDR_SIZE + LDP_MSG_SIZE + SESS_PRMS_SIZE +
	    CAP_TLV_DYNAMIC_SIZE + CAP_TLV_TWCARD_SIZE + CAP_TLV_UNOTIF_SIZE;
	if ((buf = ibuf_open(size)) == NULL)
		fatal(__func__);

	SET_FLAG(err, gen_ldp_hdr(buf, size));
	size -= LDP_HDR_SIZE;
	SET_FLAG(err, gen_msg_hdr(buf, MSG_TYPE_INIT, size));
	SET_FLAG(err, gen_init_prms_tlv(buf, nbr));
	SET_FLAG(err, gen_cap_dynamic_tlv(buf));
	SET_FLAG(err, gen_cap_twcard_tlv(buf, 1));
	SET_FLAG(err, gen_cap_unotif_tlv(buf, 1));
	if (err) {
		ibuf_free(buf);
		return;
	}

	evbuf_enqueue(&nbr->tcp->wbuf, buf);
}

int
recv_init(struct nbr *nbr, char *buf, uint16_t len)
{
	struct ldp_msg		msg;
	struct sess_prms_tlv	sess;
	uint16_t		max_pdu_len;
	int			caps_rcvd = 0;

	debug_msg_recv("initialization: lsr-id %pI4", &nbr->id);

	memcpy(&msg, buf, sizeof(msg));
	buf += LDP_MSG_SIZE;
	len -= LDP_MSG_SIZE;

	if (len < SESS_PRMS_SIZE) {
		session_shutdown(nbr, S_BAD_MSG_LEN, msg.id, msg.type);
		return (-1);
	}
	memcpy(&sess, buf, sizeof(sess));
	if (ntohs(sess.length) != SESS_PRMS_LEN) {
		session_shutdown(nbr, S_BAD_TLV_LEN, msg.id, msg.type);
		return (-1);
	}
	if (ntohs(sess.proto_version) != LDP_VERSION) {
		session_shutdown(nbr, S_BAD_PROTO_VER, msg.id, msg.type);
		return (-1);
	}
	if (ntohs(sess.keepalive_time) < MIN_KEEPALIVE) {
		session_shutdown(nbr, S_KEEPALIVE_BAD, msg.id, msg.type);
		return (-1);
	}
	if (sess.lsr_id != ldp_rtr_id_get(leconf) ||
	    ntohs(sess.lspace_id) != 0) {
		session_shutdown(nbr, S_NO_HELLO, msg.id, msg.type);
		return (-1);
	}

	buf += SESS_PRMS_SIZE;
	len -= SESS_PRMS_SIZE;

	/* Optional Parameters */
	while (len > 0) {
		struct tlv 	tlv;
		uint16_t	tlv_type;
		uint16_t	tlv_len;

		if (len < sizeof(tlv)) {
			session_shutdown(nbr, S_BAD_TLV_LEN, msg.id, msg.type);
			return (-1);
		}

		memcpy(&tlv, buf, TLV_HDR_SIZE);
		tlv_type = ntohs(tlv.type);
		tlv_len = ntohs(tlv.length);
		if (tlv_len + TLV_HDR_SIZE > len) {
			session_shutdown(nbr, S_BAD_TLV_LEN, msg.id, msg.type);
			return (-1);
		}
		buf += TLV_HDR_SIZE;
		len -= TLV_HDR_SIZE;

		/*
		 * RFC 5561 - Section 6:
		 * "The S-bit of a Capability Parameter in an Initialization
		 * message MUST be 1 and SHOULD be ignored on receipt".
		 */
		switch (tlv_type) {
		case TLV_TYPE_ATMSESSIONPAR:
			session_shutdown(nbr, S_BAD_TLV_VAL, msg.id, msg.type);
			return (-1);
		case TLV_TYPE_FRSESSION:
			session_shutdown(nbr, S_BAD_TLV_VAL, msg.id, msg.type);
			return (-1);
		case TLV_TYPE_DYNAMIC_CAP:
			if (tlv_len != CAP_TLV_DYNAMIC_LEN) {
				session_shutdown(nbr, S_BAD_TLV_LEN, msg.id, msg.type);
				return (-1);
			}

			if (CHECK_FLAG(caps_rcvd, F_CAP_TLV_RCVD_DYNAMIC)) {
				session_shutdown(nbr, S_BAD_TLV_VAL, msg.id, msg.type);
				return (-1);
			}
			SET_FLAG(caps_rcvd, F_CAP_TLV_RCVD_DYNAMIC);

			SET_FLAG(nbr->flags, F_NBR_CAP_DYNAMIC);

			log_debug("%s: lsr-id %pI4 announced the Dynamic Capability Announcement capability", __func__,
			    &nbr->id);
			break;
		case TLV_TYPE_TWCARD_CAP:
			if (tlv_len != CAP_TLV_TWCARD_LEN) {
				session_shutdown(nbr, S_BAD_TLV_LEN, msg.id, msg.type);
				return (-1);
			}

			if (CHECK_FLAG(caps_rcvd, F_CAP_TLV_RCVD_TWCARD)) {
				session_shutdown(nbr, S_BAD_TLV_VAL, msg.id, msg.type);
				return (-1);
			}
			SET_FLAG(caps_rcvd, F_CAP_TLV_RCVD_TWCARD);

			SET_FLAG(nbr->flags, F_NBR_CAP_TWCARD);

			log_debug("%s: lsr-id %pI4 announced the Typed Wildcard FEC capability", __func__, &nbr->id);
			break;
		case TLV_TYPE_UNOTIF_CAP:
			if (tlv_len != CAP_TLV_UNOTIF_LEN) {
				session_shutdown(nbr, S_BAD_TLV_LEN, msg.id, msg.type);
				return (-1);
			}

			if (CHECK_FLAG(caps_rcvd, F_CAP_TLV_RCVD_UNOTIF)) {
				session_shutdown(nbr, S_BAD_TLV_VAL, msg.id, msg.type);
				return (-1);
			}
			SET_FLAG(caps_rcvd, F_CAP_TLV_RCVD_UNOTIF);

			SET_FLAG(nbr->flags, F_NBR_CAP_UNOTIF);

			log_debug("%s: lsr-id %pI4 announced the Unrecognized Notification capability", __func__,
			    &nbr->id);
			break;
		default:
			if (!CHECK_FLAG(ntohs(tlv.type), UNKNOWN_FLAG))
				send_notification_rtlvs(nbr, S_UNSSUPORTDCAP,
				    msg.id, msg.type, tlv_type, tlv_len, buf);
			/* ignore unknown tlv */
			break;
		}
		buf += tlv_len;
		len -= tlv_len;
	}

	nbr->keepalive = MIN(nbr_get_keepalive(nbr->af, nbr->id),
	    ntohs(sess.keepalive_time));

	max_pdu_len = ntohs(sess.max_pdu_len);
	/*
	 * RFC 5036 - Section 3.5.3:
	 * "A value of 255 or less specifies the default maximum length of
	 * 4096 octets".
	 */
	if (max_pdu_len <= 255)
		max_pdu_len = LDP_MAX_LEN;
	nbr->max_pdu_len = MIN(max_pdu_len, LDP_MAX_LEN);

	nbr_fsm(nbr, NBR_EVT_INIT_RCVD);

	return (0);
}

void
send_capability(struct nbr *nbr, uint16_t capability, int enable)
{
	struct ibuf		*buf;
	uint16_t		 size;
	int			 err = 0;

	log_debug("%s: lsr-id %pI4", __func__, &nbr->id);

	size = LDP_HDR_SIZE + LDP_MSG_SIZE + CAP_TLV_DYNAMIC_SIZE;
	if ((buf = ibuf_open(size)) == NULL)
		fatal(__func__);

	SET_FLAG(err, gen_ldp_hdr(buf, size));
	size -= LDP_HDR_SIZE;
	SET_FLAG(err, gen_msg_hdr(buf, MSG_TYPE_CAPABILITY, size));

	switch (capability) {
	case TLV_TYPE_TWCARD_CAP:
		SET_FLAG(err, gen_cap_twcard_tlv(buf, enable));
		break;
	case TLV_TYPE_UNOTIF_CAP:
		SET_FLAG(err, gen_cap_unotif_tlv(buf, enable));
		break;
	case TLV_TYPE_DYNAMIC_CAP:
		/*
		 * RFC 5561 - Section 9:
		 * "An LDP speaker MUST NOT include the Dynamic Capability
		 * Announcement Parameter in Capability messages sent to
		 * its peers".
		 */
		fallthrough;
	default:
		fatalx("send_capability: unsupported capability");
	}

	if (err) {
		ibuf_free(buf);
		return;
	}

	evbuf_enqueue(&nbr->tcp->wbuf, buf);
	nbr_fsm(nbr, NBR_EVT_PDU_SENT);
	nbr->stats.capability_sent++;
}

int
recv_capability(struct nbr *nbr, char *buf, uint16_t len)
{
	struct ldp_msg	 msg;
	int		 enable = 0;
	int		 caps_rcvd = 0;

	log_debug("%s: lsr-id %pI4", __func__, &nbr->id);

	memcpy(&msg, buf, sizeof(msg));
	buf += LDP_MSG_SIZE;
	len -= LDP_MSG_SIZE;

	/* Optional Parameters */
	while (len > 0) {
		struct tlv 	 tlv;
		uint16_t	 tlv_type;
		uint16_t	 tlv_len;
		uint8_t		 reserved;

		if (len < sizeof(tlv)) {
			session_shutdown(nbr, S_BAD_TLV_LEN, msg.id, msg.type);
			return (-1);
		}

		memcpy(&tlv, buf, TLV_HDR_SIZE);
		tlv_type = ntohs(tlv.type);
		tlv_len = ntohs(tlv.length);
		if (tlv_len + TLV_HDR_SIZE > len) {
			session_shutdown(nbr, S_BAD_TLV_LEN, msg.id, msg.type);
			return (-1);
		}
		buf += TLV_HDR_SIZE;
		len -= TLV_HDR_SIZE;

		switch (tlv_type) {
		case TLV_TYPE_TWCARD_CAP:
			if (tlv_len != CAP_TLV_TWCARD_LEN) {
				session_shutdown(nbr, S_BAD_TLV_LEN, msg.id, msg.type);
				return (-1);
			}

			if (CHECK_FLAG(caps_rcvd, F_CAP_TLV_RCVD_TWCARD)) {
				session_shutdown(nbr, S_BAD_TLV_VAL, msg.id, msg.type);
				return (-1);
			}
			SET_FLAG(caps_rcvd, F_CAP_TLV_RCVD_TWCARD);

			memcpy(&reserved, buf, sizeof(reserved));
			enable = reserved & STATE_BIT;
			if (enable)
				SET_FLAG(nbr->flags, F_NBR_CAP_TWCARD);
			else
				UNSET_FLAG(nbr->flags, F_NBR_CAP_TWCARD);

			log_debug("%s: lsr-id %pI4 %s the Typed Wildcard FEC capability", __func__, &nbr->id,
			    (enable) ? "announced" : "withdrew");
			break;
		case TLV_TYPE_UNOTIF_CAP:
			if (tlv_len != CAP_TLV_UNOTIF_LEN) {
				session_shutdown(nbr, S_BAD_TLV_LEN, msg.id, msg.type);
				return (-1);
			}

			if (CHECK_FLAG(caps_rcvd, F_CAP_TLV_RCVD_UNOTIF)) {
				session_shutdown(nbr, S_BAD_TLV_VAL, msg.id, msg.type);
				return (-1);
			}
			SET_FLAG(caps_rcvd, F_CAP_TLV_RCVD_UNOTIF);

			memcpy(&reserved, buf, sizeof(reserved));
			enable = reserved & STATE_BIT;
			if (enable)
				SET_FLAG(nbr->flags, F_NBR_CAP_UNOTIF);
			else
				UNSET_FLAG(nbr->flags, F_NBR_CAP_UNOTIF);

			log_debug("%s: lsr-id %pI4 %s the Unrecognized Notification capability", __func__,
			    &nbr->id, (enable) ? "announced" : "withdrew");
			break;
		case TLV_TYPE_DYNAMIC_CAP:
			/*
		 	 * RFC 5561 - Section 9:
			 * "An LDP speaker that receives a Capability message
			 * from a peer that includes the Dynamic Capability
			 * Announcement Parameter SHOULD silently ignore the
			 * parameter and process any other Capability Parameters
			 * in the message".
			 */
			fallthrough;
		default:
			if (!CHECK_FLAG(ntohs(tlv.type), UNKNOWN_FLAG))
				send_notification_rtlvs(nbr, S_UNSSUPORTDCAP,
				    msg.id, msg.type, tlv_type, tlv_len, buf);
			/* ignore unknown tlv */
			break;
		}
		buf += tlv_len;
		len -= tlv_len;
	}

	nbr_fsm(nbr, NBR_EVT_PDU_RCVD);

	return (0);
}

static int
gen_init_prms_tlv(struct ibuf *buf, struct nbr *nbr)
{
	struct sess_prms_tlv	parms;

	memset(&parms, 0, sizeof(parms));
	parms.type = htons(TLV_TYPE_COMMONSESSION);
	parms.length = htons(SESS_PRMS_LEN);
	parms.proto_version = htons(LDP_VERSION);
	parms.keepalive_time = htons(nbr_get_keepalive(nbr->af, nbr->id));
	parms.reserved = 0;
	parms.pvlim = 0;
	parms.max_pdu_len = 0;
	parms.lsr_id = nbr->id.s_addr;
	parms.lspace_id = 0;

	return (ibuf_add(buf, &parms, SESS_PRMS_SIZE));
}

static int
gen_cap_dynamic_tlv(struct ibuf *buf)
{
	struct capability_tlv	cap;

	memset(&cap, 0, sizeof(cap));
	cap.type = htons(TLV_TYPE_DYNAMIC_CAP);
	cap.length = htons(CAP_TLV_DYNAMIC_LEN);
	/* the S-bit is always 1 for the Dynamic Capability Announcement */
	cap.reserved = STATE_BIT;

	return (ibuf_add(buf, &cap, CAP_TLV_DYNAMIC_SIZE));
}

static int
gen_cap_twcard_tlv(struct ibuf *buf, int enable)
{
	struct capability_tlv	cap;

	memset(&cap, 0, sizeof(cap));
	cap.type = htons(TLV_TYPE_TWCARD_CAP);
	cap.length = htons(CAP_TLV_TWCARD_LEN);
	if (enable)
		cap.reserved = STATE_BIT;

	return (ibuf_add(buf, &cap, CAP_TLV_TWCARD_SIZE));
}

static int
gen_cap_unotif_tlv(struct ibuf *buf, int enable)
{
	struct capability_tlv	cap;

	memset(&cap, 0, sizeof(cap));
	cap.type = htons(TLV_TYPE_UNOTIF_CAP);
	cap.length = htons(CAP_TLV_UNOTIF_LEN);
	if (enable)
		cap.reserved = STATE_BIT;

	return (ibuf_add(buf, &cap, CAP_TLV_UNOTIF_SIZE));
}
