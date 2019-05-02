// Copyright 2019 SoloKeys Developers
//
// Licensed under the Apache License, Version 2.0, <LICENSE-APACHE or
// http://apache.org/licenses/LICENSE-2.0> or the MIT license <LICENSE-MIT or
// http://opensource.org/licenses/MIT>, at your option. This file may not be
// copied, modified, or distributed except according to those terms.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oku2f.h"
#include "WProgram.h"
#include "arduino.h"
#include "device.h"
#include "ctaphid.h"
#include "ctap.h"
#include "u2f.h"
#include "Time.h"
#include "util.h"
#include "log.h"
#include "extensions.h"

// move custom SHA512 command out,
// and the following headers too
//#include "sha2.h"
#include "crypto.h"

//#include APP_CONFIG

typedef enum
{
    IDLE = 0,
    HANDLING_REQUEST,
} CTAP_STATE;

typedef enum
{
    EMPTY = 0,
    BUFFERING,
    BUFFERED,
    HID_ERROR,
    HID_IGNORE,
} CTAP_BUFFER_STATE;


typedef struct
{
    uint8_t cmd;
    uint32_t cid;
    uint16_t bcnt;
    int offset;
    int bytes_written;
    uint8_t seq;
    uint8_t buf[HID_MESSAGE_SIZE];
} CTAPHID_WRITE_BUFFER;

struct CID
{
    uint32_t cid;
    uint64_t last_used;
    uint8_t busy;
    uint8_t last_cmd;
};


#define SUCESS          0
#define SEQUENCE_ERROR  1

static int state;
static struct CID CIDS[10];
#define CID_MAX (sizeof(CIDS)/sizeof(struct CID))

static uint64_t active_cid_timestamp;

extern uint8_t ctap_buffer[CTAPHID_BUFFER_SIZE];
static uint32_t ctap_buffer_cid;
static int ctap_buffer_cmd;
static uint16_t ctap_buffer_bcnt;
static int ctap_buffer_offset;
static int ctap_packet_seq;

static void buffer_reset();

#define CTAPHID_WRITE_INIT      0x01
#define CTAPHID_WRITE_FLUSH     0x02
#define CTAPHID_WRITE_RESET     0x04

#define     ctaphid_write_buffer_init(x)    memset(x,0,sizeof(CTAPHID_WRITE_BUFFER))
static void ctaphid_write(CTAPHID_WRITE_BUFFER * wb, void * _data, int len);

void ctaphid_init()
{
    state = IDLE;
    buffer_reset();
    //ctap_reset_state();
}

static uint32_t get_new_cid()
{
    static uint32_t cid = 1;
    do
    {
        cid++;
    }while(cid == 0 || cid == 0xffffffff);
    return cid;
}

static int8_t add_cid(uint32_t cid)
{
    uint32_t i;
    for(i = 0; i < CID_MAX-1; i++)
    {
        if (!CIDS[i].busy)
        {
            CIDS[i].cid = cid;
            CIDS[i].busy = 1;
            CIDS[i].last_used = millis();
            return 0;
        }
    }
    return -1;
}

static int8_t cid_exists(uint32_t cid)
{
    uint32_t i;
    for(i = 0; i < CID_MAX-1; i++)
    {
        if (CIDS[i].cid == cid)
        {
            return 1;
        }
    }
    return 0;
}

static int8_t cid_refresh(uint32_t cid)
{
    uint32_t i;
    for(i = 0; i < CID_MAX-1; i++)
    {
        if (CIDS[i].cid == cid)
        {
            CIDS[i].last_used = millis();
            CIDS[i].busy = 1;
            return 0;
        }
    }
    return -1;
}

static int8_t cid_del(uint32_t cid)
{
    uint32_t i;
    for(i = 0; i < CID_MAX-1; i++)
    {
        if (CIDS[i].cid == cid)
        {
            CIDS[i].busy = 0;
            return 0;
        }
    }
    return -1;
}

