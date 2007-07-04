/* packet-smtp.c
 * Routines for SMTP packet disassembly
 *
 * $Id$
 *
 * Copyright (c) 2000 by Richard Sharpe <rsharpe@ns.aus.com>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1999 Gerald Combs
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
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <glib.h>
#include <string.h>
#include <epan/packet.h>
#include <epan/conversation.h>
#include <epan/addr_resolv.h>
#include <epan/prefs.h>
#include <epan/strutil.h>
#include <epan/emem.h>
#include <epan/reassemble.h>

#define TCP_PORT_SMTP 25

static int proto_smtp = -1;

static int hf_smtp_req = -1;
static int hf_smtp_rsp = -1;
static int hf_smtp_req_command = -1;
static int hf_smtp_req_parameter = -1;
static int hf_smtp_rsp_code = -1;
static int hf_smtp_rsp_parameter = -1;

static int hf_smtp_data_fragments = -1;
static int hf_smtp_data_fragment = -1;
static int hf_smtp_data_fragment_overlap = -1;
static int hf_smtp_data_fragment_overlap_conflicts = -1;
static int hf_smtp_data_fragment_multiple_tails = -1;
static int hf_smtp_data_fragment_too_long_fragment = -1;
static int hf_smtp_data_fragment_error = -1;
static int hf_smtp_data_reassembled_in = -1;

static int ett_smtp = -1;
static int ett_smtp_cmdresp = -1;

static gint ett_smtp_data_fragment = -1;
static gint ett_smtp_data_fragments = -1;

/* desegmentation of SMTP command and response lines */
static gboolean smtp_desegment = TRUE;
static gboolean smtp_data_desegment = TRUE;

static GHashTable *smtp_data_segment_table = NULL;
static GHashTable *smtp_data_reassembled_table = NULL;

static const fragment_items smtp_data_frag_items = {
	/* Fragment subtrees */
	&ett_smtp_data_fragment,
	&ett_smtp_data_fragments,
	/* Fragment fields */
	&hf_smtp_data_fragments,
	&hf_smtp_data_fragment,
	&hf_smtp_data_fragment_overlap,
	&hf_smtp_data_fragment_overlap_conflicts,
	&hf_smtp_data_fragment_multiple_tails,
	&hf_smtp_data_fragment_too_long_fragment,
	&hf_smtp_data_fragment_error,
	/* Reassembled in field */
	&hf_smtp_data_reassembled_in,
	/* Tag */
	"DATA fragments"
};

/* Define media_type/Content type table */
static dissector_table_t media_type_dissector_table;


static  dissector_handle_t imf_handle = NULL;

/*
 * A CMD is an SMTP command, MESSAGE is the message portion, and EOM is the
 * last part of a message
 */

#define SMTP_PDU_CMD     0
#define SMTP_PDU_MESSAGE 1
#define SMTP_PDU_EOM     2

struct smtp_proto_data {
  guint16 pdu_type;
  guint16 conversation_id;
};

/*
 * State information stored with a conversation.
 */
struct smtp_request_val {
  gboolean reading_data; /* Reading message data, not commands */
  guint16 crlf_seen;     /* Have we seen a CRLF on the end of a packet */
  guint16 data_seen;     /* Have we seen a DATA command yet */
};


static void dissect_smtp_data(tvbuff_t *tvb, int offset, proto_tree *smtp_tree)
{
  gint next_offset;

  while (tvb_offset_exists(tvb, offset)) {

    /*
     * Find the end of the line.
     */
    tvb_find_line_end(tvb, offset, -1, &next_offset, FALSE);

    /*
     * Put this line.
     */
    proto_tree_add_text(smtp_tree, tvb, offset, next_offset - offset,
			"Message: %s",
			tvb_format_text(tvb, offset, next_offset - offset));

    /*
     * Step to the next line.
     */
    offset = next_offset;
	      
  }

}

