/*
	Ralink RT2501 driver for Violet embedded platforms
	(c) 2006 Sebastien Bourdeauducq
*/

#include <intrinsics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "usbctrl.h"
#include "mem.h"
#include "hcdmem.h"
#include "hcd.h"
#include "usbh.h"
#include "delay.h"
#include "debug.h"
#include "led.h"

#include "rt2501usb_hw.h"
#include "rt2501usb_io.h"
#include "rt2501usb_internal.h"
#include "rt2501usb.h"
#include "eapol.h"
#include "hash.h"
#include "rc4.h"

static void randbuffer(unsigned char *buffer, unsigned int length)
{
	unsigned int i;
	
	for(i=0;i<length;i++)
		buffer[i] = rand() & 0xff;
}

/*
 * F(P,  S, c, i) = U1 xor U2 xor ... Uc
 * U1 =  PRF(P, S || Int(i))
 * U2 =  PRF(P, U1)
 * Uc =  PRF(P, Uc-1)
 */
void F(const char *password, int passwordlength, char *ssid, int ssidlength, int iterations, int count, unsigned char *output)
{
	unsigned char digest[36], digest1[20];
	int i, j;
        struct rt2501buffer *r;
	
	/* U1 = PRF(P, S || int(i)) */
	memcpy(digest, ssid, ssidlength);
	digest[ssidlength] = (unsigned char)((count>>24) & 0xff);
	digest[ssidlength+1] = (unsigned char)((count>>16) & 0xff);
	digest[ssidlength+2] = (unsigned char)((count>>8) & 0xff);
	digest[ssidlength+3] = (unsigned char)(count & 0xff);
	hmac_sha1((unsigned char *)password, passwordlength, digest, ssidlength+4, digest1);
	/* output = U1 */
	memcpy(output, digest1, 20);
	for(i=1;i<iterations;i++) {
		/* Un = PRF(P, Un-1) */
		hmac_sha1((unsigned char *)password, passwordlength, digest1, 20, digest);
		memcpy(digest1, digest, 20);
		/* output = output xor Un */
		for(j=0;j<20;j++)
			output[j] ^= digest[j];
                if (!(i&63))
                {
                  j=(i>>6)&3;
                  set_led(1,(j==0)?0xff:0);
                  set_led(2,(j&1)?0xff:0);
                  set_led(3,(j==2)?0xff:0);
                  usbhost_events();
                  CLR_WDT;
                  while(r=rt2501_receive())
                  {
                        CLR_WDT;
                        disable_ohci_irq();
                        hcd_free(r);
                        enable_ohci_irq();
                  }
                }
	}
}

/* output must be 40 bytes long, only the first 32 bytes are useful */
static void password_to_pmk(const char *password, char *ssid, int ssidlength, unsigned char *pmk)
{
	F(password, strlen(password), ssid, ssidlength, 4096, 1, pmk);
	F(password, strlen(password), ssid, ssidlength, 4096, 2, pmk+20);
}

void mypassword_to_pmk(const char *password, char *ssid, int ssidlength, unsigned char *pmk)
{
  password_to_pmk(password,ssid,ssidlength,pmk);
}

void prf(const unsigned char *key, int key_len, const unsigned char *prefix, int prefix_len, const unsigned char *data, int data_len, unsigned char *output, int len)
{
	int i;
	unsigned char *input; /* concatenated input */
	int currentindex = 0;
	int total_len;
	
	disable_ohci_irq();
	input = hcd_malloc(1024, EXTRAM,1);
	enable_ohci_irq();
	if(input == NULL) {
		DBG_WIFI("hcd_malloc failed in prf\r\n");
		return;
	}
	memcpy(input, prefix, prefix_len);
	input[prefix_len] = 0; /* single octet 0 */
	memcpy(&input[prefix_len+1], data, data_len);
	total_len = prefix_len + 1 + data_len;
	input[total_len] = 0; /* single octet count, starts at 0 */
	total_len++;
	for(i=0;i<(len+19)/20;i++) {
		hmac_sha1(key, key_len, input, total_len, &output[currentindex]);
		currentindex += 20; /* next concatenation location */
		input[total_len-1]++; /* increment octet count */
	}
	disable_ohci_irq();
	hcd_free(input);
	enable_ohci_irq();
}

