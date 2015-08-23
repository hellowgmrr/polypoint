#include <string.h>

#include "deca_device_api.h"
#include "deca_regs.h"

#include "timer.h"
#include "dw1000.h"
#include "dw1000_tag.h"
#include "firmware.h"

// Our timer object that we use for timing packet transmissions
timer_t* _ranging_broadcast_timer;

tag_state_e _tag_state = TSTATE_IDLE;

// Which subsequence slot we are on when transmitting broadcast packets
// for ranging.
static uint8_t _ranging_broadcast_ss_num = 0;

// Which slot we are in when receiving packets from the anchor.
static uint8_t _ranging_listening_window_num = 0;

// Array of when we sent each of the broadcast ranging packets
static uint64_t _ranging_broadcast_ss_send_times[NUM_RANGING_BROADCASTS] = {0};

// How many anchor responses we have gotten
static uint8_t _anchor_response_count = 0;

// Array of when we received ANC_FINAL packets and from whom
static anchor_response_times_t _anchor_response_times[MAX_NUM_ANCHOR_RESPONSES];



static struct pp_tag_poll pp_tag_poll_pkt = {
	{ // 802.15.4 HEADER
		{
			0x41, // FCF[0]: data frame, panid compression
			0xC8  // FCF[1]: ext source address, compressed destination
		},
		0,        // Sequence number
		{
			POLYPOINT_PANID & 0xFF, // PAN ID
			POLYPOINT_PANID >> 8
		},
		{
			0xFF, // Destination address: broadcast
			0xFF
		},
		{ 0 }     // Source (blank for now)
	},
	// PACKET BODY
	MSG_TYPE_PP_NOSLOTS_TAG_POLL,  // Message type
	0,                             // Sub Sequence number
	NUM_RANGING_BROADCASTS-1,
	RANGING_LISTENING_WINDOW_US,
	RANGING_LISTENING_SLOT_US
};

// Functions
static void send_poll ();
static void ranging_broadcast_subsequence_task ();
static void ranging_listening_window_task ();
static void calculate_ranges ();

void dw1000_tag_init () {

	uint8_t eui_array[8];

	// Allow data and ack frames
	dwt_enableframefilter(DWT_FF_DATA_EN | DWT_FF_ACK_EN);

	// Set this node's ID and the PAN ID for our DW1000 ranging system
	dw1000_read_eui(eui_array);
	dwt_seteui(eui_array);
	dwt_setpanid(POLYPOINT_PANID);

	// Setup parameters of how the radio should work
	dwt_setautorxreenable(TRUE);
	dwt_setdblrxbuffmode(TRUE);
	dwt_enableautoack(DW1000_ACK_RESPONSE_TIME);

	// Configure sleep
	{
		int mode = DWT_LOADUCODE    |
		           DWT_PRESRV_SLEEP |
		           DWT_CONFIG       |
		           DWT_TANDV;
		if (dwt_getldotune() != 0) {
			// If we need to use LDO tune value from OTP kick it after sleep
			mode |= DWT_LOADLDO;
		}

		// NOTE: on the EVK1000 the DEEPSLEEP is not actually putting the
		// DW1000 into full DEEPSLEEP mode as XTAL is kept on
		dwt_configuresleep(mode, DWT_WAKE_CS | DWT_SLP_EN);
	}

	// Put source EUI in the pp_tag_poll packet
	dw1000_read_eui(pp_tag_poll_pkt.header.sourceAddr);

	// Create a timer for use when sending ranging broadcast packets
	_ranging_broadcast_timer = timer_init();

	// Make SPI fast now that everything has been setup
	dw1000_spi_fast();
}

// This starts a ranging event by causing the tag to send a series of
// ranging broadcasts.
void dw1000_tag_start_ranging_event () {
	_tag_state = TSTATE_BROADCASTS;

	// Clear state that we keep for each ranging event
	memset(_ranging_broadcast_ss_send_times, 0, sizeof(_ranging_broadcast_ss_send_times));
	_ranging_broadcast_ss_num = 0;

	// Start a timer that will kick off the broadcast ranging events
	timer_start(_ranging_broadcast_timer, RANGING_BROADCASTS_PERIOD_US, ranging_broadcast_subsequence_task);
}


void dw1000_tag_txcallback (const dwt_callback_data_t *data) {

	if (data->event == DWT_SIG_TX_DONE) {
		// Packet was sent successfully

		// Check which state we are in to decide what to do.
		// We use TX_callback because it will get called after we have sent
		// all of the broadcast packets. (Now of course we will get this
		// callback multiple times, but that is ok.)
		if (_tag_state == TSTATE_TRANSITION_TO_ANC_FINAL) {
			// At this point we have sent all of our ranging broadcasts.
			// Now we move to listening for responses from anchors.
			_tag_state = TSTATE_LISTENING;

			// Init some state
			_ranging_listening_window_num = 0;
			_anchor_response_count = 0;

			// Start a timer to switch between the windows
			timer_start(_ranging_broadcast_timer, RANGING_LISTENING_WINDOW_US, ranging_listening_window_task);

		} else {
			// We don't need to do anything on TX done for any other states
		}


	} else {
		timer_stop(_ranging_broadcast_timer);
	}

}

