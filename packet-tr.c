/* packet-tr.c
 * Routines for Token-Ring packet disassembly
 * Gilbert Ramirez <gram@verdict.uthscsa.edu>
 *
 * $Id: packet-tr.c,v 1.19 1999/08/10 02:54:59 gram Exp $
 *
 * Ethereal - Network traffic analyzer
 * By Gerald Combs <gerald@unicom.net>
 * Copyright 1998 Gerald Combs
 *
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#include <stdio.h>
#include <glib.h>
#include "packet.h"
#include "etypes.h"
	
static int proto_tr = -1;
static int hf_tr_dst = -1;
static int hf_tr_src = -1;
static int hf_tr_sr = -1;
static int hf_tr_ac = -1;
static int hf_tr_priority = -1;
static int hf_tr_frame = -1;
static int hf_tr_monitor_cnt = -1;
static int hf_tr_priority_reservation = -1;
static int hf_tr_fc = -1;
static int hf_tr_fc_type = -1;
static int hf_tr_fc_pcf = -1;
static int hf_tr_rif_bytes = -1;
static int hf_tr_broadcast = -1;
static int hf_tr_max_frame_size = -1;
static int hf_tr_direction = -1;
static int hf_tr_rif = -1;
static int hf_tr_rif_ring = -1;
static int hf_tr_rif_bridge = -1;
	
static const value_string ac_vals[] = {
	{ 0,	"Token" },
	{ 0x10,	"Frame" },
	{ 0,	NULL }
};

static const value_string pcf_vals[] = {
	{ 0,	"Normal buffer" },
	{ 1,	"Express buffer" },
	{ 2,	"Purge" },
	{ 3,	"Claim Token" },
	{ 4,	"Beacon" },
	{ 5,	"Active Monitor Present" },
	{ 6,	"Standby Monitor Present" },
	{ 0,	NULL },
};

static const value_string frame_vals[] = {
	{ 0,	"MAC" },
	{ 64,	"LLC" },
	{ 128,	"Reserved" },
	{ 0,	NULL },
};

static const value_string broadcast_vals[] = {
	{ 0 << 5,	"Non-broadcast" },
	{ 1 << 5,	"Non-broadcast" },
	{ 2 << 5,	"Non-broadcast" },
	{ 3 << 5,	"Non-broadcast" },
	{ 4 << 5,	"All-routes broadcast" },
	{ 5 << 5,	"All-routes broadcast" },
	{ 6 << 5,	"Single-route broadcast" },
	{ 7 << 5,	"Single-route broadcast" },
	{ 0,		NULL }
};

static const value_string max_frame_size_vals[] = {
	{ 0,	"516" },
	{ 1,	"1500" },
	{ 2,	"2052" },
	{ 3,	"4472" },
	{ 4,	"8144" },
	{ 5,	"11407" },
	{ 6,	"17800" },
	{ 0,	NULL }
};

static const value_string direction_vals[] = {
	{ 0,	"From originating station (-->)" },
	{ 128,	"To originating station (<--)" },
	{ 0,	NULL }
};

static void
add_ring_bridge_pairs(int rcf_len, const u_char *pd, proto_tree *tree);

void
capture_tr(const u_char *pd, guint32 cap_len, packet_counts *ld) {

	int			offset = 14;

	int			source_routed = 0;
	int			frame_type;
	guint8			trn_rif_bytes;
	guint8			actual_rif_bytes;

	/* The trn_hdr struct, as separate variables */
	guint8			trn_fc;		/* field control field */
	guint8			trn_shost[6];	/* source host */

	/* get the data */
	memcpy(&trn_fc, &pd[1], sizeof(guint8));
	memcpy(trn_shost, &pd[8], 6 * sizeof(guint8));

	frame_type = (trn_fc & 192) >> 6;

	/* if the high bit on the first byte of src hwaddr is 1, then
		this packet is source-routed */
	source_routed = trn_shost[0] & 128;

	trn_rif_bytes = pd[14] & 31;

	/* sometimes we have a RCF but no RIF... half source-routed? */
	/* I'll check for 2 bytes of RIF and the 0x70 byte */
	if (!source_routed && trn_rif_bytes > 0) {
		if (trn_rif_bytes == 2) {
			source_routed = 1;
		}
		/* the Linux 2.0 TR code strips source-route bits in
		 * order to test for SR. This can be removed from most
		 * packets with oltr, but not all. So, I try to figure out
		 * which packets should have been SR here. I'll check to
		 * see if there's a SNAP or IPX field right after
		 * my RIF fields.
		 */
		else if ( (
			pd[0x0e + trn_rif_bytes] == 0xaa &&
			pd[0x0f + trn_rif_bytes] == 0xaa &&
			pd[0x10 + trn_rif_bytes] == 0x03) ||
			  (
			pd[0x0e + trn_rif_bytes] == 0xe0 &&
			pd[0x0f + trn_rif_bytes] == 0xe0) ) {

			source_routed = 1;
		}

	}

	if (source_routed) {
		actual_rif_bytes = trn_rif_bytes;
	}
	else {
		trn_rif_bytes = 0;
		actual_rif_bytes = 0;
	}

	/* this is a silly hack for Linux 2.0.x. Read the comment below,
	in front of the other #ifdef linux. If we're sniffing our own NIC,
	 we get a full RIF, sometimes with garbage */
	if ((source_routed && trn_rif_bytes == 2 && frame_type == 1) ||
		(!source_routed && frame_type == 1)) {
		/* look for SNAP or IPX only */
		if ( (pd[0x20] == 0xaa && pd[0x21] == 0xaa && pd[0x22] == 03) ||
			 (pd[0x20] == 0xe0 && pd[0x21] == 0xe0) ) {
			actual_rif_bytes = 18;
		}
	}
	offset += actual_rif_bytes;

	/* The package is either MAC or LLC */
	switch (frame_type) {
		/* MAC */
		case 0:
			ld->other++;
			break;
		case 1:
			capture_llc(pd, offset, cap_len, ld);
			break;
		default:
			/* non-MAC, non-LLC, i.e., "Reserved" */
			ld->other++;
			break;
	}
}