static const unsigned char ptk_prefix[] = { 'P', 'a', 'i', 'r', 'w', 'i', 's', 'e', ' ', 'k', 'e', 'y', ' ', 'e', 'x', 'p', 'a', 'n', 's', 'i', 'o', 'n' };

static void compute_ptk(unsigned char *pmk, unsigned char *anonce, unsigned char *aa, unsigned char *snonce, unsigned char *sa, unsigned char *ptk, unsigned int length)
{
	unsigned char concatenation[128];
	unsigned int position;
	
	position = 0;
	
	/* Get min */
	if(memcmp(sa, aa, IEEE80211_ADDR_LEN) > 0)
		memcpy(&concatenation[position], aa, IEEE80211_ADDR_LEN);
	else
		memcpy(&concatenation[position], sa, IEEE80211_ADDR_LEN);
	position += IEEE80211_ADDR_LEN;
		
	/* Get max */
	if(memcmp(sa, aa, IEEE80211_ADDR_LEN) > 0)
		memcpy(&concatenation[position], sa, IEEE80211_ADDR_LEN);
	else
		memcpy(&concatenation[position], aa, IEEE80211_ADDR_LEN);
	position += IEEE80211_ADDR_LEN;
	
	/* Get min */
	if(memcmp(snonce, anonce, EAPOL_NONCE_LENGTH) > 0)
		memcpy(&concatenation[position], anonce, EAPOL_NONCE_LENGTH);
	else
		memcpy(&concatenation[position], snonce, EAPOL_NONCE_LENGTH);
	position += EAPOL_NONCE_LENGTH;
	
	/* Get max */
	if(memcmp(snonce, anonce, EAPOL_NONCE_LENGTH) > 0)
		memcpy(&concatenation[position], snonce, EAPOL_NONCE_LENGTH);
	else
		memcpy(&concatenation[position], anonce, EAPOL_NONCE_LENGTH);
	position += EAPOL_NONCE_LENGTH;
	
	prf(pmk, EAPOL_MASTER_KEY_LENGTH, ptk_prefix, sizeof(ptk_prefix), concatenation, position, ptk, length);
}

const unsigned char eapol_llc[LLC_LENGTH] =
	{ 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e };
static const unsigned char wpa_rsn[] = {
	0xdd, 0x16, 0x00, 0x50, 0xf2,
	0x01, 0x01, 0x00, 0x00, 0x50, 0xf2, 0x02, 0x01,
	0x00, 0x00, 0x50, 0xf2, 0x02, 0x01, 0x00, 0x00,
	0x50, 0xf2, 0x02
};

int eapol_state;
static unsigned char pmk[EAPOL_MASTER_KEY_LENGTH];
static unsigned char replay_active;
static unsigned char replay_counter[EAPOL_RPC_LENGTH];
static unsigned char anonce[EAPOL_NONCE_LENGTH];
static unsigned char snonce[EAPOL_NONCE_LENGTH];
static unsigned char ptk[EAPOL_PTK_LENGTH+20];
static unsigned char gtk[EAPOL_MICK_LENGTH+EAPOL_EK_LENGTH];
unsigned char ptk_tsc[EAPOL_TSC_LENGTH];

void eapol_init(void)
{
//	unsigned char buffer[40];
#ifdef DEBUG_WIFI
	int i;
#endif
	
	eapol_state = EAPOL_S_MSG1;
	replay_active = 0;
	/* Derive PMK */
	DBG_WIFI("Computing PMK...");
/*	password_to_pmk((const char *)ieee80211_key, ieee80211_assoc_ssid,
			strlen(ieee80211_assoc_ssid), buffer);
	memcpy(pmk, buffer, EAPOL_MASTER_KEY_LENGTH);
*/
	memcpy(pmk, ieee80211_key, EAPOL_MASTER_KEY_LENGTH);

#ifdef DEBUG_WIFI
	DBG_WIFI("done, ");
	for(i=0;i<EAPOL_MASTER_KEY_LENGTH;i++) {
		sprintf(dbg_buffer, "0x%02x,", pmk[i]);
		DBG_WIFI(dbg_buffer);
	}
	DBG_WIFI("\r\n");
#endif
}