static void
dissect_smtp(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
    struct smtp_proto_data  *frame_data;
    proto_tree              *smtp_tree;
    proto_tree              *cmdresp_tree;
    proto_item              *ti;
    int                     offset = 0;
    int                     request = 0;
    conversation_t          *conversation;
    struct smtp_request_val *request_val;
    const guchar            *line;
    guint32                 code;
    int                     linelen;
    gint                    length_remaining;
    gboolean                eom_seen = FALSE;
    gint                    next_offset;
    gboolean                is_continuation_line;
    int                     cmdlen;
    fragment_data           *frag_msg = NULL;
    tvbuff_t                *next_tvb;

    /* As there is no guarantee that we will only see frames in the
     * the SMTP conversation once, and that we will see them in
     * order - in Wireshark, the user could randomly click on frames
     * in the conversation in any order in which they choose - we
     * have to store information with each frame indicating whether
     * it contains commands or data or an EOM indication.
     *
     * XXX - what about frames that contain *both*?  TCP is a
     * byte-stream protocol, and there are no guarantees that
     * TCP segment boundaries will correspond to SMTP commands
     * or EOM indications.
     *
     * We only need that for the client->server stream; responses
     * are easy to manage.
     *
     * If we have per frame data, use that, else, we must be on the first
     * pass, so we figure it out on the first pass.
     */

    /* Find out what conversation this packet is part of ... but only
     * if we have no information on this packet, so find the per-frame
     * info first.
     */

    /* SMTP messages have a simple format ... */

    request = pinfo -> destport == pinfo -> match_port;

    /*
     * Get the first line from the buffer.
     *
     * Note that "tvb_find_line_end()" will, if it doesn't return
     * -1, return a value that is not longer than what's in the buffer,
     * and "tvb_find_line_end()" will always return a value that is not
     * longer than what's in the buffer, so the "tvb_get_ptr()" call
     * won't throw an exception.
     */
    linelen = tvb_find_line_end(tvb, offset, -1, &next_offset,
      smtp_desegment && pinfo->can_desegment);
    if (linelen == -1) {
      /*
       * We didn't find a line ending, and we're doing desegmentation;
       * tell the TCP dissector where the data for this message starts
       * in the data it handed us, and tell it we need one more byte
       * (we may need more, but we'll try again if what we get next
       * isn't enough), and return.
       */
      pinfo->desegment_offset = offset;
      pinfo->desegment_len = 1;
      return;
    }
    line = tvb_get_ptr(tvb, offset, linelen);

    frame_data = p_get_proto_data(pinfo->fd, proto_smtp);

    if (!frame_data) {

      conversation = find_conversation(pinfo->fd->num, &pinfo->src, &pinfo->dst, pinfo->ptype,
				       pinfo->srcport, pinfo->destport, 0);
      if (conversation == NULL) { /* No conversation, create one */
	conversation = conversation_new(pinfo->fd->num, &pinfo->src, &pinfo->dst, pinfo->ptype,
					pinfo->srcport, pinfo->destport, 0);

      }

      /*
       * Is there a request structure attached to this conversation?
       */
      request_val = conversation_get_proto_data(conversation, proto_smtp);

      if (!request_val) {

        /*
         * No - create one and attach it.
         */
	request_val = se_alloc(sizeof(struct smtp_request_val));
	request_val->reading_data = FALSE;
	request_val->crlf_seen = 0;
	request_val->data_seen = 0;

	conversation_add_proto_data(conversation, proto_smtp, request_val);

      }

      /*
       * Check whether or not this packet is an end of message packet
       * We should look for CRLF.CRLF and they may be split.
       * We have to keep in mind that we may see what we want on
       * two passes through here ...
       */

      if (request_val->reading_data) {

	/*
	 * The order of these is important ... We want to avoid
	 * cases where there is a CRLF at the end of a packet and a
	 * .CRLF at the begining of the same packet.
	 */

	if ((request_val->crlf_seen && tvb_strneql(tvb, offset, ".\r\n", 3) == 0) ||
	    tvb_strneql(tvb, offset, "\r\n.\r\n", 5) == 0) {

	  eom_seen = TRUE;

	}

	length_remaining = tvb_length_remaining(tvb, offset);
	if (length_remaining == tvb_reported_length_remaining(tvb, offset) &&
	    tvb_strneql(tvb, offset + length_remaining - 2, "\r\n", 2) == 0) {

	  request_val->crlf_seen = 1;

	}
	else {

	  request_val->crlf_seen = 0;

	}
      }

    /*
     * OK, Check if we have seen a DATA request. We do it here for
     * simplicity, but we have to be careful below.
     */

      if (request) {

	frame_data = se_alloc(sizeof(struct smtp_proto_data));

	frame_data->conversation_id = conversation->index;

	if (request_val->reading_data) {
	  /*
	   * This is message data.
	   */
	  if (eom_seen) { /* Seen the EOM */
	    /*
	     * EOM.
	     * Everything that comes after it is commands.
	     *
	     * XXX - what if the EOM isn't at the beginning of
	     * the TCP segment?  It can occur anywhere....
	     */
	    frame_data->pdu_type = SMTP_PDU_EOM;
	    request_val->reading_data = FALSE;
	  } else {
	    /*
	     * Message data with no EOM.
	     */
	    frame_data->pdu_type = SMTP_PDU_MESSAGE;
	  }
	} else {
	  /*
	   * This is commands - unless the capture started in the
	   * middle of a session, and we're in the middle of data.
	   * To quote RFC 821, "Command codes are four alphabetic
	   * characters"; if we don't see four alphabetic characters
	   * and, if there's anything else in the line, a space, we
	   * assume it's not a command.
	   * (We treat only A-Z and a-z as alphabetic.)
	   */
#define	ISALPHA(c)	(((c) >= 'A' && (c) <= 'Z') || \
			 ((c) >= 'a' && (c) <= 'z'))
	  if (linelen >= 4 && ISALPHA(line[0]) && ISALPHA(line[1]) &&
	      ISALPHA(line[2]) && ISALPHA(line[3]) &&
	      (linelen == 4 || line[4] == ' ')) {
	    if (strncasecmp(line, "DATA", 4) == 0) {

	      /*
	       * DATA command.
	       * This is a command, but everything that comes after it,
	       * until an EOM, is data.
	       */
	      frame_data->pdu_type = SMTP_PDU_CMD;
	      request_val->reading_data = TRUE;
	      request_val->data_seen = TRUE;

	    } else {

	      /*
	       * Regular command.
	       */
	      frame_data->pdu_type = SMTP_PDU_CMD;

	    }
	  } else {
		if ((linelen >= 7) && line[0] == 'X' && ( (strncasecmp(line, "X-EXPS ", 7) == 0) ||
			((linelen >=13) && (strncasecmp(line, "X-LINK2STATE ", 13) == 0)) || 
			((linelen >= 8) && (strncasecmp(line, "XEXCH50 ", 8) == 0)) ))
				frame_data->pdu_type = SMTP_PDU_CMD;
		else
	    /*
	     * Assume it's message data.
	     */

		  
	    frame_data->pdu_type = request_val->data_seen ? SMTP_PDU_MESSAGE : SMTP_PDU_CMD;

	  }

	}

	p_add_proto_data(pinfo->fd, proto_smtp, frame_data);

      }
    }

    /*
     * From here, we simply add items to the tree and info to the info
     * fields ...
     */

    if (check_col(pinfo->cinfo, COL_PROTOCOL))
      col_set_str(pinfo->cinfo, COL_PROTOCOL, "SMTP");

    if (check_col(pinfo->cinfo, COL_INFO)) {  /* Add the appropriate type here */

      /*
       * If it is a request, we have to look things up, otherwise, just
       * display the right things
       */

      if (request) {

	/* We must have frame_data here ... */

	switch (frame_data->pdu_type) {
	case SMTP_PDU_MESSAGE:

	  col_set_str(pinfo->cinfo, COL_INFO, smtp_data_desegment ? "DATA fragment" : "Message Body");
	  break;

	case SMTP_PDU_EOM:

	  col_add_fstr(pinfo->cinfo, COL_INFO, "EOM: %s",
	      format_text(line, linelen));
	  break;

	case SMTP_PDU_CMD:

	  col_add_fstr(pinfo->cinfo, COL_INFO, "Command: %s",
	      format_text(line, linelen));
	  break;

	}

      }
      else {

	col_add_fstr(pinfo->cinfo, COL_INFO, "Response: %s",
	    format_text(line, linelen));

      }
    }

    if (tree) { /* Build the tree info ... */

      ti = proto_tree_add_item(tree, proto_smtp, tvb, offset, -1, FALSE);
      smtp_tree = proto_item_add_subtree(ti, ett_smtp);
      if (request) {

	/*
	 * Check out whether or not we can see a command in there ...
	 * What we are looking for is not data_seen and the word DATA
	 * and not eom_seen.
	 *
	 * We will see DATA and request_val->data_seen when we process the
	 * tree view after we have seen a DATA packet when processing
	 * the packet list pane.
	 *
	 * On the first pass, we will not have any info on the packets
	 * On second and subsequent passes, we will.
	 */

	switch (frame_data->pdu_type) {

	case SMTP_PDU_MESSAGE:

	  if(smtp_data_desegment) {

	    frag_msg = fragment_add_seq_next (tvb, 0, pinfo, 
					      frame_data->conversation_id, smtp_data_segment_table,
					      smtp_data_reassembled_table, tvb_length_remaining(tvb,0), TRUE);


	    if (frag_msg && pinfo->fd->num != frag_msg->reassembled_in) {
	      /* Add a "Reassembled in" link if not reassembled in this frame */
	      proto_tree_add_uint (smtp_tree, *(smtp_data_frag_items.hf_reassembled_in),
					     tvb, 0, 0, frag_msg->reassembled_in);
	    }

	    pinfo->fragmented = TRUE;
	  } else {

	    /*
	     * Message body.
	     * Put its lines into the protocol tree, a line at a time.
	     */

	    dissect_smtp_data(tvb, offset, smtp_tree);
	    
	  }

	  break;

	case SMTP_PDU_EOM:

	  /*
	   * End-of-message-body indicator.
	   *
	   * XXX - what about stuff after the first line?
	   * Unlikely, as the client should wait for a response to the
	   * DATA command this terminates before sending another
	   * request, but we should probably handle it.
	   */
	  proto_tree_add_text(smtp_tree, tvb, offset, linelen,
	      "EOM: %s", format_text(line, linelen));

	  if(smtp_data_desegment) {

	    /* terminate the desegmentation */
	    frag_msg = fragment_end_seq_next (pinfo, frame_data->conversation_id, smtp_data_segment_table,
					      smtp_data_reassembled_table);

	    next_tvb = process_reassembled_data (tvb, offset, pinfo, "Reassembled DATA", 
						 frag_msg, &smtp_data_frag_items, NULL, smtp_tree);

	    /* XXX: this is presumptious - we may have negotiated something else */
	    if(imf_handle)
	      call_dissector(imf_handle, next_tvb, pinfo, tree);
	    else {

	      /*
	       * Message body.
	       * Put its lines into the protocol tree, a line at a time.
	       */

	      dissect_smtp_data(tvb, offset, smtp_tree);

	    }

	    pinfo->fragmented = FALSE;
	  }

	  break;

	case SMTP_PDU_CMD:

	  /*
	   * Command.
	   *
	   * XXX - what about stuff after the first line?
	   * Unlikely, as the client should wait for a response to the
	   * previous command before sending another request, but we
	   * should probably handle it.
	   */
	  if (linelen >= 4)
	    cmdlen = 4;
	  else
	    cmdlen = linelen;
	  proto_tree_add_boolean_hidden(smtp_tree, hf_smtp_req, tvb,
					0, 0, TRUE);
	  /*
	   * Put the command line into the protocol tree.
	   */
	  ti = proto_tree_add_text(smtp_tree, tvb, offset, next_offset - offset,
	        "Command: %s",
		tvb_format_text(tvb, offset, next_offset - offset));
	  cmdresp_tree = proto_item_add_subtree(ti, ett_smtp_cmdresp);

	  proto_tree_add_item(cmdresp_tree, hf_smtp_req_command, tvb,
			      offset, cmdlen, FALSE);
	  if (linelen > 5) {
	    proto_tree_add_item(cmdresp_tree, hf_smtp_req_parameter, tvb,
				offset + 5, linelen - 5, FALSE);
	  }

	}

      }
      else {

        /*
	 * Process the response, a line at a time, until we hit a line
	 * that doesn't have a continuation indication on it.
	 */
	proto_tree_add_boolean_hidden(smtp_tree, hf_smtp_rsp, tvb,
					0, 0, TRUE);

	while (tvb_offset_exists(tvb, offset)) {

	  /*
	   * Find the end of the line.
	   */
	  linelen = tvb_find_line_end(tvb, offset, -1, &next_offset, FALSE);

	  /*
	   * Put it into the protocol tree.
	   */
	  ti = proto_tree_add_text(smtp_tree, tvb, offset,
				   next_offset - offset, "Response: %s",
				   tvb_format_text(tvb, offset,
						   next_offset - offset));
	  cmdresp_tree = proto_item_add_subtree(ti, ett_smtp_cmdresp);

	  /*
	   * Is it a continuation line?
	   */
	  is_continuation_line =
	      (linelen >= 4 && tvb_get_guint8(tvb, offset + 3) == '-');

	  /*
	   * Put the response code and parameters into the protocol tree.
	   */
	  line = tvb_get_ptr(tvb, offset, linelen);
	  if (linelen >= 3 && isdigit(line[0]) && isdigit(line[1])
	 		   && isdigit(line[2])) {
	    /*
	     * We have a 3-digit response code.
	     */
	    code = (line[0] - '0')*100 + (line[1] - '0')*10 + (line[2] - '0');
	    proto_tree_add_uint(cmdresp_tree, hf_smtp_rsp_code, tvb, offset, 3,
				code);

	    if (linelen >= 4) {
	      proto_tree_add_item(cmdresp_tree, hf_smtp_rsp_parameter, tvb,
				  offset + 4, linelen - 4, FALSE);
	    }
	  }

	  /*
	   * Step past this line.
	   */
	  offset = next_offset;

	  /*
	   * If it's not a continuation line, quit.
	   */
	  if (!is_continuation_line)
	    break;

	}

      }
    }
}