void
dissect_tr(const u_char *pd, frame_data *fd, proto_tree *tree) {

	proto_tree	*tr_tree, *bf_tree;
	proto_item	*ti;
	int			offset = 14;

	int			source_routed = 0;
	int			frame_type;
	guint8		trn_rif_bytes;
	guint8		actual_rif_bytes;

	/* The trn_hdr struct, as separate variables */
	guint8			trn_ac;		/* access control field */
	guint8			trn_fc;		/* field control field */
	guint8			trn_dhost[6];	/* destination host */
	guint8			trn_shost[6];	/* source host */
	guint16			trn_rcf;	/* routing control field */
	guint16			trn_rseg[8];	/* routing registers */

	/* non-source-routed version of source addr */
	guint8			trn_shost_nonsr[6];



	/* Token-Ring Strings */
	char *fc[] = { "MAC", "LLC", "Reserved", "Unknown" };

	/* get the data */
	memcpy(&trn_ac, &pd[0], sizeof(guint8));
	memcpy(&trn_fc, &pd[1], sizeof(guint8));
	memcpy(trn_dhost, &pd[2], 6 * sizeof(guint8));
	memcpy(trn_shost, &pd[8], 6 * sizeof(guint8));
	memcpy(&trn_rcf, &pd[14], sizeof(guint16));
	memcpy(trn_rseg, &pd[16], 8 * sizeof(guint16));

	memcpy(trn_shost_nonsr, &pd[8], 6 * sizeof(guint8));
	trn_shost_nonsr[0] &= 127;
	frame_type = (trn_fc & 192) >> 6;

	/* if the high bit on the first byte of src hwaddr is 1, then
		this packet is source-routed */
	source_routed = trn_shost[0] & 128;

	trn_rif_bytes = pd[14] & 31;

	/* sometimes we have a RCF but no RIF... half source-routed? */
	/* I'll check for 2 bytes of RIF and the 0x70 byte */
	if (!source_routed && trn_rif_bytes > 0) {
		if (trn_rif_bytes == 2) {
			source_routed = 1;
		}
		/* the Linux 2.0 TR code strips source-route bits in
		 * order to test for SR. This can be removed from most
		 * packets with oltr, but not all. So, I try to figure out
		 * which packets should have been SR here. I'll check to
		 * see if there's a SNAP or IPX field right after
		 * my RIF fields.
		 */
		else if ( (
			pd[0x0e + trn_rif_bytes] == 0xaa &&
			pd[0x0f + trn_rif_bytes] == 0xaa &&
			pd[0x10 + trn_rif_bytes] == 0x03) ||
			  (
			pd[0x0e + trn_rif_bytes] == 0xe0 &&
			pd[0x0f + trn_rif_bytes] == 0xe0) ) {

			source_routed = 1;
		}
/*		else {
			printf("0e+%d = %02X   0f+%d = %02X\n", trn_rif_bytes, pd[0x0e + trn_rif_bytes],
					trn_rif_bytes, pd[0x0f + trn_rif_bytes]);
		} */

	}

	if (source_routed) {
		actual_rif_bytes = trn_rif_bytes;
	}
	else {
		trn_rif_bytes = 0;
		actual_rif_bytes = 0;
	}

	/* this is a silly hack for Linux 2.0.x. Read the comment below,
	in front of the other #ifdef linux. If we're sniffing our own NIC,
	 we get a full RIF, sometimes with garbage */
	if ((source_routed && trn_rif_bytes == 2 && frame_type == 1) ||
		(!source_routed && frame_type == 1)) {
		/* look for SNAP or IPX only */
		if ( (pd[0x20] == 0xaa && pd[0x21] == 0xaa && pd[0x22] == 03) ||
			 (pd[0x20] == 0xe0 && pd[0x21] == 0xe0) ) {
			actual_rif_bytes = 18;
		}
	}
	offset += actual_rif_bytes;


	/* information window */
	if (check_col(fd, COL_RES_DL_DST))
		col_add_str(fd, COL_RES_DL_DST, ether_to_str((guint8 *)&pd[2]));
	if (check_col(fd, COL_RES_DL_SRC))
		col_add_str(fd, COL_RES_DL_SRC, ether_to_str(trn_shost_nonsr));
	if (check_col(fd, COL_PROTOCOL))
		col_add_str(fd, COL_PROTOCOL, "TR");
	if (check_col(fd, COL_INFO))
		col_add_fstr(fd, COL_INFO, "Token-Ring %s", fc[frame_type]);

	/* protocol analysis tree */
	if (tree) {
		/* Create Token-Ring Tree */
		ti = proto_tree_add_item(tree, proto_tr, 0, 14 + actual_rif_bytes, NULL);
		tr_tree = proto_item_add_subtree(ti, ETT_TOKEN_RING);

		/* Create the Access Control bitfield tree */
		ti = proto_tree_add_item_format(tr_tree, hf_tr_ac, 0, 1, trn_ac,
			"Access Control (0x%02x)", trn_ac);
		bf_tree = proto_item_add_subtree(ti, ETT_TOKEN_RING_AC);

		proto_tree_add_item_format(bf_tree, hf_tr_priority, 0, 1, trn_ac & 0xe0,
			decode_numeric_bitfield(trn_ac, 0xe0, 8, "Priority = %d"));

		proto_tree_add_item_format(bf_tree, hf_tr_frame, 0, 1, trn_ac & 0x10,
			decode_enumerated_bitfield(trn_ac, 0x10, 8, ac_vals, "%s"));

		proto_tree_add_item_format(bf_tree, hf_tr_monitor_cnt, 0, 1, trn_ac & 0x08,
			decode_numeric_bitfield(trn_ac, 0x08, 8, "Monitor Count"));

		proto_tree_add_item_format(bf_tree, hf_tr_priority_reservation, 0, 1, trn_ac & 0x07,
			decode_numeric_bitfield(trn_ac, 0x07, 8, "Priority Reservation = %d"));

		/* Create the Frame Control bitfield tree */
		ti = proto_tree_add_item_format(tr_tree, hf_tr_fc, 1, 1, trn_fc,
			"Frame Control (0x%02x)", trn_fc);
		bf_tree = proto_item_add_subtree(ti, ETT_TOKEN_RING_FC);

		proto_tree_add_item_format(bf_tree, hf_tr_fc_type, 1, 1, trn_fc & 0xc0,
			decode_enumerated_bitfield(trn_fc, 0xc0, 8, frame_vals, "%s"));

		proto_tree_add_item_format(bf_tree, hf_tr_fc_pcf, 1, 1, trn_fc & 0x0f,
			decode_enumerated_bitfield(trn_fc, 0x0f, 8, pcf_vals, "%s"));

		proto_tree_add_item(tr_tree, hf_tr_dst, 2, 6, trn_dhost);
		proto_tree_add_item(tr_tree, hf_tr_src, 8, 6, trn_shost);
		proto_tree_add_item_hidden(tr_tree, hf_tr_sr, 8, 1, source_routed);

		/* non-source-routed version of src addr */
		proto_tree_add_item_hidden(tr_tree, hf_tr_src, 8, 6, trn_shost_nonsr);

		if (source_routed) {
			/* RCF Byte 1 */
			proto_tree_add_item(tr_tree, hf_tr_rif_bytes, 14, 1, trn_rif_bytes);
			proto_tree_add_item(tr_tree, hf_tr_broadcast, 14, 1, pd[14] & 224);

			/* RCF Byte 2 */
			proto_tree_add_item(tr_tree, hf_tr_max_frame_size, 15, 1, pd[15] & 112);
			proto_tree_add_item(tr_tree, hf_tr_direction, 15, 1, pd[15] & 128);

			/* if we have more than 2 bytes of RIF, then we have
				ring/bridge pairs */
			if (trn_rif_bytes > 2) {
				add_ring_bridge_pairs(trn_rif_bytes, pd, tr_tree);
			}
		}

		/* Linux 2.0.x has a problem in that the 802.5 code creates
		an emtpy full (18-byte) RIF area. It's up to the tr driver to
		either fill it in or remove it before sending the bytes out
		to the wire. If you run tcpdump on a Linux 2.0.x machine running
		token-ring, tcpdump will capture these 18 filler bytes. They
		are filled with garbage. The best way to detect this problem is
		to know the src hwaddr of the machine from which you were running
		tcpdump. W/o that, however, I'm guessing that DSAP == SSAP if the
		frame type is LLC.  It's very much a hack. -- Gilbert Ramirez */
		if (actual_rif_bytes > trn_rif_bytes) {
			proto_tree_add_text(tr_tree, 14 + trn_rif_bytes, actual_rif_bytes - trn_rif_bytes,
				"Empty RIF from Linux 2.0.x driver. The sniffing NIC "
				"is also running a protocol stack.");
		}
	}
	/* The package is either MAC or LLC */
	switch (frame_type) {
		/* MAC */
		case 0:
			dissect_trmac(pd, offset, fd, tree);
			break;
		case 1:
			dissect_llc(pd, offset, fd, tree);
			break;
		default:
			/* non-MAC, non-LLC, i.e., "Reserved" */
			dissect_data(pd, offset, fd, tree);
			break;
	}
}