static void eapol_input_msg1(unsigned char *frame, unsigned int length)
{
	struct eapol_frame *fr_in = (struct eapol_frame *)frame;
	struct {
		struct eapol_frame llc_eapol;
		unsigned char rsn[sizeof(wpa_rsn)];
	} fr_out;
	unsigned char ptk_buffer[80];
#ifdef DEBUG_WIFI
	int i;
#endif
	
	DBG_WIFI("Received EAPOL message 1/4\r\n");
	
	/* EAPOL_S_MSG3 can happen if the AP did not receive our reply */
	if((eapol_state != EAPOL_S_MSG1) && (eapol_state != EAPOL_S_MSG3)) {
		DBG_WIFI("Inappropriate message\r\n");
		return;
	}
	eapol_state = EAPOL_S_MSG1;
	
	/* Save ANonce */
	memcpy(anonce, fr_in->key_frame.key_nonce, EAPOL_NONCE_LENGTH);
	
	/* Generate SNonce */
	randbuffer(snonce, EAPOL_NONCE_LENGTH);
	
	/* Derive PTK */
	compute_ptk(pmk, anonce, ieee80211_assoc_mac, snonce, rt2501_mac, ptk_buffer, EAPOL_PTK_LENGTH);
	memcpy(ptk, ptk_buffer, EAPOL_PTK_LENGTH);
	
#ifdef DEBUG_WIFI
	DBG_WIFI("PTK computed, ");
	for(i=0;i<EAPOL_PTK_LENGTH;i++) {
		sprintf(dbg_buffer, "%02x", ptk[i]);
		DBG_WIFI(dbg_buffer);
	}
	DBG_WIFI("\r\n");
#endif
	
	/* Make response frame */
	memcpy(fr_out.llc_eapol.llc, eapol_llc, LLC_LENGTH);
	fr_out.llc_eapol.protocol_version = EAPOL_VERSION;
	fr_out.llc_eapol.packet_type = EAPOL_TYPE_KEY;
	fr_out.llc_eapol.body_length[0] = ((sizeof(struct eapol_key_frame)+sizeof(wpa_rsn)) & 0xff00) >> 8;
	fr_out.llc_eapol.body_length[1] = ((sizeof(struct eapol_key_frame)+sizeof(wpa_rsn)) & 0x00ff) >> 0;
	
	fr_out.llc_eapol.key_frame.descriptor_type = EAPOL_DTYPE_WPAKEY;
	fr_out.llc_eapol.key_frame.key_info.reserved = 0;
	fr_out.llc_eapol.key_frame.key_info.key_desc_ver = 1;
	fr_out.llc_eapol.key_frame.key_info.key_type = 1;
	fr_out.llc_eapol.key_frame.key_info.key_index = 0;
	fr_out.llc_eapol.key_frame.key_info.install = 0;
	fr_out.llc_eapol.key_frame.key_info.key_ack = 0;
	fr_out.llc_eapol.key_frame.key_info.key_mic = 1;
	fr_out.llc_eapol.key_frame.key_info.secure = 0;
	fr_out.llc_eapol.key_frame.key_info.error = 0;
	fr_out.llc_eapol.key_frame.key_info.request = 0;
	fr_out.llc_eapol.key_frame.key_info.ekd = 0;
	fr_out.llc_eapol.key_frame.key_length[0] = 0;
	fr_out.llc_eapol.key_frame.key_length[1] = 0;
	memcpy(fr_out.llc_eapol.key_frame.replay_counter, replay_counter, EAPOL_RPC_LENGTH);
	memcpy(fr_out.llc_eapol.key_frame.key_nonce, snonce, EAPOL_NONCE_LENGTH);
	memset(fr_out.llc_eapol.key_frame.key_iv, 0, EAPOL_KEYIV_LENGTH);
	memset(fr_out.llc_eapol.key_frame.key_rsc, 0, EAPOL_KEYRSC_LENGTH);
	memcpy(fr_out.llc_eapol.key_frame.key_id, fr_in->key_frame.key_id, EAPOL_KEYID_LENGTH);
	fr_out.llc_eapol.key_frame.key_data_length[0] = (sizeof(wpa_rsn) & 0xff00) >> 8;
	fr_out.llc_eapol.key_frame.key_data_length[1] = (sizeof(wpa_rsn) & 0x00ff) >> 0;
	memcpy(fr_out.llc_eapol.key_frame.key_data, wpa_rsn, sizeof(wpa_rsn));
	
	/* Compute MIC */
	memset(fr_out.llc_eapol.key_frame.key_mic, 0, EAPOL_KEYMIC_LENGTH);
	hmac_md5(ptk, EAPOL_MICK_LENGTH,
		 (unsigned char *)&fr_out+LLC_LENGTH, sizeof(struct eapol_frame)+sizeof(wpa_rsn)-LLC_LENGTH,
		 fr_out.llc_eapol.key_frame.key_mic);
	
	DBG_WIFI("Response computed\r\n");
	
	/* Send the response */
	rt2501_send((unsigned char *)&fr_out, sizeof(fr_out), ieee80211_assoc_mac, 1, 1);
	
	DBG_WIFI("Response sent\r\n");
	
	/* Install pairwise encryption and MIC keys */
	rt2501_set_key(0, &ptk[32], &ptk[32+16+8], &ptk[32+16], RT2501_CIPHER_TKIP);
	memset(ptk_tsc, 0, EAPOL_TSC_LENGTH);
	
	eapol_state = EAPOL_S_MSG3;
}

