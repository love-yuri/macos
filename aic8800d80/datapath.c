/* SPDX-License-Identifier: GPL-2.0-only */
#include "datapath.h"
#include "protocol.h"

size_t aic_tx_encode(uint8_t *out, size_t cap, const uint8_t *eth, size_t n,
                     uint8_t vif, uint8_t sta, uint32_t token)
{
    /* hostdesc (28 bytes), CONFIG_RWNX_FULLMAC, MUMIMO off. Linux removes
     * Ethernet's header and places its addresses/type in the descriptor. */
    if (n < 14 || n > 1514) return 0;
    size_t len = (4 + 28 + n - 14 + 3) & ~(size_t)3;
    size_t transfer = len + (len % 512 == 0);
    if (transfer > cap || len > 4095) return 0;
    memset(out, 0, transfer);
    aic_put16(out, (uint16_t)len); out[2] = 1;
    aic_put16(out+4, (uint16_t)(n-14));
    aic_put32(out+8, token);
    memcpy(out+12, eth, 14);
    out[26] = 1; /* AC_BE in lmac_mac.h */
    out[27] = 0xff; /* non-QoS */
    out[28] = vif; out[29] = sta;
    memcpy(out+32, eth+14, n-14);
    return transfer;
}

int aic_rx_decode(const uint8_t *p, size_t n, uint8_t vif,
                   aic_ethernet_rx receive, void *context)
{
    while (n) {
        if (n < 4) return -1;
        size_t length = aic_get16(p);
        if (!length) { /* USB zero packet / trailing alignment */
            for (size_t i = 0; i < n; i++) if (p[i]) return -1;
            return 0;
        }
        if (p[2] & 0x10) {
            size_t total = 4 + ((length+3) & ~(size_t)3);
            if (total > n) return -1;
            p += total; n -= total; continue;
        }
        /* hw_rxhdr = 56 bytes, plus four bytes of firmware padding. Its
         * first four bytes also serve as the data USB header. See
         * RX_HWHRD_LEN and rwnx_rxdataind_aicwf(). */
        size_t total = length + 60;
        if (total > n || length < 24) return -1;
        uint32_t flags = aic_get32(p+48), status = aic_get32(p+36);
        const uint8_t *f = p+60;
        if (!(flags & (1<<6)) || ((flags>>8)&255) != vif || (flags & (1<<1)) ||
            (f[0]&0x0c) != 8 || (f[1]&3) != 2) goto next;
        size_t header = 24;
        if (f[0]&0x80) {
            if (length < 26) return -1;
            if (f[24]&0x80) goto next; /* A-MSDU not negotiated by this profile */
            header += 2;
        }
        if (f[1]&0x80) header += 4;
        if ((f[1]&4) || (f[22]&15)) goto next; /* fragment: not a complete MSDU */
        unsigned decryption = (status>>2)&7;
        if (f[1]&0x40) {
            /* Accept encrypted data only after firmware authenticated it. */
            if (decryption != 3) goto next;
            header += 8;
        }
        if (length < header+8 || memcmp(f+header, "\xaa\xaa\x03\x00\x00\x00", 6)) goto next;
        uint8_t eth[4096];
        size_t payload = length-header-8;
        if (payload+14 > sizeof(eth)) return -1;
        memcpy(eth, f+4, 6); memcpy(eth+6, f+16, 6);
        memcpy(eth+12, f+header+6, 2);
        memcpy(eth+14, f+header+8, payload);
        /* Firmware can include cipher trailer / alignment in its RX length.
         * Protocol lengths remove it before handing data to the stack. */
        size_t used = payload+14;
        if (eth[12]==8 && eth[13]==0 && payload>=20) {
            size_t iplen = ((size_t)eth[16]<<8)|eth[17];
            if (iplen<20 || iplen>payload) goto next;
            used=14+iplen;
        } else if (eth[12]==0x88 && eth[13]==0x8e && payload>=4) {
            size_t eaplen = 4+((size_t)eth[16]<<8)+eth[17];
            if (eaplen>payload) goto next;
            used=14+eaplen;
        } else if (eth[12]==8 && eth[13]==6) {
            if (payload<28) goto next;
            used=42;
        }
        receive(context, eth, used, !!(f[1]&0x40));
next:
        p += total; n -= total;
    }
    return 0;
}

int aic_rx_feed(struct aic_rx_stream *s, const uint8_t *p, size_t n, uint8_t vif,
                 aic_ethernet_rx receive, void *context)
{
    if(n>sizeof(s->bytes)-s->used) { s->used=0; return -1; }
    memcpy(s->bytes+s->used,p,n); s->used+=n;
    size_t offset=0;
    while(s->used-offset>=4) {
        const uint8_t *frame=s->bytes+offset;
        size_t len=aic_get16(frame), total;
        if(!len) {
            for(size_t i=offset;i<s->used;i++) if(s->bytes[i]) { s->used=0; return -1; }
            s->used=0; return 0;
        }
        /* This profile negotiates neither A-MSDU nor jumbo MSDUs. Reject a
         * desynchronized length immediately instead of buffering unrelated
         * transfers behind it and starving EAPOL during reconnection. */
        if(len>4096) { s->used=0; return -1; }
        total=(frame[2]&0x10)?4+((len+3)&~(size_t)3):60+len;
        if(total>s->used-offset) break;
        if(aic_rx_decode(frame,total,vif,receive,context)) { s->used=0; return -1; }
        offset+=total;
    }
    s->used-=offset;
    if(s->used && offset) memmove(s->bytes,s->bytes+offset,s->used);
    return 0;
}
