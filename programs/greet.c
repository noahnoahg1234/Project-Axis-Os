static int sc(int n,int a,int b,int c){int r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c));return r;}
static int slen(const char*s){int l=0;while(s[l])l++;return l;}
static void pr(const char*s){sc(4,1,(int)s,slen(s));}
struct uts{char s[65],n[65],r[65],v[65],m[65],d[65];};
void _start(void){
  struct uts u; sc(122,(int)&u,0,0);
  pr("I am running on: "); pr(u.s); pr(" ("); pr(u.m); pr(")\n");
  pr("What is your name? ");
  char buf[64]; int n=sc(3,0,(int)buf,63); if(n<0)n=0; if(n&&buf[n-1]=='\n')n--; buf[n]=0;
  pr("Hello, "); pr(buf); pr("! Nice to meet you.\n");
  sc(1,0,0,0);
}