static int is_broadcast(CTAPHID_PACKET * pkt)
{
    return (pkt->cid == CTAPHID_BROADCAST_CID);
}

static int is_init_pkt(CTAPHID_PACKET * pkt)
{
    return (pkt->pkt.init.cmd == CTAPHID_INIT);
}

static int is_cont_pkt(CTAPHID_PACKET * pkt)
{
    return !(pkt->pkt.init.cmd & TYPE_INIT);
}


static int buffer_packet(CTAPHID_PACKET * pkt)
{
    if (pkt->pkt.init.cmd & TYPE_INIT)
    {
        ctap_buffer_bcnt = ctaphid_packet_len(pkt);
        int pkt_len = (ctap_buffer_bcnt < CTAPHID_INIT_PAYLOAD_SIZE) ? ctap_buffer_bcnt : CTAPHID_INIT_PAYLOAD_SIZE;
        ctap_buffer_cmd = pkt->pkt.init.cmd;
        ctap_buffer_cid = pkt->cid;
        ctap_buffer_offset = pkt_len;
        ctap_packet_seq = -1;
        memmove(ctap_buffer, pkt->pkt.init.payload, pkt_len);
    }
    else
    {
        int leftover = ctap_buffer_bcnt - ctap_buffer_offset;
        int diff = leftover - CTAPHID_CONT_PAYLOAD_SIZE;
        ctap_packet_seq++;
        if (ctap_packet_seq != pkt->pkt.cont.seq)
        {
            return SEQUENCE_ERROR;
        }

        if (diff <= 0)
        {
            // only move the leftover amount
            memmove(ctap_buffer + ctap_buffer_offset, pkt->pkt.cont.payload, leftover);
            ctap_buffer_offset += leftover;
        }
        else
        {
            memmove(ctap_buffer + ctap_buffer_offset, pkt->pkt.cont.payload, CTAPHID_CONT_PAYLOAD_SIZE);
            ctap_buffer_offset += CTAPHID_CONT_PAYLOAD_SIZE;
        }
    }
    return SUCESS;
}

static void buffer_reset()
{
    ctap_buffer_bcnt = 0;
    ctap_buffer_offset = 0;
    ctap_packet_seq = 0;
    ctap_buffer_cid = 0;
}

static int buffer_status()
{
    if (ctap_buffer_bcnt == 0)
    {
        return EMPTY;
    }
    else if (ctap_buffer_offset == ctap_buffer_bcnt)
    {
		Serial.println("BUFFERED");
        return BUFFERED;
    }
    else
    {
        return BUFFERING;
    }
}

static int buffer_cmd()
{
    return ctap_buffer_cmd;
}

static uint32_t buffer_cid()
{
    return ctap_buffer_cid;
}


static int buffer_len()
{
    return ctap_buffer_bcnt;
}

// Buffer data and send in HID_MESSAGE_SIZE chunks
// if len == 0, FLUSH
static void ctaphid_write(CTAPHID_WRITE_BUFFER * wb, void * _data, int len)
{
    uint8_t * data = (uint8_t *)_data;
    if (_data == NULL)
    {
        if (wb->offset == 0 && wb->bytes_written == 0)
        {
            memmove(wb->buf, &wb->cid, 4);
            wb->offset += 4;

            wb->buf[4] = wb->cmd;
            wb->buf[5] = (wb->bcnt & 0xff00) >> 8;
            wb->buf[6] = (wb->bcnt & 0xff) >> 0;
            wb->offset += 3;
        }

        if (wb->offset > 0)
        {
            memset(wb->buf + wb->offset, 0, HID_MESSAGE_SIZE - wb->offset);
            ctaphid_write_block(wb->buf);
        }
        return;
    }
    int i;
    for (i = 0; i < len; i++)
    {
        if (wb->offset == 0 )
        {
            memmove(wb->buf, &wb->cid, 4);
            wb->offset += 4;

            if (wb->bytes_written == 0)
            {
                wb->buf[4] = wb->cmd;
                wb->buf[5] = (wb->bcnt & 0xff00) >> 8;
                wb->buf[6] = (wb->bcnt & 0xff) >> 0;
                wb->offset += 3;
            }
            else
            {
                wb->buf[4] = wb->seq++;
                wb->offset += 1;
            }
        }
        wb->buf[wb->offset++] = data[i];
        wb->bytes_written += 1;
        if (wb->offset == HID_MESSAGE_SIZE)
        {
            ctaphid_write_block(wb->buf);
            wb->offset = 0;
        }
    }
}