static void eapol_input_msg3(unsigned char *frame, unsigned int length)
{
	struct eapol_frame *fr_in = (struct eapol_frame *)frame;
	unsigned char old_mic[EAPOL_KEYMIC_LENGTH];
	struct {
		struct eapol_frame llc_eapol;
	} fr_out;
	
	DBG_WIFI("Received EAPOL message 3/4\r\n");
	
	/* EAPOL_S_GROUP can happen if the AP did not receive our reply */
	if((eapol_state != EAPOL_S_MSG3) && (eapol_state != EAPOL_S_GROUP)) {
		DBG_WIFI("Inappropriate message\r\n");
		return;
	}
	
	if(fr_in->key_frame.key_info.key_type != 1) return;
	
	/* Check ANonce */
	if(memcmp(fr_in->key_frame.key_nonce, anonce, EAPOL_NONCE_LENGTH) != 0) return;
	DBG_WIFI("ANonce OK\r\n");

	/* Check MIC */
	memcpy(old_mic, fr_in->key_frame.key_mic, EAPOL_KEYMIC_LENGTH);
	memset(fr_in->key_frame.key_mic, 0, EAPOL_KEYMIC_LENGTH);
	hmac_md5(ptk, EAPOL_MICK_LENGTH,
		 frame+LLC_LENGTH,
		 ((fr_in->body_length[0] << 8)|fr_in->body_length[1])+4,
		 fr_in->key_frame.key_mic);
	if(memcmp(fr_in->key_frame.key_mic, old_mic, EAPOL_KEYMIC_LENGTH) != 0) return;
	DBG_WIFI("MIC OK\r\n");
	
	eapol_state = EAPOL_S_MSG3;
	
	/* Make response frame */
	memcpy(fr_out.llc_eapol.llc, eapol_llc, LLC_LENGTH);
	fr_out.llc_eapol.protocol_version = EAPOL_VERSION;
	fr_out.llc_eapol.packet_type = EAPOL_TYPE_KEY;
	fr_out.llc_eapol.body_length[0] = (sizeof(struct eapol_key_frame) & 0xff00) >> 8;
	fr_out.llc_eapol.body_length[1] = (sizeof(struct eapol_key_frame) & 0x00ff) >> 0;
	
	fr_out.llc_eapol.key_frame.descriptor_type = EAPOL_DTYPE_WPAKEY;
	fr_out.llc_eapol.key_frame.key_info.reserved = 0;
	fr_out.llc_eapol.key_frame.key_info.key_desc_ver = 1;
	fr_out.llc_eapol.key_frame.key_info.key_type = 1;
	fr_out.llc_eapol.key_frame.key_info.key_index = 0;
	fr_out.llc_eapol.key_frame.key_info.install = 0;
	fr_out.llc_eapol.key_frame.key_info.key_ack = 0;
	fr_out.llc_eapol.key_frame.key_info.key_mic = 1;
	fr_out.llc_eapol.key_frame.key_info.secure = 0;
	fr_out.llc_eapol.key_frame.key_info.error = 0;
	fr_out.llc_eapol.key_frame.key_info.request = 0;
	fr_out.llc_eapol.key_frame.key_info.ekd = 0;
	fr_out.llc_eapol.key_frame.key_length[0] = fr_in->key_frame.key_length[0];
	fr_out.llc_eapol.key_frame.key_length[1] = fr_in->key_frame.key_length[1];
	memcpy(fr_out.llc_eapol.key_frame.replay_counter, replay_counter, EAPOL_RPC_LENGTH);
	memset(fr_out.llc_eapol.key_frame.key_nonce, 0, EAPOL_NONCE_LENGTH);
	memset(fr_out.llc_eapol.key_frame.key_iv, 0, EAPOL_KEYIV_LENGTH);
	memset(fr_out.llc_eapol.key_frame.key_rsc, 0, EAPOL_KEYRSC_LENGTH);
	memset(fr_out.llc_eapol.key_frame.key_id, 0, EAPOL_KEYID_LENGTH);
	fr_out.llc_eapol.key_frame.key_data_length[0] = 0;
	fr_out.llc_eapol.key_frame.key_data_length[1] = 0;
	
	/* Compute MIC */
	memset(fr_out.llc_eapol.key_frame.key_mic, 0, EAPOL_KEYMIC_LENGTH);
	hmac_md5(ptk, EAPOL_MICK_LENGTH,
		 (unsigned char *)&fr_out+LLC_LENGTH, sizeof(struct eapol_frame)-LLC_LENGTH,
		 fr_out.llc_eapol.key_frame.key_mic);
	
	DBG_WIFI("Response computed\r\n");
	
	/* Send the response */
	rt2501_send((unsigned char *)&fr_out, sizeof(fr_out), ieee80211_assoc_mac, 1, 1);
	
	DBG_WIFI("Response sent\r\n");
	
	eapol_state = EAPOL_S_GROUP;
}