// Called when the tag receives a packet.
void dw1000_tag_rxcallback (const dwt_callback_data_t* rxd) {
	if (rxd->event == DWT_SIG_RX_OKAY) {
		// Everything went right when receiving this packet.
		// We have to process it to ensure that it is a packet we are expecting
		// to get.

		uint64_t dw_rx_timestamp;
		uint8_t  buf[DW1000_TAG_MAX_RX_PKT_LEN];
		uint8_t  message_type;

		// Get the received time of this packet first
		dwt_readrxtimestamp(buf);
		dw_rx_timestamp = DW_TIMESTAMP_TO_UINT64(buf);

		// Get the actual packet bytes
		dwt_readrxdata(buf, MIN(DW1000_TAG_MAX_RX_PKT_LEN, rxd->datalength), 0);
		message_type = buf[offsetof(struct pp_anc_final, message_type)];

		if (message_type == MSG_TYPE_PP_NOSLOTS_ANC_FINAL) {
			// This is what we were looking for, an ANC_FINAL packet
			struct pp_anc_final* anc_final;

			if (_anchor_response_count >= MAX_NUM_ANCHOR_RESPONSES) {
				// Nowhere to store this, so we have to ignore this
				return;
			}

			// Continue parsing the received packet
			anc_final = (struct pp_anc_final*) buf;

			// Save the anchor address
			memcpy(_anchor_response_times[_anchor_response_count].anchor_addr,
			       anc_final->header.sourceAddr, 8);

			// Save the anchor's list of when it received the tag broadcasts
			memcpy(_anchor_response_times[_anchor_response_count].tag_poll_TOAs,
			       anc_final->TOAs, sizeof(anc_final->TOAs));

			// Save when the anchor sent the packet we just received
			_anchor_response_times[_anchor_response_count].anc_final_tx_timestamp = ((uint64_t)anc_final->dw_time_sent) << 8;
			// Save when we received the packet
			_anchor_response_times[_anchor_response_count].anc_final_rx_timestamp = dw_rx_timestamp;

			// Increment the number of anchors heard from
			_anchor_response_count++;

		} else {
			// TAGs don't expect to receive any other types of packets.
		}

	} else {
		// Packet was NOT received correctly. Need to do some re-configuring
		// as things get blown out when this happens. (Because dwt_rxreset
		// within dwt_isr smashes everything without regard.)
		if (rxd->event == DWT_SIG_RX_PHR_ERROR ||
		    rxd->event == DWT_SIG_RX_ERROR ||
		    rxd->event == DWT_SIG_RX_SYNCLOSS ||
		    rxd->event == DWT_SIG_RX_SFDTIMEOUT ||
		    rxd->event == DWT_SIG_RX_PTOTIMEOUT) {
			dw1000_set_ranging_listening_window_settings(TAG, _ranging_listening_window_num, FALSE);
		}
	}

}

// Send one of the ranging broadcast packets.
// After it sends the last of the subsequence this function automatically
// puts the DW1000 in RX mode.
static void send_poll () {
	int err;

	// Record the packet length to send to DW1000
	uint16_t tx_len = sizeof(struct pp_tag_poll);

	// Setup what needs to change in the outgoing packet
	pp_tag_poll_pkt.header.seqNum++;
	pp_tag_poll_pkt.subsequence = _ranging_broadcast_ss_num;

	// Make sure we're out of RX mode before attempting to transmit
	dwt_forcetrxoff();

	// Tell the DW1000 about the packet
	dwt_writetxfctrl(tx_len, 0);

	// Setup the time the packet will go out at, and save that timestamp
	uint32_t delay_time = dwt_readsystimestamphi32() + DW_DELAY_FROM_PKT_LEN(tx_len);
	delay_time &= 0xFFFFFFFE; //Make sure last bit is zero
	dwt_setdelayedtrxtime(delay_time);
	_ranging_broadcast_ss_send_times[_ranging_broadcast_ss_num] = ((uint64_t) delay_time) << 8;

	// Write the data
	dwt_writetxdata(tx_len, (uint8_t*) &pp_tag_poll_pkt, 0);

	// Start the transmission
	if (_ranging_broadcast_ss_num == NUM_RANGING_BROADCASTS-1) {
		// This is the last broadcast ranging packet, so we want to transition
		// to RX mode after this packet to receive the responses from the anchors.
		dwt_setrxaftertxdelay(1); // us
		err = dwt_starttx(DWT_START_TX_DELAYED | DWT_RESPONSE_EXPECTED);
	} else {
		err = dwt_starttx(DWT_START_TX_DELAYED);
	}

	// MP bug - TX antenna delay needs reprogramming as it is not preserved
	dwt_settxantennadelay(DW1000_ANTENNA_DELAY_TX);
}

// This is called for each broadcast ranging subsequence interval where
// the tag sends broadcast packets.
static void ranging_broadcast_subsequence_task () {

	if (_ranging_broadcast_ss_num == NUM_RANGING_BROADCASTS-1) {
		// This is our last packet to send. Stop the timer so we don't generate
		// more packets.
		timer_stop(_ranging_broadcast_timer);
	}

	// Go ahead and setup and send a ranging broadcast
	dw1000_set_ranging_broadcast_subsequence_settings(TAG, _ranging_broadcast_ss_num, FALSE);

	// Actually send the packet
	send_poll();
	_ranging_broadcast_ss_num += 1;
}

// This is called after the broadcasts have been sent in order to receive
// the responses from the anchors.
static void ranging_listening_window_task () {

	// Stop after the last of the receive windows
	if (_ranging_listening_window_num == NUM_RANGING_LISTENING_WINDOWS) {
		timer_stop(_ranging_broadcast_timer);

		// Stop the radio
		dwt_forcetrxoff();

		// New state
		_tag_state = TSTATE_CALCULATE_RANGE;

		// Calculate ranges
		calculate_ranges();

	} else {

		// Set the correct listening settings
		dw1000_set_ranging_listening_window_settings(TAG, _ranging_listening_window_num, FALSE);

		// Increment and wait
		_ranging_listening_window_num++;

	}
}

static void calculate_ranges () {

}