static void ctaphid_send_error(uint32_t cid, uint8_t error)
{
    CTAPHID_WRITE_BUFFER wb;
    ctaphid_write_buffer_init(&wb);

    wb.cid = cid;
    wb.cmd = CTAPHID_ERROR;
    wb.bcnt = 1;

    ctaphid_write(&wb, &error, 1);
    ctaphid_write(&wb, NULL, 0);
}

static void send_init_response(uint32_t oldcid, uint32_t newcid, uint8_t * nonce)
{
    CTAPHID_INIT_RESPONSE init_resp;
    CTAPHID_WRITE_BUFFER wb;
    ctaphid_write_buffer_init(&wb);
    wb.cid = oldcid;
    wb.cmd = CTAPHID_INIT;
    wb.bcnt = 17;

    memmove(init_resp.nonce, nonce, 8);
    init_resp.cid = newcid;
    init_resp.protocol_version = CTAPHID_PROTOCOL_VERSION;
    init_resp.version_major = 0;//?
    init_resp.version_minor = 0;//?
    init_resp.build_version = 0;//?
    init_resp.capabilities = CTAP_CAPABILITIES;

    ctaphid_write(&wb,&init_resp,sizeof(CTAPHID_INIT_RESPONSE));
    ctaphid_write(&wb,NULL,0);
}


void ctaphid_check_timeouts()
{
    uint8_t i;
    for(i = 0; i < CID_MAX; i++)
    {
        if (CIDS[i].busy && ((millis() - CIDS[i].last_used) >= 750))
        {
            Serial.println("TIMEOUT CID: ");
			Serial.println(CIDS[i].cid);
            ctaphid_send_error(CIDS[i].cid, CTAP1_ERR_TIMEOUT);
            CIDS[i].busy = 0;
            if (CIDS[i].cid == buffer_cid())
            {
                buffer_reset();
            }
            // memset(CIDS + i, 0, sizeof(struct CID));
        }
    }

}

void ctaphid_update_status(int8_t status)
{
    CTAPHID_WRITE_BUFFER wb;
    Serial.println("Send device update");
	Serial.println(status);
    ctaphid_write_buffer_init(&wb);

    wb.cid = buffer_cid();
    wb.cmd = CTAPHID_KEEPALIVE;
    wb.bcnt = 1;

    ctaphid_write(&wb, &status, 1);
    ctaphid_write(&wb, NULL, 0);
}