static void eapol_input_group_msg1(unsigned char *frame, unsigned int length)
{
	unsigned int i;
	struct eapol_frame *fr_in = (struct eapol_frame *)frame;
	unsigned char old_mic[EAPOL_KEYMIC_LENGTH];
	unsigned char key[32];
	struct rc4_context rc4;
	struct eapol_frame fr_out;
	
	DBG_WIFI("Received GTK message\r\n");
	
	/* EAPOL_S_RUN can happen if the AP did not receive our reply, */
	/* or in case of GTK renewal */
	if((eapol_state != EAPOL_S_GROUP) && (eapol_state != EAPOL_S_RUN)) {
		DBG_WIFI("Inappropriate message\r\n");
		return;
	}
	
	/* Check MIC */
	memcpy(old_mic, fr_in->key_frame.key_mic, EAPOL_KEYMIC_LENGTH);
	memset(fr_in->key_frame.key_mic, 0, EAPOL_KEYMIC_LENGTH);
	hmac_md5(ptk, EAPOL_MICK_LENGTH, frame+LLC_LENGTH,
		 ((fr_in->body_length[0] << 8)|fr_in->body_length[1])+4,
		 fr_in->key_frame.key_mic);
	if(memcmp(fr_in->key_frame.key_mic, old_mic, EAPOL_KEYMIC_LENGTH) != 0) return;
	DBG_WIFI("MIC OK\r\n");
	
	/* Make response frame */
	memcpy(fr_out.llc, eapol_llc, LLC_LENGTH);
	fr_out.protocol_version = EAPOL_VERSION;
	fr_out.packet_type = EAPOL_TYPE_KEY;
	fr_out.body_length[0] = (sizeof(struct eapol_key_frame) & 0xff00) >> 8;
	fr_out.body_length[1] = (sizeof(struct eapol_key_frame) & 0x00ff) >> 0;
	
	fr_out.key_frame.descriptor_type = EAPOL_DTYPE_WPAKEY;
	fr_out.key_frame.key_info.reserved = 0;
	fr_out.key_frame.key_info.key_desc_ver = 1;
	fr_out.key_frame.key_info.key_type = 0;
	fr_out.key_frame.key_info.key_index = fr_in->key_frame.key_info.key_index;
	fr_out.key_frame.key_info.install = 0;
	fr_out.key_frame.key_info.key_ack = 0;
	fr_out.key_frame.key_info.key_mic = 1;
	fr_out.key_frame.key_info.secure = 1;
	fr_out.key_frame.key_info.error = 0;
	fr_out.key_frame.key_info.request = 0;
	fr_out.key_frame.key_info.ekd = 0;
	fr_out.key_frame.key_length[0] = fr_in->key_frame.key_length[0];
	fr_out.key_frame.key_length[1] = fr_in->key_frame.key_length[1];
	memcpy(fr_out.key_frame.replay_counter, replay_counter, EAPOL_RPC_LENGTH);
	memset(fr_out.key_frame.key_nonce, 0, EAPOL_NONCE_LENGTH);
	memset(fr_out.key_frame.key_iv, 0, EAPOL_KEYIV_LENGTH);
	memset(fr_out.key_frame.key_rsc, 0, EAPOL_KEYRSC_LENGTH);
	memset(fr_out.key_frame.key_id, 0, EAPOL_KEYID_LENGTH);
	fr_out.key_frame.key_data_length[0] = 0;
	fr_out.key_frame.key_data_length[1] = 0;
	
	/* Compute MIC */
	memset(fr_out.key_frame.key_mic, 0, EAPOL_KEYMIC_LENGTH);
	hmac_md5(ptk, EAPOL_MICK_LENGTH,
		 (unsigned char *)&fr_out+LLC_LENGTH, sizeof(struct eapol_frame)-LLC_LENGTH,
		 fr_out.key_frame.key_mic);
	
	DBG_WIFI("Response computed\r\n");
	
	/* Send the response */
	rt2501_send((unsigned char *)&fr_out, sizeof(struct eapol_frame), ieee80211_assoc_mac, 1, 1);
	
	DBG_WIFI("Response sent\r\n");
	
	/* Decipher and install GTK */
	memcpy(&key[0], fr_in->key_frame.key_iv, EAPOL_KEYIV_LENGTH);
	memcpy(&key[EAPOL_KEYIV_LENGTH], &ptk[EAPOL_MICK_LENGTH], 16);
	rc4_init(&rc4, key, 32);
	for(i=0;i<256;i++) rc4_byte(&rc4); /* discard first 256 bytes */
	rc4_cipher(&rc4, gtk, fr_in->key_frame.key_data, EAPOL_MICK_LENGTH+EAPOL_EK_LENGTH);
#ifdef DEBUG_WIFI
	DBG_WIFI("GTK is ");
	for(i=0;i<EAPOL_MICK_LENGTH+EAPOL_EK_LENGTH;i++) {
		sprintf(dbg_buffer, "%02x", gtk[i]);
		DBG_WIFI(dbg_buffer);
	}
	DBG_WIFI("\r\n");
#endif
	rt2501_set_key(fr_in->key_frame.key_info.key_index, &gtk[0], &gtk[16+8], &gtk[16], RT2501_CIPHER_TKIP);
	
	eapol_state = EAPOL_S_RUN;
	ieee80211_state = IEEE80211_S_RUN;
	ieee80211_timeout = IEEE80211_RUN_TIMEOUT;
}