/* this routine is taken from the Linux net/802/tr.c code, which shows
ring-bridge paires in the /proc/net/tr_rif virtual file. */
static void
add_ring_bridge_pairs(int rcf_len, const u_char *pd, proto_tree *tree)
{
	int 	j, size;
	int 	segment, brdgnmb;
	char	buffer[50];
	int	buff_offset=0;

	rcf_len -= 2;

	for(j = 1; j < rcf_len - 1; j += 2) {
		if (j==1) {
			segment=pntohs(&pd[16]) >> 4;
			size = sprintf(buffer, "%03X",segment);
			proto_tree_add_item_hidden(tree, hf_tr_rif_ring, 16, 2, segment);
			buff_offset += size;
		}
		segment=pntohs(&pd[17+j]) >> 4;
		brdgnmb=pd[16+j] & 0x0f;
		size = sprintf(buffer+buff_offset, "-%01X-%03X",brdgnmb,segment);
		proto_tree_add_item_hidden(tree, hf_tr_rif_ring, 17+j, 2, segment);
		proto_tree_add_item_hidden(tree, hf_tr_rif_bridge, 16+j, 1, brdgnmb);
		buff_offset += size;	
	}
	proto_tree_add_item(tree, hf_tr_rif, 16, rcf_len, buffer);
}