static int ctaphid_buffer_packet(uint8_t * pkt_raw, uint8_t * cmd, uint32_t * cid, int * len)
{
    CTAPHID_PACKET * pkt = (CTAPHID_PACKET *)(pkt_raw);

    Serial.println("Recv packet");

    Serial.println( pkt->cid);
    Serial.println(pkt->pkt.init.cmd);
    if (!is_cont_pkt(pkt)) {
		Serial.println(ctaphid_packet_len(pkt));
	}

    int ret;
    uint32_t oldcid;
    uint32_t newcid;


    *cid = pkt->cid;

    if (is_init_pkt(pkt))
    {
        if (ctaphid_packet_len(pkt) != 8)
        {
            Serial.println( "Error,invalid length field for init packet");
            *cmd = CTAP1_ERR_INVALID_LENGTH;
            return HID_ERROR;
        }
        if (pkt->cid == 0)
        {
            Serial.println("Error, invalid cid 0");
            *cmd = CTAP1_ERR_INVALID_CHANNEL;
            return HID_ERROR;
        }

        ctaphid_init();
        if (is_broadcast(pkt))
        {
            // Check if any existing cids are busy first ?
            Serial.println("adding a new cid\n");
            oldcid = CTAPHID_BROADCAST_CID;
            newcid = get_new_cid();
            ret = add_cid(newcid);
            // handle init here
        }
        else
        {
            Serial.println("synchronizing to cid");
            oldcid = pkt->cid;
            newcid = pkt->cid;
            if (cid_exists(newcid))
                ret = cid_refresh(newcid);
            else
                ret = add_cid(newcid);
        }
        if (ret == -1)
        {
            Serial.println( "Error, not enough memory for new CID.  return BUSY.");
            *cmd = CTAP1_ERR_CHANNEL_BUSY;
            return HID_ERROR;
        }
        send_init_response(oldcid, newcid, pkt->pkt.init.payload);
        cid_del(newcid);

        return HID_IGNORE;
    }
    else
    {
        if (pkt->cid == CTAPHID_BROADCAST_CID)
        {
            *cmd = CTAP1_ERR_INVALID_CHANNEL;
            return HID_ERROR;
        }

        if (! cid_exists(pkt->cid) && ! is_cont_pkt(pkt))
        {
            if (buffer_status() == EMPTY)
            {
                add_cid(pkt->cid);
            }
        }

        if (cid_exists(pkt->cid))
        {
            if (buffer_status() == BUFFERING)
            {
                if (pkt->cid == buffer_cid() && ! is_cont_pkt(pkt))
                {
                    Serial.println("INVALID_SEQ");
                    Serial.println(ctap_buffer_offset);
					Serial.println(ctap_buffer_bcnt);
                    *cmd = CTAP1_ERR_INVALID_SEQ;
                    return HID_ERROR;
                }
                else if (pkt->cid != buffer_cid())
                {
                    if (! is_cont_pkt(pkt))
                    {
                        Serial.println("Channel busy");
						Serial.println(buffer_cid());
                        *cmd = CTAP1_ERR_CHANNEL_BUSY;
                        return HID_ERROR;
                    }
                    else
                    {
                        Serial.println("ignoring random cont packet from ");
						Serial.println(pkt->cid);
                        return HID_IGNORE;
                    }
                }
            }
            if (! is_cont_pkt(pkt))
            {

                if (ctaphid_packet_len(pkt) > CTAPHID_BUFFER_SIZE)
                {
                    Serial.println("Invalid Length");
					*cmd = CTAP1_ERR_INVALID_LENGTH;
                    return HID_ERROR;
                }
            }
            else
            {
                if (buffer_status() == EMPTY || pkt->cid != buffer_cid())
                {
                    Serial.println("ignoring random cont packet from ");
					Serial.println(pkt->cid);
                    return HID_IGNORE;
                }
            }

            if (buffer_packet(pkt) == SEQUENCE_ERROR)
            {
                Serial.println("Buffering sequence error");
                *cmd = CTAP1_ERR_INVALID_SEQ;
                return HID_ERROR;
            }
            ret = cid_refresh(pkt->cid);
            if (ret != 0)
            {
                Serial.println("Error, refresh cid failed");
                exit(1);
            }
        }
        else if (is_cont_pkt(pkt))
        {
            Serial.println("ignoring unwarranted cont packet");

            // Ignore
            return HID_IGNORE;
        }
        else
        {
            Serial.println("BUSY");
            *cmd = CTAP1_ERR_CHANNEL_BUSY;
            return HID_ERROR;
        }
    }

    *len = buffer_len();
	Serial.println("Length/buffercmd");
	Serial.println(*len);
    *cmd = buffer_cmd();
	Serial.println(*cmd);
    return buffer_status();
}

extern void _check_ret(CborError ret, int line, const char * filename);
#define check_hardcore(r)   _check_ret(r,__LINE__, __FILE__);\
                            if ((r) != CborNoError) exit(1);