void eapol_input(unsigned char *frame, unsigned int length)
{
	struct eapol_frame *fr = (struct eapol_frame *)frame;

#ifdef DEBUG_WIFI
	sprintf(dbg_buffer, "Received EAPOL frame, key info 0x%02hhx%02hhx\r\n",
		*(((unsigned char *)&fr->key_frame.key_info)+0),
		*(((unsigned char *)&fr->key_frame.key_info)+1));
	DBG_WIFI(dbg_buffer);
#endif
	
	/* drop EAPOL frames when WPA is disabled */
	if(ieee80211_encryption != IEEE80211_CRYPT_WPA) return;
	
	if(length < sizeof(struct eapol_frame)) return;
	
	if(fr->protocol_version != EAPOL_VERSION) return;
	if(fr->packet_type != EAPOL_TYPE_KEY) return;
	if(fr->key_frame.descriptor_type != EAPOL_DTYPE_WPAKEY) return;
	
	/* Validate replay counter */
	if(replay_active) {
		if(memcmp(fr->key_frame.replay_counter, replay_counter, EAPOL_RPC_LENGTH) <= 0) {
			DBG_WIFI("Replay counter ERR\r\n");
			return;
		}
	}
	/* Update local replay counter */
	memcpy(replay_counter, fr->key_frame.replay_counter, EAPOL_RPC_LENGTH);
	replay_active = 1;
	DBG_WIFI("Replay counter OK\r\n");
	
#ifdef DEBUG_WIFI
	sprintf(dbg_buffer, "KeyInfo Key Description Version %d\r\n", fr->key_frame.key_info.key_desc_ver);
	DBG_WIFI(dbg_buffer);
	sprintf(dbg_buffer, "KeyInfo Key Type %d\r\n", fr->key_frame.key_info.key_type);
	DBG_WIFI(dbg_buffer);
	sprintf(dbg_buffer, "KeyInfo Key Index %d\r\n", fr->key_frame.key_info.key_index);
	DBG_WIFI(dbg_buffer);
	sprintf(dbg_buffer, "KeyInfo Install %d\r\n", fr->key_frame.key_info.install);
	DBG_WIFI(dbg_buffer);
	sprintf(dbg_buffer, "KeyInfo Key Ack %d\r\n", fr->key_frame.key_info.key_ack);
	DBG_WIFI(dbg_buffer);
	sprintf(dbg_buffer, "KeyInfo Key MIC %d\r\n", fr->key_frame.key_info.key_mic);
	DBG_WIFI(dbg_buffer);
	sprintf(dbg_buffer, "KeyInfo Secure %d\r\n", fr->key_frame.key_info.secure);
	DBG_WIFI(dbg_buffer);
	sprintf(dbg_buffer, "KeyInfo Error %d\r\n", fr->key_frame.key_info.error);
	DBG_WIFI(dbg_buffer);
	sprintf(dbg_buffer, "KeyInfo Request %d\r\n", fr->key_frame.key_info.request);
	DBG_WIFI(dbg_buffer);
	sprintf(dbg_buffer, "KeyInfo EKD_DL %d\r\n", fr->key_frame.key_info.ekd);
	DBG_WIFI(dbg_buffer);
#endif
	
	if((fr->key_frame.key_info.key_type == 1) &&
		   (fr->key_frame.key_info.key_index == 0) &&
		   (fr->key_frame.key_info.key_ack == 1) &&
		   (fr->key_frame.key_info.key_mic == 0) &&
		   (fr->key_frame.key_info.secure == 0) &&
		   (fr->key_frame.key_info.error == 0) &&
		   (fr->key_frame.key_info.request == 0))
		eapol_input_msg1(frame, length);
	else if((fr->key_frame.key_info.key_type == 1) &&
			(fr->key_frame.key_info.key_index == 0) &&
			(fr->key_frame.key_info.key_ack == 1) &&
			(fr->key_frame.key_info.key_mic == 1) &&
			(fr->key_frame.key_info.secure == 0) &&
			(fr->key_frame.key_info.error == 0) &&
			(fr->key_frame.key_info.request == 0))
		eapol_input_msg3(frame, length);
	else if((fr->key_frame.key_info.key_type == 0) &&
			(fr->key_frame.key_info.key_index != 0) &&
			(fr->key_frame.key_info.key_ack == 1) &&
			(fr->key_frame.key_info.key_mic == 1) &&
			(fr->key_frame.key_info.secure == 1) &&
			(fr->key_frame.key_info.error == 0) &&
			(fr->key_frame.key_info.request == 0))
		eapol_input_group_msg1(frame, length);
}
