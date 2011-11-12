/* Copyright (C) 2010 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Endace Technology Limited.
 *
 * Support for reading ERF files.
 *
 * Only ethernet supported at this time.
 */

#include "suricata-common.h"
#include "suricata.h"
#include "tm-threads.h"

#define DAG_TYPE_ETH 2

typedef struct DagFlags_ {
    uint8_t iface:2;
    uint8_t vlen:1;
    uint8_t trunc:1;
    uint8_t rxerror:1;
    uint8_t dserror:1;
    uint8_t reserved:1;
    uint8_t direction:1;
} DagFlags;

typedef struct DagRecord_ {
    uint64_t ts;
    uint8_t type;
    DagFlags flags;
    uint16_t rlen;
    uint16_t lctr;
    uint16_t wlen;
    uint16_t pad;
} __attribute__((packed)) DagRecord;

typedef struct ErfFileThreadVars_ {
    FILE *erf;
    ThreadVars *tv;

    uint32_t pkts;
    uint64_t bytes;
} ErfFileThreadVars;

TmEcode ReceiveErfFile(ThreadVars *, Packet *, void *, PacketQueue *, PacketQueue *);
TmEcode ReceiveErfFileThreadInit(ThreadVars *, void *, void **);
void ReceiveErfFileThreadExitStats(ThreadVars *, void *);
TmEcode ReceiveErfFileThreadDeinit(ThreadVars *, void *);

TmEcode DecodeErfFileThreadInit(ThreadVars *, void *, void **);
TmEcode DecodeErfFile(ThreadVars *, Packet *, void *, PacketQueue *, PacketQueue *);

/**
 * \brief Register the ERF file receiver (reader) module.
 */
void
TmModuleReceiveErfFileRegister(void)
{
    tmm_modules[TMM_RECEIVEERFFILE].name = "ReceiveErfFile";
    tmm_modules[TMM_RECEIVEERFFILE].ThreadInit = ReceiveErfFileThreadInit;
    tmm_modules[TMM_RECEIVEERFFILE].Func = ReceiveErfFile;
    tmm_modules[TMM_RECEIVEERFFILE].ThreadExitPrintStats =
        ReceiveErfFileThreadExitStats;
    tmm_modules[TMM_RECEIVEERFFILE].ThreadDeinit = NULL;
    tmm_modules[TMM_RECEIVEERFFILE].RegisterTests = NULL;
    tmm_modules[TMM_RECEIVEERFFILE].cap_flags = 0;
    tmm_modules[TMM_RECEIVEERFFILE].flags = TM_FLAG_RECEIVE_TM;
}

/**
 * \brief Register the ERF file decoder module.
 */
void
TmModuleDecodeErfFileRegister(void)
{
    tmm_modules[TMM_DECODEERFFILE].name = "DecodeErfFile";
    tmm_modules[TMM_DECODEERFFILE].ThreadInit = DecodeErfFileThreadInit;
    tmm_modules[TMM_DECODEERFFILE].Func = DecodeErfFile;
    tmm_modules[TMM_DECODEERFFILE].ThreadExitPrintStats = NULL;
    tmm_modules[TMM_DECODEERFFILE].ThreadDeinit = NULL;
    tmm_modules[TMM_DECODEERFFILE].RegisterTests = NULL;
    tmm_modules[TMM_DECODEERFFILE].cap_flags = 0;
}

/**
 * \brief Thread entry function for ERF reading.
 *
 * Reads a new ERF record from the file and sets up the Packet for
 * decoding.
 */
TmEcode
ReceiveErfFile(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    SCEnter();

    ErfFileThreadVars *etv = (ErfFileThreadVars *)data;
    DagRecord dr;

    int r = fread(&dr, sizeof(DagRecord), 1, etv->erf);
    if (r < 1) {
        SCLogInfo("End of ERF file reached or an error occurred.");
        EngineStop();
        SCReturnInt(TM_ECODE_FAILED);
    }
    int rlen = ntohs(dr.rlen);
    int wlen = ntohs(dr.wlen);
    r = fread(GET_PKT_DATA(p), rlen - sizeof(DagRecord), 1, etv->erf);
    if (r < 1) {
        SCLogInfo("End of ERF file reached or an error occurred.");
        EngineStop();
        SCReturnInt(TM_ECODE_FAILED);
    }

    /* Only support ethernet at this time. */
    if (dr.type != DAG_TYPE_ETH) {
        SCLogError(SC_ERR_UNIMPLEMENTED,
            "DAG record type %d not implemented.", dr.type);
        SCReturnInt(TM_ECODE_FAILED);
    }

    GET_PKT_LEN(p) = wlen - 4; /* Trim the FCS... */
    p->datalink = LINKTYPE_ETHERNET;

    /* Convert ERF time to timeval - from libpcap. */
    uint64_t ts = dr.ts;
    p->ts.tv_sec = ts >> 32;
    ts = (ts & 0xffffffffULL) * 1000000;
    ts += 0x80000000; /* rounding */
    p->ts.tv_usec = ts >> 32;
    if (p->ts.tv_usec >= 1000000) {
        p->ts.tv_usec -= 1000000;
        p->ts.tv_sec++;
    }

    etv->pkts++;
    etv->bytes += wlen;

    SCReturnInt(TM_ECODE_OK);
}

