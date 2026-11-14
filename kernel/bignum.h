/*
 * AxisOS - fixed-width big integer (for RSA + ECDSA).
 * Little-endian array of 32-bit limbs. Host-testable (no kernel deps).
 */
#ifndef AXIS_BIGNUM_H
#define AXIS_BIGNUM_H
#include <stdint.h>
/* includer must provide memset/memcpy (axis.h in-kernel, <string.h> on host) */

#define BN_W 132                 /* 132*32 = 4224 bits, covers RSA-4096 */
typedef struct { uint32_t w[BN_W]; } bn_t;

static inline void bn_zero(bn_t *a){ memset(a->w,0,sizeof a->w); }
static inline void bn_copy(bn_t *d,const bn_t *s){ memcpy(d->w,s->w,sizeof d->w); }
static inline int bn_is_zero(const bn_t *a){ for(int i=0;i<BN_W;i++) if(a->w[i]) return 0; return 1; }

/* load from big-endian bytes */
static void bn_from_be(bn_t *a,const uint8_t *b,uint32_t n){
    bn_zero(a);
    for(uint32_t i=0;i<n;i++){
        uint32_t bit = (n-1-i)*8;
        uint32_t limb = bit/32, sh = bit%32;
        if(limb<BN_W) a->w[limb] |= (uint32_t)b[i] << sh;
    }
}
/* store to big-endian bytes (n bytes, left-padded with zero) */
static void bn_to_be(const bn_t *a,uint8_t *b,uint32_t n){
    for(uint32_t i=0;i<n;i++){
        uint32_t bit = (n-1-i)*8;
        uint32_t limb = bit/32, sh = bit%32;
        b[i] = limb<BN_W ? (uint8_t)(a->w[limb]>>sh) : 0;
    }
}
static int bn_bits(const bn_t *a){
    for(int i=BN_W-1;i>=0;i--) if(a->w[i]){ uint32_t v=a->w[i]; int b=0; while(v){v>>=1;b++;} return i*32+b; }
    return 0;
}
static int bn_cmp(const bn_t *a,const bn_t *b){
    for(int i=BN_W-1;i>=0;i--){ if(a->w[i]<b->w[i]) return -1; if(a->w[i]>b->w[i]) return 1; }
    return 0;
}
static int bn_getbit(const bn_t *a,int i){ return (a->w[i/32]>>(i%32))&1; }
static void bn_shl1(bn_t *a){ uint32_t c=0; for(int i=0;i<BN_W;i++){ uint32_t nc=a->w[i]>>31; a->w[i]=(a->w[i]<<1)|c; c=nc; } }
/* a += b (ignores final carry past BN_W) */
static void bn_add(bn_t *r,const bn_t *a,const bn_t *b){
    uint64_t c=0; for(int i=0;i<BN_W;i++){ uint64_t s=(uint64_t)a->w[i]+b->w[i]+c; r->w[i]=(uint32_t)s; c=s>>32; }
}
/* r = a - b  (assumes a>=b) */
static void bn_sub(bn_t *r,const bn_t *a,const bn_t *b){
    int64_t br=0; for(int i=0;i<BN_W;i++){ int64_t s=(int64_t)a->w[i]-b->w[i]-br; if(s<0){s+=((int64_t)1<<32);br=1;}else br=0; r->w[i]=(uint32_t)s; }
}
/* r = a mod m  (bit-by-bit; a may be full-width) */
static void bn_mod(bn_t *r,const bn_t *a,const bn_t *m){
    bn_t rem; bn_zero(&rem);
    int top=bn_bits(a);
    for(int i=top-1;i>=0;i--){
        bn_shl1(&rem);
        rem.w[0] |= bn_getbit(a,i);
        if(bn_cmp(&rem,m)>=0){ bn_t t; bn_sub(&t,&rem,m); bn_copy(&rem,&t); }
    }
    bn_copy(r,&rem);
}
/* full-width product then reduce: r = (a*b) mod m */
static void bn_mulmod(bn_t *r,const bn_t *a,const bn_t *b,const bn_t *m){
    /* schoolbook into 2*BN_W limbs */
    static uint32_t prod[2*BN_W];
    memset(prod,0,sizeof prod);
    int aw=(bn_bits(a)+31)/32, bw=(bn_bits(b)+31)/32;
    for(int i=0;i<aw;i++){
        uint64_t c=0;
        for(int j=0;j<bw;j++){
            uint64_t s=(uint64_t)a->w[i]*b->w[j]+prod[i+j]+c;
            prod[i+j]=(uint32_t)s; c=s>>32;
        }
        prod[i+bw]+=(uint32_t)c;
    }
    /* reduce prod mod m, bit by bit from the top */
    bn_t rem; bn_zero(&rem);
    int topbit=2*BN_W*32;
    while(topbit>0 && ((prod[(topbit-1)/32]>>((topbit-1)%32))&1)==0) topbit--;
    for(int i=topbit-1;i>=0;i--){
        bn_shl1(&rem);
        rem.w[0] |= (prod[i/32]>>(i%32))&1;
        if(bn_cmp(&rem,m)>=0){ bn_t t; bn_sub(&t,&rem,m); bn_copy(&rem,&t); }
    }
    bn_copy(r,&rem);
}
/* r = base^exp mod m */
static void bn_modexp(bn_t *r,const bn_t *base,const bn_t *exp,const bn_t *m){
    bn_t result,b; bn_zero(&result); result.w[0]=1; bn_copy(&b,base);
    bn_t bm; bn_mod(&bm,&b,m); bn_copy(&b,&bm);
    int bits=bn_bits(exp);
    for(int i=0;i<bits;i++){
        if(bn_getbit(exp,i)){ bn_t t; bn_mulmod(&t,&result,&b,m); bn_copy(&result,&t); }
        bn_t t2; bn_mulmod(&t2,&b,&b,m); bn_copy(&b,&t2);
    }
    bn_copy(r,&result);
}
/* modular inverse via Fermat: a^(m-2) mod m  (m prime) */
static void bn_modinv_prime(bn_t *r,const bn_t *a,const bn_t *m){
    bn_t e,two; bn_zero(&two); two.w[0]=2; bn_sub(&e,m,&two); bn_modexp(r,a,&e,m);
}
#endif