static void smtp_data_reassemble_init (void)
{
	fragment_table_init (&smtp_data_segment_table);
	reassembled_table_init (&smtp_data_reassembled_table);
}


/* Register all the bits needed by the filtering engine */

void
proto_register_smtp(void)
{
  static hf_register_info hf[] = {
    { &hf_smtp_req,
      { "Request", "smtp.req", FT_BOOLEAN, BASE_NONE, NULL, 0x0, "", HFILL }},

    { &hf_smtp_rsp,
      { "Response", "smtp.rsp", FT_BOOLEAN, BASE_NONE, NULL, 0x0, "", HFILL }},

    { &hf_smtp_req_command,
      { "Command", "smtp.req.command", FT_STRING,  BASE_NONE, NULL, 0x0,
      	"", HFILL }},

    { &hf_smtp_req_parameter,
      { "Request parameter", "smtp.req.parameter", FT_STRING, BASE_NONE, NULL, 0x0,
      	"", HFILL }},

    { &hf_smtp_rsp_code,
      { "Response code", "smtp.response.code", FT_UINT32, BASE_DEC, NULL, 0x0,
      	"", HFILL }},

    { &hf_smtp_rsp_parameter,
      { "Response parameter", "smtp.rsp.parameter", FT_STRING, BASE_NONE, NULL, 0x0,
      	"", HFILL }},

    /* Fragment entries */
    { &hf_smtp_data_fragments,
      { "DATA fragments", "smtp.data.fragments", FT_NONE, BASE_NONE,
	NULL, 0x00, "Message fragments", HFILL } },
    { &hf_smtp_data_fragment,
      { "DATA fragment", "smtp.data.fragment", FT_FRAMENUM, BASE_NONE,
	NULL, 0x00, "Message fragment", HFILL } },
    { &hf_smtp_data_fragment_overlap,
      { "DATA fragment overlap", "smtp.data.fragment.overlap", FT_BOOLEAN,
	BASE_NONE, NULL, 0x00, "Message fragment overlap", HFILL } },
    { &hf_smtp_data_fragment_overlap_conflicts,
      { "DATA fragment overlapping with conflicting data",
	"smtp.data.fragment.overlap.conflicts", FT_BOOLEAN, BASE_NONE, NULL,
	0x00, "Message fragment overlapping with conflicting data", HFILL } },
    { &hf_smtp_data_fragment_multiple_tails,
      { "DATA has multiple tail fragments",
	"smtp.data.fragment.multiple_tails", FT_BOOLEAN, BASE_NONE,
	NULL, 0x00, "Message has multiple tail fragments", HFILL } },
    { &hf_smtp_data_fragment_too_long_fragment,
      { "DATA fragment too long", "smtp.data.fragment.too_long_fragment",
	FT_BOOLEAN, BASE_NONE, NULL, 0x00, "Message fragment too long",
	HFILL } },
    { &hf_smtp_data_fragment_error,
      { "DATA defragmentation error", "smtp.data.fragment.error", FT_FRAMENUM,
	BASE_NONE, NULL, 0x00, "Message defragmentation error", HFILL } },
    { &hf_smtp_data_reassembled_in,
      { "Reassembled DATA in frame", "smtp.data.reassembled.in", FT_FRAMENUM, BASE_NONE,
	NULL, 0x00, "This DATA fragment is reassembled in this frame", HFILL } },
  };
  static gint *ett[] = {
    &ett_smtp,
    &ett_smtp_cmdresp,
    &ett_smtp_data_fragment,
    &ett_smtp_data_fragments,

  };
  module_t *smtp_module;


  proto_smtp = proto_register_protocol("Simple Mail Transfer Protocol",
				       "SMTP", "smtp");

  proto_register_field_array(proto_smtp, hf, array_length(hf));
  proto_register_subtree_array(ett, array_length(ett));
  register_init_routine (&smtp_data_reassemble_init);

  /* Allow dissector to find be found by name. */
  register_dissector("smtp", dissect_smtp, proto_smtp);

  /* Preferences */
  smtp_module = prefs_register_protocol(proto_smtp, NULL);
  prefs_register_bool_preference(smtp_module, "desegment_lines",
    "Reassemble SMTP command and response lines\nspanning multiple TCP segments",
    "Whether the SMTP dissector should reassemble command and response lines spanning multiple TCP segments."
    " To use this option, you must also enable \"Allow subdissectors to reassemble TCP streams\" in the TCP protocol settings.",
    &smtp_desegment);

  prefs_register_bool_preference(smtp_module, "desegment_data",
    "Reassemble SMTP DATA commands spanning multiple TCP segments",
    "Whether the SMTP dissector should reassemble DATA command and lines spanning multiple TCP segments."
    " To use this option, you must also enable \"Allow subdissectors to reassemble TCP streams\" in the TCP protocol settings.",
    &smtp_data_desegment);

}

/* The registration hand-off routine */
void
proto_reg_handoff_smtp(void)
{
  dissector_handle_t smtp_handle;

  smtp_handle = create_dissector_handle(dissect_smtp, proto_smtp);
  dissector_add("tcp.port", TCP_PORT_SMTP, smtp_handle);

  /*
   * Get the content type and Internet media type table
   */
  media_type_dissector_table = find_dissector_table("media_type");

  /* find the IMF dissector */
  imf_handle = find_dissector("imf");

}