uint8_t ctaphid_handle_packet(uint8_t * pkt_raw)
{
    uint8_t cmd;
    uint32_t cid;
    int len;
#ifndef DISABLE_CTAPHID_CBOR
    int status;
#endif

    static uint8_t is_busy = 0;
    static CTAPHID_WRITE_BUFFER wb;
    CTAP_RESPONSE ctap_resp;

    int bufstatus = ctaphid_buffer_packet(pkt_raw, &cmd, &cid, &len);

    if (bufstatus == HID_IGNORE)
    {
        Serial.println("HID_IGNORE");
		return 0;
    }

    if (bufstatus == HID_ERROR)
    {
        cid_del(cid);
        if (cmd == CTAP1_ERR_INVALID_SEQ)
        {
            buffer_reset();
        }
		Serial.println("HID_ERROR CID/CMD:");
		Serial.print(cid);
		Serial.print(cmd);
        ctaphid_send_error(cid, cmd);
        return 0;
    }

    if (bufstatus == BUFFERING)
    {
        active_cid_timestamp = millis();
		Serial.println("BUFFERING");
        return 0;
    }

    switch(cmd)
    {

        case CTAPHID_INIT:
            Serial.println("CTAPHID_INIT, error this should already be handled");
            exit(1);
            break;
#ifndef DISABLE_CTAPHID_PING
        case CTAPHID_PING:
            Serial.println("CTAPHID_PING");

            ctaphid_write_buffer_init(&wb);
            wb.cid = cid;
            wb.cmd = CTAPHID_PING;
            wb.bcnt = len;
            timestamp();
            ctaphid_write(&wb, ctap_buffer, len);
            ctaphid_write(&wb, NULL,0);
            Serial.println("PING writeback:");

            break;
#endif
#ifndef DISABLE_CTAPHID_WINK
        case CTAPHID_WINK:
            Serial.println("CTAPHID_WINK\n");

            ctaphid_write_buffer_init(&wb);

            device_wink();

            wb.cid = cid;
            wb.cmd = CTAPHID_WINK;

            ctaphid_write(&wb,NULL,0);

            break;
#endif
#ifndef DISABLE_CTAPHID_CBOR
        case CTAPHID_CBOR:
            Serial.println("CTAPHID_CBOR");

            if (len == 0)
            {
                Serial.println("Error,invalid 0 length field for cbor packet");
                ctaphid_send_error(cid, CTAP1_ERR_INVALID_LENGTH);
                return 0;
            }
            if (is_busy)
            {
                Serial.println("Channel busy for CBOR");
                ctaphid_send_error(cid, CTAP1_ERR_CHANNEL_BUSY);
                return 0;
            }
            is_busy = 1;
            ctap_response_init(&ctap_resp);
            status = ctap_request(ctap_buffer, len, &ctap_resp);

            ctaphid_write_buffer_init(&wb);
            wb.cid = cid;
            wb.cmd = CTAPHID_CBOR;
            wb.bcnt = (ctap_resp.length+1);


            timestamp();
            ctaphid_write(&wb, &status, 1);
            ctaphid_write(&wb, ctap_resp.data, ctap_resp.length);
            ctaphid_write(&wb, NULL, 0);
            Serial.println("CBOR writeback:");
            is_busy = 0;
            break;
#endif
        case CTAPHID_MSG:

            Serial.println("CTAPHID_MSG\n");
            if (len == 0)
            {
                Serial.println("Error,invalid 0 length field for MSG/U2F packet");
                ctaphid_send_error(cid, CTAP1_ERR_INVALID_LENGTH);
                return 0;
            }
            if (is_busy)
            {
                Serial.println("Channel busy for MSG\n");
                ctaphid_send_error(cid, CTAP1_ERR_CHANNEL_BUSY);
                return 0;
            }
            is_busy = 1;
            ctap_response_init(&ctap_resp);
			if (!recv_custom_msg(ctap_buffer+7, pkt_raw)) u2f_request((struct u2f_request_apdu*)ctap_buffer, &ctap_resp);

            ctaphid_write_buffer_init(&wb);
            wb.cid = cid;
            wb.cmd = CTAPHID_MSG;
            wb.bcnt = (ctap_resp.length);

            ctaphid_write(&wb, ctap_resp.data, ctap_resp.length);
            ctaphid_write(&wb, NULL, 0);
            is_busy = 0;
            break;
        case CTAPHID_CANCEL:
            Serial.println("CTAPHID_CANCEL\n");
            is_busy = 0;
            break;
#if defined(IS_BOOTLOADER)
        case CTAPHID_BOOT:
            Serial.println("CTAPHID_BOOT\n");
            ctap_response_init(&ctap_resp);
            u2f_set_writeback_buffer(&ctap_resp);
            is_busy = bootloader_bridge(len, ctap_buffer);

            ctaphid_write_buffer_init(&wb);
            wb.cid = cid;
            wb.cmd = CTAPHID_BOOT;
            wb.bcnt = (ctap_resp.length + 1);
            ctaphid_write(&wb, &is_busy, 1);
            ctaphid_write(&wb, ctap_resp.data, ctap_resp.length);
            ctaphid_write(&wb, NULL, 0);
            is_busy = 0;
        break;
#endif
#if defined(SOLO_HACKER)
        case CTAPHID_ENTERBOOT:
            Serial.println("CTAPHID_ENTERBOOT\n");
            boot_solo_bootloader();
            ctaphid_write_buffer_init(&wb);
            wb.cid = cid;
            wb.cmd = CTAPHID_ENTERBOOT;
            wb.bcnt = 0;
            ctaphid_write(&wb, NULL, 0);
            is_busy = 0;
        break;
        case CTAPHID_ENTERSTBOOT:
            Serial.println("CTAPHID_ENTERBOOT\n");
            boot_st_bootloader();
        break;
#endif
#if !defined(IS_BOOTLOADER)
        case CTAPHID_GETRNG:
            Serial.println("CTAPHID_GETRNG\n");
            ctap_response_init(&ctap_resp);
            ctaphid_write_buffer_init(&wb);
            wb.cid = cid;
            wb.cmd = CTAPHID_GETRNG;
            wb.bcnt = ctap_buffer[0];
            if (!wb.bcnt)
                wb.bcnt = 57;
            memset(ctap_buffer,0,wb.bcnt);
            ctap_generate_rng(ctap_buffer, wb.bcnt);
            ctaphid_write(&wb, &ctap_buffer, wb.bcnt);
            ctaphid_write(&wb, NULL, 0);
            is_busy = 0;
        break;
#endif
#if defined(SOLO_HACKER) && (DEBUG_LEVEL > 0) && (!IS_BOOTLOADER == 1)
        case CTAPHID_PROBE:

            /*
             * Expects CBOR-serialized data of the form
             * {"subcommand": "hash_type", "data": b"the_data"}
             * with hash_type in SHA256, SHA512
             */

            // some random logging
            Serial.println("CTAPHID_PROBE\n");
            // initialise CTAP response object
            ctap_response_init(&ctap_resp);
            // initialise write buffer
            ctaphid_write_buffer_init(&wb);
            wb.cid = cid;
            wb.cmd = CTAPHID_PROBE;

            // prepare parsing (or halt)
            int ret;
            CborParser parser;
            CborValue it, map;
            ret = cbor_parser_init(
                ctap_buffer, (size_t) buffer_len(),
                // strictly speaking, CTAP is not RFC canonical...
                CborValidateCanonicalFormat,
                &parser, &it);
            check_hardcore(ret);

            CborType type = cbor_value_get_type(&it);
            if (type != CborMapType) exit(1);

            ret = cbor_value_enter_container(&it,&map);
            check_hardcore(ret);

            size_t map_length = 0;
            ret = cbor_value_get_map_length(&it, &map_length);
            if (map_length != 2) exit(1);

            // parse subcommand (or halt)
            CborValue val;
            ret = cbor_value_map_find_value(&it, "subcommand", &val);
            check_hardcore(ret);
            if (!cbor_value_is_text_string(&val))
                exit(1);

            int sha_version = 0;
            bool found = false;
            if (!found) {
                ret = cbor_value_text_string_equals(
                        &val, "SHA256", &found);
                check_hardcore(ret);
                if (found)
                    sha_version = 256;
            }
            if (!found) {
                ret = cbor_value_text_string_equals(
                        &val, "SHA512", &found);
                check_hardcore(ret);
                if (found)
                    sha_version = 512;
            }
            if (sha_version == 0)
                exit(1);

            // parse data (or halt)
            ret = cbor_value_map_find_value(&it, "data", &val);
            check_hardcore(ret);
            if (!cbor_value_is_byte_string(&val))
                exit(1);

            size_t data_length = 0;
            ret = cbor_value_calculate_string_length(&val, &data_length);
            check_hardcore(ret);
            if (data_length > 6*1024)
                exit(1);

            unsigned char data[6*1024];
            ret = cbor_value_copy_byte_string (
                    &val, &data[0], &data_length, &val);
            check_hardcore(ret);

            // execute subcommand
            if (sha_version == 256) {
                // calculate hash
                crypto_sha256_init();
                crypto_sha256_update(data, data_length);
                crypto_sha256_final(ctap_buffer);
                // write output
                wb.bcnt = CF_SHA256_HASHSZ;  // 32 bytes
                ctaphid_write(&wb, &ctap_buffer, CF_SHA256_HASHSZ);
            }

            if (sha_version == 512) {
                // calculate hash
                crypto_sha512_init();
                crypto_sha512_update(data, data_length);
                crypto_sha512_final(ctap_buffer);
                // write output
                wb.bcnt = CF_SHA512_HASHSZ;  // 64 bytes
                ctaphid_write(&wb, &ctap_buffer, CF_SHA512_HASHSZ);
            }

            // finalize
            ctaphid_write(&wb, NULL, 0);
            is_busy = 0;
        break;

        /*
        case CTAPHID_SHA256:
            // some random logging
            Serial.println("CTAPHID_SHA256\n");
            // initialise CTAP response object
            ctap_response_init(&ctap_resp);
            // initialise write buffer
            ctaphid_write_buffer_init(&wb);
            wb.cid = cid;
            wb.cmd = CTAPHID_SHA256;
            wb.bcnt = CF_SHA256_HASHSZ;  // 32 bytes
            // calculate hash
            crypto_sha256_init();
            crypto_sha256_update(ctap_buffer, buffer_len());
            crypto_sha256_final(ctap_buffer);
            // copy to output
            ctaphid_write(&wb, &ctap_buffer, CF_SHA256_HASHSZ);
            ctaphid_write(&wb, NULL, 0);
            is_busy = 0;
        break;
        case CTAPHID_SHA512:
            // some random logging
            Serial.println("CTAPHID_SHA512\n");
            // initialise CTAP response object
            ctap_response_init(&ctap_resp);
            // initialise write buffer
            ctaphid_write_buffer_init(&wb);
            wb.cid = cid;
            wb.cmd = CTAPHID_SHA512;
            wb.bcnt = CF_SHA512_HASHSZ;  // 64 bytes
            // calculate hash
            crypto_sha512_init();
            crypto_sha512_update(ctap_buffer, buffer_len());
            crypto_sha512_final(ctap_buffer);
            // copy to output
            ctaphid_write(&wb, &ctap_buffer, CF_SHA512_HASHSZ);
            ctaphid_write(&wb, NULL, 0);
            is_busy = 0;
        break;
        */
#endif
        default:
            Serial.println("error, unimplemented HID cmd: ");
			Serial.println(buffer_cmd());
            ctaphid_send_error(cid, CTAP1_ERR_INVALID_COMMAND);
            break;
    }
    cid_del(cid);
    buffer_reset();

    Serial.println("n");
    if (!is_busy) return cmd;
    else return 0;

}