void
proto_register_tr(void)
{
	static hf_register_info hf[] = {
		{ &hf_tr_ac,
		{ "Access Control",	"tr.ac", FT_UINT8, NULL }},

		{ &hf_tr_priority,
		{ "Priority",		"tr.priority", FT_UINT8, NULL }},

		{ &hf_tr_frame,
		{ "Frame",		"tr.frame", FT_VALS_UINT8, VALS(ac_vals) }},

		{ &hf_tr_monitor_cnt,
		{ "Monitor Count",	"tr.monitor_cnt", FT_UINT8, NULL }},

		{ &hf_tr_priority_reservation,
		{ "Priority Reservation","tr.priority_reservation", FT_UINT8, NULL }},

		{ &hf_tr_fc,
		{ "Frame Control",	"tr.fc", FT_UINT8, NULL }},

		{ &hf_tr_fc_type,
		{ "Frame Type",		"tr.frame_type", FT_VALS_UINT8, VALS(frame_vals) }},

		{ &hf_tr_fc_pcf,
		{ "Frame PCF",		"tr.frame_pcf", FT_VALS_UINT8, VALS(pcf_vals) }},

		{ &hf_tr_dst,
		{ "Destination",	"tr.dst", FT_ETHER, NULL }},

		{ &hf_tr_src,
		{ "Source",		"tr.src", FT_ETHER, NULL }},

		{ &hf_tr_sr,
		{ "Source Routed",	"tr.sr", FT_BOOLEAN, NULL }},

		{ &hf_tr_rif_bytes,
		{ "RIF Bytes",		"tr.rif_bytes", FT_UINT8, NULL }},

		{ &hf_tr_broadcast,
		{ "Broadcast Type",	"tr.broadcast", FT_VALS_UINT8, VALS(broadcast_vals) }},

		{ &hf_tr_max_frame_size,
		{ "Maximum Frame Size",	"tr.max_frame_size", FT_VALS_UINT8, VALS(max_frame_size_vals) }},

		{ &hf_tr_direction,
		{ "Direction",		"tr.direction", FT_VALS_UINT8, VALS(direction_vals) }},

		{ &hf_tr_rif,
		{ "Ring-Bridge Pairs",	"tr.rif", FT_STRING, NULL }},

		{ &hf_tr_rif_ring,
		{ "RIF Ring",		"tr.rif.ring", FT_UINT16, NULL }},

		{ &hf_tr_rif_bridge,
		{ "RIF Bridge",		"tr.rif.bridge", FT_UINT8, NULL }}
	};

	proto_tr = proto_register_protocol("Token-Ring", "tr");
	proto_register_field_array(proto_tr, hf, array_length(hf));
}