/**
 * \brief Initialize the ERF receiver thread.
 */
TmEcode
ReceiveErfFileThreadInit(ThreadVars *tv, void *initdata, void **data)
{
    SCEnter();

    if (initdata == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "Error: No filename provided.");
        SCReturnInt(TM_ECODE_FAILED);
    }

    FILE *erf = fopen((const char *)initdata, "r");
    if (erf == NULL) {
        SCLogError(SC_ERR_FOPEN, "Failed to open %s: %s", (char *)initdata,
            strerror(errno));
        exit(EXIT_FAILURE);
    }

    ErfFileThreadVars *etv = SCMalloc(sizeof(ErfFileThreadVars));
    if (etv == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC,
            "Failed to allocate memory for ERF file thread vars.");
        fclose(erf);
        SCReturnInt(TM_ECODE_FAILED);
    }
    memset(etv, 0, sizeof(*etv));
    etv->erf = erf;
    etv->tv = tv;
    *data = (void *)etv;

    SCLogInfo("Processing ERF file %s", (char *)initdata);

    SCReturnInt(TM_ECODE_OK);
}

/**
 * \brief Initialize the ERF decoder thread.
 */
TmEcode
DecodeErfFileThreadInit(ThreadVars *tv, void *initdata, void **data)
{
    SCEnter();
    DecodeThreadVars *dtv = NULL;
    dtv = DecodeThreadVarsAlloc(tv);

    if (dtv == NULL)
        SCReturnInt(TM_ECODE_FAILED);

    DecodeRegisterPerfCounters(dtv, tv);

    *data = (void *)dtv;

    SCReturnInt(TM_ECODE_OK);
}

/**
 * \brief Decode the ERF file.
 *
 * This function ups the decoder counters and then passes the packet
 * off to the ethernet decoder.
 */
TmEcode
DecodeErfFile(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    SCEnter();
    DecodeThreadVars *dtv = (DecodeThreadVars *)data;

    /* Update counters. */
    SCPerfCounterIncr(dtv->counter_pkts, tv->sc_perf_pca);
    SCPerfCounterIncr(dtv->counter_pkts_per_sec, tv->sc_perf_pca);

    SCPerfCounterAddUI64(dtv->counter_bytes, tv->sc_perf_pca, GET_PKT_LEN(p));
#if 0
    SCPerfCounterAddDouble(dtv->counter_bytes_per_sec, tv->sc_perf_pca, GET_PKT_LEN(p));
    SCPerfCounterAddDouble(dtv->counter_mbit_per_sec, tv->sc_perf_pca,
                           (GET_PKT_LEN(p) * 8)/1000000.0 );
#endif

    SCPerfCounterAddUI64(dtv->counter_avg_pkt_size, tv->sc_perf_pca, GET_PKT_LEN(p));
    SCPerfCounterSetUI64(dtv->counter_max_pkt_size, tv->sc_perf_pca, GET_PKT_LEN(p));

    DecodeEthernet(tv, dtv, p, GET_PKT_DATA(p), GET_PKT_LEN(p), pq);

    SCReturnInt(TM_ECODE_OK);
}

/**
 * \brief Print some stats to the log at program exit.
 *
 * \param tv Pointer to ThreadVars.
 * \param data Pointer to data, ErfFileThreadVars.
 */
void
ReceiveErfFileThreadExitStats(ThreadVars *tv, void *data)
{
    ErfFileThreadVars *etv = (ErfFileThreadVars *)data;

    SCLogInfo("Packets: %"PRIu32"; Bytes: %"PRIu64, etv->pkts, etv->bytes);
}
